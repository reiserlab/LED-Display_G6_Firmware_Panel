// LAB-43 — v0.3.1 two-PIO scanner implementation. See display_scan_twopio.h.
//
// Ported from G6_Panels_Test_Firmware/test_firmware/single_led/src/main.cpp
// (Phase 4 "PIOFULL", commit bb26a44), adapted for production: per-row
// arm+poll, columns DMA'd directly out of bcm_plane_data[], no jitter-measure
// scaffolding (multicore_lockout / noInterrupts / DWT / GP43 probe stripped).

#include "constants.h"
#include "bcm.h"
#include "display_scan_twopio.h"

#if PANEL_REV == 31

#include <Arduino.h>
#include <hardware/pio.h>
#include <hardware/dma.h>
#include <pico/time.h>

// ---------------------------------------------------------------------------
// PIO programs (identical shape on both axes; same as the donor's PIOFULL).
//
// Row SM (PIO1, GPIOBASE=16, out base GP20, 20 pins). Init pulls all-rows-OFF
// (0xFFFFF) into Y; per row: pull pattern (active row LOW), OUT rows, pull
// delay, delay loop (holds the row for the whole column burst), restore Y,
// OUT all-rows-OFF, wrap to pull (stall for next row).
//   addr 0: pull block            ; init: all-rows-OFF mask
//   addr 1: mov y, osr            ; y = permanent all-rows-OFF
//   addr 2: pull block   [wrap tgt]; row pattern (active row LOW, others HIGH)
//   addr 3: out pins, 20          ; drive rows
//   addr 4: pull block            ; delay count
//   addr 5: mov x, osr
//   addr 6: jmp x--, 6            ; delay loop
//   addr 7: mov osr, y            ; restore all-rows-OFF
//   addr 8: out pins, 20  [wrap]  ; all rows OFF, wrap to addr 2 (stall)
static const uint16_t twopio_row_program_insn[] = {
    0x80a0, 0xa047, 0x80a0, 0x6014, 0x80a0, 0xa027, 0x0046, 0xa0e2, 0x6014,
};
#define TWOPIO_ROW_WRAP_TARGET 2
#define TWOPIO_ROW_WRAP        8
// Per-row non-delay cycles: pull+out+pull+mov+jmp_entry+mov+out = 7.
#define TWOPIO_ROW_OVERHEAD    7

static const pio_program_t twopio_row_program = {
    .instructions = twopio_row_program_insn,
    .length = 9,
    .origin = -1,
    .pio_version = 0,
#if PICO_PIO_VERSION > 0
    .used_gpio_ranges = 0x06,   // ranges 1+2: GPIO 16-47 (PIO1 GPIOBASE=16)
#endif
};

// Col SM (PIO0, GPIOBASE=0, out base GP0, 20 pins). Same as the v0.2.1
// led_col_program but with the terminal `irq wait 0` removed — the col SM
// loops autonomously, pulling the next {pattern, delay} as soon as DMA
// provides it, and stalls at `pull block` when DMA stops feeding it.
static const uint16_t twopio_col_program_insn[] = {
    0x80a0, 0xa047, 0x80a0, 0x6014, 0x80a0, 0xa027, 0x0046, 0xa0e2, 0x6014,
};
#define TWOPIO_COL_WRAP_TARGET 2
#define TWOPIO_COL_WRAP        8
// Per-bit-plane non-delay cycles (for the row-hold sum), distinct from
// bcm.cpp's PIO_ON_OVERHEAD_CYCLES (=5) which governs column ON brightness.
#define TWOPIO_COL_OVERHEAD    7

static const pio_program_t twopio_col_program = {
    .instructions = twopio_col_program_insn,
    .length = 9,
    .origin = -1,
    .pio_version = 0,
#if PICO_PIO_VERSION > 0
    .used_gpio_ranges = 0x03,   // ranges 0+1: GPIO 0-31 (PIO0 GPIOBASE=0)
#endif
};

// ---------------------------------------------------------------------------
// Runtime state
// ---------------------------------------------------------------------------
static PIO  row_pio = nullptr;   // PIO1
static PIO  col_pio = nullptr;   // PIO0
static uint row_sm  = 0;
static uint col_sm  = 0;
static uint row_off = 0;
static uint col_off = 0;
static int  ch_row  = -1;
static int  ch_col  = -1;
static bool twopio_loaded = false;

// Per-row {pattern, delay} pairs. Pattern is 20-bit inverted one-hot (active
// row LOW). Delay = sum of the row's column bit-plane durations + overhead +
// margin, in PIO cycles. Columns are DMA'd straight from bcm_plane_data[r].
static uint32_t row_data[PANEL_SIZE * 2];

// Per-row completion-poll timeout. A full-duty Gray_16 row burst is ~50 us;
// 2 ms is a generous backstop that only fires on a genuine SM/DMA hang (which
// would otherwise hold a row LOW with its LEDs stuck on indefinitely).
#define TWOPIO_ROW_TIMEOUT_US 2000u

// ---------------------------------------------------------------------------
void twopio_fail_dark() {
    if (row_pio) pio_sm_set_enabled(row_pio, row_sm, false);
    if (col_pio) pio_sm_set_enabled(col_pio, col_sm, false);
    for (int c = 0; c < PANEL_SIZE; c++) {
        gpio_set_function(COL_PIN[c], GPIO_FUNC_SIO);
        gpio_set_dir(COL_PIN[c], GPIO_OUT);
        gpio_put(COL_PIN[c], 0);          // cols LOW = OFF (normal polarity)
    }
    for (int r = 0; r < PANEL_SIZE; r++) {
        gpio_set_function(ROW_PIN[r], GPIO_FUNC_SIO);
        gpio_set_dir(ROW_PIN[r], GPIO_OUT);
        gpio_put(ROW_PIN[r], 1);          // rows HIGH = OFF
    }
}

// ---------------------------------------------------------------------------
bool twopio_init() {
    if (twopio_loaded) return true;

    // Row program on PIO1 with GPIOBASE=16 (covers GP16-47 → reaches GP20-39).
    row_pio = pio1;
    row_pio->gpiobase = 16;
    int rsm = pio_claim_unused_sm(row_pio, false);
    if (rsm < 0 || !pio_can_add_program(row_pio, &twopio_row_program)) {
        Serial.println("ERR: twopio row program: no SM/space on PIO1");
        if (rsm >= 0) pio_sm_unclaim(row_pio, (uint)rsm);
        row_pio->gpiobase = 0;
        row_pio = nullptr;
        return false;
    }
    row_sm  = (uint)rsm;
    row_off = pio_add_program(row_pio, &twopio_row_program);

    // Col program on PIO0 (GPIOBASE=0 default, covers GP0-31 → reaches GP0-19).
    col_pio = pio0;
    int csm = pio_claim_unused_sm(col_pio, false);
    if (csm < 0 || !pio_can_add_program(col_pio, &twopio_col_program)) {
        Serial.println("ERR: twopio col program: no SM/space on PIO0");
        if (csm >= 0) pio_sm_unclaim(col_pio, (uint)csm);
        pio_remove_program(row_pio, &twopio_row_program, row_off);
        pio_sm_unclaim(row_pio, row_sm);
        row_pio->gpiobase = 0;
        row_pio = nullptr;
        col_pio = nullptr;
        return false;
    }
    col_sm  = (uint)csm;
    col_off = pio_add_program(col_pio, &twopio_col_program);

    // SM configs. out_pins takes the absolute GPIO number; hardware subtracts
    // the per-PIO GPIOBASE. Shift right, manual pull, 20 OUT pins, full sysclk.
    pio_sm_config rc = pio_get_default_sm_config();
    sm_config_set_wrap(&rc, row_off + TWOPIO_ROW_WRAP_TARGET, row_off + TWOPIO_ROW_WRAP);
    sm_config_set_out_pins(&rc, ROW_PIN[0], PANEL_SIZE);
    sm_config_set_clkdiv(&rc, 1.0f);
    sm_config_set_out_shift(&rc, true, false, 32);
    pio_sm_init(row_pio, row_sm, row_off, &rc);
    pio_sm_set_consecutive_pindirs(row_pio, row_sm, ROW_PIN[0], PANEL_SIZE, true);

    pio_sm_config cc = pio_get_default_sm_config();
    sm_config_set_wrap(&cc, col_off + TWOPIO_COL_WRAP_TARGET, col_off + TWOPIO_COL_WRAP);
    sm_config_set_out_pins(&cc, COL_PIN[0], PANEL_SIZE);
    sm_config_set_clkdiv(&cc, 1.0f);
    sm_config_set_out_shift(&cc, true, false, 32);
    pio_sm_init(col_pio, col_sm, col_off, &cc);
    pio_sm_set_consecutive_pindirs(col_pio, col_sm, COL_PIN[0], PANEL_SIZE, true);

    // Switch pin funcsel from SIO (set by Display::initialize()) to PIO. After
    // this, any gpio_init() on these pins silently breaks PIO output — do not
    // re-init col/row pins downstream (same hazard as the v0.2.1 column path).
    for (int r = 0; r < PANEL_SIZE; r++) pio_gpio_init(row_pio, ROW_PIN[r]);
    for (int c = 0; c < PANEL_SIZE; c++) pio_gpio_init(col_pio, COL_PIN[c]);

    // Two DMA channels, DREQ-paced by the two TX FIFOs. write addr fixed at the
    // FIFO, read addr incrementing; per-row code sets read addr + trans count.
    ch_row = dma_claim_unused_channel(false);
    ch_col = dma_claim_unused_channel(false);
    if (ch_row < 0 || ch_col < 0) {
        Serial.println("ERR: twopio cannot claim 2 DMA channels");
        twopio_fail_dark();
        return false;
    }
    dma_channel_config dr = dma_channel_get_default_config(ch_row);
    channel_config_set_transfer_data_size(&dr, DMA_SIZE_32);
    channel_config_set_read_increment(&dr, true);
    channel_config_set_write_increment(&dr, false);
    channel_config_set_dreq(&dr, pio_get_dreq(row_pio, row_sm, true));
    dma_channel_configure(ch_row, &dr, &row_pio->txf[row_sm], &row_data[0], 2, false);

    dma_channel_config dc = dma_channel_get_default_config(ch_col);
    channel_config_set_transfer_data_size(&dc, DMA_SIZE_32);
    channel_config_set_read_increment(&dc, true);
    channel_config_set_write_increment(&dc, false);
    channel_config_set_dreq(&dc, pio_get_dreq(col_pio, col_sm, true));
    dma_channel_configure(ch_col, &dc, &col_pio->txf[col_sm], &bcm_plane_data[0][0][0], 2, false);

    // Prime: reset both SMs to entry, enable, push the one-time all-OFF masks
    // (consumed into Y), leaving both SMs stalled at their wrap_target waiting
    // for the first per-row DMA word.
    pio_sm_set_enabled(row_pio, row_sm, false);
    pio_sm_set_enabled(col_pio, col_sm, false);
    pio_sm_clear_fifos(row_pio, row_sm);
    pio_sm_clear_fifos(col_pio, col_sm);
    pio_sm_exec(row_pio, row_sm, pio_encode_jmp(row_off));
    pio_sm_exec(col_pio, col_sm, pio_encode_jmp(col_off));
    pio_sm_set_enabled(row_pio, row_sm, true);
    pio_sm_set_enabled(col_pio, col_sm, true);
    pio_sm_put_blocking(row_pio, row_sm, 0xFFFFFu);   // all rows OFF (HIGH)
    pio_sm_put_blocking(col_pio, col_sm, 0x00000u);   // all cols OFF (LOW)

    twopio_loaded = true;
    Serial.print("TWOPIO OK: row=pio");
    Serial.print(pio_get_index(row_pio)); Serial.print("/sm"); Serial.print(row_sm);
    Serial.print(" col=pio");
    Serial.print(pio_get_index(col_pio)); Serial.print("/sm"); Serial.print(col_sm);
    Serial.print(" dma row="); Serial.print(ch_row);
    Serial.print(" col="); Serial.println(ch_col);
    return true;
}

// ---------------------------------------------------------------------------
void twopio_precompute(int bcm_bits) {
    const uint32_t ALL_ROWS_OFF_20  = 0xFFFFFu;   // 20-bit, all rows HIGH = OFF
    const uint32_t ROW_SAFETY_MARGIN = 10;        // PIO cycles of overshoot
    for (int r = 0; r < PANEL_SIZE; r++) {
        // Inverted one-hot on the GP20 base (ROW_PIN[r]-20 → bit 0..19).
        row_data[r * 2 + 0] = ALL_ROWS_OFF_20 & ~(1UL << (ROW_PIN[r] - 20));
        // Row hold = sum of the row's column bit-plane wall-clock durations.
        uint32_t total_col_cycles = 0;
        for (int b = 0; b < bcm_bits; b++) {
            total_col_cycles += bcm_plane_data[r][b][1] + TWOPIO_COL_OVERHEAD;
        }
        total_col_cycles += ROW_SAFETY_MARGIN;
        row_data[r * 2 + 1] = (total_col_cycles > TWOPIO_ROW_OVERHEAD)
                            ? (total_col_cycles - TWOPIO_ROW_OVERHEAD) : 1;
    }
}

// Block until the in-flight row burst completes. The col-DMA-finish wait is the
// barrier that defeats the "SM is at wrap_target before it started" race: the
// DMA cannot finish until the col SM has DREQ-consumed every word, i.e. run the
// burst. Returns false on timeout.
static bool wait_burst_done() {
    uint32_t t0 = time_us_32();
    while (dma_channel_is_busy(ch_col)) {
        if ((uint32_t)(time_us_32() - t0) > TWOPIO_ROW_TIMEOUT_US) return false;
    }
    while (!(pio_sm_is_tx_fifo_empty(col_pio, col_sm) &&
             pio_sm_get_pc(col_pio, col_sm) == col_off + TWOPIO_COL_WRAP_TARGET)) {
        if ((uint32_t)(time_us_32() - t0) > TWOPIO_ROW_TIMEOUT_US) return false;
    }
    // Row SM drove its all-rows-OFF and stalled at wrap_target — panel dark.
    while (pio_sm_get_pc(row_pio, row_sm) != row_off + TWOPIO_ROW_WRAP_TARGET) {
        if ((uint32_t)(time_us_32() - t0) > TWOPIO_ROW_TIMEOUT_US) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
bool twopio_scan_row(int r, int bcm_bits) {
    // Row: 2 words {pattern, delay}. Col: bcm_bits {col_word, pio_delay} pairs,
    // straight out of bcm_plane_data[r] (contiguous [4][2] layout). Start both
    // in lockstep so they share a common phase origin.
    dma_channel_set_read_addr(ch_row, &row_data[r * 2], false);
    dma_channel_set_trans_count(ch_row, 2, false);
    dma_channel_set_read_addr(ch_col, &bcm_plane_data[r][0][0], false);
    dma_channel_set_trans_count(ch_col, (uint)(bcm_bits * 2), false);
    dma_start_channel_mask((1u << ch_row) | (1u << ch_col));

    if (!wait_burst_done()) {
        twopio_fail_dark();
        twopio_loaded = false;     // force re-init on next twopio_init() call
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
bool twopio_scan_frame(int bcm_bits) {
    for (int r = 0; r < PANEL_SIZE; r++) {
        if (!twopio_scan_row(r, bcm_bits)) return false;
    }
    return true;
}

#endif // PANEL_REV == 31

// Stage 2 (S2.1) — PIO column scanner implementation.
//
// Ported from G6_Panels_Test_Firmware/test_firmware/single_led/src/main.cpp
// (lines 382-472). See display_pio.h for resource-claim notes.

#include "display_pio.h"
#include "constants.h"
#include <hardware/pio.h>
#include <Streaming.h>

// PIO column driver program (Phase 3b from donor).
// Protocol:
//   1. Push all-OFF mask (0x00000 under normal polarity) once at start → stored in Y register
//   2. Per row: push column pattern (1-bit = col HIGH = ON, normal polarity), push delay count
//   3. PIO sets columns, delays, clears columns, signals IRQ
//
// Pin mapping: OUT pins base = COL_PIN[0], count = PANEL_SIZE (20)
// Timing: ON time = (delay_count + 5) PIO cycles from column set to clear
//
// Assembly:
//   addr 0: pull block           ; get all-OFF mask (0x00000, normal polarity)
//   addr 1: mov y, osr           ; y = permanent all-OFF value
//   addr 2: pull block           ; [wrap_target] get column pattern
//   addr 3: out pins, 20         ; set all 20 column pins → LEDs ON
//   addr 4: pull block           ; get delay count
//   addr 5: mov x, osr           ; x = delay count
//   addr 6: jmp x--, 6           ; delay loop (x+1 cycles)
//   addr 7: mov osr, y           ; restore all-OFF mask
//   addr 8: out pins, 20         ; all columns OFF (LOW under normal polarity)
//   addr 9: irq wait 0           ; [wrap] signal + stall until CPU clears flag
static const uint16_t led_col_program_insn[] = {
    0x80a0, // 0: pull block
    0xa047, // 1: mov y, osr
    0x80a0, // 2: pull block        [wrap target]
    0x6014, // 3: out pins, 20
    0x80a0, // 4: pull block
    0xa027, // 5: mov x, osr
    0x0046, // 6: jmp x--, 6
    0xa0e2, // 7: mov osr, y
    0x6014, // 8: out pins, 20
    0xc020, // 9: irq wait 0          [wrap]  (blocks PIO until CPU clears flag)
};

#define LED_COL_WRAP_TARGET 2
#define LED_COL_WRAP        9

static const pio_program_t led_col_program = {
    .instructions = led_col_program_insn,
    .length = 10,
    .origin = -1,
    .pio_version = 0,
#if PICO_PIO_VERSION > 0
    .used_gpio_ranges = 0x03,   // pins in both 0-15 and 16-31 ranges
#endif
};

// PIO state — populated by pio_init_program().
static PIO  pio_hw_inst  = nullptr;
static uint pio_sm_idx   = 0;
static uint pio_offset   = 0;
static bool pio_loaded   = false;


bool pio_init_program() {
    if (pio_loaded) return true;

    bool ok = pio_claim_free_sm_and_add_program(
        &led_col_program, &pio_hw_inst, &pio_sm_idx, &pio_offset);
    if (!ok) {
        Serial.println("ERR: No free PIO SM available for column scanner");
        return false;
    }

    pio_sm_config c = pio_get_default_sm_config();
    sm_config_set_wrap(&c, pio_offset + LED_COL_WRAP_TARGET,
                           pio_offset + LED_COL_WRAP);
    sm_config_set_out_pins(&c, COL_PIN[0], PANEL_SIZE);
    sm_config_set_clkdiv(&c, 1.0f);                          // full sys_clk
    sm_config_set_out_shift(&c, true, false, 32);            // shift right, manual pull

    pio_sm_init(pio_hw_inst, pio_sm_idx, pio_offset, &c);

    // Set column pins as outputs for the PIO SM (SM-side direction; pin
    // function is switched separately via col_pins_to_pio() in pio_start()).
    pio_sm_set_consecutive_pindirs(pio_hw_inst, pio_sm_idx,
                                   COL_PIN[0], PANEL_SIZE, true);

    pio_loaded = true;
    Serial.print("PIO column scanner OK: pio");
    Serial.print(pio_get_index(pio_hw_inst));
    Serial.print(" sm");
    Serial.print(pio_sm_idx);
    Serial.print(" offset");
    Serial.println(pio_offset);
    return true;
}


void col_pins_to_pio() {
    for (int c = 0; c < PANEL_SIZE; c++) {
        pio_gpio_init(pio_hw_inst, COL_PIN[c]);
    }
}


void col_pins_to_sio() {
    for (int c = 0; c < PANEL_SIZE; c++) {
        gpio_set_function(COL_PIN[c], GPIO_FUNC_SIO);
        gpio_set_dir(COL_PIN[c], GPIO_OUT);
        gpio_put(COL_PIN[c], 0);  // OFF state (col LOW under normal polarity)
    }
}


void pio_start() {
    // CRITICAL ORDERING: switch col pin function from SIO to PIO FIRST. If
    // Display::initialize() or any later code calls gpio_init() on the col
    // pins after this point, the pins revert to SIO and PIO output never
    // reaches the pads (silent failure — display stays dark with PIO
    // appearing healthy from the CPU side).
    col_pins_to_pio();

    pio_sm_set_enabled(pio_hw_inst, pio_sm_idx, false);
    pio_sm_clear_fifos(pio_hw_inst, pio_sm_idx);
    pio_sm_restart(pio_hw_inst, pio_sm_idx);
    pio_sm_exec(pio_hw_inst, pio_sm_idx, pio_encode_jmp(pio_offset));
    pio_interrupt_clear(pio_hw_inst, 0);
    // Re-assert SM output pin directions after restart (Production
    // Architecture § 6.5 — pio_sm_restart leaves pindirs at SM defaults).
    pio_sm_set_consecutive_pindirs(pio_hw_inst, pio_sm_idx,
                                   COL_PIN[0], PANEL_SIZE, true);
    pio_sm_set_enabled(pio_hw_inst, pio_sm_idx, true);

    // Prime the Y register with the all-off mask (0x00000 = all cols LOW under
    // normal polarity). PIO program instr 0+1 pulls this and copies to Y on
    // first dispatch.
    pio_sm_put_blocking(pio_hw_inst, pio_sm_idx, 0x00000);
}


PIO  pio_get_instance() { return pio_hw_inst; }
uint pio_get_sm()       { return pio_sm_idx; }

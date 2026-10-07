// Stage 2 (S2.3) — BCM precompute implementation.
//
// Ported from G6_Panels_Test_Firmware/test_firmware/single_led/src/main.cpp
// (lines 263-329) with V1-specific changes:
//   - base_T scaled to 3.0 µs (1 kHz refresh) from the donor's 0.5 µs (2P regime)
//   - Pattern matrix accessed via Eigen-backed pat.matrix()(lr, lc)
//   - Gray_2: 1 plane with weight 15 (donor's per-plane weights array is
//     simplified to a fixed branch)
//   - Gray_16: 4 planes with weights {1, 2, 4, 8} (standard BCM)
//   - duty_cycle == 0 zeroes ALL column words (strict-off per spec)

#include "bcm.h"
#include "bank_logic.h"
#include "constants.h"
#include "layout.h"
#include "protocol.h"

// Globals defined in this TU and declared extern in bcm.h.
float    bcm_base_on_us           = BCM_BASE_ON_US;            // 3.0 µs (1 kHz refresh); selftest can retune
float    bcm_triggered_base_on_us = BCM_TRIGGERED_BASE_ON_US;  // 1.0 µs, Triggered only (constants.h)
uint32_t cycles_per_us  = 150;        // overwritten in setup1() from clock_get_hz()
uint32_t bcm_plane_data[PANEL_SIZE][BCM_MAX_PLANES][2];
uint64_t row_on_mask[PANEL_SIZE];


void precompute_scan_masks() {
    for (int r = 0; r < PANEL_SIZE; r++) {
        row_on_mask[r] = (1ULL << ROW_PIN[r]);
    }
}


float bcm_base_on_us_for(Pattern &pat) {
    // Triggered rows must fit the line-sync gap, so that mode has its own,
    // shorter base (constants.h). Every other mode uses the 1 kHz base.
    return (pat.mode() == DisplayMode::Triggered) ? bcm_triggered_base_on_us
                                                  : bcm_base_on_us;
}


int precompute_bcm_data(Pattern &pat) {
    return precompute_bcm_data(pat, bcm_base_on_us_for(pat));
}


// Gray_2: 1 plane with weight 15 (equals Gray_16's full-bright sum).
// Gray_16: 4 planes with weights {1, 2, 4, 8}.
static int gray_bits(Pattern &pat) {
    return (pat.gray_level() == GrayLevel::Gray_2) ? 1 : 4;
}
static const uint32_t GRAY2_WEIGHTS[1]  = {15};
static const uint32_t GRAY16_WEIGHTS[4] = {1, 2, 4, 8};
static const uint32_t GRAY_WEIGHT_SUM   = 15;   // both gray levels


// Zero every plane from `first` up (col word and delay), so the scan loop
// never sees stale data even if the plane count shrinks mid-flight (e.g. a
// Gray_16 -> Gray_2 transition).
static void zero_planes_from(int first) {
    for (int r = 0; r < PANEL_SIZE; r++) {
        for (int p = first; p < BCM_MAX_PLANES; p++) {
            bcm_plane_data[r][p][0] = 0x00000;
            bcm_plane_data[r][p][1] = 0;
        }
    }
}


// Column words per schematic row and bit-plane: for each layout pixel, OR
// its bit-plane contributions into its schematic row's column pattern.
static void build_plane_words(Pattern &pat, int bcm_bits,
                              uint32_t words[PANEL_SIZE][4]) {
    for (int r = 0; r < PANEL_SIZE; r++)
        for (int b = 0; b < 4; b++)
            words[r][b] = 0x00000;

    for (int lr = 0; lr < PANEL_SIZE; lr++) {
        for (int lc = 0; lc < PANEL_SIZE; lc++) {
            uint8_t sch_row = layout_to_sch_row[lr][lc];
            uint8_t sch_col = layout_to_sch_col[lr][lc];
            uint8_t intensity = pat.matrix()(lr, lc)
                              & ((bcm_bits == 1) ? 0x01 : 0x0F);

            // Gray_2: intensity ∈ {0,1}; set the single plane iff intensity != 0
            // Gray_16: intensity ∈ [0,15]; set each plane b iff (intensity >> b) & 1
            for (int b = 0; b < bcm_bits; b++) {
                bool set_this_plane = (bcm_bits == 1)
                                    ? (intensity != 0)
                                    : ((intensity >> b) & 1);
                if (set_this_plane) {
                    words[sch_row][b] |= (1UL << sch_col);
                }
            }
        }
    }
}


int precompute_bcm_data(Pattern &pat, float base_on_us) {
    uint8_t duty_cycle = pat.duty_cycle();
    int bcm_bits = gray_bits(pat);
    const uint32_t *bcm_weights = (bcm_bits == 1) ? GRAY2_WEIGHTS : GRAY16_WEIGHTS;

    // Strict-off guard: at duty_cycle == 0, zero ALL column words so no LEDs
    // light. The PIO program will still emit its 33 ns (5-cycle) overhead
    // pulse per plane, but with col_word == 0 those overhead pulses drive no
    // LEDs. Documented spec semantics.
    if (duty_cycle == 0) {
        zero_planes_from(0);
        return bcm_bits;
    }

    uint32_t base_cycles = (uint32_t)(base_on_us * (float)cycles_per_us);

    // Per-plane PIO delay = (base_cycles * weight * duty_cycle / 255) - 5
    // overhead. At very low duty_cycle the scaled time may fall below the
    // 5-cycle overhead floor — clamp to 0 (single 33 ns overhead pulse,
    // documented brightness floor at the bottom of the scale).
    uint32_t pio_delays[4] = {0, 0, 0, 0};
    for (int b = 0; b < bcm_bits; b++) {
        uint32_t scaled = (uint32_t)((uint64_t)base_cycles
                                    * bcm_weights[b]
                                    * duty_cycle / 255);
        pio_delays[b] = (scaled > PIO_ON_OVERHEAD_CYCLES)
                      ? (scaled - PIO_ON_OVERHEAD_CYCLES) : 0;
    }

    uint32_t words[PANEL_SIZE][4];
    build_plane_words(pat, bcm_bits, words);
    for (int r = 0; r < PANEL_SIZE; r++) {
        for (int b = 0; b < bcm_bits; b++) {
            bcm_plane_data[r][b][0] = words[r][b];
            bcm_plane_data[r][b][1] = pio_delays[b];
        }
    }
    zero_planes_from(bcm_bits);
    return bcm_bits;
}


int precompute_bank_data(Pattern &pat, float base_on_us, const uint16_t window_ns[4]) {
    uint8_t duty_cycle = pat.duty_cycle();
    int bcm_bits = gray_bits(pat);
    const uint32_t *bcm_weights = (bcm_bits == 1) ? GRAY2_WEIGHTS : GRAY16_WEIGHTS;
    int nplanes = Bank::kNumBanks * bcm_bits;

    // Strict-off, as in precompute_bcm_data(): every plane dark at duty 0.
    if (duty_cycle == 0) {
        zero_planes_from(0);
        return nplanes;
    }

    // Per-(bank, bit-plane) PIO delays. The window table is specified at the
    // 3.0 µs base; Triggered's 1.0 µs base (or a selftest retune) scales it so
    // the row keeps the same length relative to the legacy row.
    float base_scale = base_on_us / BCM_BASE_ON_US;
    uint32_t pio_delays[Bank::kNumBanks][4] = {};
    for (int k = 0; k < Bank::kNumBanks; k++) {
        uint32_t win = Bank::window_cycles(window_ns[k], cycles_per_us, base_scale);
        for (int b = 0; b < bcm_bits; b++) {
            pio_delays[k][b] = Bank::pio_delay(
                Bank::plane_on_cycles(win, bcm_weights[b], GRAY_WEIGHT_SUM, duty_cycle));
        }
    }

    // Each window lights only its bank's columns. A zero-length window keeps
    // its bank dark (no 5-cycle floor pulse); its planes still run, with
    // col_word 0, so the row's plane layout never changes.
    //
    // Room for later (not built): a per-row compensation for the row-driver
    // droop would scale pio_delays[k][b] by a gain indexed by the number of
    // LEDs lit in (row, bank) — popcount(words[r][b] & mask), 0..5 — here;
    // twopio_precompute() already sums each row's own delays.
    uint32_t words[PANEL_SIZE][4];
    build_plane_words(pat, bcm_bits, words);
    for (int r = 0; r < PANEL_SIZE; r++) {
        for (int k = 0; k < Bank::kNumBanks; k++) {
            uint32_t mask = (window_ns[k] != 0) ? Bank::kBankMask[k] : 0u;
            for (int b = 0; b < bcm_bits; b++) {
                int p = Bank::plane_index(k, b, bcm_bits);
                bcm_plane_data[r][p][0] = words[r][b] & mask;
                bcm_plane_data[r][p][1] = pio_delays[k][b];
            }
        }
    }
    zero_planes_from(nplanes);
    return nplanes;
}

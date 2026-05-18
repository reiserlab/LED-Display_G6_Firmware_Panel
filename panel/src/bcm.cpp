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
#include "constants.h"
#include "layout.h"
#include "display_pio.h"
#include "protocol.h"

// Globals defined in this TU and declared extern in bcm.h.
float    bcm_base_on_us = 3.0f;       // 1 kHz refresh default; selftest can retune
uint32_t cycles_per_us  = 150;        // overwritten in setup1() from clock_get_hz()
uint32_t bcm_plane_data[PANEL_SIZE][4][2];
uint64_t row_on_mask[PANEL_SIZE];


void precompute_scan_masks() {
    for (int r = 0; r < PANEL_SIZE; r++) {
        row_on_mask[r] = (1ULL << ROW_PIN[r]);
    }
}


void precompute_bcm_data(Pattern &pat) {
    uint8_t duty_cycle = pat.duty_cycle();
    uint8_t bcm_bits;
    uint32_t bcm_weights[4] = {0, 0, 0, 0};

    if (pat.gray_level() == GrayLevel::Gray_2) {
        bcm_bits        = 1;
        bcm_weights[0]  = 15;          // single plane, full-bright weight
    } else {                            // Gray_16
        bcm_bits        = 4;
        bcm_weights[0]  = 1;
        bcm_weights[1]  = 2;
        bcm_weights[2]  = 4;
        bcm_weights[3]  = 8;
    }

    // Strict-off guard: at duty_cycle == 0, zero ALL column words so no LEDs
    // light. The PIO program will still emit its 33 ns (5-cycle) overhead
    // pulse per plane, but with col_word == 0 those overhead pulses drive no
    // LEDs. Documented spec semantics.
    if (duty_cycle == 0) {
        for (int r = 0; r < PANEL_SIZE; r++) {
            for (int b = 0; b < 4; b++) {
                bcm_plane_data[r][b][0] = 0x00000;
                bcm_plane_data[r][b][1] = 0;
            }
        }
        return;
    }

    uint32_t base_cycles = (uint32_t)(bcm_base_on_us * (float)cycles_per_us);

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

    // Initialize all rows × planes to all-off + per-plane delay.
    for (int r = 0; r < PANEL_SIZE; r++) {
        for (int b = 0; b < bcm_bits; b++) {
            bcm_plane_data[r][b][0] = 0x00000;
            bcm_plane_data[r][b][1] = pio_delays[b];
        }
        // Zero out any unused planes (Gray_2 has 3 unused planes) so the scan
        // loop reading them never sees stale data even if bcm_bits changes
        // mid-flight on a Gray_16 → Gray_2 transition.
        for (int b = bcm_bits; b < 4; b++) {
            bcm_plane_data[r][b][0] = 0x00000;
            bcm_plane_data[r][b][1] = 0;
        }
    }

    // For each layout pixel, OR its bit-plane contributions into the
    // corresponding schematic-row column pattern.
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
                    bcm_plane_data[sch_row][b][0] |= (1UL << sch_col);
                }
            }
        }
    }
}

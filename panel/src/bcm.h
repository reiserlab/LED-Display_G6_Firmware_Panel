#ifndef BCM_H
#define BCM_H
// Stage 2 (S2.3) — Binary-Code Modulation (BCM) precompute for the PIO column
// scanner.
//
// Builds `bcm_plane_data[row][plane][{col_word, pio_delay}]` from a Pattern's
// 20x20 layout-indexed pixel matrix. The scan loop in Display::show() pushes
// the (col_word, pio_delay) pairs to the PIO TX FIFO for each row × plane.
//
// Bit-plane semantics:
//   Gray_2:  1 plane,  weight {15}      (single weight-15 plane equals
//                                         Gray_16's full-bright sum)
//   Gray_16: 4 planes, weights {1,2,4,8} (standard 4-bit BCM)
//
// Total ON time at max + duty_cycle=255 matches between Gray_2 (pixel=1) and
// Gray_16 (pixel=15): both = 15 * base_T = 45 µs per row at base_T = 3 µs.
//
// Strict-off: duty_cycle == 0 zeroes all column words. The PIO program still
// emits its 33 ns (5-cycle) overhead pulse per plane, but with zero column
// words no LEDs light. Documented in the spec.

#include <Arduino.h>
#include "constants.h"
#include "pattern.h"

// Target 1 kHz refresh. Per-row budget: 1 ms / 20 rows = 50 µs.
// At full intensity + duty_cycle=255: weights sum to 15, so 15 * 3 µs = 45 µs ON
// per row + ~1 µs row-switch overhead = ~46 µs / row → ~920 µs scan ≈ 1.09 kHz.
//
// Runtime-tunable (NOT constexpr) so the selftest can retune brightness via a
// serial command without reflashing. Production builds never modify it.
extern float bcm_base_on_us;

// System clock cycles per microsecond. Set in setup1() from
// clock_get_hz(clk_sys) / 1000000UL (typically 150 on RP2350).
extern uint32_t cycles_per_us;

// BCM precompute output. [schematic_row][bit_plane][{pio_col_word, pio_delay}].
// pio_col_word: 20-bit mask, 1 = col HIGH = ON under normal polarity.
// pio_delay:    PIO delay count for jmp x--, 6 loop (cycles minus 5 overhead).
// Sized for max 4 planes (Gray_16); Gray_2 uses index 0 only.
extern uint32_t bcm_plane_data[PANEL_SIZE][4][2];

// Per-row GPIO 64-bit mask: bit set at ROW_PIN[r]. Used by Display::show() to
// gpio_clr_mask64() / gpio_set_mask64() for row-LOW (ON) / row-HIGH (OFF).
extern uint64_t row_on_mask[PANEL_SIZE];

// Build row_on_mask[] from ROW_PIN[]. Call once from setup1() before any
// scanning. Independent of pattern content.
void precompute_scan_masks();

// Build bcm_plane_data[] from `pat`. Reads pat.matrix()(lr, lc) for the
// pixel intensity at layout position (lr, lc), looks up the schematic
// (sch_row, sch_col) via layout_to_sch_*[lr][lc], and sets the appropriate
// bit-plane column word(s).
//
// Performance: ~400 inner iterations × 1-4 plane updates each. At ~50 Hz
// pattern-change cadence this is trivially cheap; safe to call from
// Display::update() on each new pattern dequeue.
void precompute_bcm_data(Pattern &pat);

#endif

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

// Base ON time for Triggered patterns only (1.0 µs: a full-duty row is 15 µs,
// short enough for a resonant scanner's turnaround gap — constants.h).
// precompute_bcm_data() picks it from the pattern's display mode.
extern float bcm_triggered_base_on_us;

// Column-program ON overhead: 5 cycles per bit-plane (pull + mov + jmp loop
// entry + mov + out), ~33 ns at 150 MHz regardless of the delay value.
// precompute_bcm_data() subtracts it from each plane's delay; it sets the
// brightness floor (very-low-duty_cycle nonlinearity).
constexpr uint32_t PIO_ON_OVERHEAD_CYCLES = 5;

// System clock cycles per microsecond. Set in setup1() from
// clock_get_hz(clk_sys) / 1000000UL (typically 150 on RP2350).
extern uint32_t cycles_per_us;

// Most planes a row can have: 4 banks x 4 Gray_16 bit-planes in the
// bank-window scan (LAB-286). Legacy scan uses 1 (Gray_2) or 4 (Gray_16).
constexpr int BCM_MAX_PLANES = 16;

// BCM precompute output. [schematic_row][plane][{pio_col_word, pio_delay}].
// pio_col_word: 20-bit mask, 1 = col HIGH = ON under normal polarity.
// pio_delay:    PIO delay count for jmp x--, 6 loop (cycles minus 5 overhead).
// Planes per row: legacy 1 (Gray_2) / 4 (Gray_16); bank windows 4 / 16
// (bank-major: T0's planes, then T1's, T2's, T3's). Planes past the row's
// count are zeroed. Each row's planes are one contiguous run of
// {col_word, delay} pairs, DMA'd straight to the column SM.
extern uint32_t bcm_plane_data[PANEL_SIZE][BCM_MAX_PLANES][2];

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
int precompute_bcm_data(Pattern &pat);

// Same, with an explicit base ON time (µs). The selftest retunes
// bcm_base_on_us from the other core, so callers that record the base read it
// once and pass it here. Returns the planes per row written (1 or 4).
int precompute_bcm_data(Pattern &pat, float base_on_us);

// LAB-286 bank-window scan: build bcm_plane_data[] for `pat` with one window
// per LED bank (bank_logic.h). Each Gray_2 row gets 4 planes (T0..T3, each
// lit for its whole window x duty/255); each Gray_16 row gets 16 (weights
// 1, 2, 4, 8 inside every window). window_ns[] is the table at duty 255 and
// the 3.0 µs base; another base scales it by base_on_us / BCM_BASE_ON_US.
// A window of 0 keeps that bank dark. Returns the planes per row (4 or 16).
int precompute_bank_data(Pattern &pat, float base_on_us, const uint16_t window_ns[4]);

// The BCM base ON time (µs) precompute_bcm_data() uses for `pat`: the
// Triggered base for Triggered patterns, bcm_base_on_us otherwise.
float bcm_base_on_us_for(Pattern &pat);

#endif

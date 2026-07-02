#ifndef DISPLAY_SCAN_TWOPIO_H
#define DISPLAY_SCAN_TWOPIO_H
// LAB-43 — v0.3.1 two-PIO scanner (rows on PIO1 + columns on PIO0, DMA-fed).
//
// v0.3.1 has contiguous row pins GP20-39 (no SPI gap) and column pins GP0-19,
// so BOTH axes can be driven by 20-bit PIO OUT programs + DMA, replacing the
// v0.2.1 single-PIO-columns + CPU-GPIO-rows path. This frees core 1 from the
// per-bit-plane busy-wait on the column scanner's IRQ.
//
// Ported from G6_Panels_Test_Firmware/test_firmware/single_led/src/main.cpp
// (Phase 4 "PIOFULL", commit bb26a44). The production adaptation:
//   - per-ROW arm+poll (one DMA burst per row) rather than per external
//     trigger. The col SM stalling after each row's bit-planes is the
//     alignment barrier; a single whole-frame DMA start would let the col SM
//     race ahead into the next row's pattern.
//   - columns DMA directly from bcm_plane_data[r] (its [4][2] layout is
//     already contiguous {col_word, pio_delay} pairs) — no flatten array.
//   - NO multicore_lockout / noInterrupts / DWT / GP43 probe (those existed
//     only for the donor's jitter measurement; locking out core 0 would
//     freeze production SPI ingest).
//
// Entire compilation unit is empty on v0.2.1 (PANEL_REV != 31).
#if PANEL_REV == 31

// Initialize the dual-PIO + dual-DMA scanner. Claims a row SM on PIO1
// (GPIOBASE 16 → reaches GP20-39) and a col SM on PIO0 (GPIOBASE 0 → GP0-19),
// two DMA channels, loads both programs, switches col+row pins to PIO funcsel,
// and primes both SMs to their stall point. Returns false on resource failure
// (caller should fail dark). Replaces pio_init_program()+pio_start() in
// setup1() on v0.3.1.
bool twopio_init();

// Recompute the per-row {pattern, delay} array from the current bcm_plane_data[]
// for `bcm_bits` planes (1 = Gray_2, 4 = Gray_16). Each row's PIO hold delay is
// the sum of that row's column bit-plane durations + per-plane overhead + a
// safety margin, so the row SM holds the row active for exactly the column
// burst. Call after precompute_bcm_data() whenever the pattern changes.
void twopio_precompute(int bcm_bits);

// Scan a single row r through all bcm_bits planes as one autonomous DMA burst,
// then block until it completes (col DMA drained + col SM stalled + row SM back
// at all-rows-OFF). Returns false on completion-poll timeout (fail-dark applied
// before returning). Used by Display::show_row() — covers Triggered and Gated.
bool twopio_scan_row(int r, int bcm_bits);

// Scan a full 20-row frame (loops twopio_scan_row over all rows). Returns false
// if any row burst timed out. Used by Display::show() for Persistent/Oneshot.
bool twopio_scan_frame(int bcm_bits);

// Drive all pins dark via SIO (cols LOW, rows HIGH = OFF) and disable both SMs.
// Used on fatal init failure (the per-row timeout path self-heals instead).
void twopio_fail_dark();

// Count of per-row completion-poll timeouts since boot (should stay 0). Each
// one aborted the row's DMA, drove a fault frame, and re-primed the SMs.
// Surfaced in the SPI_DIAG dump for bring-up + production visibility.
uint32_t twopio_get_timeouts();

#if SPI_DIAG
// LAB-43 rare-burst hunt: worst single-row completion-poll wait. A row held far
// longer than its neighbours is a localized bright burst. win_* = worst since
// last call (read-and-cleared, i.e. per heartbeat window); all_* = worst ever.
void twopio_get_longrow(uint32_t &win_us, uint32_t &win_row,
                        uint32_t &all_us, uint32_t &all_row);
#endif

#endif // PANEL_REV == 31
#endif // DISPLAY_SCAN_TWOPIO_H

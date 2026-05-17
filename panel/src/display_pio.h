#ifndef DISPLAY_PIO_H
#define DISPLAY_PIO_H
// Stage 2 (S2.1) — PIO column scanner for BCM display engine.
//
// Ported from G6_Panels_Test_Firmware/test_firmware/single_led/src/main.cpp
// (lines 382-472, commit reference: pre-stage-2 single_led firmware).
//
// PIO program drives all 20 column pins from a 20-bit column word with a
// per-plane CPU-supplied delay count. After each (col_word, delay) pair the
// PIO raises IRQ flag 0 and stalls; CPU consumes the IRQ + clears it before
// pushing the next plane.
//
// Resource claim: PIO state machine and IRQ flag 0 are claimed dynamically by
// pio_init_program() via pio_claim_free_sm_and_add_program(). Module-private
// statics in display_pio.cpp hold the assigned PIO instance, SM index, and
// program offset. Future PIO consumers (v3 trigger, v2 PSRAM-DMA) must claim
// their own resources via the same dynamic API — do not hardcode SM indices.

#include <Arduino.h>
#include <hardware/pio.h>

// PIO ON overhead: the column program takes 5 cycles of overhead per plane
// (pull + mov + jmp_loop_entry + mov + out). At sys_clk = 150 MHz this is
// ~33 ns per plane regardless of delay value. Used by bcm.cpp to compute
// the brightness floor (very-low-stretch nonlinearity).
constexpr uint32_t PIO_ON_OVERHEAD_CYCLES = 5;

// Load PIO program + claim a free SM. Returns true on success, false if no
// SM is available. Caller is responsible for fail-dark on false (see setup1
// in main.cpp).
bool pio_init_program();

// Full-lifecycle scanner start: switches column pin function from SIO to PIO,
// clears FIFOs, restarts the SM, enables it, and pushes the all-off Y-register
// init word. Must be called AFTER pio_init_program() and AFTER all SIO-mode
// column setup (Display::initialize()) has completed.
void pio_start();

// Switch all 20 column pins between PIO-driven and plain SIO (GPIO) output.
// col_pins_to_pio() is called inside pio_start(). col_pins_to_sio() is provided
// for fail-dark / selftest shutdown; not normally needed during steady-state
// scanning.
void col_pins_to_pio();
void col_pins_to_sio();

// Accessors for bcm.cpp / display.cpp scan loop. Return the PIO instance and
// SM index assigned by pio_init_program().
PIO  pio_get_instance();
uint pio_get_sm();

#endif

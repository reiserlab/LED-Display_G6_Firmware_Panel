#include "constants.h"
#include "display.h"
#include "bcm.h"
#include "display_pio.h"
#include "display_scan_twopio.h"
#include "predef_patterns.h"
#include "protocol.h"
#include <hardware/pio.h>
#include <hardware/gpio.h>
#include <pico/time.h>


// EINT (GP45) fast read. The pin is in the high GPIO bank (bits 32..47 of
// sio_hw->gpio_hi_in). Reading the SIO register directly is ~10 ns vs
// gpio_get()'s function-call overhead. Same pattern as the prototype's
// G6_Panels_Test_Firmware/single_led/src/main.cpp:wait_for_trigger.
static inline bool eint_high() {
    return (sio_hw->gpio_hi_in & (1u << (EINT_PIN - 32))) != 0;
}


// Tight-poll wait for an EINT rising edge with a wall-clock timeout.
// First waits for LOW (re-arm — handles the case where the pin is already
// HIGH from a prior edge), then for LOW->HIGH. Returns false if the timeout
// expires in either phase, true on rising edge detected. The 64-bit
// `time_us_64()` read is ~50 ns; well below the 125 µs min-edge-period at
// the spec's 8 kHz target.
static bool wait_eint_rising(uint32_t timeout_us) {
    uint64_t deadline = time_us_64() + timeout_us;

    // Phase 1: wait for LOW (re-arm). If pin is already LOW we skip.
    while (eint_high()) {
        if (time_us_64() >= deadline) return false;
        tight_loop_contents();
    }
    // Phase 2: wait for LOW -> HIGH (rising edge).
    while (!eint_high()) {
        if (time_us_64() >= deadline) return false;
        tight_loop_contents();
    }
    return true;
}

// ---- Scan timing instrumentation + target-period enforcement ----
//
// Default 1000 µs = 1 kHz refresh. Tunable at runtime via the selftest 'r'
// command. At low duty_cycle the scan completes in tens of µs and we busy-
// wait for the remaining time to maintain a constant scan period; this is
// what gives duty_cycle its intended linear-PWM brightness semantics.
volatile uint32_t display_target_period_us = 1000;

static volatile uint32_t s_scan_count        = 0;
static volatile uint32_t s_period_us_total   = 0;
static volatile uint32_t s_scan_us_total     = 0;
static volatile uint32_t s_period_us_min     = UINT32_MAX;
static volatile uint32_t s_period_us_max     = 0;
static volatile uint32_t s_scan_us_min       = UINT32_MAX;
static volatile uint32_t s_scan_us_max       = 0;

#if STAGE2_SELFTEST
// Bench: per-row "free work" injection (µs) — see display.h / the 'k' command.
volatile uint32_t g_bench_inject_us = 0;

// Cycle-precise (DWT CYCCNT, ~6.67 ns @ 150 MHz) per-frame scan-time stats for
// the 'j' jitter command — finer than the 1 µs time_us_32 stats. DWT is
// core-local on the M33, so it is enabled lazily here on core 1 (where show()
// runs). Ported from G6_Panels_Test_Firmware dwt_init().
#include <hardware/structs/m33.h>
static volatile uint32_t s_scan_cyc_min   = UINT32_MAX;
static volatile uint32_t s_scan_cyc_max   = 0;
static volatile uint64_t s_scan_cyc_total = 0;
static volatile uint32_t s_scan_cyc_count = 0;
static bool s_dwt_ready = false;
static inline void dwt_ensure() {
    if (s_dwt_ready) return;
    m33_hw->demcr    |= (1UL << 24);   // TRCENA
    m33_hw->dwt_ctrl |= (1UL << 0);    // CYCCNTENA
    m33_hw->dwt_cyccnt = 0;
    s_dwt_ready = true;
}
void display_get_scan_cycle_stats(uint32_t &cmin, uint32_t &cmax,
                                  uint32_t &cavg, uint32_t &ccount) {
    ccount = s_scan_cyc_count;
    cmin   = (s_scan_cyc_min == UINT32_MAX) ? 0 : s_scan_cyc_min;
    cmax   = s_scan_cyc_max;
    cavg   = s_scan_cyc_count ? (uint32_t)(s_scan_cyc_total / s_scan_cyc_count) : 0;
}
#endif

void display_reset_scan_stats() {
    s_scan_count       = 0;
    s_period_us_total  = 0;
    s_scan_us_total    = 0;
    s_period_us_min    = UINT32_MAX;
    s_period_us_max    = 0;
    s_scan_us_min      = UINT32_MAX;
    s_scan_us_max      = 0;
#if STAGE2_SELFTEST
    s_scan_cyc_min     = UINT32_MAX;
    s_scan_cyc_max     = 0;
    s_scan_cyc_total   = 0;
    s_scan_cyc_count   = 0;
#endif
}

void display_get_scan_stats(ScanStats &out) {
    out.count            = s_scan_count;
    out.period_us_total  = s_period_us_total;
    out.scan_us_total    = s_scan_us_total;
    out.period_us_min    = (s_period_us_min == UINT32_MAX) ? 0 : s_period_us_min;
    out.period_us_max    = s_period_us_max;
    out.scan_us_min      = (s_scan_us_min == UINT32_MAX) ? 0 : s_scan_us_min;
    out.scan_us_max      = s_scan_us_max;
}


// Cross-core flag — defined here, declared in display.h.
volatile bool Display::error_display_active = false;


Display::Display(queue_t &display_queue, queue_t &error_request_queue)
    : display_queue_(display_queue),
      error_request_queue_(error_request_queue)
{}

void Display::initialize() {
    // S2.2: Initialize col and row pins as plain SIO outputs in their dark
    // resting state. PIO will take over the col pins later via pio_start()
    // (in setup1, AFTER this function returns). The ordering matters — see
    // the comment block in main.cpp::setup1().
    col_pin_mask_ = 0;
    for (size_t i=0; i<PANEL_SIZE; i++) {
        gpio_init(COL_PIN[i]);
        col_pin_mask_ |= (uint64_t(1) << COL_PIN[i]);
    }
    row_pin_mask_ = 0;
    for (size_t i=0; i<PANEL_SIZE; i++) {
        gpio_init(ROW_PIN[i]);
        row_pin_mask_ |= (uint64_t(1) << ROW_PIN[i]);
    }

    gpio_set_dir_out_masked64(col_pin_mask_);
    gpio_clr_mask64(col_pin_mask_);          // cols LOW = OFF (normal polarity)

    gpio_set_dir_out_masked64(row_pin_mask_);
    gpio_set_mask64(row_pin_mask_);          // rows HIGH = OFF (normal polarity)

    // EINT input for V1 Triggered (0x12 / 0x32) and Gated (0x13 / 0x33).
    // Pull-down so a disconnected EINT line stays LOW (no spurious rising
    // edges in Triggered mode, panel dark in Gated mode).
    gpio_init(EINT_PIN);
    gpio_set_dir(EINT_PIN, GPIO_IN);
    gpio_pull_down(EINT_PIN);
}


void Display::update() {
    // Error-display window: highest priority. Drain (and discard) any
    // patterns queued by core 0 while the window is active so they don't
    // pile up to be replayed when the window ends. The display keeps
    // refreshing the error glyph until the timer expires.
    if (error_display_active) {
        // Discard any patterns queued during the error window; do NOT
        // mutate pat_/have_pattern_. The saved snapshot is what we restore.
        Pattern discarded;
        while (queue_try_remove(&display_queue_, &discarded)) { /* drop */ }
        // Also drain any redundant error-raises that arrived during the
        // window (rate-limit should prevent this, but be defensive).
        uint32_t dropped_slot;
        while (queue_try_remove(&error_request_queue_, &dropped_slot)) { /* drop */ }

        if (time_us_64() >= error_until_us_) {
            exit_error_display();
            // Fall through to the normal path below — restored state will
            // be scanned this iteration.
        } else {
            show();   // continue rendering the error glyph
            return;
        }
    }

    // Pending error-raise from core 0? Enter the error window before
    // touching the display queue, so we capture the current state cleanly.
    uint32_t slot;
    if (queue_try_remove(&error_request_queue_, &slot)) {
        enter_error_display(slot);
        show();
        return;
    }

    // S2.2: drain-to-latest. In practice the queue should never have more than
    // one pending pattern; if it does (controller over-pushing or scan loop
    // stalling), keep only the most recent. Older frames are accounted for in
    // frames_skipped_ and surfaced in the serial heartbeat.
    Pattern latest;
    int pops = 0;
    while (queue_try_remove(&display_queue_, &latest)) {
        pops++;
    }
    if (pops > 0) {
        pat_ = latest;
        have_pattern_ = true;
        oneshot_pending_ = (pat_.mode() == DisplayMode::Oneshot);
        // V1 Triggered: arm a fresh consumption window; the spec's "new
        // pattern resets the internal row counter to 0" (g6_01:205) is
        // realized by this re-arm. Drain-to-latest above already drops
        // intermediate patterns.
        if (pat_.mode() == DisplayMode::Triggered) {
            triggered_active_   = true;
            triggered_next_row_ = 0;
        } else {
            triggered_active_   = false;
        }
        precompute_bcm_data(pat_);
#if PANEL_REV == 31
        twopio_precompute((pat_.gray_level() == GrayLevel::Gray_2) ? 1 : 4);
#endif
        if (pops > 1) frames_skipped_ += (pops - 1);
    }

    if (!have_pattern_) {
        return;  // boot state — rows already OFF + cols already OFF via initialize()
    }

    switch (pat_.mode()) {
        case DisplayMode::Persistent:
            // V1 Persistent: scan one frame; loop1() re-enters update() and
            // we scan again. Pattern stays loaded until next message replaces
            // it.
            show();
            break;

        case DisplayMode::Oneshot:
            // V1 Oneshot: scan exactly one full frame after the message
            // arrives, then go idle (display dark) until the next message.
            if (oneshot_pending_) {
                show();
                oneshot_pending_ = false;
            }
            break;

        case DisplayMode::Triggered: {
            // V1 Triggered (0x12 / 0x32): tight-poll EINT and drive one row
            // per rising edge. 20 edges = one frame consumed → return to
            // dark/idle. 1 s sanity timeout aborts the consumption if no
            // edges arrive (e.g., EINT disconnected) so core 1 doesn't lock.
            // Per the chosen tight-poll-no-yield model, a new pattern
            // arriving mid-consumption is delayed up to one frame (or 1 s)
            // before taking effect — controller's responsibility per spec.
            if (!triggered_active_) break;
            bool timed_out = false;
            while (triggered_next_row_ < PANEL_SIZE) {
                if (!wait_eint_rising(1'000'000)) {
                    timed_out = true;
                    break;
                }
                // next-steps-pr-15.md #1 / gh-16 #1: a faulted row must not
                // silently advance the row counter — that would consume the
                // EINT edge without the row ever actually displaying. Retry
                // the SAME row on the next edge instead.
                if (show_row(triggered_next_row_)) {
                    triggered_next_row_++;
                }
            }
            if (timed_out || triggered_next_row_ >= PANEL_SIZE) {
                triggered_active_ = false;
                have_pattern_     = false;   // dark until next command
            }
            break;
        }

        case DisplayMode::Gated:
            // V1 Gated (0x13 / 0x33): EINT level is a global LED output-
            // enable mask. While HIGH, refresh the latest queued pattern
            // (Persistent-like behavior — the spec's "continuous refresh
            // while HIGH" matches this without requiring the controller to
            // re-stream at 1 kHz). While LOW, do nothing — drain-to-latest
            // above keeps the queue from backing up; new patterns are
            // accepted but not visibly displayed (g6_01:218).
            if (eint_high()) {
                show_gated();
            }
            break;
    }
}


void Display::enter_error_display(uint32_t slot) {
    // Snapshot the live display state. Pattern is copyable (only an Eigen
    // 20x20 + scalars), so by-value is fine. Includes Triggered consumption
    // state so a Triggered pattern interrupted by an error resumes from the
    // saved row counter when the error window closes.
    saved_pattern_            = pat_;
    saved_have_pattern_       = have_pattern_;
    saved_oneshot_pending_    = oneshot_pending_;
    saved_triggered_active_   = triggered_active_;
    saved_triggered_next_row_ = triggered_next_row_;

    // Load the error glyph. Fall back to slot 0 ("ERR") if the requested
    // slot is unprogrammed, and fall back to the compiled-in glyph if even
    // that fails (blob missing/corrupt).
    bool ok = predef::load_into(error_pattern_, slot);
    if (!ok && slot != 0) {
        ok = predef::load_into(error_pattern_, 0);
    }
    if (!ok) {
        predef::load_fallback_err(error_pattern_);
    }

    pat_                  = error_pattern_;
    have_pattern_         = true;
    oneshot_pending_      = false;     // Persistent within the error window
    triggered_active_     = false;     // suspended; will be restored on exit
    precompute_bcm_data(pat_);
#if PANEL_REV == 31
    twopio_precompute((pat_.gray_level() == GrayLevel::Gray_2) ? 1 : 4);
#endif
    error_until_us_       = time_us_64() + ERROR_DISPLAY_DURATION_US;
    error_display_active  = true;      // single-writer; volatile suffices
}


void Display::exit_error_display() {
    error_display_active = false;

    if (!saved_have_pattern_) {
        // Nothing was displayed before the error — go dark.
        have_pattern_     = false;
        oneshot_pending_  = false;
        triggered_active_ = false;
        return;
    }

    if (saved_pattern_.mode() == DisplayMode::Oneshot && !saved_oneshot_pending_) {
        // The previous Oneshot had already completed its single scan; the
        // panel was dark before the error. Don't re-display it.
        have_pattern_     = false;
        oneshot_pending_  = false;
        triggered_active_ = false;
        return;
    }

    pat_                = saved_pattern_;
    have_pattern_       = true;
    oneshot_pending_    = saved_oneshot_pending_;
    triggered_active_   = saved_triggered_active_;
    triggered_next_row_ = saved_triggered_next_row_;
    precompute_bcm_data(pat_);
#if PANEL_REV == 31
    twopio_precompute((pat_.gray_level() == GrayLevel::Gray_2) ? 1 : 4);
#endif
}


bool Display::show_row(int r) {
    // Drive one row × all bit-planes for the current pat_. Called by:
    //   - show()'s CPU-driven loop (v0.2.1 / PANEL_REV != 31 only; the
    //     PANEL_REV==31 path calls twopio_scan_frame() directly instead)
    //   - show_gated() in the 20-row loop with per-row EINT check
    //   - V1 Triggered: one call per EINT rising edge
    //
    // Returns false if the row faulted (two-PIO completion-poll timeout,
    // PANEL_REV==31 only; always true on the CPU-driven v0.2.1 path outside
    // STAGE2_SELFTEST, which has no bounded failure mode in production).
    //
    //   N = 1 for Gray_2 (single weight-15 plane)
    //   N = 4 for Gray_16 (weights {1, 2, 4, 8})
    //
    // Strict-off (duty_cycle == 0): bcm_plane_data column words are all
    // zero (precompute_bcm_data guard); the row still scans but drives no
    // LEDs.
    //
    // ---- Timing (duty_cycle controls LED-on window) ----------------------
    // Per-row drive time = the window the row pin is LOW (LEDs enabled).
    // Scales with duty_cycle and gray_level:
    //   Gray_2  duty=255: ~45 µs   duty=128: ~23 µs   duty=85: ~15 µs
    //   Gray_2  duty=64:  ~11 µs   duty=1:    ~1-3 µs (PIO floor)
    //   Gray_16 duty=255: ~50 µs   duty=128: ~25 µs   duty=85: ~17 µs
    //   Gray_16 duty=1:   ~5 µs (4× plane floor)
    // After the planes complete, show_row sets the row HIGH (=OFF) on exit;
    // the panel is naturally dark between rows.
    //
    // Implications for V1 Triggered (0x12 / 0x32):
    //   Canonical use case is sub-frame sync where each row's LED flash
    //   should occupy a SMALL fraction (~10-15%) of its EINT trigger
    //   interval, so the LED-on window stays clear of adjacent triggers'
    //   downstream sampling. At 8 kHz EINT (125 µs interval):
    //       duty_cycle ~64-85 → ~10-15% LED-on (per-row), 400 fps total
    //   At duty_cycle=255 the per-row drive (~45-50 µs) is ~40% of the
    //   125 µs interval — a HARD UPPER BOUND, not a target. Operating
    //   near it leaves no dead time between rows.
    //   See g6_01-panel-protocol.md § Timing considerations for the full
    //   table and worked examples. **Bench-test the intended (duty_cycle,
    //   EINT freq, gray level) combination before production.**
    //
    // Implications for V1 Gated (0x13 / 0x33):
    //   Mid-scan HIGH->LOW response latency ≈ one per_row_drive_time
    //   (we check eint_high() before each row in show_gated). At full
    //   duty that's ~50 µs; at low duty it's a few µs. Spec wording is
    //   "within one bit-plane interval" — per-row is a documented
    //   departure (see plan).
    // ----------------------------------------------------------------------
    uint8_t bcm_bits = (pat_.gray_level() == GrayLevel::Gray_2) ? 1 : 4;
#if PANEL_REV == 31
    // v0.3.1: both axes are PIO/DMA-driven — one autonomous burst scans this
    // row through all bit-planes. Reached by Triggered (one call per EINT
    // rising edge) and Gated (one call per row, level checked between rows).
    return twopio_scan_row(r, bcm_bits);
#else
    PIO  pio = pio_get_instance();
    uint sm  = pio_get_sm();

#if STAGE2_SELFTEST
    // Bench: CPU-row path has no autonomous burst to overlap, so injected
    // "free work" lands here and adds directly to this row's scan time.
    if (g_bench_inject_us) busy_wait_us(g_bench_inject_us);
#endif
    gpio_clr_mask64(row_on_mask[r]);   // row LOW = ON (normal polarity)
    for (int b = 0; b < bcm_bits; b++) {
        pio_sm_put_blocking(pio, sm, bcm_plane_data[r][b][0]);
        pio_sm_put_blocking(pio, sm, bcm_plane_data[r][b][1]);
        #if STAGE2_SELFTEST
            // Selftest-only diagnostic: 100 µs timeout + fail-dark on
            // missed PIO IRQ. Without this, a PIO mis-config silently
            // hangs core 1 with one row LOW (LEDs stuck on indefinitely).
            uint32_t t0 = time_us_32();
            while (!pio_interrupt_get(pio, 0)) {
                if ((uint32_t)(time_us_32() - t0) > 100) {
                    gpio_set_mask64(row_on_mask[r]);   // row OFF
                    Serial.print("PIO IRQ TIMEOUT row=");
                    Serial.print(r);
                    Serial.print(" plane=");
                    Serial.println(b);
                    pio_sm_set_enabled(pio, sm, false);
                    return false;
                }
            }
        #else
            while (!pio_interrupt_get(pio, 0)) { tight_loop_contents(); }
        #endif
        pio_interrupt_clear(pio, 0);
    }
    gpio_set_mask64(row_on_mask[r]);   // row HIGH = OFF
    return true;
#endif
}


void Display::show() {
    // Full 20-row BCM scan. Walks all rows pushing N bit-planes' worth of
    // (col_word, pio_delay) pairs through the PIO TX FIFO via show_row().
    //
    // After the scan, busy-wait until `display_target_period_us` has elapsed
    // since the start of this scan. This enforces a fixed scan rate so that
    // brightness ∝ ON_time / Period. Without this, low-duty_cycle scans
    // complete in tens of µs and loop1() calls show() back-to-back — the
    // LED-off portion of the duty cycle shrinks, collapsing the
    // duty_cycle=1 vs duty_cycle=255 brightness ratio from the intended
    // ~260x to a useless ~4x.

    uint32_t t_start = time_us_32();
#if STAGE2_SELFTEST
    dwt_ensure();
    uint32_t c_start = m33_hw->dwt_cyccnt;   // cycle-precise scan-time start
#endif

#if PANEL_REV == 31
    // v0.3.1: one DMA-fed two-PIO burst per row; core 1 only arms + polls,
    // freed from the per-bit-plane FIFO push + IRQ busy-wait of the v0.2.1 path.
    int bcm_bits = (pat_.gray_level() == GrayLevel::Gray_2) ? 1 : 4;
    if (!twopio_scan_frame(bcm_bits)) {
        // Faulted frame: twopio already aborted DMA + re-primed the SMs. Skip
        // this frame's period pad + scan-stats accounting (a timeout would skew
        // the timing stats) and let loop1() re-enter. twopio_get_timeouts()
        // surfaces the fault count in the SPI_DIAG dump.
        return;
    }
#else
    for (int r = 0; r < PANEL_SIZE; r++) {
        show_row(r);
    }
#endif

    uint32_t t_scan_end = time_us_32();
    uint32_t scan_us    = t_scan_end - t_start;
#if STAGE2_SELFTEST
    uint32_t scan_cyc   = m33_hw->dwt_cyccnt - c_start;   // scan-only cycles
#endif

    // Pad to target period. If the scan already overran the target (e.g.,
    // Gray_16 duty_cycle=255 needs ~920 µs which is close to 1000 µs), skip
    // padding and let the scan rate degrade gracefully.
    uint32_t target = display_target_period_us;
    if (scan_us < target) {
        uint32_t remaining = target - scan_us;
        busy_wait_us(remaining);
    }

    uint32_t t_period_end = time_us_32();
    uint32_t period_us    = t_period_end - t_start;

    // Update rolling stats. Single-writer (core 1 only); reader on core 0
    // tolerates the racy snapshot.
    s_scan_count++;
    s_period_us_total += period_us;
    s_scan_us_total   += scan_us;
    if (period_us < s_period_us_min) s_period_us_min = period_us;
    if (period_us > s_period_us_max) s_period_us_max = period_us;
    if (scan_us   < s_scan_us_min)   s_scan_us_min   = scan_us;
    if (scan_us   > s_scan_us_max)   s_scan_us_max   = scan_us;
#if STAGE2_SELFTEST
    s_scan_cyc_count++;
    s_scan_cyc_total += scan_cyc;
    if (scan_cyc < s_scan_cyc_min) s_scan_cyc_min = scan_cyc;
    if (scan_cyc > s_scan_cyc_max) s_scan_cyc_max = scan_cyc;
#endif
}


void Display::show_gated() {
    // Gated full-frame scan: 20 rows with a per-row EINT-level check, then
    // pad to display_target_period_us (1 kHz default) so per-pixel duty
    // matches Persistent — the spec's "standard BCM scan rate" at line 217
    // (g6_01-panel-protocol.md). Without padding, scans would back-to-back
    // and brightness would saturate to ~100% regardless of duty_cycle.
    //
    // EINT mid-scan response: per-row granularity (~one row_drive_time,
    // which itself depends on duty_cycle + gray_level — see show_row).
    // Spec wants "one bit-plane interval"; ~50 µs at full duty is well
    // below behavior-rig-observer perception. Per-plane granularity is a
    // future refinement if needed.
    //
    // On EINT drop mid-scan: abandon remaining rows AND skip the pad — the
    // goal is "panel dark within one row interval", so we return ASAP and
    // let update() see EINT LOW and not call show_gated() again. LEDs
    // are already OFF for any rows that didn't fire and rows that DID
    // fire have already had their row pin set HIGH=OFF by show_row's exit.
    uint32_t t_start = time_us_32();
    for (int r = 0; r < PANEL_SIZE; r++) {
        if (!eint_high()) return;
        show_row(r);
    }
    // Full scan completed. Pad to target period — LEDs are OFF during the
    // pad (last row's row-OFF already ran), so this neither delays the
    // gate-drop response nor lights anything.
    uint32_t scan_us = time_us_32() - t_start;
    uint32_t target  = display_target_period_us;
    if (scan_us < target) {
        busy_wait_us(target - scan_us);
    }
}

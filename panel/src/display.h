#ifndef DISPLAY_H
#define DISPLAY_H
#include <pico.h>
#include "pico/util/queue.h"
#include "pattern.h"

class Display {

    public:
        Display(queue_t &display_queue, queue_t &error_request_queue);
        void initialize();
        void update();
        void show();

        // V1 Gated (cmd 0x13 / 0x33): show one full 20-row scan with a
        // per-row EINT-level check. EINT going LOW mid-scan abandons the
        // remaining rows; spec requires "within one bit-plane interval"
        // but we sample per-row (~50 us granularity) — see plan.
        void show_gated();

        // Drive a single row × all bit-planes of the current pat_. Used by
        // both show_gated() (in its 20-row loop) and the V1 Triggered state
        // machine (one call per EINT rising edge). Returns false if the row
        // faulted (two-PIO completion-poll timeout, PANEL_REV==31 only) so
        // Triggered can retry the same row on the next edge instead of
        // silently advancing past it (next-steps-pr-15.md #1 / gh-16 #1).
        bool show_row(int r);

        // S2.2: counters surfaced in Messenger serial heartbeat.
        uint32_t frames_skipped() const { return frames_skipped_; }

        // Cross-core flag used by Messenger to skip command dispatch + CIPO
        // confirmation while the panel is rendering an error glyph. Single
        // writer (core 1) / single reader (core 0); volatile is sufficient
        // — see plan §8 for the staleness-bound rationale.
        //
        // (Static-storage to allow access from a non-`Display` context if
        // ever needed; in practice only Messenger reads it.)
        static volatile bool error_display_active;

    protected:
        uint64_t col_pin_mask_ = 0;
        uint64_t row_pin_mask_ = 0;
        queue_t &display_queue_;
        queue_t &error_request_queue_;   // carries uint32_t pattern slot indices
        Pattern pat_;
        bool have_pattern_ = false;     // true once a Pattern has been dequeued at least once
        bool oneshot_pending_ = false;  // S1.9: latch for Oneshot — true between dequeue and first scan

        // Error-display state (all on core 1).
        Pattern error_pattern_;
        Pattern saved_pattern_;
        bool    saved_have_pattern_      = false;
        bool    saved_oneshot_pending_   = false;
        bool    saved_triggered_active_  = false;
        uint8_t saved_triggered_next_row_= 0;
        uint64_t error_until_us_         = 0;

        // V1 Triggered (cmd 0x12 / 0x32) consumption state. Reset to row 0
        // each time a new Triggered pattern is dequeued. `triggered_active_`
        // is true between arming and consumption of all 20 rows (or sanity
        // timeout). Spec says no timeout is required; the 1 s bound here is
        // a defensive backstop so a Triggered cmd with no EINT source
        // doesn't hold core 1 forever.
        bool    triggered_active_     = false;
        uint8_t triggered_next_row_   = 0;

        // S2.2: drain-to-latest. Bumped by (pops - 1) when update() finds
        // more than one pattern queued at frame boundary. In practice this
        // should stay zero; non-zero indicates either controller is over-
        // pushing or Display::update() is starving (e.g., scan loop hung).
        uint32_t frames_skipped_ = 0;

        // Begin / end the error-display window (called only from core 1).
        void enter_error_display(uint32_t slot);
        void exit_error_display();
};

// ---- Scan timing instrumentation + target-period enforcement (S2.x) ----
//
// `display_target_period_us` is the wall-clock period that Display::show()
// pads to. At low duty_cycle the scan completes quickly and we busy-wait for
// the remainder, ensuring the LED-off portion of the duty cycle dominates.
// This is critical for duty_cycle semantics: brightness ∝ ON_time / Period,
// and the spec assumes Period is fixed (1 kHz refresh). Without this,
// the scan rate scales with duty_cycle and the perceived brightness ratio
// between duty_cycle=1 and duty_cycle=255 collapses to ~3-5x instead of ~260x.
//
// Default: 1000 µs = 1 kHz refresh.
extern volatile uint32_t display_target_period_us;

// Rolling per-scan timing stats. Display::show() updates these every scan.
// Reset via display_reset_scan_stats(); read via display_get_scan_stats().
struct ScanStats {
    uint32_t count;
    uint32_t period_us_total;   // sum of measured scan periods (post-padding)
    uint32_t scan_us_total;     // sum of "scan only" times (pre-padding)
    uint32_t period_us_min;
    uint32_t period_us_max;
    uint32_t scan_us_min;
    uint32_t scan_us_max;
};
void display_reset_scan_stats();
void display_get_scan_stats(ScanStats &out);

#if STAGE2_SELFTEST
// Bench only: simulated per-row "free work" (µs) for the 'k' reclaimable-
// headroom test. On v0.3.1 two-PIO this busy-wait overlaps the autonomous DMA
// burst (hidden from scan time until it exceeds the per-row burst); on the
// v0.2.1 CPU-row path it adds straight to scan time (core 1 must be present to
// feed each bit-plane). 0 = disabled.
extern volatile uint32_t g_bench_inject_us;

// Cycle-precise (DWT CYCCNT) per-frame scan-time stats for the 'j' jitter
// command: min/max/avg in core cycles over the frames since the last
// display_reset_scan_stats(). Jitter = max - min.
void display_get_scan_cycle_stats(uint32_t &cmin, uint32_t &cmax,
                                  uint32_t &cavg, uint32_t &ccount);
#endif

#endif

#ifndef DISPLAY_H
#define DISPLAY_H
#include <pico.h>
#include "pico/util/queue.h"
#include "pattern.h"

class Display {

    public:
        Display(queue_t &display_queue);
        void initialize();
        void update();
        void show();

        // S2.2: counters surfaced in Messenger serial heartbeat.
        uint32_t frames_skipped() const { return frames_skipped_; }

    protected:
        uint64_t col_pin_mask_ = 0;
        uint64_t row_pin_mask_ = 0;
        queue_t &display_queue_;
        Pattern pat_;
        bool have_pattern_ = false;     // true once a Pattern has been dequeued at least once
        bool oneshot_pending_ = false;  // S1.9: latch for Oneshot — true between dequeue and first scan

        // S2.2: drain-to-latest. Bumped by (pops - 1) when update() finds
        // more than one pattern queued at frame boundary. In practice this
        // should stay zero; non-zero indicates either controller is over-
        // pushing or Display::update() is starving (e.g., scan loop hung).
        uint32_t frames_skipped_ = 0;
};

// ---- Scan timing instrumentation + target-period enforcement (S2.x) ----
//
// `display_target_period_us` is the wall-clock period that Display::show()
// pads to. At low stretch the scan completes quickly and we busy-wait for
// the remainder, ensuring the LED-off portion of the duty cycle dominates.
// This is critical for stretch semantics: brightness ∝ ON_time / Period,
// and the spec assumes Period is fixed (1 kHz refresh). Without this,
// the scan rate scales with stretch and the perceived brightness ratio
// between stretch=1 and stretch=255 collapses to ~3-5x instead of ~200x.
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

#endif

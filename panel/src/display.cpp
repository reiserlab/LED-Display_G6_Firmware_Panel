#include "constants.h"
#include "display.h"
#include "bcm.h"
#include "display_pio.h"
#include "protocol.h"
#include <hardware/pio.h>
#include <pico/time.h>

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

void display_reset_scan_stats() {
    s_scan_count       = 0;
    s_period_us_total  = 0;
    s_scan_us_total    = 0;
    s_period_us_min    = UINT32_MAX;
    s_period_us_max    = 0;
    s_scan_us_min      = UINT32_MAX;
    s_scan_us_max      = 0;
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


Display::Display(queue_t &display_queue) : display_queue_(display_queue) {}

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
}


void Display::update() {
    // S2.2: drain-to-latest. In practice the queue should never have more than
    // one pending pattern; if it does (controller over-pushing or scan loop
    // stalling), keep only the most recent. Older frames are accounted for in
    // frames_skipped_ and surfaced in the serial heartbeat.
    //
    // Note on Oneshot semantics: with drain-to-latest, fast-burst Oneshot
    // frames that pile up are collapsed to the latest. The user-visible
    // result is "panel was dark, now it shows latest frame for 1 scan"
    // rather than "panel strobes through the burst, displaying each frame
    // for one scan." For the documented frame-streaming arena use case
    // these are visually equivalent.
    Pattern latest;
    int pops = 0;
    while (queue_try_remove(&display_queue_, &latest)) {
        pops++;
    }
    if (pops > 0) {
        pat_ = latest;
        have_pattern_ = true;
        oneshot_pending_ = (pat_.mode() == DisplayMode::Oneshot);
        precompute_bcm_data(pat_);
        if (pops > 1) frames_skipped_ += (pops - 1);
    }

    if (!have_pattern_) {
        return;  // boot state — rows already OFF + cols already OFF via initialize()
    }

    if (pat_.mode() == DisplayMode::Persistent) {
        // V1 Persistent: scan one frame; loop1() re-enters update() and we scan
        // again. Pattern stays loaded until next message replaces it.
        show();
    } else {
        // V1 Oneshot: scan exactly one full frame after the message arrives,
        // then go idle (display dark) until the next message.
        if (oneshot_pending_) {
            show();
            oneshot_pending_ = false;
        }
    }
}


void Display::show() {
    // S2.2: BCM scan via PIO column program. Walks all 20 schematic rows
    // pushing N bit-planes' worth of (col_word, pio_delay) pairs through the
    // PIO TX FIFO. The PIO sets cols ON, hardware-delays, clears cols OFF,
    // then raises IRQ flag 0; CPU clears the IRQ before pushing the next
    // plane.
    //   N = 1 for Gray_2 (single weight-15 plane)
    //   N = 4 for Gray_16 (weights {1, 2, 4, 8})
    //
    // Strict-off (duty_cycle == 0): bcm_plane_data column words are all zero
    // (precompute_bcm_data guard); the scan still runs but drives no LEDs.
    //
    // After the scan, busy-wait until `display_target_period_us` has elapsed
    // since the start of this scan. This enforces a fixed scan rate so that
    // brightness ∝ ON_time / Period. Without this, low-duty_cycle scans
    // complete in tens of µs and loop1() calls show() back-to-back — the
    // LED-off portion of the duty cycle shrinks, collapsing the
    // duty_cycle=1 vs duty_cycle=255 brightness ratio from the intended
    // ~260x to a useless ~4x.

    uint32_t t_start = time_us_32();

    uint8_t bcm_bits = (pat_.gray_level() == GrayLevel::Gray_2) ? 1 : 4;
    PIO  pio = pio_get_instance();
    uint sm  = pio_get_sm();

    for (int r = 0; r < PANEL_SIZE; r++) {
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
                        return;
                    }
                }
            #else
                while (!pio_interrupt_get(pio, 0)) { tight_loop_contents(); }
            #endif
            pio_interrupt_clear(pio, 0);
        }
        gpio_set_mask64(row_on_mask[r]);   // row HIGH = OFF
    }

    uint32_t t_scan_end = time_us_32();
    uint32_t scan_us    = t_scan_end - t_start;

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
}

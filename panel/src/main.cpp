#include <Streaming.h>
#include "pico/util/queue.h"
#include <hardware/clocks.h>
#include "constants.h"
#include "messenger.h"
#include "pattern.h"
#include "display.h"
#include "bcm.h"
#include "layout.h"
#include "display_pio.h"

queue_t display_queue;

// Core 0
// -----------------------------------------------------------------------
Messenger messenger(display_queue);

#if STAGE2_SELFTEST
// =======================================================================
// STAGE2_SELFTEST mode — single-panel BCM visual validation.
// Skips messenger.initialize() (no SPI), instead pushes a 30-second cycle
// of test patterns into display_queue from core 0. Banner prints to USB
// serial; runtime commands let you retune base_T and probe single pixels.
// =======================================================================

static const uint32_t SELFTEST_PATTERN_DURATION_MS = 10000;  // 10 s per pattern
static const int      SELFTEST_NUM_PATTERNS        = 6;
// Full cycle = 6 × 10 s = 60 s. Originally 5 s; bumped to 10 s because the
// sub-phase boundaries (esp. idx 4 duty_cycle ramp and idx 5 Oneshot demo) were
// too brief for a human observer to register.

// Per-pattern push policy state (file scope so loop() can track edges).
static int      st_last_idx     = -1;
static uint8_t  st_last_duty_cycle = 0xFF;
static uint32_t st_last_push_ms = 0;
static Pattern  st_singlepixel;            // updated by 'p' command; pushed if set
static bool     st_singlepixel_pending = false;

static void selftest_banner() {
    Serial.println("");
    Serial.println("======================================================");
    Serial.println("  STAGE2 SELFTEST BUILD - do not deploy to production");
    Serial.println("  Cycle: 60 s total, 6 patterns @ 10 s each");
    Serial.println("     0..10s:  all-off (Persistent, strict-off; no glow)");
    Serial.println("    10..20s:  all-on Gray_2 duty_cycle=255 (full bright; USB current test)");
    Serial.println("    20..30s:  checkerboard Gray_2 duty_cycle=192");
    Serial.println("    30..40s:  horizontal Gray_16 gradient duty_cycle=255");
    Serial.println("    40..50s:  Uniform brightness staircase (Gray_2 all-on):");
    Serial.println("              off / duty_cycle=1 / off / =16 / off / =64 / off / =255");
    Serial.println("              All-off pulses bracket each lit step so floor vs off is clear");
    Serial.println("    50..60s:  Oneshot demo:");
    Serial.println("               50..52s dark | 52..57s dim glow @500Hz | 57..60s dark");
    Serial.println("  Phase transitions logged on serial as [idx=N t=...ms] <desc>");
    Serial.print  ("  base_T = "); Serial.print(bcm_base_on_us, 2);
    Serial.println(" us (runtime-tunable: \"b<float>\\n\")");
    Serial.println("  Commands: b<float>=set base_T, r<int>=set scan period (us), p<lr>,<lc>=single pixel,");
    Serial.println("            t=timing benchmark, i=banner, ?=help");
    Serial.println("======================================================");
}

static void selftest_help() {
    Serial.println("Commands:");
    Serial.println("  b<float>     set bcm_base_on_us (e.g. b2.5)");
    Serial.println("  r<int>       set target scan period in us (100..10000; default 1000 = 1 kHz)");
    Serial.println("  p<lr>,<lc>   light single layout pixel at (lr,lc), Gray_2 duty_cycle=255");
    Serial.println("  t            scan-period timing benchmark across duty_cycle values");
    Serial.println("  i            re-print the boot banner");
    Serial.println("  ?            this help");
}

// Build all-off, all-on, checkerboard, gradient patterns. Helpers keep
// selftest_push_pattern() readable.
static void build_alloff(Pattern &pat, uint8_t duty_cycle, DisplayMode mode) {
    pat.set_gray_level(GrayLevel::Gray_2);
    pat.set_duty_cycle(duty_cycle);
    pat.set_mode(mode);
    pat.matrix().setZero();
}

static void build_allon_gray2(Pattern &pat, uint8_t duty_cycle, DisplayMode mode) {
    pat.set_gray_level(GrayLevel::Gray_2);
    pat.set_duty_cycle(duty_cycle);
    pat.set_mode(mode);
    for (size_t i = 0; i < PANEL_SIZE; i++)
        for (size_t j = 0; j < PANEL_SIZE; j++)
            pat.matrix()(i, j) = 1;
}

static void build_checkerboard(Pattern &pat, uint8_t duty_cycle, DisplayMode mode) {
    pat.set_gray_level(GrayLevel::Gray_2);
    pat.set_duty_cycle(duty_cycle);
    pat.set_mode(mode);
    for (size_t i = 0; i < PANEL_SIZE; i++)
        for (size_t j = 0; j < PANEL_SIZE; j++)
            pat.matrix()(i, j) = ((i + j) & 1) ? 1 : 0;
}

static void build_gradient_gray16(Pattern &pat, uint8_t duty_cycle, DisplayMode mode) {
    pat.set_gray_level(GrayLevel::Gray_16);
    pat.set_duty_cycle(duty_cycle);
    pat.set_mode(mode);
    for (size_t i = 0; i < PANEL_SIZE; i++)
        for (size_t j = 0; j < PANEL_SIZE; j++)
            // Horizontal gradient: col 0 -> intensity 0, col 19 -> intensity 15
            pat.matrix()(i, j) = (uint8_t)((j * 15) / (PANEL_SIZE - 1));
}

// idx=4 staircase: uniform Gray_2 all-on pattern modulated by 4 duty_cycle levels,
// bracketed by all-off pulses so the eye can distinguish "barely visible" from
// "actually off." Returns 0 for the all-off segments.
//
// Schedule across 10000 ms:
//   [0.0,  0.5)s  → 0     (lead-in dark; baseline)
//   [0.5,  2.5)s  → 1     (floor; ~0.5% of full — barely-visible glow)
//   [2.5,  3.0)s  → 0     (separator)
//   [3.0,  5.0)s  → 16    (~6% of full — clearly dim)
//   [5.0,  5.5)s  → 0     (separator)
//   [5.5,  7.5)s  → 64    (~25% of full — medium-bright)
//   [7.5,  8.0)s  → 0     (separator)
//   [8.0, 10.0)s  → 255   (full)
//
// Each transition is logged on serial via the sub-phase log in
// selftest_push_pattern.
static uint8_t selftest_duty_cycle_for(uint32_t t_in_window_ms) {
    if (t_in_window_ms <   500) return 0;
    if (t_in_window_ms <  2500) return 1;
    if (t_in_window_ms <  3000) return 0;
    if (t_in_window_ms <  5000) return 16;
    if (t_in_window_ms <  5500) return 0;
    if (t_in_window_ms <  7500) return 64;
    if (t_in_window_ms <  8000) return 0;
    return 255;
}

// Push the right pattern for `idx`, with per-idx push cadence policy.
// Called every ~20 ms from loop(); only enqueues a fresh Pattern when state
// changes.
static void selftest_push_pattern(int idx, uint32_t t_ms) {
    uint32_t t_in_window = t_ms % SELFTEST_PATTERN_DURATION_MS;
    Pattern  pat;

    // Single-pixel command takes priority over the cycle for one push.
    if (st_singlepixel_pending) {
        st_singlepixel_pending = false;
        if (!queue_try_add(&display_queue, &st_singlepixel)) {
            Serial.println("WARN: queue full on singlepixel push");
        }
        return;
    }

    bool need_push = (idx != st_last_idx);

    if (idx == 4) {
        // Brightness staircase on a uniform Gray_2 all-on pattern. All-off
        // pulses (duty_cycle=0) bracket each lit step so the eye can tell
        // "barely visible glow" from "actually off." Log every transition
        // with expected brightness percentage so the host operator has a
        // numeric reference against what they're seeing.
        uint8_t s = selftest_duty_cycle_for(t_in_window);
        if (s != st_last_duty_cycle) {
            need_push = true;
            Serial.print("  [idx=4 sub duty_cycle=");
            Serial.print(s);
            Serial.print(" t=");
            Serial.print(t_in_window);
            Serial.print("ms ~");
            // Expected per-pixel duty as % of full (duty_cycle=255, intensity=15)
            // Gray_2 at duty_cycle=s, intensity=1, single weight-15 plane:
            //   ON cycles = max(450*15*s/255 - 5, 0) + 5  ≈ 26.5*s
            // Duty at 1 kHz refresh ≈ ON cycles / 150_000 cycles
            // As % of full (duty_cycle=255 → ~4.5% duty):
            //   pct_of_full ≈ (s / 255) * 100
            // Numerically: duty_cycle=1 → 0.4%, 16 → 6%, 64 → 25%, 255 → 100%
            // (Slight nonlinearity at very low duty_cycle due to PIO 5-cycle floor.)
            int pct = (s == 0) ? 0 : (s == 1) ? 1 : (int)((uint32_t)s * 100 / 255);
            Serial.print(pct);
            Serial.print("% of full");
            if (s == 0) Serial.print(" (all-off baseline)");
            else if (s == 1) Serial.print(" (FLOOR — barely visible; PIO overhead dominates)");
            Serial.println("]");
        }
        if (need_push) {
            // Uniform Gray_2 all-on pattern. At duty_cycle=0 the strict-off guard
            // in precompute_bcm_data zeroes all column words so the panel goes
            // fully dark regardless of pattern content. At duty_cycle>0 every
            // pixel lights at the chosen modulation depth.
            if (s == 0) {
                build_alloff(pat, 0, DisplayMode::Persistent);
            } else {
                build_allon_gray2(pat, s, DisplayMode::Persistent);
            }
            st_last_duty_cycle = s;
        }
    } else if (idx == 5) {
        // Oneshot brightness-contrast demo. A single Oneshot scan is ~45 µs of
        // LED-on time per pixel — invisibly brief on its own. To make Oneshot
        // semantics visually clear, push at high rate during a middle window
        // and observe the panel go dark when pushes stop. Within the 10 s
        // window:
        //   0..2 s   no push   → panel dark (no Oneshot pending)
        //   2..7 s   push every loop iter at ~500 Hz (loop delay drops to 2 ms)
        //                      → panel shows ~50% brightness dim glow
        //   7..10 s  no push   → panel goes dark within 1 ms
        // If oneshot_pending_ is broken (never resets) the 2..7s window would
        // be as bright as Persistent (idx 1); if it never sets, the window
        // would stay dark.
        if (t_in_window >= 2000 && t_in_window < 7000) {
            build_allon_gray2(pat, 255, DisplayMode::Oneshot);
            need_push = true;
        }
        // sub-phases 0..2s and 7..10s: no push, panel goes/stays dark
    } else if (need_push) {
        switch (idx) {
            case 0: build_alloff       (pat, 0,   DisplayMode::Persistent); break;
            case 1: build_allon_gray2  (pat, 255, DisplayMode::Persistent); break;
            case 2: build_checkerboard (pat, 192, DisplayMode::Persistent); break;
            case 3: build_gradient_gray16(pat, 255, DisplayMode::Persistent); break;
        }
    }

    if (need_push) {
        if (!queue_try_add(&display_queue, &pat)) {
            // Queue full — Display::update() hasn't drained yet. Note:
            // drain-to-latest in update() means we usually beat this even
            // under burst push.
            Serial.print("WARN: queue full at idx="); Serial.println(idx);
        }
        st_last_idx     = idx;
        st_last_push_ms = t_ms;
    }
}

// Handle runtime serial commands: b<float>, p<lr>,<lc>, ?.
static void selftest_handle_serial() {
    if (!Serial.available()) return;
    String line = Serial.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) return;

    char c = line.charAt(0);
    if (c == '?' || c == 'h') {
        selftest_help();
        return;
    }
    if (c == 'i') {
        // Re-print boot banner on demand. Useful because the original boot-time
        // banner often gets dropped before the host has opened the read endpoint
        // (USB CDC race). Type 'i' at any time to recapture.
        selftest_banner();
        return;
    }
    if (c == 't') {
        // Timing benchmark. Drive each test duty_cycle value as a Persistent
        // pattern, let core 1 stabilize on it, then measure scan period stats
        // over a fixed sampling window.
        Serial.println("Scan-period benchmark (Gray_2 all-on @ different duty_cycle values):");
        Serial.print("  target_period_us = ");
        Serial.println(display_target_period_us);
        const uint8_t test_duty_cyclees[] = {0, 1, 16, 64, 192, 255};
        for (size_t k = 0; k < sizeof(test_duty_cyclees); k++) {
            uint8_t s = test_duty_cyclees[k];
            Pattern bp;
            if (s == 0) build_alloff(bp, 0, DisplayMode::Persistent);
            else        build_allon_gray2(bp, s, DisplayMode::Persistent);
            // Drain any pending pattern then push ours; wait for core 1 to swap.
            queue_try_add(&display_queue, &bp);
            delay(100);
            display_reset_scan_stats();
            delay(500);   // 500 ms sample window
            ScanStats st; display_get_scan_stats(st);
            uint32_t avg_period = st.count ? (st.period_us_total / st.count) : 0;
            uint32_t avg_scan   = st.count ? (st.scan_us_total   / st.count) : 0;
            Serial.print("  duty_cycle=");   Serial.print(s);
            Serial.print("  scans=");     Serial.print(st.count);
            Serial.print("  period(avg/min/max us)=");
            Serial.print(avg_period); Serial.print("/");
            Serial.print(st.period_us_min); Serial.print("/");
            Serial.print(st.period_us_max);
            Serial.print("  scan-only(avg/min/max us)=");
            Serial.print(avg_scan); Serial.print("/");
            Serial.print(st.scan_us_min); Serial.print("/");
            Serial.print(st.scan_us_max);
            // Approximate duty cycle = scan / period (the scan time is when
            // some LED is being driven; the remainder is busy-wait padding).
            // For an all-on pattern, scan_us is also the "any LED ON" time.
            uint32_t duty_pct_x100 = avg_period ? (uint32_t)((uint64_t)avg_scan * 10000 / avg_period) : 0;
            Serial.print("  duty=");
            Serial.print(duty_pct_x100 / 100);
            Serial.print(".");
            int frac = duty_pct_x100 % 100;
            if (frac < 10) Serial.print("0");
            Serial.print(frac);
            Serial.println("%");
        }
        // Restore the selftest cycle by invalidating last_idx so the next
        // loop iteration re-pushes whatever pattern idx requires.
        st_last_idx     = -1;
        st_last_duty_cycle = 0xFF;
        return;
    }
    if (c == 'r') {
        // Set target scan period in µs. Range 100..10000 (10 kHz to 100 Hz).
        // Default is 1000 µs = 1 kHz.
        long v = line.substring(1).toInt();
        if (v < 100 || v > 10000) {
            Serial.print("ERR: target_period_us out of range (got ");
            Serial.print(v); Serial.println(", expected 100..10000)");
            return;
        }
        display_target_period_us = (uint32_t)v;
        Serial.print("target_period_us = ");
        Serial.print(display_target_period_us);
        Serial.print("  (= ");
        Serial.print(1000000UL / display_target_period_us);
        Serial.println(" Hz scan rate)");
        return;
    }
    if (c == 'b') {
        float v = line.substring(1).toFloat();
        if (v <= 0.0f || v > 50.0f) {
            Serial.print("ERR: base_T out of range (got "); Serial.print(v); Serial.println(", expected 0..50)");
            return;
        }
        bcm_base_on_us = v;
        // Force a re-push of the current pattern so Display::update() runs
        // precompute_bcm_data() with the new base_T. Cheapest way: invalidate
        // last-idx so the next selftest_push_pattern() rebuilds and enqueues.
        st_last_idx     = -1;
        st_last_duty_cycle = 0xFF;
        Serial.print("base_T = "); Serial.print(bcm_base_on_us, 3); Serial.println(" us");
        return;
    }
    if (c == 'p') {
        // Parse "p<lr>,<lc>"
        int comma = line.indexOf(',');
        if (comma < 2) { Serial.println("ERR: usage p<lr>,<lc>"); return; }
        int lr = line.substring(1, comma).toInt();
        int lc = line.substring(comma + 1).toInt();
        if (lr < 0 || lr >= PANEL_SIZE || lc < 0 || lc >= PANEL_SIZE) {
            Serial.print("ERR: pixel out of range ("); Serial.print(lr);
            Serial.print(","); Serial.print(lc); Serial.println(")");
            return;
        }
        st_singlepixel.set_gray_level(GrayLevel::Gray_2);
        st_singlepixel.set_duty_cycle(255);
        st_singlepixel.set_mode(DisplayMode::Persistent);
        st_singlepixel.matrix().setZero();
        st_singlepixel.matrix()((size_t)lr, (size_t)lc) = 1;
        st_singlepixel_pending = true;
        Serial.print("singlepixel ("); Serial.print(lr); Serial.print(",");
        Serial.print(lc); Serial.println(") queued");
        return;
    }
    Serial.print("ERR: unknown cmd '"); Serial.print(c); Serial.println("' (try ?)");
}
#endif // STAGE2_SELFTEST


void setup() {
    Serial.begin(BAUDRATE);
    queue_init(&display_queue, sizeof(Pattern), DISPLAY_QUEUE_SIZE);
#if STAGE2_SELFTEST
    // Wait briefly for USB CDC to enumerate, then print banner. Selftest does
    // not initialize the SPI peripheral — messenger.initialize() is skipped.
    uint32_t t0 = millis();
    while (!Serial && (millis() - t0) < 2000) { /* spin briefly */ }
    selftest_banner();
#else
    messenger.initialize();
#endif
}

void loop() {
#if STAGE2_SELFTEST
    static uint32_t t_start = millis();
    static int      last_logged_idx = -1;
    uint32_t t_ms    = millis() - t_start;
    int      cur_idx = (t_ms / SELFTEST_PATTERN_DURATION_MS) % SELFTEST_NUM_PATTERNS;

    // Log every phase transition so the host can correlate visuals to wall-clock.
    if (cur_idx != last_logged_idx) {
        const char *desc = "?";
        switch (cur_idx) {
            case 0: desc = "all-off Persistent (strict-off test)"; break;
            case 1: desc = "all-on Gray_2 duty_cycle=255 (full bright + USB-current stress)"; break;
            case 2: desc = "checkerboard Gray_2 duty_cycle=192"; break;
            case 3: desc = "Gray_16 horizontal gradient duty_cycle=255"; break;
            case 4: desc = "Brightness staircase (Gray_2 all-on): off/1/off/16/off/64/off/255 (logged below)"; break;
            case 5: desc = "Oneshot demo: 2s dark | 5s glow | 3s dark"; break;
        }
        Serial.print("[idx=");
        Serial.print(cur_idx);
        Serial.print(" t=");
        Serial.print(t_ms);
        Serial.print("ms] ");
        Serial.println(desc);
        last_logged_idx = cur_idx;
    }

    selftest_push_pattern(cur_idx, t_ms);
    selftest_handle_serial();
    // idx 5 needs ~500 Hz outer rate to drive Oneshot bright enough to see;
    // other phases idle at 50 Hz to leave CPU room.
    delay(cur_idx == 5 ? 2 : 20);
#else
    messenger.update();
#endif
}


// Core 1
// -----------------------------------------------------------------------
bool core1_separate_stack = true;
Display display(display_queue);

void setup1() {
    // S2.3: Stage 2 BCM display engine init.
    //
    // ORDER IS CRITICAL (Codex-flagged col-pin sequencing bug):
    //   1. cycles_per_us from clock_get_hz(clk_sys) — needed by precompute.
    //   2. display.initialize() — sets cols + rows to SIO outputs in dark
    //      resting state (cols LOW, rows HIGH).
    //   3. init_index_maps() + precompute_scan_masks() — pure data.
    //   4. pio_init_program() — dynamically claims a PIO SM; returns false
    //      if none available. Caller fails dark.
    //   5. pio_start() — switches col pins from SIO to PIO and starts the SM.
    //      Once this runs, gpio_init() on the col pins (or anything that
    //      reverts pin function to SIO) will break the PIO output silently.
    //   6. Seed a Persistent all-off boot_pat so the BCM engine refreshes
    //      continuously even with no host pattern.

    cycles_per_us = clock_get_hz(clk_sys) / 1000000UL;
    Serial.print("clk_sys = ");
    Serial.print(clock_get_hz(clk_sys));
    Serial.print(" Hz, cycles_per_us = ");
    Serial.println(cycles_per_us);

    display.initialize();

    init_index_maps();
    precompute_scan_masks();

    if (!pio_init_program()) {
        Serial.println("FATAL: pio_init_program() failed - display dark");
        // Fail-dark: rows already HIGH (off), cols already LOW (off) from
        // display.initialize(). Stay here.
        while (true) { tight_loop_contents(); }
    }
    pio_start();

    // Seed with all-off Persistent boot pattern so first scan iteration is
    // valid and the BCM engine continuously refreshes (cleaner than
    // Pattern's default Oneshot, which would leave Display::update()
    // returning early before any host pattern arrives).
    Pattern boot_pat;
    boot_pat.set_mode(DisplayMode::Persistent);
    precompute_bcm_data(boot_pat);
}

void loop1() {
    display.update();
}

#include <Streaming.h>
#include "pico/util/queue.h"
#include <hardware/clocks.h>
#include "constants.h"
#include "isp.h"
#include "messenger.h"
#include "pattern.h"
#include "display.h"
#include "bcm.h"
#include "layout.h"
#include "display_pio.h"
#include "display_scan_twopio.h"
#include "predef_patterns.h"
#include "psram_store.h"

queue_t display_queue;
queue_t error_request_queue;   // uint32_t slot indices, core 0 -> core 1

// Core 0
// -----------------------------------------------------------------------
Messenger messenger(display_queue, error_request_queue);

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
static bool     autocycle_paused = false;   // 'x' toggles; halts phase pushes for bench eyeball tests

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
    Serial.println("  Commands: b<float>=set base_T, r<int>=set scan period (us, 100..50000),");
    Serial.println("            f<duty>/F<duty>=hold all-on Gray_2/Gray_16 field,");
    Serial.println("            s<duty>/v<duty>=hold horiz/vert bright stripe on dim field, p<lr>,<lc>=single pixel,");
    Serial.println("            e<slot>=raise error glyph (slot 0=ERR, 1-5=PE01-05, 100=CE00),");
    Serial.println("            T=push Triggered all-on Gray_2 (drive GP45 with edges),");
    Serial.println("            g=push Gated checkerboard (drive GP45 level),");
    Serial.println("            x=toggle autocycle pause, t=timing benchmark, i=banner, ?=help");
    Serial.println("======================================================");
}

static void selftest_help() {
    Serial.println("Commands:");
    Serial.println("  b<float>     set bcm_base_on_us (e.g. b2.5)");
    Serial.println("  r<int>       set target scan period in us (100..50000; default 1000 = 1 kHz)");
    Serial.println("  f<duty>      hold all-on Gray_2 field at duty_cycle (Persistent; pauses autocycle)");
    Serial.println("  F<duty>      hold all-on Gray_16 field (intensity 15) at duty_cycle (Persistent)");
    Serial.println("  s<duty>[,bg[,fg]]  horiz bright stripe on dim Gray_16 (bg/fg intensity 0..15; def 1,15)");
    Serial.println("  v<duty>[,bg[,fg]]  vertical bright stripe on dim Gray_16 (e.g. v2,3,15)");
    Serial.println("  p<lr>,<lc>   light single layout pixel at (lr,lc), Gray_2 duty_cycle=255");
    Serial.println("  e<slot>      raise an error glyph (e.g., e0=ERR, e1=PE01, e100=CE00)");
    Serial.println("  T            push Triggered all-on Gray_2 pattern (V1 0x12) — drives one row per EINT rising edge on GP45");
    Serial.println("  g            push Gated checkerboard pattern (V1 0x13) — refreshes only while GP45 is HIGH");
    Serial.println("  t            scan-period timing benchmark across duty_cycle values");
    Serial.println("  x            toggle the 60s autocycle on/off — pause for clean T/g/p observation");
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

// One bright stripe (2 wide, intensity 15) through the middle of an otherwise
// dim (intensity 1) Gray_16 field. LAB-43: the operator recalls this structured
// pattern as a reliable trigger for the drifting-bright artifact, unlike a
// uniform field. `vertical` selects a column stripe (cols 9,10) vs a row stripe
// (rows 9,10). Global duty_cycle scales the whole thing (keep it low).
static void build_stripe_gray16(Pattern &pat, uint8_t duty_cycle, bool vertical,
                                uint8_t bg, uint8_t fg) {
    pat.set_gray_level(GrayLevel::Gray_16);
    pat.set_duty_cycle(duty_cycle);
    pat.set_mode(DisplayMode::Persistent);
    for (size_t i = 0; i < PANEL_SIZE; i++)
        for (size_t j = 0; j < PANEL_SIZE; j++)
            pat.matrix()(i, j) = bg;   // dim background
    for (size_t k = 0; k < PANEL_SIZE; k++) {
        if (vertical) { pat.matrix()(k, 9) = fg; pat.matrix()(k, 10) = fg; }
        else          { pat.matrix()(9, k) = fg; pat.matrix()(10, k) = fg; }
    }
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
    if (c == 'k') {
        // Reclaimable-headroom benchmark. Inject simulated "free work" per row
        // and watch scan_us respond. On v0.3.1 two-PIO the work overlaps the
        // autonomous DMA burst (scan stays flat until inject > per-row burst);
        // on the v0.2.1 CPU-row path every injected µs adds straight to scan
        // time. Run on both panels and compare — this is the reclaimable
        // core-1 headroom the two-PIO scanner exposes.
        Serial.println("Reclaimable-headroom (Gray_2 all-on duty=255; inject = free work us/row):");
        const uint32_t injects[] = {0, 10, 20, 40};
        Pattern bp; build_allon_gray2(bp, 255, DisplayMode::Persistent);
        queue_try_add(&display_queue, &bp);
        delay(150);
        for (size_t k = 0; k < sizeof(injects) / sizeof(injects[0]); k++) {
            g_bench_inject_us = injects[k];
            delay(100);
            display_reset_scan_stats();
            delay(500);
            ScanStats st; display_get_scan_stats(st);
            uint32_t avg_scan = st.count ? (st.scan_us_total / st.count) : 0;
            Serial.print("  inject=");  Serial.print(injects[k]);
            Serial.print(" us/row  scans="); Serial.print(st.count);
            Serial.print("  scan-only(avg/min/max us)=");
            Serial.print(avg_scan); Serial.print("/");
            Serial.print(st.scan_us_min); Serial.print("/");
            Serial.print(st.scan_us_max);
            Serial.print("  per-row="); Serial.print(avg_scan / PANEL_SIZE);
            Serial.println(" us");
        }
        g_bench_inject_us = 0;
        st_last_idx     = -1;
        st_last_duty_cycle = 0xFF;
        return;
    }
    if (c == 'j') {
        // Cycle-precise per-frame scan jitter (DWT CYCCNT, ~6.67 ns @ 150 MHz),
        // finer than the 1 µs 't' stats. Jitter = (max - min) scan time over a
        // ~1 s window. Two-PIO (DMA/PIO self-timed) should be near-flat;
        // CPU-rows carries per-plane handshake variability.
        Serial.println("Scan jitter (DWT; Gray_2 all-on; cyc + us/ns):");
        const uint8_t duties[] = {64, 255};
        uint32_t cpu = cycles_per_us ? cycles_per_us : 150;
        for (size_t i = 0; i < sizeof(duties) / sizeof(duties[0]); i++) {
            Pattern bp; build_allon_gray2(bp, duties[i], DisplayMode::Persistent);
            queue_try_add(&display_queue, &bp);
            delay(150);
            display_reset_scan_stats();
            delay(1000);   // ~1000 frames at the 1 kHz target
            uint32_t cmin, cmax, cavg, ccount;
            display_get_scan_cycle_stats(cmin, cmax, cavg, ccount);
            uint32_t jit = (cmax >= cmin) ? (cmax - cmin) : 0;
            Serial.print("  duty=");    Serial.print(duties[i]);
            Serial.print("  frames=");  Serial.print(ccount);
            Serial.print("  scan avg="); Serial.print(cavg);
            Serial.print("cyc/");       Serial.print(cavg / cpu); Serial.print("us");
            Serial.print("  min=");     Serial.print(cmin);
            Serial.print("  max=");     Serial.print(cmax);
            Serial.print("  JITTER=");  Serial.print(jit);
            Serial.print("cyc/");       Serial.print(jit * 1000 / cpu); Serial.println("ns");
        }
        st_last_idx     = -1;
        st_last_duty_cycle = 0xFF;
        return;
    }
    if (c == 'r') {
        // Set target scan period in µs. Range 100..50000 (10 kHz to 20 Hz).
        // Default is 1000 µs = 1 kHz. The upper bound is deliberately very low
        // (20 Hz) for the LAB-43 low-duty-artifact eye test: a slow refresh
        // exaggerates the burst-then-dark flicker so a sweeping "phantom" event
        // is slow enough to see its structure and photograph it.
        long v = line.substring(1).toInt();
        if (v < 100 || v > 50000) {
            Serial.print("ERR: target_period_us out of range (got ");
            Serial.print(v); Serial.println(", expected 100..50000)");
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
    if (c == 'e') {
        // Enqueue an error-glyph raise. This bypasses Messenger (which
        // wouldn't be running in selftest) and pushes directly into the
        // error_request_queue. Useful for visual validation of the error
        // path independent of SPI traffic.
        long v = line.substring(1).toInt();
        if (v < 0 || v > 65535) {
            Serial.print("ERR: slot out of range (got "); Serial.print(v);
            Serial.println(", expected 0..65535)");
            return;
        }
        uint32_t slot = (uint32_t)v;
        if (!queue_try_add(&error_request_queue, &slot)) {
            Serial.println("ERR: error_request_queue full");
            return;
        }
        Serial.print("raise_error(slot=");
        Serial.print(slot);
        Serial.println(") queued");
        return;
    }
    if (c == 'T') {
        // V1 Triggered (cmd 0x12) selftest: push an all-on Gray_2 pattern
        // tagged DisplayMode::Triggered. Drive GP45 with a function gen
        // (1-1000 Hz square wave) to advance rows; 20 rising edges = one
        // visible frame; then dark until another T.
        Pattern p;
        build_allon_gray2(p, 255, DisplayMode::Triggered);
        if (!queue_try_add(&display_queue, &p)) {
            Serial.println("ERR: display_queue full");
            return;
        }
        // Invalidate the cycle's last-pushed idx so the cycle re-pushes
        // its own pattern on the next iteration once Triggered completes.
        st_last_idx     = -1;
        st_last_duty_cycle = 0xFF;
        Serial.println("Triggered all-on Gray_2 queued; drive GP45 rising edges to advance rows (20 = 1 frame)");
        return;
    }
    if (c == 'g') {
        // V1 Gated (cmd 0x13) selftest: push a checkerboard Gray_2 pattern
        // tagged DisplayMode::Gated. Drive GP45 HIGH to enable display,
        // LOW to mask off. Persists until the next pattern arrives.
        Pattern p;
        build_checkerboard(p, 192, DisplayMode::Gated);
        if (!queue_try_add(&display_queue, &p)) {
            Serial.println("ERR: display_queue full");
            return;
        }
        st_last_idx     = -1;
        st_last_duty_cycle = 0xFF;
        Serial.println("Gated checkerboard queued; LEDs visible only while GP45 is HIGH");
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
    if (c == 'x') {
        // Toggle the 6-pattern autocycle. While paused, no new patterns are
        // pushed from the cycle, so manually-pushed Triggered/Gated/error
        // patterns stay on the panel until the user resumes — makes the
        // bench eyeball test practical.
        autocycle_paused = !autocycle_paused;
        if (autocycle_paused) {
            Serial.println("autocycle: PAUSED (next 'x' resumes)");
        } else {
            // Force the next push by invalidating last-idx state.
            st_last_idx     = -1;
            st_last_duty_cycle = 0xFF;
            Serial.println("autocycle: RESUMED");
        }
        return;
    }
    if (c == 'f' || c == 'F') {
        // Hold a uniform all-on field at a chosen duty_cycle, Persistent, and
        // pause the autocycle so it stays up for the LAB-43 low-duty-artifact
        // eye test. 'f' = Gray_2 (intensity 1), 'F' = Gray_16 (intensity 15).
        // Reuses the st_singlepixel scratch buffer + pending flag (loop() pushes
        // it while paused). Sweep with r<us> (refresh) and b<float> (base_T).
        long v = line.substring(1).toInt();
        if (v < 0 || v > 255) {
            Serial.print("ERR: duty_cycle out of range (got ");
            Serial.print(v); Serial.println(", expected 0..255)");
            return;
        }
        uint8_t duty = (uint8_t)v;
        if (c == 'f') {
            build_allon_gray2(st_singlepixel, duty, DisplayMode::Persistent);
        } else {
            st_singlepixel.set_gray_level(GrayLevel::Gray_16);
            st_singlepixel.set_duty_cycle(duty);
            st_singlepixel.set_mode(DisplayMode::Persistent);
            for (size_t i = 0; i < PANEL_SIZE; i++)
                for (size_t j = 0; j < PANEL_SIZE; j++)
                    st_singlepixel.matrix()(i, j) = 15;  // full intensity
        }
        autocycle_paused       = true;   // hold: stop the cycle overwriting it
        st_singlepixel_pending = true;
        Serial.print("hold all-on ");
        Serial.print(c == 'F' ? "Gray_16" : "Gray_2");
        Serial.print(" duty_cycle="); Serial.print(duty);
        Serial.println(" (Persistent); autocycle PAUSED — 'x' to resume");
        return;
    }
    if (c == 's' || c == 'v') {
        // Hold a bright stripe on a dim Gray_16 field. 's' = horizontal row
        // stripe, 'v' = vertical column stripe. LAB-43 structured-pattern repro.
        // Format: <duty>[,<bg>[,<fg>]] — duty 0..255, bg/fg intensity 0..15
        // (defaults bg=1, fg=15). E.g. "v2,3,15".
        String rest = line.substring(1);
        int c1 = rest.indexOf(',');
        int c2 = (c1 >= 0) ? rest.indexOf(',', c1 + 1) : -1;
        long dutyv = rest.toInt();
        long bgv = (c1 >= 0) ? rest.substring(c1 + 1).toInt() : 1;
        long fgv = (c2 >= 0) ? rest.substring(c2 + 1).toInt() : 15;
        if (dutyv < 0 || dutyv > 255 || bgv < 0 || bgv > 15 || fgv < 0 || fgv > 15) {
            Serial.println("ERR: expected <duty 0..255>[,<bg 0..15>[,<fg 0..15>]]");
            return;
        }
        uint8_t duty = (uint8_t)dutyv;
        build_stripe_gray16(st_singlepixel, duty, /*vertical=*/(c == 'v'),
                            (uint8_t)bgv, (uint8_t)fgv);
        autocycle_paused       = true;
        st_singlepixel_pending = true;
        Serial.print("hold ");
        Serial.print(c == 'v' ? "vertical" : "horizontal");
        Serial.print(" bright stripe (Gray_16, bg="); Serial.print((int)bgv);
        Serial.print(" stripe="); Serial.print((int)fgv);
        Serial.print(") duty_cycle="); Serial.print(duty);
        Serial.println(" (Persistent); autocycle PAUSED — 'x' to resume");
        return;
    }
    Serial.print("ERR: unknown cmd '"); Serial.print(c); Serial.println("' (try ?)");
}
#endif // STAGE2_SELFTEST


#if PSRAM_SELFTEST
// =======================================================================
// PSRAM_SELFTEST mode — single-board LAB-41 validation (no SPI master).
// Read-back-verifies the demo store at boot and prints a PASS/FAIL banner,
// then runs a tiny serial console:
//   p        play the animation locally (feed 0..99 into display_queue)
//   s / x    stop playback
//   D<n>     dump PSRAM slot n as hex (e.g. "D42")
//   v        re-run the read-back verify
// =======================================================================
static bool     pst_playing = false;
static uint16_t pst_index   = 0;
static uint32_t pst_last_ms = 0;
static const uint32_t PST_FRAME_MS = 33;   // ~30 fps animation cadence

static void psram_selftest_verify(bool psram_ok) {
    Serial.println();
    Serial.println("=== LAB-41 PSRAM SELFTEST ===");
    if (!psram_ok) {
        Serial.println("PSRAM SELFTEST: FAIL (pmalloc/init failed)");
        return;
    }
    psram_store::VerifyResult r = psram_store::verify();
    Serial.print("PSRAM SELFTEST: ");
    Serial.print(r.checked - r.mismatched);
    Serial.print("/");
    Serial.print(r.checked);
    Serial.print(r.mismatched == 0 ? " OK" : " MISMATCH");
    Serial.print(" size=");
    Serial.print((uint32_t)rp2040.getPSRAMSize());
    Serial.print(" freeheap=");
    Serial.println((int)rp2040.getFreePSRAMHeap());
    Serial.println("cmds: p=play s=stop D<n>=dump v=verify");
}

static void psram_selftest_dump(uint16_t idx) {
    const uint8_t *f = psram_store::frame_ptr(idx);
    if (!f) {
        Serial.print("slot ");
        Serial.print(idx);
        Serial.println(" out of range");
        return;
    }
    Serial.print("slot ");
    Serial.print(idx);
    Serial.print(" [");
    Serial.print((uint32_t)psram_store::FRAME_BYTES);
    Serial.print("B]:");
    uint8_t xs = 0;
    for (size_t i = 0; i < psram_store::FRAME_BYTES; i++) {
        if (i % 16 == 0) Serial.println();
        if (f[i] < 0x10) Serial.print('0');
        Serial.print(f[i], HEX);
        Serial.print(' ');
        xs ^= f[i];
    }
    Serial.println();
    Serial.print("xor8=");
    Serial.println(xs, HEX);
}

static void psram_selftest_serial() {
    while (Serial.available()) {
        int c = Serial.read();
        if      (c == 'p')             { pst_playing = true;  Serial.println("play"); }
        else if (c == 's' || c == 'x') { pst_playing = false; Serial.println("stop"); }
        else if (c == 'v')             { psram_selftest_verify(psram_store::ready()); }
        else if (c == 'D')             { psram_selftest_dump((uint16_t)Serial.parseInt()); }
    }
}

static void psram_selftest_step() {
    psram_selftest_serial();
    if (!pst_playing) return;
    uint32_t now = millis();
    if (now - pst_last_ms < PST_FRAME_MS) return;
    pst_last_ms = now;
    Pattern pat;
    if (psram_store::load(pst_index, pat, DisplayMode::Persistent)) {
        queue_try_add(&display_queue, &pat);   // drop-on-full is fine
    }
    pst_index = (uint16_t)((pst_index + 1) % psram_store::FRAME_COUNT);
}
#endif // PSRAM_SELFTEST


void setup() {
    Serial.begin(BAUDRATE);
    queue_init(&display_queue, sizeof(Pattern), DISPLAY_QUEUE_SIZE);
    queue_init(&error_request_queue, sizeof(uint32_t), ERROR_REQUEST_QUEUE_SIZE);
    // Validate the INCBIN'd predefined-pattern blob. Logs to serial on
    // failure but never blocks boot — the error-display path falls back to
    // the compiled-in glyph in that case.
    predef::init();
#if STAGE2_SELFTEST
    // Wait briefly for USB CDC to enumerate, then print banner. Selftest does
    // not initialize the SPI peripheral — messenger.initialize() is skipped.
    uint32_t t0 = millis();
    while (!Serial && (millis() - t0) < 2000) { /* spin briefly */ }
    selftest_banner();
#else
    // LAB-41: load the demo animation into PSRAM before SPI ingest comes up, so
    // a V2 "display PSRAM index" command can be served from the first frame.
    // pmalloc failure (no PSRAM) logs FATAL to serial but does not block boot —
    // V1 live-display still works; V2 PSRAM commands raise PE05 at dispatch time.
    bool psram_ok = psram_store::init();
    if (psram_ok) {
        psram_store::generate_demo();
    } else {
        Serial.println("FATAL: PSRAM store init failed; V2 PSRAM display unavailable");
    }
#if PSRAM_SELFTEST
    // Single-board PSRAM validation: wait briefly for USB CDC, read-back verify,
    // print a PASS/FAIL banner. SPI ingest is intentionally NOT started so the
    // serial console stays responsive on a bare board.
    {
        uint32_t t0 = millis();
        while (!Serial && (millis() - t0) < 2000) { /* spin briefly */ }
        psram_selftest_verify(psram_ok);
    }
#else
    messenger.initialize();
    Serial.println("[boot] G6 panel core0 ready (ISP firmware)");
#endif
#endif
}

void loop() {
#if STAGE2_SELFTEST
    static uint32_t t_start = millis();
    static int      last_logged_idx = -1;
    uint32_t t_ms    = millis() - t_start;
    int      cur_idx = (t_ms / SELFTEST_PATTERN_DURATION_MS) % SELFTEST_NUM_PATTERNS;

    // Log every phase transition so the host can correlate visuals to wall-clock.
    // Suppressed while paused so the serial log stays clean for bench tests.
    if (!autocycle_paused && cur_idx != last_logged_idx) {
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

    if (!autocycle_paused) {
        selftest_push_pattern(cur_idx, t_ms);
    } else if (st_singlepixel_pending) {
        // While paused, still honor user 'p' commands.
        st_singlepixel_pending = false;
        if (!queue_try_add(&display_queue, &st_singlepixel)) {
            Serial.println("WARN: queue full on singlepixel push");
        }
    }
    selftest_handle_serial();
    // idx 5 needs ~500 Hz outer rate to drive Oneshot bright enough to see;
    // other phases idle at 50 Hz to leave CPU room.
    delay(cur_idx == 5 ? 2 : 20);
#elif PSRAM_SELFTEST
    // LAB-41 single-board console: serve serial cmds + local animation play.
    // No SPI ingest; core 1 refreshes the queued Persistent frame at 1 kHz.
    psram_selftest_step();
    delay(1);
#else
    // One-shot: if this is the first boot after an ISP flash, show the smiley
    // boot indicator (isp.cpp). Runs here — not in setup() — so both cores are
    // in steady state before any LittleFS access. The while(true) below never
    // returns, so this executes exactly once.
    Isp::boot_indicator_check();
    while(true){
    messenger.update();
    // Service USB CDC (TinyUSB) so Serial output actually flushes. Without this
    // the tight loop never returns to the core's USB task and all Serial.print
    // output is buffered forever (no boot banner, no diagnostics over USB).
    yield();
    }
#endif
}


// Core 1
// -----------------------------------------------------------------------
bool core1_separate_stack = true;
Display display(display_queue, error_request_queue);

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

    // Allow core 0 to park core 1 in a RAM-resident stub during an ISP flash
    // commit (LittleFS flash erase/program needs the other core out of XIP).
    // Harmless when ISP is never used. See isp.cpp stage_image_to_ota().
    multicore_lockout_victim_init();

    init_index_maps();
    precompute_scan_masks();

#if PANEL_REV == 31
    // v0.3.1: dual-PIO (rows on PIO1 + columns on PIO0) + dual-DMA scanner,
    // replacing the v0.2.1 single-PIO-columns + CPU-GPIO-rows path.
    if (!twopio_init()) {
        Serial.println("FATAL: twopio_init() failed - display dark");
        twopio_fail_dark();
        while (true) { tight_loop_contents(); }
    }
#else
    if (!pio_init_program()) {
        Serial.println("FATAL: pio_init_program() failed - display dark");
        // Fail-dark: rows already HIGH (off), cols already LOW (off) from
        // display.initialize(). Stay here.
        while (true) { tight_loop_contents(); }
    }
    pio_start();
#endif

    // Seed with all-off Persistent boot pattern so first scan iteration is
    // valid and the BCM engine continuously refreshes (cleaner than
    // Pattern's default Oneshot, which would leave Display::update()
    // returning early before any host pattern arrives).
    Pattern boot_pat;
    boot_pat.set_mode(DisplayMode::Persistent);
    precompute_bcm_data(boot_pat);
#if PANEL_REV == 31
    twopio_precompute((boot_pat.gray_level() == GrayLevel::Gray_2) ? 1 : 4);
#endif
}

void loop1() {
    display.update();
}

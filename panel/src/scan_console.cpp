// LAB-286 — USB console commands for the bank-window scan mode. See
// scan_console.h.

#include "scan_console.h"

#include <stdlib.h>
#include "scan_config.h"
#include "display.h"
#include "display_scan_twopio.h"
#include "bcm.h"

static const char *const BANK_NAMES[Bank::kNumBanks] = {"violet", "blue", "green", "yellow"};

static void print_scan_config() {
    const ScanConfig &cfg = scan_config_requested();
    Serial.print("scan mode: ");
    Serial.print(cfg.mode == ScanMode::BankWindows ? "bank windows" : "legacy");
    Serial.println(scan_config_applied() ? "" : " (pending: core 1 applies it before its next frame)");
    Serial.print("windows (ns at duty 255):");
    for (int k = 0; k < Bank::kNumBanks; k++) {
        Serial.print(" T"); Serial.print(k); Serial.print(' ');
        Serial.print(BANK_NAMES[k]); Serial.print('='); Serial.print(cfg.window_ns[k]);
    }
    Serial.print("  sum="); Serial.print(Bank::window_total_ns(cfg.window_ns));
    Serial.println(cfg.mode == ScanMode::BankWindows ? "" : "  (used when bank windows are on)");
}

static void print_request_error(ScanConfigResult r) {
    switch (r) {
        case ScanConfigResult::WindowTooLong:
            Serial.print("ERR: each window must be 0.."); Serial.print(Bank::kMaxWindowNs);
            Serial.println(" ns");
            break;
        case ScanConfigResult::TotalTooLong:
            Serial.print("ERR: the four windows may sum to at most "); Serial.print(Bank::kMaxTotalNs);
            Serial.println(" ns (20 rows must fit the 1 ms refresh)");
            break;
        case ScanConfigResult::QueueFull:
            Serial.println("ERR: previous request not yet applied; try again");
            break;
        case ScanConfigResult::Ok:
            break;
    }
}

// Parse exactly four comma-separated non-negative integers (ns) into w[].
static bool parse_windows(const char *s, uint16_t w[Bank::kNumBanks]) {
    for (int k = 0; k < Bank::kNumBanks; k++) {
        while (*s == ' ') s++;
        char *end = nullptr;
        long v = strtol(s, &end, 10);
        if (end == s || v < 0 || v > 65535) return false;
        w[k] = (uint16_t)v;
        s = end;
        while (*s == ' ') s++;
        if (k < Bank::kNumBanks - 1) {
            if (*s != ',') return false;
            s++;
        }
    }
    return *s == '\0';
}

bool scan_console_command(const String &line) {
    if (line.length() == 0) return false;
    char c = line.charAt(0);
    if (c != 'B' && c != 'W') return false;
    String arg = line.substring(1);
    arg.trim();

    if (arg.length() == 0) {           // 'B' or 'W' alone: status
        print_scan_config();
        return true;
    }

    ScanConfig cfg = scan_config_requested();
    if (c == 'B') {
        if (arg == "0")      cfg.mode = ScanMode::Legacy;
        else if (arg == "1") cfg.mode = ScanMode::BankWindows;
        else { Serial.println("ERR: usage B (status), B0 (legacy scan), B1 (bank windows)"); return true; }
    } else {
        if (!parse_windows(arg.c_str(), cfg.window_ns)) {
            Serial.println("ERR: usage W<t0>,<t1>,<t2>,<t3> in ns, e.g. W4200,5500,12700,22500");
            return true;
        }
    }
    ScanConfigResult r = scan_config_request(cfg);
    if (r != ScanConfigResult::Ok) { print_request_error(r); return true; }
    print_scan_config();
    return true;
}


#if PANEL_CONSOLE
#if SPI_DIAG || STAGE2_SELFTEST || PSRAM_SELFTEST
#error "PANEL_CONSOLE is its own build (pico_v031_bankwin); it shares USB serial input with no other console"
#endif

static void print_help() {
    Serial.println("G6 panel bench console (pico_v031_bankwin; production SPI ingest):");
    Serial.println("  B          scan mode + window table");
    Serial.println("  B0 / B1    legacy scan / bank windows (LAB-286)");
    Serial.println("  W          window table");
    Serial.println("  W<t0>,<t1>,<t2>,<t3>  windows in ns at duty 255 (T0 violet, T1 blue, T2 green,");
    Serial.println("             T3 yellow-orange); each 0..45000, sum <= 48000. Option B: W4200,5500,12700,22500");
    Serial.println("  S          frame timing since the last S (Persistent/Oneshot frames), then reset");
    Serial.println("  ?          this help");
    Serial.println("Set the mode between arena runs: a command typed mid-stream can cost one frame.");
}

static void print_scan_stats() {
    ScanStats st;
    display_get_scan_stats(st);
    uint32_t cmin, cmax, cavg, ccount;
    display_get_scan_cycle_stats(cmin, cmax, cavg, ccount);
    uint32_t cpu = cycles_per_us ? cycles_per_us : 150;
    uint32_t per_avg  = st.count ? st.period_us_total / st.count : 0;
    uint32_t scan_avg = st.count ? st.scan_us_total / st.count : 0;
    uint32_t jit = (cmax >= cmin) ? (cmax - cmin) : 0;

    Serial.print("frames="); Serial.print(st.count);
    Serial.print("  period avg/min/max us="); Serial.print(per_avg);
    Serial.print('/'); Serial.print(st.period_us_min);
    Serial.print('/'); Serial.print(st.period_us_max);
    Serial.print("  scan avg/min/max us="); Serial.print(scan_avg);
    Serial.print('/'); Serial.print(st.scan_us_min);
    Serial.print('/'); Serial.println(st.scan_us_max);
    Serial.print("scan cyc avg/min/max="); Serial.print(cavg);
    Serial.print('/'); Serial.print(cmin);
    Serial.print('/'); Serial.print(cmax);
    Serial.print("  JITTER="); Serial.print(jit);
    Serial.print("cyc/"); Serial.print(jit * 1000 / cpu);
    Serial.print("ns  row timeouts="); Serial.println(twopio_get_timeouts());
    display_reset_scan_stats();
}

static void handle_line(const char *buf) {
    String line(buf);
    line.trim();
    if (line.length() == 0) return;
    if (scan_console_command(line)) return;
    char c = line.charAt(0);
    if (c == 'S')                 { print_scan_stats(); return; }
    if (c == '?' || c == 'h')     { print_help(); return; }
    Serial.print("ERR: unknown cmd '"); Serial.print(c); Serial.println("' (try ?)");
}

void panel_console_poll() {
    static uint32_t last_ms = 0;
    static char     buf[64];
    static size_t   len = 0;
    static bool     overflow = false;

    uint32_t now = millis();
    if ((uint32_t)(now - last_ms) < 10) return;
    last_ms = now;

    while (Serial.available()) {
        int ch = Serial.read();
        if (ch < 0) break;
        if (ch == '\n' || ch == '\r') {
            if (overflow) Serial.println("ERR: line too long");
            else if (len > 0) { buf[len] = '\0'; handle_line(buf); }
            len = 0;
            overflow = false;
            return;   // at most one command per poll
        }
        if (len < sizeof(buf) - 1) buf[len++] = (char)ch;
        else overflow = true;
    }
}
#endif // PANEL_CONSOLE

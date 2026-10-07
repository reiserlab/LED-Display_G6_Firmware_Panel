#ifndef SCAN_CONFIG_H
#define SCAN_CONFIG_H
// LAB-286 — runtime scan configuration: legacy scan vs bank windows, plus the
// 4-entry bank-window table (bank_logic.h).
//
// Core 0 (the USB bench console today, an arena command later) requests a
// config; core 1 picks it up in Display::update() and rebuilds the scan data
// before its next frame, so a frame never mixes two configs. The config lives
// in RAM only: every boot starts in legacy scan (the bank-on trial build,
// SCAN_BANK_WINDOWS_DEFAULT=1, starts in bank windows instead). The firmware
// cannot tell a four-color panel from a green-only one (same board, same
// pins), so only that explicitly chosen build turns bank windows on.

#include <stdint.h>
#include "bank_logic.h"

enum class ScanMode : uint8_t { Legacy = 0, BankWindows = 1 };

// Boot scan mode. 0 (production, pico_v031_bankwin): legacy scan.
// 1 (pico_v031_bankon, four-color trial build): bank windows with the Option B
// table from the first frame, no console needed.
#ifndef SCAN_BANK_WINDOWS_DEFAULT
#define SCAN_BANK_WINDOWS_DEFAULT 0
#endif

struct ScanConfig {
    ScanMode mode = SCAN_BANK_WINDOWS_DEFAULT ? ScanMode::BankWindows : ScanMode::Legacy;
    uint16_t window_ns[Bank::kNumBanks] = {
        Bank::kDefaultWindowNs[0], Bank::kDefaultWindowNs[1],
        Bank::kDefaultWindowNs[2], Bank::kDefaultWindowNs[3]};
};

enum class ScanConfigResult : uint8_t { Ok, WindowTooLong, TotalTooLong, QueueFull };

// Core 0, from setup(): create the core 0 -> core 1 request queue.
void scan_config_init();

// Core 0: validate `cfg` and queue it for core 1. On anything but Ok nothing
// changes. The request is applied before core 1's next frame.
ScanConfigResult scan_config_request(const ScanConfig &cfg);

// Core 0: the last accepted request (what core 1 is running, or will be once
// scan_config_applied() turns true).
const ScanConfig &scan_config_requested();
bool scan_config_applied();

// Core 1: take the newest pending request, if any (drain-to-latest). Returns
// true if `out` was updated; that also marks the request applied, so call it
// only where the caller rebuilds the scan data right away.
bool scan_config_poll(ScanConfig &out);

#endif // SCAN_CONFIG_H

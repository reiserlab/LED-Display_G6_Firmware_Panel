// LAB-286 — runtime scan configuration. See scan_config.h.

#include "scan_config.h"

#include <hardware/sync.h>
#include "pico/util/queue.h"

// Depth 2 is plenty: requests come from a human at the console (or, later, one
// arena command per trial), and core 1 drains to the latest every frame.
static const uint SCAN_CONFIG_QUEUE_SIZE = 2;

static queue_t           s_queue;
static volatile bool     s_ready = false;     // queue initialized (core 1 may poll before setup() runs)
static ScanConfig        s_requested;         // core 0 only
static uint32_t          s_requested_gen = 0; // core 0 only
static volatile uint32_t s_applied_gen   = 0; // written by core 1

// What travels through the queue: the config plus its request number, so
// core 0 can tell when core 1 has caught up.
struct ScanConfigMsg {
    ScanConfig cfg;
    uint32_t   gen;
};

void scan_config_init() {
    queue_init(&s_queue, sizeof(ScanConfigMsg), SCAN_CONFIG_QUEUE_SIZE);
    __dmb();
    s_ready = true;
}

ScanConfigResult scan_config_request(const ScanConfig &cfg) {
    switch (Bank::check_windows(cfg.window_ns)) {
        case Bank::WindowCheck::WindowTooLong: return ScanConfigResult::WindowTooLong;
        case Bank::WindowCheck::TotalTooLong:  return ScanConfigResult::TotalTooLong;
        case Bank::WindowCheck::Ok:            break;
    }
    if (!s_ready) return ScanConfigResult::QueueFull;
    ScanConfigMsg m{cfg, s_requested_gen + 1};
    if (!queue_try_add(&s_queue, &m)) return ScanConfigResult::QueueFull;
    s_requested     = cfg;
    s_requested_gen = m.gen;
    return ScanConfigResult::Ok;
}

const ScanConfig &scan_config_requested() { return s_requested; }

bool scan_config_applied() { return s_applied_gen == s_requested_gen; }

bool scan_config_poll(ScanConfig &out) {
    if (!s_ready) return false;
    ScanConfigMsg m;
    bool got = false;
    while (queue_try_remove(&s_queue, &m)) got = true;
    if (!got) return false;
    out = m.cfg;
    s_applied_gen = m.gen;
    return true;
}

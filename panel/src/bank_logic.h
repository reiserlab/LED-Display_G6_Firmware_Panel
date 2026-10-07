#ifndef BANK_LOGIC_H
#define BANK_LOGIC_H
// LAB-286 — pure math for the bank-window scan mode (no Arduino, no hardware),
// so the host unit tests in test/test_bank_logic/ can check it.
//
// Four-color v0.4 panels wire one LED bank per electrical column:
// bank = sch_col % 4 (T0 violet, T1 blue, T2 green, T3 yellow-orange), 20 LEDs
// per column, 5 LEDs of each bank per row. (The host's 2x2 lattice,
// 2*(lr%2) + (lc%2), is the same bank seen through layout.cpp's map.)
//
// Bank-window scan lights one bank at a time inside each row slot: window T0,
// then T1, T2, T3. Each window is one or more {col_word, pio_delay} planes in
// the column DMA stream, so at most 5 LEDs share the row driver at once.
// Window lengths are set at duty_cycle 255 and the 3.0 µs BCM base; duty scales
// every window proportionally, and a different base (Triggered's 1.0 µs)
// scales them by base / 3.0 so the row keeps its length relative to legacy.

#include <stdint.h>

namespace Bank {

constexpr int kNumBanks = 4;

// Column-word masks: bank k = schematic columns c with c % 4 == k (bit c of
// the 20-bit column word drives COL_PIN[c] = GPc).
constexpr uint32_t kBankMask[kNumBanks] = {0x11111u, 0x22222u, 0x44444u, 0x88888u};

// Option B window lengths (ns at duty 255, 3.0 µs base): violet, blue, green,
// yellow-orange. Chosen from the 2026-10-06 arena bench so violet, blue and
// green emit equal photons and yellow about half. Sum 44.9 µs ~ the legacy
// 45 µs row (GS2 weight 15 x 3.0 µs).
constexpr uint16_t kDefaultWindowNs[kNumBanks] = {4200, 5500, 12700, 22500};

// Table limits. One window may take a whole legacy row (45 µs); the four
// together may not exceed 48 µs so a 20-row frame still fits the 1 ms
// (1 kHz) refresh period with the per-row arm/poll overhead.
constexpr uint32_t kMaxWindowNs = 45000;
constexpr uint32_t kMaxTotalNs  = 48000;

// Column-program ON overhead: a plane with PIO delay x lights its columns for
// x + 5 cycles (bcm.h PIO_ON_OVERHEAD_CYCLES), so 5 cycles is the shortest
// pulse a plane can make.
constexpr uint32_t kOnOverheadCycles = 5;

enum class WindowCheck : uint8_t { Ok, WindowTooLong, TotalTooLong };

inline uint32_t window_total_ns(const uint16_t w[kNumBanks]) {
    uint32_t sum = 0;
    for (int k = 0; k < kNumBanks; k++) sum += w[k];
    return sum;
}

inline WindowCheck check_windows(const uint16_t w[kNumBanks]) {
    for (int k = 0; k < kNumBanks; k++) {
        if (w[k] > kMaxWindowNs) return WindowCheck::WindowTooLong;
    }
    if (window_total_ns(w) > kMaxTotalNs) return WindowCheck::TotalTooLong;
    return WindowCheck::Ok;
}

// Window length in PIO cycles at duty 255. base_scale = BCM base in use / the
// 3.0 µs base the table is specified at (1.0 for Persistent/Oneshot/Gated).
inline uint32_t window_cycles(uint16_t window_ns, uint32_t cycles_per_us,
                              float base_scale) {
    float c = (float)window_ns * (float)cycles_per_us / 1000.0f * base_scale;
    return (c <= 0.0f) ? 0u : (uint32_t)(c + 0.5f);
}

// Requested ON cycles for one plane of a window: window * weight / weight_sum
// * duty / 255, rounded to the nearest cycle. weight_sum is 15 for both gray
// levels (Gray_2: one weight-15 plane; Gray_16: weights 1, 2, 4, 8).
inline uint32_t plane_on_cycles(uint32_t win_cycles, uint32_t weight,
                                uint32_t weight_sum, uint8_t duty) {
    uint64_t num = (uint64_t)win_cycles * weight * duty;
    uint64_t den = (uint64_t)weight_sum * 255u;
    return (uint32_t)((2 * num + den) / (2 * den));
}

// PIO delay word for a requested ON time. Below the 5-cycle overhead the
// plane still lights for 5 cycles (the brightness floor, same as legacy).
inline uint32_t pio_delay(uint32_t on_cycles) {
    return (on_cycles > kOnOverheadCycles) ? (on_cycles - kOnOverheadCycles) : 0u;
}

// Plane index of (bank, bit-plane) in a bank-window row: all of T0's planes,
// then T1's, T2's, T3's. bits = 1 (Gray_2) or 4 (Gray_16).
inline int plane_index(int bank, int bit, int bits) { return bank * bits + bit; }

} // namespace Bank

#endif // BANK_LOGIC_H

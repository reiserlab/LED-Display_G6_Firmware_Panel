#include "psram_store.h"
#include "message.h"
#include <Arduino.h>
#include <cstring>

namespace psram_store {

namespace {
    uint8_t  *g_store        = nullptr;   // pmalloc'd FRAME_COUNT * FRAME_BYTES
    uint16_t  g_count        = 0;         // frames programmed so far
    bool      g_ready        = false;

    // Serialize a Pattern into the canonical Gray_16 wire payload (200 packed
    // nibbles + 1 duty byte) and copy those FRAME_BYTES into store slot `idx`.
    // Reuses Message::from_pattern so the stored bytes are identical to what a
    // V1 0x30 frame would carry on the wire.
    void store_pattern(uint16_t idx, Pattern &pat) {
        Message msg;
        msg.from_pattern(pat, CMD_PROTOCOL_V1);   // packs Gray_16 (cmd 0x30)
        // Payload begins at HEADER_SIZE and is exactly FRAME_BYTES long
        // (200 nibble bytes + duty). data_ptr()[HEADER_SIZE .. +200].
        memcpy(g_store + (size_t)idx * FRAME_BYTES,
               msg.data_ptr() + HEADER_SIZE,
               FRAME_BYTES);
    }

    // Build a full-row ("horizontal bar") Gray_16 pattern at row `r`.
    void make_h_bar(Pattern &pat, uint8_t r, uint8_t duty) {
        pat.matrix() = PixelMatrix::Zero();
        pat.set_gray_level(GrayLevel::Gray_16);
        pat.set_duty_cycle(duty);
        for (uint8_t c = 0; c < PANEL_SIZE; c++) pat.at(r, c) = 15;
    }

    // Build a full-column ("vertical bar") Gray_16 pattern at column `c`.
    void make_v_bar(Pattern &pat, uint8_t c, uint8_t duty) {
        pat.matrix() = PixelMatrix::Zero();
        pat.set_gray_level(GrayLevel::Gray_16);
        pat.set_duty_cycle(duty);
        for (uint8_t r = 0; r < PANEL_SIZE; r++) pat.at(r, c) = 15;
    }

    // Build a 2x2-cell checkerboard drifting diagonally with phase `p`.
    void make_checker(Pattern &pat, uint8_t p, uint8_t duty) {
        pat.set_gray_level(GrayLevel::Gray_16);
        pat.set_duty_cycle(duty);
        for (uint8_t i = 0; i < PANEL_SIZE; i++) {
            for (uint8_t j = 0; j < PANEL_SIZE; j++) {
                uint8_t cell = (uint8_t)(((i + p) / 2 + (j + p) / 2) & 1u);
                pat.at(i, j) = cell ? 15 : 0;
            }
        }
    }

    // Map a global frame index 0..99 to its demo Pattern. Single source of
    // truth shared by generate_demo() (write) and verify() (read-back check)
    // and by the host reference model in test_psram_demo.py.
    void build_demo_frame(uint16_t index, Pattern &pat) {
        if (index < V_SWEEP_BEGIN) {
            // [0..39] horizontal bar, rows down (0->19) then up (19->0).
            uint16_t f = index - H_SWEEP_BEGIN;
            uint8_t r = (f < 20) ? (uint8_t)f : (uint8_t)(39 - f);
            make_h_bar(pat, r, 255);
        } else if (index < CHECKER_BEGIN) {
            // [40..79] vertical bar, cols right (0->19) then left (19->0).
            uint16_t f = index - V_SWEEP_BEGIN;
            uint8_t c = (f < 20) ? (uint8_t)f : (uint8_t)(39 - f);
            make_v_bar(pat, c, 255);
        } else {
            // [80..99] drifting checkerboard, phase 0..19.
            uint16_t f = index - CHECKER_BEGIN;
            make_checker(pat, (uint8_t)f, 192);
        }
    }
} // namespace

bool init() {
    if (g_ready) return true;
    g_store = (uint8_t *)pmalloc((size_t)FRAME_COUNT * FRAME_BYTES);
    if (!g_store) {
        Serial.print("PSRAM: pmalloc(");
        Serial.print((uint32_t)((size_t)FRAME_COUNT * FRAME_BYTES));
        Serial.println(") FAILED - PSRAM unavailable");
        g_ready = false;
        return false;
    }
    g_ready = true;
    g_count = 0;
    Serial.print("PSRAM: store @0x");
    Serial.print((uint32_t)(uintptr_t)g_store, HEX);
    Serial.print(" size=");
    Serial.print((uint32_t)rp2040.getPSRAMSize());
    Serial.print(" frames=");
    Serial.print(FRAME_COUNT);
    Serial.print(" x ");
    Serial.print((uint32_t)FRAME_BYTES);
    Serial.println("B");
    return true;
}

bool ready() { return g_ready; }

uint16_t count() { return g_count; }

void generate_demo() {
    if (!g_ready) return;
    Pattern pat;
    for (uint16_t i = 0; i < FRAME_COUNT; i++) {
        build_demo_frame(i, pat);
        store_pattern(i, pat);
    }
    g_count = FRAME_COUNT;
    Serial.print("PSRAM: generated ");
    Serial.print(g_count);
    Serial.println(" demo frames");
}

VerifyResult verify() {
    VerifyResult res{0, 0};
    if (!g_ready) return res;
    Pattern pat;
    uint8_t expected[FRAME_BYTES];
    for (uint16_t i = 0; i < g_count; i++) {
        build_demo_frame(i, pat);
        // Re-serialize the expected frame the same way store_pattern does.
        Message msg;
        msg.from_pattern(pat, CMD_PROTOCOL_V1);
        memcpy(expected, msg.data_ptr() + HEADER_SIZE, FRAME_BYTES);

        const uint8_t *stored = frame_ptr(i);
        res.checked++;
        if (!stored || memcmp(stored, expected, FRAME_BYTES) != 0) {
            res.mismatched++;
        }
    }
    return res;
}

const uint8_t *frame_ptr(uint16_t index) {
    if (!g_ready || index >= g_count) return nullptr;
    return g_store + (size_t)index * FRAME_BYTES;
}

bool load(uint16_t index, Pattern &out, DisplayMode mode, int duty_override) {
    const uint8_t *frame = frame_ptr(index);
    if (!frame) return false;

    // Decode via the Message Gray_16 codec: build a scratch 0x30 frame whose
    // payload is the stored record, then to_pattern() unpacks it (and reads
    // the stored duty from the trailing byte).
    Message msg;
    msg.set_num_bytes(HEADER_SIZE + FRAME_BYTES);
    msg.set_header_byte(CMD_PROTOCOL_V1);
    msg.set_command_byte(CMD_ID_DISPLAY_GRAY_16);
    for (size_t i = 0; i < FRAME_BYTES; i++) {
        msg.payload_at(i) = frame[i];
    }
    bool err = false;
    msg.to_pattern(out, err);
    if (err) return false;

    out.set_mode(mode);
    if (duty_override >= 0) {
        out.set_duty_cycle((uint8_t)(duty_override & 0xFF));
    }
    return true;
}

} // namespace psram_store

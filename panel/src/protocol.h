#ifndef PROTOCOL_H
#define PROTOCOL_H
#include <unordered_map>
#include <Arduino.h>

// Protocol versions (header byte bits 0..6; bit 7 is parity)
extern const uint8_t CMD_PROTOCOL_V1;
extern const uint8_t CMD_PROTOCOL;   // default for outgoing; firmware accepts V1 inbound only

// Command opcodes use low-nibble mode encoding:
//   high nibble = pattern type (1 = 2L, 3 = 16L, 5 = PSRAM-indexed, 6 = PSRAM-w-stretch,
//                               7 = Predefined-pattern)
//   low nibble  = mode        (0 = Oneshot, 1 = Persistent, 2 = Triggered, 3 = Gated)
//
// Protocol version split:
//   V1 (header 0x01/0x81) = live SPI display — patterns received over the wire
//                           (Oneshot, Persistent, Triggered, Gated × 2L, 16L) + COMM_CHECK
//   V2 (header 0x02/0x82) = PSRAM-backed display — patterns stored on-panel, indexed
//                           (PSRAM write, PSRAM reset, PSRAM display × 4 modes,
//                            PSRAM display with explicit stretch × 4 modes)
//   V3 (header 0x03/0x83) = everything else — diagnostics, predefined patterns,
//                           future feature classes
// V1 firmware (this build) implements COMM_CHECK + Oneshot + Persistent only.
// V1 Triggered/Gated specced and prototyped in G6_Panels_Test_Firmware but not
// in this firmware yet.
enum CommandId: uint8_t {
    // ---- V1 (header byte 0x01 / 0x81) — live SPI display ----
    CMD_ID_COMMS_CHECK              = 0x01,

    // 2-Level (1bpp) display, 50 B pixel + 1 B stretch payload
    CMD_ID_DISPLAY_GRAY_2           = 0x10,   // Oneshot     — single scan, then idle
    CMD_ID_DISPLAY_GRAY_2_PERSIST   = 0x11,   // Persistent  — continuous refresh until next cmd
    // 0x12 — V1 Triggered  (2L)  (specced; not implementing now)
    // 0x13 — V1 Gated      (2L)  (specced; not implementing now)

    // 16-Level (4bpp) display, 200 B pixel + 1 B stretch payload
    CMD_ID_DISPLAY_GRAY_16          = 0x30,   // Oneshot
    CMD_ID_DISPLAY_GRAY_16_PERSIST  = 0x31,   // Persistent
    // 0x32 — V1 Triggered  (16L) (specced; not implementing now)
    // 0x33 — V1 Gated      (16L) (specced; not implementing now)

    // ---- V2 reservations (specced; header 0x02/0x82) — PSRAM-backed display ----
    //   0x0F                 — Reset PSRAM
    //   0x3F                 — Write 16L pattern to PSRAM (16L only; no 2L PSRAM write)
    //   0x50/0x51/0x52/0x53  — Display PSRAM index, mode in low nibble (stretch implicit
    //                          from the value stored at PSRAM-write time)
    //   0x60/0x61/0x62/0x63  — Display PSRAM index with explicit stretch byte
    // Symbols below are placeholders; not dispatched by Messenger today.
    CMD_ID_RESET_PSRAM       = 0x0F,
    CMD_ID_SET_PSRAM_GRAY_16 = 0x3F,
    CMD_ID_DISPLAY_PSRAM     = 0x50,    // Oneshot from PSRAM index (mode in low nibble)

    // ---- V3 reservations (specced; header 0x03/0x83) — diagnostics + predefined patterns ----
    //   0x02                 — Query diagnostics (data shape TBD)
    //   0x03                 — Reset diagnostic stats
    //   0x70/0x71/0x72/0x73  — Display Predefined Pattern (panel flash; modes in low nibble)
    //   v5 grayscale/color modes — future
    CMD_ID_QUERY_DIAGNOSTIC  = 0x02,
    CMD_ID_RESET_DIAGNOSTICS = 0x03,
};

constexpr size_t  MESSAGE_MAXIMUM_SIZE = 300;
constexpr uint8_t PANEL_SIZE = 20;

// Header and payload sizes
extern const size_t HEADER_SIZE;
extern const size_t PAYLOAD_MINIMUM_SIZE;
extern const size_t PAYLOAD_COMMS_CHECK;
extern const size_t PAYLOAD_DISPLAY_GRAY_2;
extern const size_t PAYLOAD_DISPLAY_GRAY_16;
extern const size_t MESSAGE_MINIMUM_SIZE;

using PayloadSizeUMap = std::unordered_map<uint8_t, size_t>;
extern const PayloadSizeUMap PAYLOAD_SIZE_UMAP;

using DisplayCommandsUMap = std::unordered_map<uint8_t, uint8_t>;
extern const DisplayCommandsUMap DISPLAY_COMMANDS_UMAP;

// Pattern graylevels
constexpr size_t MAX_GRAY_LEVEL = 16;
enum class GrayLevel {
    Gray_2,
    Gray_16,
};
using GrayLevelUMap = std::unordered_map<GrayLevel, uint8_t>;
extern const GrayLevelUMap GRAY_LEVEL_UMAP;

// Display mode — V1 implements Oneshot + Persistent.
//   0x10 / 0x30 → Oneshot   (single scan, then idle)
//   0x11 / 0x31 → Persistent (scan continuously until next command)
// Triggered (0x12 / 0x32) + Gated (0x13 / 0x33) are V2 reservations.
enum class DisplayMode : uint8_t {
    Oneshot    = 0,
    Persistent = 1,
    // Triggered = 2,  // V2 reservation; not implemented
    // Gated     = 3,  // V2 reservation; not implemented
};

#endif

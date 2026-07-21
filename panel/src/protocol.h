#ifndef PROTOCOL_H
#define PROTOCOL_H
// Arduino-free on purpose: this header is compiled host-side by the native
// unit tests (via isp_logic.h). Keep it to stdint/stddef/STL includes.
#include <stddef.h>
#include <stdint.h>
#include <unordered_map>

// Protocol versions (header byte bits 0..6; bit 7 is parity)
extern const uint8_t CMD_PROTOCOL_V1;
extern const uint8_t CMD_PROTOCOL_V2;   // PSRAM-backed display (LAB-41/42)
extern const uint8_t CMD_PROTOCOL;   // default for outgoing V1; inbound version is
                                     // gated per-command via command_protocol_version()

// Command opcodes use low-nibble mode encoding:
//   high nibble = pattern type (1 = 2L, 3 = 16L, 5 = PSRAM-indexed, 6 = PSRAM-w-duty-cycle,
//                               7 = Predefined-pattern)
//   low nibble  = mode        (0 = Oneshot, 1 = Persistent, 2 = Triggered, 3 = Gated)
//
// Protocol version split:
//   V1 (header 0x01/0x81) = live SPI display — patterns received over the wire
//                           (Oneshot, Persistent, Triggered, Gated × 2L, 16L) + COMM_CHECK
//   V2 (header 0x02/0x82) = PSRAM-backed display — patterns stored on-panel, indexed
//                           (PSRAM write, PSRAM reset, PSRAM display × 4 modes,
//                            PSRAM display with explicit duty_cycle × 4 modes)
//   V3 (header 0x03/0x83) = everything else — diagnostics, predefined patterns,
//                           future feature classes
// V1 firmware (this build) implements COMM_CHECK + Oneshot + Persistent only.
// V1 Triggered/Gated specced and prototyped in G6_Panels_Test_Firmware but not
// in this firmware yet.
enum CommandId: uint8_t {
    // ---- V1 (header byte 0x01 / 0x81) — live SPI display ----
    CMD_ID_COMMS_CHECK              = 0x01,

    // 2-Level (1bpp) display, 50 B pixel + 1 B duty_cycle payload
    CMD_ID_DISPLAY_GRAY_2             = 0x10,   // Oneshot     — single scan, then idle
    CMD_ID_DISPLAY_GRAY_2_PERSIST     = 0x11,   // Persistent  — continuous refresh until next cmd
    CMD_ID_DISPLAY_GRAY_2_TRIGGERED   = 0x12,   // Triggered   — one row per EINT edge, 20 edges = 1 frame
    CMD_ID_DISPLAY_GRAY_2_GATED       = 0x13,   // Gated       — EINT level masks LED output

    // 16-Level (4bpp) display, 200 B pixel + 1 B duty_cycle payload
    CMD_ID_DISPLAY_GRAY_16            = 0x30,   // Oneshot
    CMD_ID_DISPLAY_GRAY_16_PERSIST    = 0x31,   // Persistent
    CMD_ID_DISPLAY_GRAY_16_TRIGGERED  = 0x32,   // Triggered
    CMD_ID_DISPLAY_GRAY_16_GATED      = 0x33,   // Gated

    // V1 panel-error display: panel renders a predefined error-glyph pattern
    // from the firmware-baked flash blob. Payload is a 24-bit LE slot index
    // (shape compatible with V3 0x70 to preserve the option). V1 firmware
    // reads the low byte and raises PE05 if upper two bytes are nonzero.
    CMD_ID_ERROR_DISPLAY            = 0xC2,

    // ---- V2 (header 0x02/0x82) — PSRAM-backed display (LAB-41/42) ----
    // Display commands take a 16-bit LE PSRAM frame index as payload; the panel
    // renders the locally-stored frame at that index. Low nibble = display mode
    // (0 Oneshot / 1 Persistent / 2 Triggered / 3 Gated), same as V1.
    //   0x50/0x51/0x52/0x53  — Display PSRAM index (2 B LE index). duty_cycle is
    //                          implicit (the value stored with the frame).
    //   0x60/0x61/0x62/0x63  — Display PSRAM index with explicit duty_cycle
    //                          (3 B: 2 B LE index + 1 B duty_cycle).
    // The over-the-wire PSRAM write/reset path is the full V2 (out of scope for
    // the LAB-41 demo, which loads frames locally at boot); symbols kept for the
    // shared opcode map but not dispatched by Messenger today.
    CMD_ID_RESET_PSRAM                  = 0x0F,
    CMD_ID_SET_PSRAM_GRAY_16            = 0x3F,

    CMD_ID_DISPLAY_PSRAM                = 0x50,   // Oneshot     (duty implicit)
    CMD_ID_DISPLAY_PSRAM_PERSIST        = 0x51,   // Persistent  (duty implicit)
    CMD_ID_DISPLAY_PSRAM_TRIGGERED      = 0x52,   // Triggered   (duty implicit)
    CMD_ID_DISPLAY_PSRAM_GATED          = 0x53,   // Gated       (duty implicit)

    CMD_ID_DISPLAY_PSRAM_DUTY           = 0x60,   // Oneshot     (explicit duty)
    CMD_ID_DISPLAY_PSRAM_DUTY_PERSIST   = 0x61,   // Persistent  (explicit duty)
    CMD_ID_DISPLAY_PSRAM_DUTY_TRIGGERED = 0x62,   // Triggered   (explicit duty)
    CMD_ID_DISPLAY_PSRAM_DUTY_GATED     = 0x63,   // Gated       (explicit duty)

    // ---- V3 reservations (specced; header 0x03/0x83) — diagnostics + predefined patterns ----
    //   0x02                 — Query diagnostics (data shape TBD)
    //   0x03                 — Reset diagnostic stats
    //   0x70/0x71/0x72/0x73  — Display Predefined Pattern (panel flash; modes in low nibble)
    //   v5 grayscale/color modes — future
    CMD_ID_QUERY_DIAGNOSTIC  = 0x02,
    CMD_ID_RESET_DIAGNOSTICS = 0x03,

    // ---- ISP (header 0x01/0x81) — in-system programming of panel firmware ----
    // See isp.{h,cpp}. Replies are extended (multi-byte CIPO), driven on a
    // follow-up read transaction via panel_spi_drive_response().
    CMD_ID_ISP_ENTER         = 0xE4,
    CMD_ID_ISP_WRITE_PAGE    = 0xE5,
    CMD_ID_ISP_VERIFY_STAGED = 0xE6,
    CMD_ID_ISP_COMMIT        = 0xE7,
    CMD_ID_ISP_VERIFY_CRC    = 0xE8,
    CMD_ID_ISP_EXIT_REBOOT   = 0xE9,
};

constexpr size_t  MESSAGE_MAXIMUM_SIZE = 300;
constexpr uint8_t PANEL_SIZE = 20;

// Header and payload sizes
extern const size_t HEADER_SIZE;
extern const size_t PAYLOAD_MINIMUM_SIZE;
extern const size_t PAYLOAD_COMMS_CHECK;
extern const size_t PAYLOAD_DISPLAY_GRAY_2;
extern const size_t PAYLOAD_DISPLAY_GRAY_16;
extern const size_t PAYLOAD_ERROR_DISPLAY;       // 3 B: 24-bit LE slot index (V3 0x70-compatible)
extern const size_t PAYLOAD_DISPLAY_PSRAM;       // 2 B: 16-bit LE PSRAM frame index
extern const size_t PAYLOAD_DISPLAY_PSRAM_DUTY;  // 3 B: 16-bit LE index + 1 B duty_cycle
extern const size_t MESSAGE_MINIMUM_SIZE;

// Per-command protocol version (header bits 0..6) that an opcode belongs to.
// V1 display/comms/error opcodes -> 1; V2 PSRAM opcodes -> 2. Unknown opcodes
// default to V1 so a bogus opcode under a V2 header fails the version gate and
// a bogus opcode under a V1 header still surfaces as unknown-cmd. Messenger
// gates each inbound message on header_version == command_protocol_version(cmd).
uint8_t command_protocol_version(uint8_t cmd);

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

// V1 display modes. Per g6_01-panel-protocol.md § Display Mode Summary:
//   0x10 / 0x30 → Oneshot    — single scan, then idle (dark)
//   0x11 / 0x31 → Persistent — continuous refresh until next command
//   0x12 / 0x32 → Triggered  — one row × all bit-planes per EINT rising edge;
//                              20 edges = 1 frame; mid-consumption overwrite
//                              resets the row counter
//   0x13 / 0x33 → Gated      — Oneshot-style pattern processing; EINT level
//                              acts as a global LED output-enable mask
//                              (HIGH = visible, LOW = dark)
enum class DisplayMode : uint8_t {
    Oneshot    = 0,
    Persistent = 1,
    Triggered  = 2,
    Gated      = 3,
};

#endif

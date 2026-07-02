#include "protocol.h"

// Protocol versions. V1 firmware accepts header 0x01 / 0x81 only.
// V2 (0x02 / 0x82) is reserved for Triggered/Gated/PSRAM; not yet
// implemented in this firmware.
const uint8_t CMD_PROTOCOL_V1 = 0x01;
const uint8_t CMD_PROTOCOL = CMD_PROTOCOL_V1;  // default outgoing version

// Header and payload sizes
const size_t HEADER_SIZE = 2;
const size_t PAYLOAD_MINIMUM_SIZE = 1;
const size_t PAYLOAD_COMMS_CHECK = 200;
const size_t PAYLOAD_DISPLAY_GRAY_2 = 51;    // 50 pattern + 1 duty_cycle
const size_t PAYLOAD_DISPLAY_GRAY_16 = 201;  // 200 pattern + 1 duty_cycle
const size_t PAYLOAD_ERROR_DISPLAY = 3;      // 24-bit LE slot index (V3 0x70-compatible)
const size_t MESSAGE_MINIMUM_SIZE = HEADER_SIZE + PAYLOAD_MINIMUM_SIZE;

const PayloadSizeUMap PAYLOAD_SIZE_UMAP {
    {CMD_ID_COMMS_CHECK,                 PAYLOAD_COMMS_CHECK},
    // V1 display commands all share payload shape per gray level (Oneshot,
    // Persistent, Triggered, Gated only differ in low nibble = mode).
    {CMD_ID_DISPLAY_GRAY_2,              PAYLOAD_DISPLAY_GRAY_2},
    {CMD_ID_DISPLAY_GRAY_2_PERSIST,      PAYLOAD_DISPLAY_GRAY_2},
    {CMD_ID_DISPLAY_GRAY_2_TRIGGERED,    PAYLOAD_DISPLAY_GRAY_2},
    {CMD_ID_DISPLAY_GRAY_2_GATED,        PAYLOAD_DISPLAY_GRAY_2},
    {CMD_ID_DISPLAY_GRAY_16,             PAYLOAD_DISPLAY_GRAY_16},
    {CMD_ID_DISPLAY_GRAY_16_PERSIST,     PAYLOAD_DISPLAY_GRAY_16},
    {CMD_ID_DISPLAY_GRAY_16_TRIGGERED,   PAYLOAD_DISPLAY_GRAY_16},
    {CMD_ID_DISPLAY_GRAY_16_GATED,       PAYLOAD_DISPLAY_GRAY_16},
    {CMD_ID_ERROR_DISPLAY,               PAYLOAD_ERROR_DISPLAY},
    // ISP payloads (bytes after the 2-byte header). Must match IspController.
    {CMD_ID_ISP_ENTER,                   20},   // sentinel(16) + token(4)
    {CMD_ID_ISP_WRITE_PAGE,              267},  // page_idx(3)+nonce(4)+data(256)+crc32(4)
    {CMD_ID_ISP_VERIFY_STAGED,           11},   // nonce(4)+len(3)+crc32(4)
    {CMD_ID_ISP_COMMIT,                  7},    // nonce(4)+len(3)
    {CMD_ID_ISP_VERIFY_CRC,              14},   // start(3)+len(3)+nonce(4)+crc32(4)
    {CMD_ID_ISP_EXIT_REBOOT,             5},    // nonce(4)+mode(1)
};

const DisplayCommandsUMap DISPLAY_COMMANDS_UMAP {
    {CMD_ID_DISPLAY_GRAY_2,              0},
    {CMD_ID_DISPLAY_GRAY_2_PERSIST,      0},
    {CMD_ID_DISPLAY_GRAY_2_TRIGGERED,    0},
    {CMD_ID_DISPLAY_GRAY_2_GATED,        0},
    {CMD_ID_DISPLAY_GRAY_16,             0},
    {CMD_ID_DISPLAY_GRAY_16_PERSIST,     0},
    {CMD_ID_DISPLAY_GRAY_16_TRIGGERED,   0},
    {CMD_ID_DISPLAY_GRAY_16_GATED,       0},
};
const GrayLevelUMap GRAY_LEVEL_UMAP  {
    {GrayLevel::Gray_2,   2},
    {GrayLevel::Gray_16, 16},
};

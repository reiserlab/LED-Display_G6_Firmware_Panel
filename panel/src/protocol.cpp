#include "protocol.h"

// Protocol versions. V1 = live SPI display (header 0x01 / 0x81). V2 = PSRAM-
// backed display (header 0x02 / 0x82, LAB-41/42). Inbound version is gated
// per-command (see command_protocol_version); outgoing CIPO echoes the inbound
// version, so CMD_PROTOCOL stays V1 as the default for V1 helpers.
const uint8_t CMD_PROTOCOL_V1 = 0x01;
const uint8_t CMD_PROTOCOL_V2 = 0x02;
const uint8_t CMD_PROTOCOL = CMD_PROTOCOL_V1;  // default outgoing version

// Header and payload sizes
const size_t HEADER_SIZE = 2;
const size_t PAYLOAD_MINIMUM_SIZE = 1;
const size_t PAYLOAD_COMMS_CHECK = 200;
const size_t PAYLOAD_DISPLAY_GRAY_2 = 51;    // 50 pattern + 1 duty_cycle
const size_t PAYLOAD_DISPLAY_GRAY_16 = 201;  // 200 pattern + 1 duty_cycle
const size_t PAYLOAD_ERROR_DISPLAY = 3;      // 24-bit LE slot index (V3 0x70-compatible)
const size_t PAYLOAD_DISPLAY_PSRAM = 2;      // 16-bit LE PSRAM frame index
const size_t PAYLOAD_DISPLAY_PSRAM_DUTY = 3; // 16-bit LE index + 1 B duty_cycle
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
    // V2 PSRAM display commands. 0x5x: 2 B LE index (duty implicit). 0x6x: 2 B
    // LE index + 1 B explicit duty_cycle. All four modes share a payload shape.
    {CMD_ID_DISPLAY_PSRAM,               PAYLOAD_DISPLAY_PSRAM},
    {CMD_ID_DISPLAY_PSRAM_PERSIST,       PAYLOAD_DISPLAY_PSRAM},
    {CMD_ID_DISPLAY_PSRAM_TRIGGERED,     PAYLOAD_DISPLAY_PSRAM},
    {CMD_ID_DISPLAY_PSRAM_GATED,         PAYLOAD_DISPLAY_PSRAM},
    {CMD_ID_DISPLAY_PSRAM_DUTY,           PAYLOAD_DISPLAY_PSRAM_DUTY},
    {CMD_ID_DISPLAY_PSRAM_DUTY_PERSIST,   PAYLOAD_DISPLAY_PSRAM_DUTY},
    {CMD_ID_DISPLAY_PSRAM_DUTY_TRIGGERED, PAYLOAD_DISPLAY_PSRAM_DUTY},
    {CMD_ID_DISPLAY_PSRAM_DUTY_GATED,     PAYLOAD_DISPLAY_PSRAM_DUTY},
};

// Per-command protocol version. Disjoint opcode ranges mean a simple switch
// is enough; unknown opcodes default to V1 (see header note).
uint8_t command_protocol_version(uint8_t cmd) {
    switch (cmd) {
        case CMD_ID_RESET_PSRAM:
        case CMD_ID_SET_PSRAM_GRAY_16:
        case CMD_ID_DISPLAY_PSRAM:
        case CMD_ID_DISPLAY_PSRAM_PERSIST:
        case CMD_ID_DISPLAY_PSRAM_TRIGGERED:
        case CMD_ID_DISPLAY_PSRAM_GATED:
        case CMD_ID_DISPLAY_PSRAM_DUTY:
        case CMD_ID_DISPLAY_PSRAM_DUTY_PERSIST:
        case CMD_ID_DISPLAY_PSRAM_DUTY_TRIGGERED:
        case CMD_ID_DISPLAY_PSRAM_DUTY_GATED:
            return CMD_PROTOCOL_V2;
        default:
            return CMD_PROTOCOL_V1;
    }
}

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

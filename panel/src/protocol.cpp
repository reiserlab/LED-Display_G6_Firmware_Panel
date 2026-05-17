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
const size_t PAYLOAD_DISPLAY_GRAY_2 = 51;    // 50 pattern + 1 stretch
const size_t PAYLOAD_DISPLAY_GRAY_16 = 201;  // 200 pattern + 1 stretch
const size_t MESSAGE_MINIMUM_SIZE = HEADER_SIZE + PAYLOAD_MINIMUM_SIZE;

const PayloadSizeUMap PAYLOAD_SIZE_UMAP {
    {CMD_ID_COMMS_CHECK,              PAYLOAD_COMMS_CHECK},
    {CMD_ID_DISPLAY_GRAY_2,           PAYLOAD_DISPLAY_GRAY_2},
    {CMD_ID_DISPLAY_GRAY_16,          PAYLOAD_DISPLAY_GRAY_16},
    // V1 Persistent — same payload shape as the V1 Oneshot commands
    {CMD_ID_DISPLAY_GRAY_2_PERSIST,   PAYLOAD_DISPLAY_GRAY_2},
    {CMD_ID_DISPLAY_GRAY_16_PERSIST,  PAYLOAD_DISPLAY_GRAY_16},
};

const DisplayCommandsUMap DISPLAY_COMMANDS_UMAP {
    {CMD_ID_DISPLAY_GRAY_2,           0},
    {CMD_ID_DISPLAY_GRAY_16,          0},
    {CMD_ID_DISPLAY_GRAY_2_PERSIST,   0},
    {CMD_ID_DISPLAY_GRAY_16_PERSIST,  0},
};
const GrayLevelUMap GRAY_LEVEL_UMAP  {
    {GrayLevel::Gray_2,   2},
    {GrayLevel::Gray_16, 16},
};

#include <bitset>
#include <algorithm>
#include "message.h"

#include <Streaming.h>

Message::Message() {}


size_t Message::num_bytes() {
    return num_bytes_;
}


void Message::set_num_bytes(size_t num_bytes) {
    num_bytes_ = std::min(MESSAGE_MAXIMUM_SIZE, num_bytes);
    num_bytes_ = std::max(MESSAGE_MINIMUM_SIZE, num_bytes_);
}


uint8_t Message::header_byte() {
    return data_.at(0);
}


void Message::set_header_byte(uint8_t header_byte) {
    data_.at(0) = header_byte & 0b01111111;
}


uint8_t Message::command_byte() {
    return data_.at(1);
}

void Message::set_command_byte(uint8_t command_byte) {
    data_.at(1) = command_byte;
}


uint8_t Message::parity_bit() {
    return bitRead(header_byte(), 7);
}


void Message::set_parity_bit() {
    uint8_t parity = calculate_parity_bit();
    bitWrite(data_.at(0), 7, parity); 
}


bool Message::check_parity() {
    return parity_bit() == calculate_parity_bit();
}


bool Message::check_length() {
    bool ok = num_bytes_ >= MESSAGE_MINIMUM_SIZE; 
    uint8_t cmd = command_byte();
    if (PAYLOAD_SIZE_UMAP.find(cmd) != PAYLOAD_SIZE_UMAP.end()) {
        size_t payload_size = PAYLOAD_SIZE_UMAP.at(cmd);
        if (num_bytes_ != (payload_size + HEADER_SIZE)) {
            ok = false;
        }
    }
    return ok;
}


bool Message::check_protocol(uint8_t protocol) {
    // S1.2: mask the parity bit before comparing — otherwise a valid V1
    // message with parity=1 (header 0x81) would fail the version check.
    //
    // Persistent commands (0x11 / 0x31) live in V1 namespace, so V1 is the
    // only accepted version. V2 (header 0x02/0x82) reserved for future
    // Triggered/Gated/PSRAM commands; firmware will accept it then.
    uint8_t v = header_byte() & 0b01111111;
    return v == (protocol & 0b01111111);
}


// S1.3 helper: build a parity-correct header byte for a 3-byte CIPO
// confirmation message {header, cmd, checksum}. Parity rule per spec:
// MSB of byte 0 = (count of 1-bits across bits[0..6] of byte 0, all bits of
// cmd, all bits of checksum) mod 2.
uint8_t Message::header_with_parity_for_3byte(
    uint8_t version_byte_no_parity, uint8_t cmd, uint8_t checksum)
{
    uint8_t v = version_byte_no_parity & 0b01111111;
    uint32_t count = 0;
    count += std::bitset<sizeof(uint8_t)>(v).count();
    count += std::bitset<sizeof(uint8_t)>(cmd).count();
    count += std::bitset<sizeof(uint8_t)>(checksum).count();
    uint8_t parity = count & 1;
    return v | (parity << 7);
}


void Message::from_pattern(Pattern &pat, uint8_t protocol) {
    switch (pat.gray_level()) {
        case GrayLevel::Gray_2:
            from_pattern_gray_2(pat, protocol);
            break;
        case GrayLevel::Gray_16:
            from_pattern_gray_16(pat, protocol);
            break;
        default:
            // TODO: set some kind of error pattern.
            break;
    }
}

Pattern Message::to_pattern(bool &err) {
    Pattern pat;
    to_pattern(pat, err);
    return pat;
}

void Message::to_pattern(Pattern &pat, bool &err) {

    uint8_t cmd = command_byte();

    // Check if message is something we can create a display pattern
    // from.  If not exit with error.
    if (DISPLAY_COMMANDS_UMAP.find(cmd) == DISPLAY_COMMANDS_UMAP.end()) {
        err = true;
    }
    else {
        // Gray_2 vs Gray_16 + Oneshot vs Persistent are both encoded in the
        // command byte. Payload shape only depends on gray level (Persistent
        // commands have the same wire shape as the V1 Oneshot commands).
        switch (cmd) {
            case CMD_ID_DISPLAY_GRAY_2:
                to_pattern_gray_2(pat);
                pat.set_mode(DisplayMode::Oneshot);
                break;

            case CMD_ID_DISPLAY_GRAY_16:
                to_pattern_gray_16(pat);
                pat.set_mode(DisplayMode::Oneshot);
                break;

            case CMD_ID_DISPLAY_GRAY_2_PERSIST:
                to_pattern_gray_2(pat);   // same payload shape as 0x10
                pat.set_mode(DisplayMode::Persistent);
                break;

            case CMD_ID_DISPLAY_GRAY_16_PERSIST:
                to_pattern_gray_16(pat);  // same payload shape as 0x30
                pat.set_mode(DisplayMode::Persistent);
                break;

            default:
                // We shouldn't be here
                err = true;
                break;
        }
    }
}

void Message::to_comms_check(uint8_t protocol) {
    uint8_t cmd = CMD_ID_COMMS_CHECK;
    size_t payload_size = PAYLOAD_SIZE_UMAP.at(cmd);

    set_num_bytes(HEADER_SIZE + payload_size);
    set_header_byte(protocol);
    set_command_byte(cmd);

    // Set payload to 0, 1, 2, .... 200
    for (size_t i=0; i<payload_size; i++) {
        payload_at(i) = uint8_t(i);
    }

    //Set the parity bit from the message data
    set_parity_bit();
}


size_t Message::payload_size() {
    return num_bytes_ - HEADER_SIZE;
}


uint8_t &Message::payload_at(size_t n) {
    size_t i = std::min(n + HEADER_SIZE, num_bytes_-1);
    return data_.at(i);
}


void Message::payload_to_zeros() {
    for (size_t i=0; i<payload_size(); i++) {
        payload_at(i) = 0;
    }
}


uint8_t Message::calculate_parity_bit() {
    size_t sum = 0;
    for (size_t i=0; i<num_bytes_; i++) {
        uint8_t byte = data_.at(i);
        if (i==0) {
            // Mask parity bit
            byte &= 0b01111111; 
        }
        sum += std::bitset<sizeof(uint8_t)>(byte).count();
    }
    return sum % 2;
}


// CRC-8/AUTOSAR lookup table.
// Params: poly=0x2F, init=0xFF, refin=false, refout=false, xorout=0xFF.
// Universal check value: CRC over ASCII "123456789" = 0xDF.
// Generator (Python, equivalent to `reveng -p 0x2F -i FF -F FF`):
//   table[b] = repeat 8x: (crc<<1)^0x2F if crc&0x80 else crc<<1, starting crc=b.
static const uint8_t CRC8_AUTOSAR_TABLE[256] = {
    0x00, 0x2F, 0x5E, 0x71, 0xBC, 0x93, 0xE2, 0xCD, 0x57, 0x78, 0x09, 0x26, 0xEB, 0xC4, 0xB5, 0x9A,
    0xAE, 0x81, 0xF0, 0xDF, 0x12, 0x3D, 0x4C, 0x63, 0xF9, 0xD6, 0xA7, 0x88, 0x45, 0x6A, 0x1B, 0x34,
    0x73, 0x5C, 0x2D, 0x02, 0xCF, 0xE0, 0x91, 0xBE, 0x24, 0x0B, 0x7A, 0x55, 0x98, 0xB7, 0xC6, 0xE9,
    0xDD, 0xF2, 0x83, 0xAC, 0x61, 0x4E, 0x3F, 0x10, 0x8A, 0xA5, 0xD4, 0xFB, 0x36, 0x19, 0x68, 0x47,
    0xE6, 0xC9, 0xB8, 0x97, 0x5A, 0x75, 0x04, 0x2B, 0xB1, 0x9E, 0xEF, 0xC0, 0x0D, 0x22, 0x53, 0x7C,
    0x48, 0x67, 0x16, 0x39, 0xF4, 0xDB, 0xAA, 0x85, 0x1F, 0x30, 0x41, 0x6E, 0xA3, 0x8C, 0xFD, 0xD2,
    0x95, 0xBA, 0xCB, 0xE4, 0x29, 0x06, 0x77, 0x58, 0xC2, 0xED, 0x9C, 0xB3, 0x7E, 0x51, 0x20, 0x0F,
    0x3B, 0x14, 0x65, 0x4A, 0x87, 0xA8, 0xD9, 0xF6, 0x6C, 0x43, 0x32, 0x1D, 0xD0, 0xFF, 0x8E, 0xA1,
    0xE3, 0xCC, 0xBD, 0x92, 0x5F, 0x70, 0x01, 0x2E, 0xB4, 0x9B, 0xEA, 0xC5, 0x08, 0x27, 0x56, 0x79,
    0x4D, 0x62, 0x13, 0x3C, 0xF1, 0xDE, 0xAF, 0x80, 0x1A, 0x35, 0x44, 0x6B, 0xA6, 0x89, 0xF8, 0xD7,
    0x90, 0xBF, 0xCE, 0xE1, 0x2C, 0x03, 0x72, 0x5D, 0xC7, 0xE8, 0x99, 0xB6, 0x7B, 0x54, 0x25, 0x0A,
    0x3E, 0x11, 0x60, 0x4F, 0x82, 0xAD, 0xDC, 0xF3, 0x69, 0x46, 0x37, 0x18, 0xD5, 0xFA, 0x8B, 0xA4,
    0x05, 0x2A, 0x5B, 0x74, 0xB9, 0x96, 0xE7, 0xC8, 0x52, 0x7D, 0x0C, 0x23, 0xEE, 0xC1, 0xB0, 0x9F,
    0xAB, 0x84, 0xF5, 0xDA, 0x17, 0x38, 0x49, 0x66, 0xFC, 0xD3, 0xA2, 0x8D, 0x40, 0x6F, 0x1E, 0x31,
    0x76, 0x59, 0x28, 0x07, 0xCA, 0xE5, 0x94, 0xBB, 0x21, 0x0E, 0x7F, 0x50, 0x9D, 0xB2, 0xC3, 0xEC,
    0xD8, 0xF7, 0x86, 0xA9, 0x64, 0x4B, 0x3A, 0x15, 0x8F, 0xA0, 0xD1, 0xFE, 0x33, 0x1C, 0x6D, 0x42,
};

// Computes the V1 CIPO-confirmation checksum byte over the incoming wire
// message. Per g6_01-panel-protocol.md § CRC-8 algorithm:
//   - The header byte (data_[0]) is hashed with its parity bit cleared.
//     This is done INSIDE this function; callers pass the raw Message.
//   - Scope is the incoming wire bytes (data_[0 .. num_bytes_-1]); the
//     CRC byte itself is NOT part of the input.
//   - V1 confirmation-slot only. ISP-slot CRC and pattern-file header CRC
//     are different scopes (and pattern-file frame CRC is a different
//     algorithm). Don't repurpose without re-reading those specs.
uint8_t Message::calculate_crc8() {
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < num_bytes_; i++) {
        uint8_t b = data_.at(i);
        if (i == 0) b &= 0b01111111;
        crc = CRC8_AUTOSAR_TABLE[crc ^ b];
    }
    return crc ^ 0xFF;
}


uint8_t *Message::data_ptr() {
    return data_.data();
}


void Message::print_data() {
    for (size_t i=0; i<num_bytes_; i++) {
        Serial << data_.at(i) << endl;
    }
}


// Protected methods
// -------------------------------------------------------------

void Message::from_pattern_gray_2(Pattern &pat, uint8_t protocol) {
    if (pat.gray_level() != GrayLevel::Gray_2) {
        return;
    }

    uint8_t cmd_id = CMD_ID_DISPLAY_GRAY_2;
    size_t payload_size = PAYLOAD_SIZE_UMAP.at(cmd_id);
    size_t total_size = HEADER_SIZE + payload_size;

    // Set message header
    set_num_bytes(total_size);
    set_header_byte(protocol);
    set_command_byte(cmd_id);

    // Pack pattern pixel data
    size_t pixel_num = 0;
    for (size_t i=0; i<PANEL_SIZE; i++) {
        for (size_t j=0; j<PANEL_SIZE; j++) {
            size_t byte_num = pixel_num/8;
            size_t bit_pos = 7 - (pixel_num - 8*byte_num);
            uint8_t pixel_value = pat.at(i,j) & 0b00000001;
            bitWrite(payload_at(byte_num), bit_pos, pixel_value);
            pixel_num++;
        }
    }

    // Add duty_cycle value (last item)
    data_.at(total_size-1) = pat.duty_cycle();
    set_parity_bit();
}

void Message::from_pattern_gray_16(Pattern &pat, uint8_t protocol) {
    if (pat.gray_level() != GrayLevel::Gray_16) {
        return;
    }

    uint8_t cmd_id = CMD_ID_DISPLAY_GRAY_16;
    size_t payload_size = PAYLOAD_SIZE_UMAP.at(cmd_id);
    size_t total_size = HEADER_SIZE + payload_size;

    // Set message header
    set_num_bytes(total_size);
    set_header_byte(protocol);
    set_command_byte(cmd_id);

    // Pack pattern pixel data
    size_t pixel_num = 0;
    for (size_t i=0; i<PANEL_SIZE; i++) {
        for (size_t j=0; j<PANEL_SIZE; j++) {
            size_t byte_num = pixel_num/2;
            uint8_t pixel_value = pat.at(i,j) & 0b00001111;
            uint8_t upper = 0;
            uint8_t lower = 0;
            if (pixel_num%2 ==0) {
                upper = pixel_value << 4;
                lower = payload_at(byte_num) & 0b00001111;
            }
            else {
                upper = payload_at(byte_num) & 0b11110000;
                lower = pixel_value;
            }
            payload_at(byte_num) = upper | lower;
            pixel_num++;
        }
    }

    // Add duty_cycle value (last item)
    data_.at(total_size-1) = pat.duty_cycle();
    set_parity_bit();
}


void Message::to_pattern_gray_2(Pattern &pat) {
    // Accept V1 Oneshot (0x10) and V1 Persistent (0x11) — both share the
    // 50 B + duty_cycle payload shape. Mode is set by the caller (to_pattern()).
    uint8_t cmd = command_byte();
    if (cmd != CMD_ID_DISPLAY_GRAY_2 && cmd != CMD_ID_DISPLAY_GRAY_2_PERSIST) {
        return;
    }
    pat.set_gray_level(GrayLevel::Gray_2);
    // Extract pixel values
    size_t pixel_num = 0;
    for (size_t i=0; i<PANEL_SIZE; i++) {
        for (size_t j=0; j<PANEL_SIZE; j++) {
            size_t byte_num = pixel_num/8;
            size_t bit_pos = 7 - (pixel_num - 8*byte_num);
            pat.at(i,j) = bitRead(payload_at(byte_num), bit_pos);
            pixel_num++;
        }
    }
    // Set duty_cycle to last payload value
    pat.set_duty_cycle(data_.at(num_bytes_-1));
}


void Message::to_pattern_gray_16(Pattern &pat) {
    // Accept V1 Oneshot (0x30) and V1 Persistent (0x31) — same 200 B +
    // duty_cycle payload shape; mode is set by to_pattern().
    uint8_t cmd = command_byte();
    if (cmd != CMD_ID_DISPLAY_GRAY_16 && cmd != CMD_ID_DISPLAY_GRAY_16_PERSIST) {
        return;
    }
    pat.set_gray_level(GrayLevel::Gray_16);
    // Extract pixel values
    size_t pixel_num = 0;
    for (size_t i=0; i<PANEL_SIZE; i++) {
        for (size_t j=0; j<PANEL_SIZE; j++) {
            size_t byte_num = pixel_num/2;
            uint8_t pixel_value = 0;
            if (pixel_num%2 == 0) {
                pixel_value = (payload_at(byte_num) & 0b11110000) >> 4;
            }
            else {
                pixel_value = payload_at(byte_num) & 0b00001111;
            }
            pat.at(i,j) = pixel_value;
            pixel_num++;
        }
    }
    // Set duty_cycle to last payload value
    pat.set_duty_cycle(data_.at(num_bytes_-1));
}


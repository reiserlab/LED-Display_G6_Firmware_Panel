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


uint8_t Message::calculate_8bit_checksum() {
    size_t sum = 0;
    for (size_t i=0; i<num_bytes_; i++) {
       sum += data_.at(i);
    }
    return uint8_t(sum);
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

    // Add stretch value (last item)
    data_.at(total_size-1) = pat.stretch();
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

    // Add stretch value (last item)
    data_.at(total_size-1) = pat.stretch();
    set_parity_bit();
}


void Message::to_pattern_gray_2(Pattern &pat) {
    // Accept V1 Oneshot (0x10) and V1 Persistent (0x11) — both share the
    // 50 B + stretch payload shape. Mode is set by the caller (to_pattern()).
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
    // Set stretch to last payload value
    pat.set_stretch(data_.at(num_bytes_-1));
}


void Message::to_pattern_gray_16(Pattern &pat) {
    // Accept V1 Oneshot (0x30) and V1 Persistent (0x31) — same 200 B +
    // stretch payload shape; mode is set by to_pattern().
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
    // Set stretch to last payload value
    pat.set_stretch(data_.at(num_bytes_-1));
}


#include <hardware/spi.h>
#include <hardware/gpio.h>
#include "constants.h"
#include "messenger.h"
#include "panel_spi_custom.h"
#include <Streaming.h>


Messenger::Messenger(queue_t &display_queue) : display_queue_(display_queue) {

    // Load callbacks into command table
    cmd_umap_.insert( {
            CMD_ID_COMMS_CHECK,
            [this](Message &msg){this -> on_cmd_comms_check(msg);}
    });

    cmd_umap_.insert( {
            CMD_ID_DISPLAY_GRAY_2,
            [this](Message &msg){this -> on_cmd_display_gray_2(msg);}
    });

    cmd_umap_.insert( {
            CMD_ID_DISPLAY_GRAY_16,
            [this](Message &msg){this -> on_cmd_display_gray_16(msg);}
    });

    // S1.9: V3 Persistent commands (same payload shape as V1 Oneshot; mode is
    // set on the Pattern via Message::to_pattern() based on cmd id).
    cmd_umap_.insert( {
            CMD_ID_DISPLAY_GRAY_2_PERSIST,
            [this](Message &msg){this -> on_cmd_display_gray_2(msg);}
    });

    cmd_umap_.insert( {
            CMD_ID_DISPLAY_GRAY_16_PERSIST,
            [this](Message &msg){this -> on_cmd_display_gray_16(msg);}
    });

}

void Messenger::initialize() {

    // Setup SPI (SPI_INST = spi0 on v0.2.1, spi1 on v0.3.1; see constants.cpp)
    spi_init(SPI_INST, SPI_SPEED);
    gpio_init(SPI_SCK_PIN);
    gpio_init(SPI_MOSI_PIN);
    gpio_init(SPI_MISO_PIN);
    gpio_init(SPI_CS_PIN);
    gpio_set_function(SPI_MOSI_PIN, GPIO_FUNC_SPI);
    gpio_set_function(SPI_SCK_PIN, GPIO_FUNC_SPI);
    gpio_set_function(SPI_MISO_PIN, GPIO_FUNC_SPI);
    gpio_set_function(SPI_CS_PIN, GPIO_FUNC_SPI);
    spi_set_slave(SPI_INST, true);
    spi_set_format(SPI_INST, 8, SPI_CPOL_1, SPI_CPHA_1, SPI_MSB_FIRST);

    // S1.3: prime the CIPO confirmation buffer + TX FIFO with the empty-buffer
    // sentinel BEFORE the first CS falling edge from a master.
    panel_spi_clear_confirmation();
}

void Messenger::update() {

    static Message msg;
    panel_spi_read(msg);
    msg_count_ += 1;

    // S1.4: reset COMM_CHECK byte-validation flag at the start of every
    // update() so it doesn't carry across non-COMM_CHECK messages.
    comm_check_ok_ = true;

    // S1.2: wire check_protocol() into the validity gate alongside parity
    // and length. Without this, a message with unsupported version bits but
    // a known command would still be dispatched.
    bool parity_ok   = msg.check_parity();
    bool length_ok   = msg.check_length();
    bool protocol_ok = msg.check_protocol(CMD_PROTOCOL);
    bool cmd_ok      = false;
    uint8_t cmd_id   = msg.command_byte();
    if (parity_ok && length_ok && protocol_ok) {
        if (cmd_umap_.count(cmd_id) > 0) {
            cmd_umap_.at(cmd_id)(msg);
            cmd_ok = true;
        }
    }

    // S1.3: arm the CIPO confirmation buffer per the plan's buffer-update rule.
    //  - Valid + COMM_CHECK passed:  arm {header, cmd, checksum}
    //  - Valid COMM_CHECK that byte-mismatched (comm_check_ok_ == false):
    //                                arm {header, 0xFF, 0x00}  (sentinel)
    //  - Any other invalidity:       do NOT touch the buffer (per spec)
    //
    // S1.9: the confirmation's version byte echoes whichever protocol version
    // the incoming message used (V1 = 0x01 or V3 = 0x03 for the Persistent
    // subset we support). Spec says "panel returns the version, command, and
    // checksum from the previously received command."
    if (parity_ok && length_ok && protocol_ok && cmd_ok) {
        uint8_t in_version = msg.header_byte() & 0b01111111;  // 0x01 or 0x03
        if (cmd_id == CMD_ID_COMMS_CHECK && !comm_check_ok_) {
            uint8_t hdr = Message::header_with_parity_for_3byte(
                in_version, 0xFF, 0x00);
            panel_spi_arm_confirmation(hdr, 0xFF, 0x00);
        } else {
            uint8_t chk = msg.calculate_8bit_checksum();
            uint8_t hdr = Message::header_with_parity_for_3byte(
                in_version, cmd_id, chk);
            panel_spi_arm_confirmation(hdr, cmd_id, chk);
        }
    }
    // else: buffer stays as the previous valid confirmation OR the empty
    // sentinel (already loaded into the TX FIFO between transactions).

    // DEVEL serial heartbeat
    // -----------------------------------------------------------
    if (msg_count_ % 1000 == 0) {
        Serial << "msg_count:      " << msg_count_  << endl;
        Serial << "parity_ok:      " << parity_ok   << endl;
        Serial << "length_ok:      " << length_ok   << endl;
        Serial << "protocol_ok:    " << protocol_ok << endl;
        Serial << "cmd_ok:         " << cmd_ok      << endl;
        Serial << "comm_check_ok:  " << comm_check_ok_ << endl;
        Serial << "queue_drops:    " << queue_drops_   << endl;
        Serial << endl;
    }
    // -----------------------------------------------------------
}


void Messenger::on_cmd_comms_check(Message &msg) {
    // S1.4: byte-for-byte validation of the canonical COMM_CHECK payload
    // (0, 1, 2, ..., 199). Result consumed by update() when arming the
    // CIPO confirmation buffer.
    bool ok = true;
    for (size_t i = 0; i < PAYLOAD_COMMS_CHECK; i++) {
        if (msg.payload_at(i) != uint8_t(i)) {
            ok = false;
            break;
        }
    }
    comm_check_ok_ = ok;
}


void Messenger::on_cmd_display_gray_2(Message &msg) {
    bool err = false;
    msg.to_pattern(pat_, err);
    // S1.6: track queue overflow drops; surfaced in the serial heartbeat.
    if (!queue_try_add(&display_queue_, &pat_)) {
        queue_drops_++;
    }
}


void Messenger::on_cmd_display_gray_16(Message &msg) {
    bool err = false;
    msg.to_pattern(pat_, err);
    if (!queue_try_add(&display_queue_, &pat_)) {
        queue_drops_++;
    }
}






//Pattern pat;
//pat.set_gray_level(GrayLevel::Gray_2);
//pat.set_stretch(255);
//
//pat.matrix() << 
//    1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 
//    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 
//    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 
//    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 
//    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 
//    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 
//    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 
//    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 
//    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 
//    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 
//    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 
//    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 
//    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 
//    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 
//    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 
//    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 
//    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 
//    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 
//    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 
//    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0; 
//
//Message msg; 
//msg.from_pattern(pat);
//
//Serial << endl;
//
//for (size_t i=0; i<msg.payload_size(); i++) {
//    Serial << i << ", " << msg.payload_at(i) << endl;
//}
//Serial << endl;
//
//bool err = false;
//Pattern pat2 = msg.to_pattern(err);
//Serial << "err: " << err << endl;
//for (size_t i=0; i<PANEL_SIZE; i++) {
//    for (size_t j=0; j<PANEL_SIZE; j++) {
//        Serial << pat2.at(i,j) << " ";
//    }
//    Serial << endl;
//}
//Serial << endl;






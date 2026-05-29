#include <hardware/spi.h>
#include <hardware/gpio.h>
#include <pico/time.h>
#include "constants.h"
#include "messenger.h"
#include "panel_spi_custom.h"
#include "predef_patterns.h"
#include "predef_patterns_table.h"
#include "display.h"
#include <Streaming.h>

// S2.2: Display lives in main.cpp; we read frames_skipped_ for the heartbeat.
extern Display display;

// Build with -DSPI_DIAG=1 to enable the SPI timing + validity-gate serial
// diagnostics in update() (per-1000-message histograms and a parity dump).
// Off by default: the diagnostics do blocking Serial work on core 0 and can
// cost the occasional frame.

Messenger::Messenger(queue_t &display_queue, queue_t &error_request_queue)
    : display_queue_(display_queue),
      error_request_queue_(error_request_queue)
{

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

    // V1 Persistent commands (same payload shape as V1 Oneshot; mode is set
    // on the Pattern via Message::to_pattern() based on cmd id).
    cmd_umap_.insert( {
            CMD_ID_DISPLAY_GRAY_2_PERSIST,
            [this](Message &msg){this -> on_cmd_display_gray_2(msg);}
    });

    cmd_umap_.insert( {
            CMD_ID_DISPLAY_GRAY_16_PERSIST,
            [this](Message &msg){this -> on_cmd_display_gray_16(msg);}
    });

    // V1 Triggered + Gated — same payload as the corresponding Oneshot
    // command; mode is set by Message::to_pattern() based on cmd id and
    // consumed by the display state machine on core 1.
    cmd_umap_.insert( {
            CMD_ID_DISPLAY_GRAY_2_TRIGGERED,
            [this](Message &msg){this -> on_cmd_display_gray_2(msg);}
    });

    cmd_umap_.insert( {
            CMD_ID_DISPLAY_GRAY_2_GATED,
            [this](Message &msg){this -> on_cmd_display_gray_2(msg);}
    });

    cmd_umap_.insert( {
            CMD_ID_DISPLAY_GRAY_16_TRIGGERED,
            [this](Message &msg){this -> on_cmd_display_gray_16(msg);}
    });

    cmd_umap_.insert( {
            CMD_ID_DISPLAY_GRAY_16_GATED,
            [this](Message &msg){this -> on_cmd_display_gray_16(msg);}
    });

    cmd_umap_.insert( {
            CMD_ID_ERROR_DISPLAY,
            [this](Message &msg){this -> on_cmd_error_display(msg);}
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
    // sentinel BEFORE the first CS falling edge from the controller.
    panel_spi_clear_confirmation();
}

void Messenger::update() {

    static Message msg;

#if SPI_DIAG
    // Wall-clock gap between consecutive update() entries: detects when
    // post-receive processing spills past the controller's inter-frame window.
    static uint64_t t_last_enter = 0;
    static uint32_t gap_bins[6] = {0};   // <1, <2, <4, <8, <16, >=16 ms
    static uint32_t gap_max_us  = 0;
    uint64_t t_enter = time_us_64();
    if (t_last_enter != 0) {
        uint32_t gap_us = (uint32_t)(t_enter - t_last_enter);
        if      (gap_us <  1000) gap_bins[0]++;
        else if (gap_us <  2000) gap_bins[1]++;
        else if (gap_us <  4000) gap_bins[2]++;
        else if (gap_us <  8000) gap_bins[3]++;
        else if (gap_us < 16000) gap_bins[4]++;
        else                     gap_bins[5]++;
        if (gap_us > gap_max_us) gap_max_us = gap_us;
    }
    t_last_enter = t_enter;

    uint64_t t_spi_start = time_us_64();
#endif

    panel_spi_read(msg);

#if SPI_DIAG
    // Time inside panel_spi_read, bucketed by received length so a full frame
    // (n==203) is distinguishable from a short / overflow capture (n==8).
    static uint32_t spi_bins_n203[6] = {0};
    static uint32_t spi_bins_n8  [6] = {0};
    static uint32_t spi_bins_oth [6] = {0};
    {
        uint32_t spi_us = (uint32_t)(time_us_64() - t_spi_start);
        uint32_t *bins = (msg.num_bytes() == 203) ? spi_bins_n203
                       : (msg.num_bytes() ==   8) ? spi_bins_n8
                                                  : spi_bins_oth;
        if      (spi_us <  1000) bins[0]++;
        else if (spi_us <  2000) bins[1]++;
        else if (spi_us <  4000) bins[2]++;
        else if (spi_us <  8000) bins[3]++;
        else if (spi_us < 16000) bins[4]++;
        else                     bins[5]++;
    }

    uint64_t t_post_recv_start = time_us_64();
    msg_count_ += 1;
#endif

    // Reset the COMM_CHECK byte-validation flag each message so it can't carry
    // across non-COMM_CHECK messages.
    comm_check_ok_ = true;

    // Snapshot the error-display flag once so dispatch, CIPO arming, and the
    // error-raise decision all see a consistent view.
    bool err_active = Display::error_display_active;

    bool parity_ok   = msg.check_parity();
    bool length_ok   = msg.check_length();
    bool protocol_ok = msg.check_protocol(CMD_PROTOCOL);
    bool cmd_ok      = false;
    uint8_t cmd_id   = msg.command_byte();
    if (parity_ok && length_ok && protocol_ok && !err_active) {
        if (cmd_umap_.count(cmd_id) > 0) {
            cmd_umap_.at(cmd_id)(msg);
            cmd_ok = true;
        }
    }

    // Raise a PE code on the first failing gate (parity > length > protocol >
    // unknown opcode). Suppressed while an error glyph is already showing.
    if (!err_active) {
        if (!parity_ok) {
            raise_error(PREDEF_SLOT_PE02);
        } else if (!length_ok) {
            raise_error(PREDEF_SLOT_PE03);
        } else if (!protocol_ok) {
            raise_error(PREDEF_SLOT_PE04);
        } else if (cmd_umap_.count(cmd_id) == 0) {
            raise_error(PREDEF_SLOT_PE01);
        }
    }

    // Arm the CIPO confirmation for the next transaction, only on a fully valid
    // message. A byte-mismatched COMM_CHECK arms the {header, 0xFF, 0x00}
    // sentinel; any invalidity leaves the previous buffer untouched (per spec).
    if (!err_active && parity_ok && length_ok && protocol_ok && cmd_ok) {
        uint8_t in_version = msg.header_byte() & 0b01111111;  // V1 only
        if (cmd_id == CMD_ID_COMMS_CHECK && !comm_check_ok_) {
            uint8_t hdr = Message::header_with_parity_for_3byte(
                in_version, 0xFF, 0x00);
            panel_spi_arm_confirmation(hdr, 0xFF, 0x00);
        } else {
            uint8_t chk = msg.calculate_crc8();
            uint8_t hdr = Message::header_with_parity_for_3byte(
                in_version, cmd_id, chk);
            panel_spi_arm_confirmation(hdr, cmd_id, chk);
        }
    }

#if SPI_DIAG
    static uint32_t proc_bins[6] = {0};
    static uint32_t proc_max_us  = 0;
    {
        uint32_t proc_us = (uint32_t)(time_us_64() - t_post_recv_start);
        if      (proc_us <  1000) proc_bins[0]++;
        else if (proc_us <  2000) proc_bins[1]++;
        else if (proc_us <  4000) proc_bins[2]++;
        else if (proc_us <  8000) proc_bins[3]++;
        else if (proc_us < 16000) proc_bins[4]++;
        else                      proc_bins[5]++;
        if (proc_us > proc_max_us) proc_max_us = proc_us;
    }

    // Terse single-line histograms + last-frame gate state every 1000 msgs.
    // Bins are cumulative across the run; read deltas between prints.
    if (msg_count_ % 1000 == 0) {
        Serial.print("gap_us [<1k <2k <4k <8k <16k >=16k]: ");
        for (int i = 0; i < 6; i++) { Serial.print(gap_bins[i]);  Serial.print(' '); }
        Serial.print(" max="); Serial.println(gap_max_us);

        Serial.print("proc_us[<1k <2k <4k <8k <16k >=16k]: ");
        for (int i = 0; i < 6; i++) { Serial.print(proc_bins[i]); Serial.print(' '); }
        Serial.print(" max="); Serial.println(proc_max_us);

        Serial.print("spi_us n=203 ");
        for (int i = 0; i < 6; i++) { Serial.print(spi_bins_n203[i]); Serial.print(' '); }
        Serial.println();
        Serial.print("spi_us n=  8 ");
        for (int i = 0; i < 6; i++) { Serial.print(spi_bins_n8[i]); Serial.print(' '); }
        Serial.println();
        Serial.print("spi_us n=oth ");
        for (int i = 0; i < 6; i++) { Serial.print(spi_bins_oth[i]); Serial.print(' '); }
        Serial.print(" last_n=");
        Serial.println(msg.num_bytes());

        Serial.print("gate p/l/pr/cmd/cc=");
        Serial.print(parity_ok);   Serial.print('/');
        Serial.print(length_ok);   Serial.print('/');
        Serial.print(protocol_ok); Serial.print('/');
        Serial.print(cmd_ok);      Serial.print('/');
        Serial.print(comm_check_ok_);
        Serial.print("  hdr=0x");  Serial.print(msg.header_byte(), HEX);
        Serial.print(" cmd=0x");   Serial.print(msg.command_byte(), HEX);
        Serial.print("  b[0..5]=");
        for (size_t i = 0; i < 6 && i < msg.num_bytes(); i++) {
            uint8_t b = msg.data_ptr()[i];
            if (b < 0x10) Serial.print('0');
            Serial.print(b, HEX); Serial.print(' ');
        }
        Serial.print(" qdrop=");   Serial.print(queue_drops_);
        Serial.print(" fskip=");   Serial.print(display.frames_skipped());
        Serial.print(" errD/S=");  Serial.print(error_displayed_count_);
        Serial.print('/');         Serial.println(error_suppressed_count_);

        // On a parity miss, show the panel's computed parity vs the received
        // bit and which payload bytes differ from the all-0xFF "All On" frame.
        if (!parity_ok) {
            uint8_t *d = msg.data_ptr();
            size_t   n = msg.num_bytes();
            size_t   non_ff = 0, first_bad = SIZE_MAX; uint8_t first_bad_val = 0;
            for (size_t i = 2; i < n; i++) {
                if (d[i] != 0xFF) {
                    if (first_bad == SIZE_MAX) { first_bad = i; first_bad_val = d[i]; }
                    non_ff++;
                }
            }
            Serial.print("  PARITY calc=");  Serial.print(msg.calculate_parity_bit());
            Serial.print(" recv_bit=");      Serial.print(msg.parity_bit());
            Serial.print(" n=");             Serial.print((uint32_t)n);
            Serial.print(" payload_non0xFF="); Serial.print((uint32_t)non_ff);
            if (first_bad != SIZE_MAX) {
                Serial.print(" first_bad@");  Serial.print((uint32_t)first_bad);
                Serial.print("=0x");          Serial.print(first_bad_val, HEX);
            }
            Serial.print(" last@");           Serial.print((uint32_t)(n - 1));
            Serial.print("=0x");              Serial.println(d[n - 1], HEX);
        }
    }
#endif
}


bool Messenger::raise_error(uint32_t slot) {
    uint64_t now = time_us_64();
    // Allow the first ever raise (last_error_raised_us_ == 0) through even
    // though "now - 0" comfortably exceeds the rate-limit — defensive cast
    // would be needed only for hypothetical wrap-around (uint64 doesn't
    // wrap within firmware lifetime).
    if (last_error_raised_us_ != 0 &&
        (now - last_error_raised_us_) < ERROR_RATE_LIMIT_US) {
        error_suppressed_count_++;
        last_error_raised_us_ = now;
        return false;
    }
    last_error_raised_us_ = now;
    if (!queue_try_add(&error_request_queue_, &slot)) {
        // Queue full — core 1 hasn't drained the previous raise yet. Treat
        // as suppressed (don't block, don't retry).
        error_suppressed_count_++;
        return false;
    }
    error_displayed_count_++;
    return true;
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


void Messenger::on_cmd_error_display(Message &msg) {
    // V1 0xC2 payload (3 bytes, LE) is the predefined-pattern slot index.
    // Shape matches V3 0x70 to preserve the option. V1 firmware reads only
    // the low byte (0..255 slots cover the V1 catalog with margin); if the
    // upper two bytes are nonzero, raise PE05 (invalid pattern index) and
    // refuse to display the requested glyph — defensive against hostpages/Reiser/Funnel.html bugs.
    uint8_t lo  = msg.payload_at(0);
    uint8_t mid = msg.payload_at(1);
    uint8_t hi  = msg.payload_at(2);
    if (mid != 0 || hi != 0) {
        raise_error(PREDEF_SLOT_PE05);
        return;
    }
    raise_error(uint32_t(lo));
}






//Pattern pat;
//pat.set_gray_level(GrayLevel::Gray_2);
//pat.set_duty_cycle(255);
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

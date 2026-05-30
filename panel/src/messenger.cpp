#include <hardware/spi.h>
#include <hardware/gpio.h>
#include <pico/time.h>
#include "constants.h"
#include "messenger.h"
#include "panel_spi_custom.h"
#include "predef_patterns.h"
#include "predef_patterns_table.h"
#include "display.h"
#include "protocol.h"
#include <Streaming.h>
#include <cstdio>

// S2.2: Display lives in main.cpp; we read frames_skipped_ for the heartbeat.
extern Display display;


#if SPI_DIAG
// ---------------------------------------------------------------------------
// SPI_DIAG — silent reception diagnostics (build flag -DSPI_DIAG=1).
//
// Behaves identically to production during streaming: counters are cheap
// in-RAM increments and there is NO serial output. On the one-shot 'd'
// command the whole picture is dumped in a single burst; 'z' zeros the
// window. Goal: localize the ~7% rejected-frame flicker — is it parity (bit
// corruption) vs length (dropped/shifted bytes), which command, and is the
// got-vs-expected byte count "short by N" (framing) or random?
// ---------------------------------------------------------------------------
namespace {
struct DiagFail { uint8_t cmd; uint16_t got; uint16_t expected; uint8_t flags; };
// flags: bit0=parity bit1=length bit2=protocol bit3=unknown-cmd
constexpr size_t DIAG_RING = 32;
uint32_t diag_msgs = 0, diag_reject_any = 0;
uint32_t diag_parity_fail = 0, diag_length_fail = 0, diag_protocol_fail = 0, diag_unknown_cmd = 0;
uint32_t diag_cmd_hist[256] = {0};
DiagFail diag_ring[DIAG_RING];
size_t   diag_ring_head = 0, diag_ring_count = 0;

uint16_t diag_expected_len(uint8_t cmd) {
    auto it = PAYLOAD_SIZE_UMAP.find(cmd);
    return (it != PAYLOAD_SIZE_UMAP.end()) ? (uint16_t)(it->second + HEADER_SIZE) : 0;
}
void diag_record(uint8_t cmd, uint16_t got, uint8_t flags) {
    DiagFail &f = diag_ring[diag_ring_head];
    f.cmd = cmd; f.got = got; f.expected = diag_expected_len(cmd); f.flags = flags;
    diag_ring_head = (diag_ring_head + 1) % DIAG_RING;
    if (diag_ring_count < DIAG_RING) diag_ring_count++;
}
void diag_reset() {
    diag_msgs = diag_reject_any = 0;
    diag_parity_fail = diag_length_fail = diag_protocol_fail = diag_unknown_cmd = 0;
    diag_ring_head = diag_ring_count = 0;
    for (int i = 0; i < 256; i++) diag_cmd_hist[i] = 0;
}
inline void diag_emit(const char *b, int n) {
    if (n > 0) Serial.write(reinterpret_cast<const uint8_t *>(b),
                            (size_t)(n < (int)160 ? n : 159));
}
// Blocking burst — only ever runs on an explicit 'd', never during the
// measurement window, so blocking on the USB-CDC FIFO is fine here.
void diag_dump() {
    char b[160];
    diag_emit(b, snprintf(b, sizeof(b),
        "\r\n=== SPI_DIAG (PANEL_REV=%d) ===\r\n", (int)PANEL_REV));
    unsigned long pml = diag_msgs
        ? (unsigned long)((uint64_t)diag_reject_any * 1000u / diag_msgs) : 0;
    diag_emit(b, snprintf(b, sizeof(b), "msgs=%lu reject_any=%lu (%lu.%lu%%)\r\n",
        (unsigned long)diag_msgs, (unsigned long)diag_reject_any, pml / 10, pml % 10));
    diag_emit(b, snprintf(b, sizeof(b),
        "parity_fail=%lu length_fail=%lu protocol_fail=%lu unknown_cmd=%lu\r\n",
        (unsigned long)diag_parity_fail, (unsigned long)diag_length_fail,
        (unsigned long)diag_protocol_fail, (unsigned long)diag_unknown_cmd));
    diag_emit(b, snprintf(b, sizeof(b), "cmd_hist:"));
    for (int c = 0; c < 256; c++)
        if (diag_cmd_hist[c])
            diag_emit(b, snprintf(b, sizeof(b), " 0x%02X=%lu", c,
                                  (unsigned long)diag_cmd_hist[c]));
    Serial.write(reinterpret_cast<const uint8_t *>("\r\n"), 2);
    diag_emit(b, snprintf(b, sizeof(b),
        "last %u fails (cmd got/exp PLRU):\r\n", (unsigned)diag_ring_count));
    for (size_t i = 0; i < diag_ring_count; i++) {
        size_t idx = (diag_ring_head + DIAG_RING - diag_ring_count + i) % DIAG_RING;
        const DiagFail &f = diag_ring[idx];
        diag_emit(b, snprintf(b, sizeof(b), "  0x%02X got=%u exp=%u %c%c%c%c\r\n",
            f.cmd, f.got, f.expected,
            (f.flags & 1) ? 'P' : '.', (f.flags & 2) ? 'L' : '.',
            (f.flags & 4) ? 'R' : '.', (f.flags & 8) ? 'U' : '.'));
    }
    Serial.write(reinterpret_cast<const uint8_t *>("=== end ===\r\n"), 13);
}
} // namespace
#endif


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

    // Snapshot the error-display flag once per message so all decisions
    // (dispatch, CIPO arming, error raise) see a consistent view.
    bool err_active = Display::error_display_active;

    // S1.2: wire check_protocol() into the validity gate alongside parity
    // and length. Without this, a message with unsupported version bits but
    // a known command would still be dispatched.
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

#if SPI_DIAG
    // Silent per-message accounting. Count each failing check INDEPENDENTLY
    // (a dropped-bytes frame fails both parity and length — we want to see
    // both, not just the first-wins one the error-raise below picks).
    {
        diag_msgs++;
        diag_cmd_hist[cmd_id]++;
        uint8_t flags = 0;
        if (!parity_ok)   { diag_parity_fail++;   flags |= 1; }
        if (!length_ok)   { diag_length_fail++;   flags |= 2; }
        if (!protocol_ok) { diag_protocol_fail++; flags |= 4; }
        if (parity_ok && length_ok && protocol_ok && cmd_umap_.count(cmd_id) == 0) {
            diag_unknown_cmd++; flags |= 8;
        }
        if (flags) { diag_reject_any++; diag_record(cmd_id, (uint16_t)msg.num_bytes(), flags); }
    }
#endif

    // Trigger PE codes on validity-gate failures. First-detected wins:
    // parity > length > protocol > unknown opcode. Suppressed while the
    // panel is already showing an error glyph.
    //
    // Sub-minimum transactions (num_bytes < MESSAGE_MINIMUM_SIZE) are runts —
    // a 1-2 byte CS glitch or aborted clock, not a message attempt. Flashing a
    // 3 s PE03/PE04 glyph for what is line noise blanks a streaming display, so
    // treat runts as "ignore" (SPI_DIAG still counts them, so visibility is
    // retained). The genuine framing bug behind 'got = expected-1' rejects is
    // fixed in panel_spi_custom.cpp (drain RX FIFO after CS-high).
    bool is_runt = msg.num_bytes() < MESSAGE_MINIMUM_SIZE;
    if (!err_active && !is_runt) {
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

    // S1.3: arm the CIPO confirmation buffer per the plan's buffer-update rule.
    //  - Valid + COMM_CHECK passed:  arm {header, cmd, checksum}
    //  - Valid COMM_CHECK that byte-mismatched (comm_check_ok_ == false):
    //                                arm {header, 0xFF, 0x00}  (sentinel)
    //  - Any other invalidity:       do NOT touch the buffer (per spec)
    //  - During error-display window: do NOT touch the buffer (matches the
    //    spec rule for invalid messages; observable behavior matches "panel
    //    silently rejected the command", which is the truth)
    if (!err_active && parity_ok && length_ok && protocol_ok && cmd_ok) {
        uint8_t in_version = msg.header_byte() & 0b01111111;  // 0x01 (V1 only)
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
    // else: buffer stays as the previous valid confirmation OR the empty
    // sentinel (already loaded into the TX FIFO between transactions).

#if SPI_DIAG
    // SPI_DIAG: stay SILENT during streaming (no TX). React only to one-shot
    // input commands — 'd' dumps all counters in one burst, 'z' zeros the
    // window. Reading input does not transmit, so this adds no traffic to the
    // measurement window. NOTE: commands are serviced only between transactions,
    // so send 'd' WHILE the master is still streaming (slowly, e.g. 100 Hz) —
    // if the stream stops, update() blocks in panel_spi_read and won't see it.
    while (Serial.available()) {
        int c = Serial.read();
        if      (c == 'd') diag_dump();
        else if (c == 'z') diag_reset();
    }
#else
    // DEVEL serial heartbeat — NON-BLOCKING.
    // -----------------------------------------------------------
    // Emitted once per 1000 messages, but only when the USB-CDC TX FIFO has
    // room for the whole line. The old version did 11 blocking `Serial <<`
    // writes (~280 B); at 115200 baud a full host-side buffer could stall
    // core 0 for milliseconds *between* SPI transactions, so the panel wasn't
    // back in custom_spi_read_blocking() when the master started the next
    // transaction -> missed leading bytes -> num_rx undercount -> PE03. The
    // heartbeat is diagnostic and safely droppable: build one compact line and
    // skip it entirely if there isn't guaranteed room (so write() never blocks).
    if (msg_count_ % 1000 == 0) {
        char hb[192];
        int n = snprintf(hb, sizeof(hb),
            "HB msg=%llu par=%d len=%d proto=%d cmd=%d cc=%d "
            "qdrop=%llu fskip=%llu errd=%llu errs=%llu\r\n",
            (unsigned long long)msg_count_,
            (int)parity_ok, (int)length_ok, (int)protocol_ok, (int)cmd_ok,
            (int)comm_check_ok_,
            (unsigned long long)queue_drops_,
            (unsigned long long)display.frames_skipped(),
            (unsigned long long)error_displayed_count_,
            (unsigned long long)error_suppressed_count_);
        if (n > 0 && n < (int)sizeof(hb) && Serial.availableForWrite() >= n) {
            Serial.write(reinterpret_cast<const uint8_t *>(hb), (size_t)n);
        }
    }
    // -----------------------------------------------------------
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
    // refuse to display the requested glyph — defensive against host bugs.
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






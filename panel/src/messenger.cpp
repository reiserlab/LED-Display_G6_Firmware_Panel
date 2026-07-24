#include <hardware/spi.h>
#include <hardware/gpio.h>
#include <pico/time.h>
#include "constants.h"
#include "messenger.h"
#include "panel_spi_custom.h"
#include "isp.h"
#include "display_scan_twopio.h"
#include "predef_patterns.h"
#include "predef_patterns_table.h"
#include "psram_store.h"
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
// LAB-39 harness: ported from the polled spi-bringup-step0 path onto Frank's
// PL022+DMA path so reject rate can be measured with production-identical
// streaming timing. Counters are cheap in-RAM increments and there is NO
// serial output while streaming. 'd' dumps the whole picture in one burst;
// 'z' zeros the window. The 'sent vs received' denominator comes from the
// arena master's frames-sent counter (driven over its own serial port).
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

// V2 PSRAM-display reception accounting (LAB-41/42). A streamed 0..99 run
// should land cmds==total, oor==0, and distinct==FRAME_COUNT with reject_any==0.
uint32_t diag_psram_cmds = 0, diag_psram_oor = 0;
uint16_t diag_psram_idx_min = 0xFFFF, diag_psram_idx_max = 0;
uint32_t diag_psram_idx_hist[psram_store::FRAME_COUNT] = {0};

inline bool diag_is_psram_display_cmd(uint8_t cmd) {
    return (cmd >= CMD_ID_DISPLAY_PSRAM      && cmd <= CMD_ID_DISPLAY_PSRAM_GATED) ||
           (cmd >= CMD_ID_DISPLAY_PSRAM_DUTY && cmd <= CMD_ID_DISPLAY_PSRAM_DUTY_GATED);
}

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
    diag_psram_cmds = diag_psram_oor = 0;
    diag_psram_idx_min = 0xFFFF; diag_psram_idx_max = 0;
    for (uint16_t i = 0; i < psram_store::FRAME_COUNT; i++) diag_psram_idx_hist[i] = 0;
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
    // Currently-armed CIPO confirmation {header, cmd, crc} — what the panel
    // intends to clock back on the next transaction. Compare against the
    // master-side MISO capture (cipo_capture.py). It is static state, so
    // reading it here on an explicit 'd' adds no streaming-window traffic.
    // (PR #7 surfaced this in the streaming heartbeat that the timing-neutral
    // SPI_DIAG retired; folded into the dump to keep the observability.)
    {
        uint8_t tx3[3];
        panel_spi_debug_tx(tx3);
        diag_emit(b, snprintf(b, sizeof(b), "txCIPO= %02X %02X %02X\r\n",
                              tx3[0], tx3[1], tx3[2]));
    }
#if PANEL_REV == 31
    // v0.3.1 two-PIO scanner fault counter (should stay 0). Non-zero means a
    // per-row burst hit its completion-poll timeout and self-healed.
    diag_emit(b, snprintf(b, sizeof(b), "twopio_timeouts=%lu\r\n",
                          (unsigned long)twopio_get_timeouts()));
#endif
    // V2 PSRAM reception summary (only meaningful once V2 traffic has run).
    if (diag_psram_cmds || diag_psram_oor) {
        unsigned distinct = 0;
        for (uint16_t i = 0; i < psram_store::FRAME_COUNT; i++)
            if (diag_psram_idx_hist[i]) distinct++;
        diag_emit(b, snprintf(b, sizeof(b),
            "psram: cmds=%lu oor=%lu idx=[%u..%u] distinct=%u/%u\r\n",
            (unsigned long)diag_psram_cmds, (unsigned long)diag_psram_oor,
            (unsigned)(diag_psram_cmds ? diag_psram_idx_min : 0),
            (unsigned)diag_psram_idx_max, distinct, (unsigned)psram_store::FRAME_COUNT));
    }
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

// LAB-43 telemetry heartbeat: a compact one-line periodic dump of scan-timing
// + delivery metrics, emitted from the idle path (between arena bursts) so
// streaming reception stays production-identical. Lets us run this build as one
// panel in the live arena and correlate a timing anomaly with a visible glitch.
// Fields (all per-window unless noted): dmsg=frames this window, rate=frame/s,
// cmd=last opcode, duty=last GS2/GS16 duty_cycle, rej=reject delta, skip=drain-
// to-latest drops (delta,total), to=two-PIO self-heal timeouts (delta,total),
// scans=scan count, per_us/scan_us = period & burst avg/min/max (jitter).
bool     hb_enabled     = true;
uint32_t hb_interval_ms = 250;
uint32_t hb_last_ms     = 0;
uint32_t hb_last_msgs   = 0;
uint32_t hb_last_reject = 0;
uint32_t hb_last_to     = 0;
uint32_t hb_last_skip   = 0;
uint8_t  diag_last_cmd  = 0;
int      diag_last_duty = -1;

void diag_heartbeat() {
    if (!hb_enabled) return;
    uint32_t now = millis();
    if (hb_last_ms == 0) {                 // prime baseline, print nothing yet
        hb_last_ms = now; hb_last_msgs = diag_msgs; hb_last_reject = diag_reject_any;
        hb_last_skip = display.frames_skipped();
#if PANEL_REV == 31
        hb_last_to = twopio_get_timeouts();
        { uint32_t a,b2,c,d2; twopio_get_longrow(a,b2,c,d2); }  // clear window max
#endif
        display_reset_scan_stats();
        return;
    }
    uint32_t elapsed = now - hb_last_ms;
    if (elapsed < hb_interval_ms) return;

    uint32_t d_msgs = diag_msgs - hb_last_msgs;
    uint32_t d_rej  = diag_reject_any - hb_last_reject;
    uint32_t skip_t = display.frames_skipped();
    uint32_t d_skip = skip_t - hb_last_skip;
    uint32_t to_t   = 0, d_to = 0;
    uint32_t rmx_us = 0, rmx_row = 0, rall_us = 0, rall_row = 0;
#if PANEL_REV == 31
    to_t = twopio_get_timeouts(); d_to = to_t - hb_last_to;
    twopio_get_longrow(rmx_us, rmx_row, rall_us, rall_row);  // read-and-clear window max
#endif
    ScanStats ss; display_get_scan_stats(ss);
    uint32_t per_avg  = ss.count ? (uint32_t)(ss.period_us_total / ss.count) : 0;
    uint32_t scan_avg = ss.count ? (uint32_t)(ss.scan_us_total   / ss.count) : 0;
    uint32_t per_mn   = (ss.period_us_min == 0xFFFFFFFFu) ? 0 : ss.period_us_min;
    uint32_t scan_mn  = (ss.scan_us_min   == 0xFFFFFFFFu) ? 0 : ss.scan_us_min;
    uint32_t rate     = elapsed ? (uint32_t)((uint64_t)d_msgs * 1000u / elapsed) : 0;

    char b[320];
    diag_emit(b, snprintf(b, sizeof(b),
        "HB t=%lu win=%lu dmsg=%lu rate=%luHz cmd=0x%02X duty=%d rej=%lu "
        "skip=%lu/%lu to=%lu/%lu scans=%lu per_us=%lu/%lu/%lu scan_us=%lu/%lu/%lu "
        "rowmax=%luus@r%lu allmax=%luus@r%lu\r\n",
        (unsigned long)now, (unsigned long)elapsed, (unsigned long)d_msgs,
        (unsigned long)rate, (unsigned)diag_last_cmd, diag_last_duty,
        (unsigned long)d_rej, (unsigned long)d_skip, (unsigned long)skip_t,
        (unsigned long)d_to, (unsigned long)to_t, (unsigned long)ss.count,
        (unsigned long)per_avg, (unsigned long)per_mn, (unsigned long)ss.period_us_max,
        (unsigned long)scan_avg, (unsigned long)scan_mn, (unsigned long)ss.scan_us_max,
        (unsigned long)rmx_us, (unsigned long)rmx_row,
        (unsigned long)rall_us, (unsigned long)rall_row));

    // Live pin-level snapshot as its own SHORT line (diag_emit clamps writes
    // to ~159 bytes — gh-16 item 2 — so this cannot ride on the HB line).
    // Input synchronizers read the pad regardless of funcsel. Bit i of each
    // field is ROW_PIN[i]/COL_PIN[i]'s level, so the output is correct for
    // both revisions (v0.2.1's pins are neither contiguous nor 0-based).
    // Rows are active-LOW = ON; cols HIGH = ON.
    uint64_t gpins = gpio_get_all64();
    uint32_t pin_rows = 0, pin_cols = 0;
    for (int i = 0; i < PANEL_SIZE; i++) {
        pin_rows |= (uint32_t)((gpins >> ROW_PIN[i]) & 1u) << i;
        pin_cols |= (uint32_t)((gpins >> COL_PIN[i]) & 1u) << i;
    }
    char b2[64];
    diag_emit(b2, snprintf(b2, sizeof(b2), "PINS r=%05lX c=%05lX\r\n",
        (unsigned long)pin_rows, (unsigned long)pin_cols));

    hb_last_ms = now; hb_last_msgs = diag_msgs; hb_last_reject = diag_reject_any;
    hb_last_skip = skip_t; hb_last_to = to_t;
    display_reset_scan_stats();            // fresh min/max window
}

// Service one-shot host commands (input only, no TX): 'd' dump, 'z' zero,
// 'H' toggle heartbeat, 'm' timestamped marker (type when a glitch is seen).
void diag_service_commands() {
    while (Serial.available()) {
        int c = Serial.read();
        if      (c == 'd') diag_dump();
        else if (c == 'z') diag_reset();
        else if (c == 'H') { hb_enabled = !hb_enabled;
                             Serial.print("HB "); Serial.println(hb_enabled ? "ON" : "OFF"); }
        else if (c == 'm') { Serial.print("MARK t="); Serial.println((unsigned long)millis()); }
    }
    diag_heartbeat();
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

    // V2 (header 0x02/0x82) PSRAM display. All eight opcodes share one handler;
    // it derives mode from the low nibble and explicit-vs-implicit duty from the
    // high nibble (0x5x implicit / 0x6x explicit).
    cmd_umap_.insert( { CMD_ID_DISPLAY_PSRAM,
            [this](Message &msg){this -> on_cmd_display_psram(msg);} });
    cmd_umap_.insert( { CMD_ID_DISPLAY_PSRAM_PERSIST,
            [this](Message &msg){this -> on_cmd_display_psram(msg);} });
    cmd_umap_.insert( { CMD_ID_DISPLAY_PSRAM_TRIGGERED,
            [this](Message &msg){this -> on_cmd_display_psram(msg);} });
    cmd_umap_.insert( { CMD_ID_DISPLAY_PSRAM_GATED,
            [this](Message &msg){this -> on_cmd_display_psram(msg);} });
    cmd_umap_.insert( { CMD_ID_DISPLAY_PSRAM_DUTY,
            [this](Message &msg){this -> on_cmd_display_psram(msg);} });
    cmd_umap_.insert( { CMD_ID_DISPLAY_PSRAM_DUTY_PERSIST,
            [this](Message &msg){this -> on_cmd_display_psram(msg);} });
    cmd_umap_.insert( { CMD_ID_DISPLAY_PSRAM_DUTY_TRIGGERED,
            [this](Message &msg){this -> on_cmd_display_psram(msg);} });
    cmd_umap_.insert( { CMD_ID_DISPLAY_PSRAM_DUTY_GATED,
            [this](Message &msg){this -> on_cmd_display_psram(msg);} });

    // ISP (in-system programming) opcodes 0xE4–0xE9 — dispatched to the ISP
    // receiver (isp.cpp), which arms an extended multi-byte CIPO reply driven on
    // the next read transaction (see update() / panel_spi_drive_response).
    for (uint8_t isp_cmd : { (uint8_t)CMD_ID_ISP_ENTER,
                             (uint8_t)CMD_ID_ISP_WRITE_PAGE,
                             (uint8_t)CMD_ID_ISP_VERIFY_STAGED,
                             (uint8_t)CMD_ID_ISP_COMMIT,
                             (uint8_t)CMD_ID_ISP_VERIFY_CRC,
                             (uint8_t)CMD_ID_ISP_EXIT_REBOOT }) {
        cmd_umap_.insert( { isp_cmd, [](Message &msg){ Isp::handle(msg); } });
    }

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
    // Park the bus in its idle state while the controller is absent or still
    // booting (its pins are high-Z until SpiManager::begin()): CS pulled HIGH
    // so the PL022 ignores everything, SCK pulled HIGH to its MODE3 idle
    // level. Floating lines otherwise drift across thresholds and clock junk
    // "transactions" that raise PE02/PE03 glyphs at arena power-on.
    gpio_pull_up(SPI_CS_PIN);
    gpio_pull_up(SPI_SCK_PIN);
    spi_set_slave(SPI_INST, true);
    spi_set_format(SPI_INST, 8, SPI_CPOL_1, SPI_CPHA_1, SPI_MSB_FIRST);

    // S1.3: prime the CIPO confirmation buffer + TX FIFO with the empty-buffer
    // sentinel BEFORE the first CS falling edge from the controller.
    panel_spi_clear_confirmation();

    // Reserve the ISP PSRAM staging buffer up front so ISP_ENTER is a fast
    // handshake (no pmalloc stall in the phase-A → phase-B window). See isp.cpp.
    Isp::init();
}

void Messenger::update() {

    static Message msg;

    // ISP extended-reply phase: a prior ISP command armed a multi-byte CIPO
    // reply. Drive it on this transaction (and run a due flash commit after),
    // instead of reading a new command.
    if (Isp::response_pending()) {
        Isp::service_pending();
        return;
    }

    panel_spi_read(msg);

    // Zero-byte read: a CS envelope with no clocked bytes. This is not a
    // message, so return without counting it or raising PE02/PE03. It happens
    // legitimately all the time: the controller's CS lines each gate TWO
    // panels (one per SPI bus) and single-panel ops like ISP clock only the
    // target's bus, so the bus-paired panel sees ~1100 clockless CS pulses
    // per flash; controller boot adds a brief CS-low init pulse. Real data
    // loss is still caught controller-side (page CRC + CIPO confirmation).
    // In SPI_DIAG builds this is also the CS-idle timeout path (master
    // between bursts): service the one-shot 'd'/'z' serial commands here so
    // they work between bursts without polluting the measurement window.
    if (msg.num_bytes() == 0) {
#if SPI_DIAG
        diag_service_commands();
#endif
        return;
    }
    msg_count_ += 1;

    // Reset the COMM_CHECK byte-validation flag each message so it can't carry
    // across non-COMM_CHECK messages.
    comm_check_ok_ = true;

    // Snapshot the error-display flag once so dispatch, CIPO arming, and the
    // error-raise decision all see a consistent view.
    bool err_active = Display::error_display_active;

    uint8_t cmd_id   = msg.command_byte();
    bool parity_ok   = msg.check_parity();
    bool length_ok   = msg.check_length();
    // Version gate is per-command: V1 opcodes require a V1 header, V2 PSRAM
    // opcodes require a V2 header (command_protocol_version()). A V2 opcode
    // under a V1 header (or vice-versa) fails here and raises PE04.
    bool protocol_ok = msg.check_protocol(command_protocol_version(cmd_id));
    bool cmd_ok      = false;
    if (parity_ok && length_ok && protocol_ok && !err_active) {
        if (cmd_umap_.count(cmd_id) > 0) {
            cmd_umap_.at(cmd_id)(msg);
            cmd_ok = true;
        }
    }

#if SPI_DIAG
    // Silent per-message accounting. Count each failing check INDEPENDENTLY (a
    // dropped-bytes frame fails both parity and length — record both, not just
    // the first-wins gate the error-raise below picks).
    {
        diag_msgs++;
        diag_cmd_hist[cmd_id]++;
        // LAB-43: remember what content is on the panel so the heartbeat can
        // correlate a glitch with the current mode/gray/duty. duty_cycle is the
        // last payload byte: GS2 (0x10-0x13) at index 50, GS16 (0x30-0x33) at
        // index 200. Only read on a length-valid frame.
        diag_last_cmd = cmd_id;
        if (length_ok) {
            if (cmd_id >= 0x10 && cmd_id <= 0x13)      diag_last_duty = msg.payload_at(50);
            else if (cmd_id >= 0x30 && cmd_id <= 0x33) diag_last_duty = msg.payload_at(200);
        }
        uint8_t flags = 0;
        if (!parity_ok)   { diag_parity_fail++;   flags |= 1; }
        if (!length_ok)   { diag_length_fail++;   flags |= 2; }
        if (!protocol_ok) { diag_protocol_fail++; flags |= 4; }
        if (parity_ok && length_ok && protocol_ok && cmd_umap_.count(cmd_id) == 0) {
            diag_unknown_cmd++; flags |= 8;
        }
        if (flags) { diag_reject_any++; diag_record(cmd_id, (uint16_t)msg.num_bytes(), flags); }

        // V2 PSRAM index accounting — only for well-formed (wire-valid) frames.
        // Out-of-range indices are an application-level reject (PE05), tracked
        // separately from reject_any so a clean 0..99 run reads oor=0.
        if (flags == 0 && diag_is_psram_display_cmd(cmd_id)) {
            uint16_t idx = (uint16_t)msg.payload_at(0)
                         | ((uint16_t)msg.payload_at(1) << 8);
            diag_psram_cmds++;
            if (idx < psram_store::FRAME_COUNT) {
                diag_psram_idx_hist[idx]++;
                if (idx < diag_psram_idx_min) diag_psram_idx_min = idx;
                if (idx > diag_psram_idx_max) diag_psram_idx_max = idx;
            } else {
                diag_psram_oor++;
            }
        }
    }
#endif

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

    // An ISP command armed an extended (multi-byte) CIPO reply, which is driven
    // on the next read transaction by Isp::service_pending(). Skip the normal
    // 3-byte confirmation arming below so it doesn't clobber the ISP reply.
    if (Isp::response_pending()) return;

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

    // First content command retires the ISP boot-indicator flag (the
    // post-flash smiley; see retires_boot_indicator in isp_logic.h for what
    // counts). Placed AFTER the confirmation arming above: the one-time
    // retire is a LittleFS flash erase (tens of ms, core 1 parked), so it
    // must not delay this transaction's CIPO reply. It can still stall the
    // NEXT transaction; after an ISP flash, send a throwaway display command
    // before starting a stream.
    if (cmd_ok && Isp::retires_boot_indicator(cmd_id)) {
        Isp::notify_host_command();
    }

#if SPI_DIAG
    // Stay SILENT during streaming (no TX). Service the one-shot 'd' (dump) /
    // 'z' (zero) host commands between transactions — reading input adds no SPI
    // traffic, so the measurement window's timing is production-identical.
    diag_service_commands();
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


void Messenger::on_cmd_display_psram(Message &msg) {
    // V2 PSRAM display. Payload is a 16-bit LE frame index; the 0x6x variants
    // append an explicit duty_cycle byte. Mode comes from the command's low
    // nibble (0 Oneshot / 1 Persistent / 2 Triggered / 3 Gated). The frame is
    // read from PSRAM, decoded into pat_, and queued for core 1 — identical
    // downstream path to a V1 live Gray_16 frame.
    uint8_t cmd = msg.command_byte();
    DisplayMode mode = (DisplayMode)(cmd & 0x0F);

    uint16_t index = (uint16_t)msg.payload_at(0)
                   | ((uint16_t)msg.payload_at(1) << 8);

    int duty_override = -1;
    if ((cmd & 0xF0) == 0x60) {
        duty_override = (int)msg.payload_at(2);
    }

    if (!psram_store::load(index, pat_, mode, duty_override)) {
        // Out-of-range index / store unavailable → invalid-index panel error.
        raise_error(PREDEF_SLOT_PE05);
        return;
    }
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

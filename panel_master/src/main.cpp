// =============================================================================
// G6 panel firmware — panel-as-master bench harness (Stage 1)
// =============================================================================
//
// This firmware turns a G6 panel into a fake controller (SPI master). It emits
// V1 wire-protocol frames to a second panel running the `panel/` slave firmware
// and reads back the 3-byte CIPO confirmation, printing both to USB serial.
//
// Wiring (see panel_master/README.md for the pre-flight checklist):
//   - Bottom J2 header on master  ↔  bottom J2 header on slave
//   - Pins: MISO, MOSI, SCK, GND (NOT +5V — use independent USB power on each)
//   - CS (J3 pin 5) on master  →  CS (J3 pin 5) on slave
//
// SPI peripheral + pins per panel rev:
//   v0.2.1: SPI0 on GP32(MOSI), GP33(CS), GP34(SCK), GP35(MISO)
//   v0.3.1: SPI1 on GP40(MOSI), GP41(CS), GP42(SCK), GP43(MISO)
//
// Test sequence (1 Hz cadence):
//   1. COMM_CHECK with canonical payload 0..199
//   2. Gray_2 row+column cross pattern, stretch=192
//   3. Gray_16 gradient pattern, stretch=128
//   4. Stretch sweep 0→255 on a Gray_16 frame (~20 steps over 20s)
//   5. COMM_CHECK with one byte deliberately flipped (validation should fail)
//   6. Truncated frame (header + cmd, no payload — length check should fail)
//   7. Parity-corrupted frame (flipped payload byte, parity NOT recomputed)
// =============================================================================

#include <Arduino.h>
#include <Streaming.h>
#include <hardware/spi.h>
#include <hardware/gpio.h>
#include "constants.h"
#include "message.h"
#include "pattern.h"
#include "protocol.h"

// Start at 1 MHz — well below the marginal hardware ceiling (target 25 MHz, max
// 30 MHz). Bump after the harness passes once at 1 MHz.
static constexpr uint32_t MASTER_SPI_HZ = 1'000'000;

static void cs_high() { gpio_put(SPI_CS_PIN, 1); }
static void cs_low()  { gpio_put(SPI_CS_PIN, 0); }

// One SPI transaction: drive CS LOW, exchange `len` bytes via the SPI block
// (write `tx` and capture `rx`), drive CS HIGH. Caller owns `rx` buffer.
static void spi_xfer(const uint8_t *tx, uint8_t *rx, size_t len) {
    cs_low();
    delayMicroseconds(5);   // CS-setup time (slave parser reset)
    spi_write_read_blocking(SPI_INST, tx, rx, len);
    delayMicroseconds(5);
    cs_high();
    delayMicroseconds(10);  // inter-transaction idle time
}

// Print the first 3 RX bytes (the CIPO confirmation slot) hex-formatted.
static void print_cipo(const char *label, const uint8_t *rx) {
    Serial.print(label);
    Serial.print(" CIPO: [0x");
    if (rx[0] < 0x10) Serial.print('0');
    Serial.print(rx[0], HEX);
    Serial.print(" 0x");
    if (rx[1] < 0x10) Serial.print('0');
    Serial.print(rx[1], HEX);
    Serial.print(" 0x");
    if (rx[2] < 0x10) Serial.print('0');
    Serial.print(rx[2], HEX);
    Serial.println("]");
}


void setup() {
    Serial.begin(115200);
    delay(2000);  // give USB serial a moment to enumerate
    Serial.println("=== G6 panel_master bench harness ===");
    Serial.print("PANEL_REV: ");  Serial.println(PANEL_REV);
    Serial.print("SPI clock: ");  Serial.print(MASTER_SPI_HZ / 1'000'000); Serial.println(" MHz");

    // SPI master setup. Same pins as the slave build (SPI_INST is configured
    // per-PANEL_REV in constants.cpp), but spi_set_slave(false) and driving
    // CS manually as a GPIO so we control transaction boundaries explicitly.
    spi_init(SPI_INST, MASTER_SPI_HZ);
    gpio_set_function(SPI_SCK_PIN,  GPIO_FUNC_SPI);
    gpio_set_function(SPI_MOSI_PIN, GPIO_FUNC_SPI);
    gpio_set_function(SPI_MISO_PIN, GPIO_FUNC_SPI);
    // CS is a regular GPIO output, NOT under SPI peripheral control.
    gpio_init(SPI_CS_PIN);
    gpio_set_function(SPI_CS_PIN, GPIO_FUNC_SIO);
    gpio_set_dir(SPI_CS_PIN, GPIO_OUT);
    cs_high();
    spi_set_format(SPI_INST, 8, SPI_CPOL_1, SPI_CPHA_1, SPI_MSB_FIRST);
    spi_set_slave(SPI_INST, false);

    // 1-second startup delay so the slave panel completes its own boot +
    // FIFO prime before our first transaction.
    delay(1000);
    Serial.println("setup() done; entering test loop");
}


// Helper: build a Gray_2 row+column cross pattern and serialize it.
// `protocol_version` selects 0x01 (V1 Oneshot, cmd 0x10) or 0x03 (V3 Persistent, cmd 0x13).
static void build_gray_2_cross(uint8_t *out, size_t out_len, uint8_t stretch,
                               uint8_t protocol_version, uint8_t cmd_id) {
    Pattern pat;
    pat.set_gray_level(GrayLevel::Gray_2);
    pat.set_stretch(stretch);
    // Cross through center: row 10 and column 10 all ON
    for (size_t k = 0; k < PANEL_SIZE; k++) {
        pat.matrix()(10, k) = 1;
        pat.matrix()(k, 10) = 1;
    }
    Message msg;
    msg.from_pattern(pat, protocol_version);
    // Override cmd byte if caller wants the V3 Persistent opcode (0x13)
    // instead of V1 Oneshot (0x10). from_pattern() sets it to 0x10 by
    // default; we patch byte 1 and recompute parity.
    if (cmd_id != CMD_ID_DISPLAY_GRAY_2) {
        msg.set_command_byte(cmd_id);
        msg.set_parity_bit();
    }
    for (size_t i = 0; i < msg.num_bytes() && i < out_len; i++) {
        out[i] = msg.data_ptr()[i];
    }
}

// Helper: build a Gray_16 gradient pattern (intensity = col index ramps 0..15).
// `protocol_version` selects 0x01 (V1) or 0x03 (V3); `cmd_id` selects 0x30
// (Oneshot) or 0x33 (Persistent).
static void build_gray_16_gradient(uint8_t *out, size_t out_len, uint8_t stretch,
                                   uint8_t protocol_version, uint8_t cmd_id) {
    Pattern pat;
    pat.set_gray_level(GrayLevel::Gray_16);
    pat.set_stretch(stretch);
    for (size_t i = 0; i < PANEL_SIZE; i++) {
        for (size_t j = 0; j < PANEL_SIZE; j++) {
            pat.matrix()(i, j) = uint8_t(j * 15 / (PANEL_SIZE - 1));  // 0..15
        }
    }
    Message msg;
    msg.from_pattern(pat, protocol_version);
    if (cmd_id != CMD_ID_DISPLAY_GRAY_16) {
        msg.set_command_byte(cmd_id);
        msg.set_parity_bit();
    }
    for (size_t i = 0; i < msg.num_bytes() && i < out_len; i++) {
        out[i] = msg.data_ptr()[i];
    }
}

// Helper: build the canonical COMM_CHECK message (0..199 payload).
static void build_comm_check(uint8_t *out, size_t out_len) {
    Message msg;
    msg.to_comms_check();
    for (size_t i = 0; i < msg.num_bytes() && i < out_len; i++) {
        out[i] = msg.data_ptr()[i];
    }
}


void loop() {
    static uint32_t iter = 0;
    static uint8_t  stretch_sweep = 0;
    iter++;

    // -- Step 1: COMM_CHECK -------------------------------------------------
    {
        uint8_t tx[HEADER_SIZE + PAYLOAD_COMMS_CHECK] = {0};
        uint8_t rx[HEADER_SIZE + PAYLOAD_COMMS_CHECK] = {0};
        build_comm_check(tx, sizeof(tx));
        spi_xfer(tx, rx, sizeof(tx));
        Serial.print("[iter ");
        Serial.print(iter);
        Serial.print("] COMM_CHECK -> ");
        print_cipo("", rx);
    }

    // -- Step 2: Gray_2 cross PERSISTENT (V3 0x13), stretch=192 -------------
    // Persistent: one send → display stays visible until next message. Easy
    // to verify visually at 1 Hz cadence (steps 2-4 each replace the prior
    // pattern, so the panel cycles through them).
    {
        uint8_t tx[HEADER_SIZE + PAYLOAD_DISPLAY_GRAY_2] = {0};
        uint8_t rx[HEADER_SIZE + PAYLOAD_DISPLAY_GRAY_2] = {0};
        build_gray_2_cross(tx, sizeof(tx), 192,
                           CMD_PROTOCOL_V3, CMD_ID_DISPLAY_GRAY_2_PERSIST);
        spi_xfer(tx, rx, sizeof(tx));
        print_cipo("Gray_2 cross PERSIST s=192", rx);
    }

    // -- Step 3: Gray_16 gradient PERSISTENT (V3 0x33), stretch=128 ---------
    {
        uint8_t tx[HEADER_SIZE + PAYLOAD_DISPLAY_GRAY_16] = {0};
        uint8_t rx[HEADER_SIZE + PAYLOAD_DISPLAY_GRAY_16] = {0};
        build_gray_16_gradient(tx, sizeof(tx), 128,
                               CMD_PROTOCOL_V3, CMD_ID_DISPLAY_GRAY_16_PERSIST);
        spi_xfer(tx, rx, sizeof(tx));
        print_cipo("Gray_16 gradient PERSIST s=128", rx);
    }

    // -- Step 4: stretch sweep (Persistent gradient, one step per iter) ----
    {
        uint8_t tx[HEADER_SIZE + PAYLOAD_DISPLAY_GRAY_16] = {0};
        uint8_t rx[HEADER_SIZE + PAYLOAD_DISPLAY_GRAY_16] = {0};
        build_gray_16_gradient(tx, sizeof(tx), stretch_sweep,
                               CMD_PROTOCOL_V3, CMD_ID_DISPLAY_GRAY_16_PERSIST);
        spi_xfer(tx, rx, sizeof(tx));
        Serial.print("Gray_16 sweep stretch=");
        Serial.print(stretch_sweep);
        Serial.print(" -> ");
        print_cipo("", rx);
        stretch_sweep = (stretch_sweep + 13) & 0xFF;  // ~20 steps over 0..255
    }

    // -- Step 4b: V1 ONESHOT burst (0x10 streamed at ~500 Hz for 1 second) --
    // Oneshot semantics: panel displays for one scan, then idles. Without
    // streaming, the pattern would just flash once. We send the Gray_2 cross
    // 500 times in quick succession; if Oneshot is working, the slave should
    // show the cross continuously during this burst, then go dark after.
    {
        uint8_t tx[HEADER_SIZE + PAYLOAD_DISPLAY_GRAY_2] = {0};
        uint8_t rx[HEADER_SIZE + PAYLOAD_DISPLAY_GRAY_2] = {0};
        build_gray_2_cross(tx, sizeof(tx), 192,
                           CMD_PROTOCOL_V1, CMD_ID_DISPLAY_GRAY_2);
        Serial.println("V1 Oneshot burst: 500x at ~2ms cadence");
        for (int i = 0; i < 500; i++) {
            spi_xfer(tx, rx, sizeof(tx));
            // 2 ms gap = ~500 Hz "frame rate" from the master side
            delayMicroseconds(1500);
        }
        print_cipo("Oneshot last CIPO", rx);
        // After this burst the slave should go dark (Oneshot timed out;
        // queue_drops_ may be nonzero in slave serial — many of the 500
        // frames will land while a previous frame is still on the queue).
        delay(1500);  // observe the slave go dark
    }

    // -- Step 5: COMM_CHECK with deliberate byte-flip ----------------------
    {
        uint8_t tx[HEADER_SIZE + PAYLOAD_COMMS_CHECK] = {0};
        uint8_t rx[HEADER_SIZE + PAYLOAD_COMMS_CHECK] = {0};
        build_comm_check(tx, sizeof(tx));
        tx[HEADER_SIZE + 5] ^= 0xAA;   // flip a payload byte; parity will no longer match
        // We must recompute parity so it's still a valid V1 message at the
        // wire layer — otherwise we'd be testing parity_ok=false, not COMM_CHECK fail.
        // Build a Message wrapper, rewrite the byte, recompute parity.
        Message msg;
        msg.to_comms_check();
        msg.data_ptr()[HEADER_SIZE + 5] = uint8_t(5) ^ 0xAA;   // canonical[5]=5, flipped
        msg.set_parity_bit();
        // Copy back into tx
        for (size_t i = 0; i < msg.num_bytes(); i++) tx[i] = msg.data_ptr()[i];
        spi_xfer(tx, rx, sizeof(tx));
        // Next COMM_CHECK's CIPO carries the previous (failed) confirmation.
        // We send another COMM_CHECK to retrieve it.
        uint8_t tx2[HEADER_SIZE + PAYLOAD_COMMS_CHECK] = {0};
        uint8_t rx2[HEADER_SIZE + PAYLOAD_COMMS_CHECK] = {0};
        build_comm_check(tx2, sizeof(tx2));
        spi_xfer(tx2, rx2, sizeof(tx2));
        Serial.print("COMM_CHECK fail probe -> ");
        print_cipo("", rx2);  // expect {parity|0x01, 0xFF, 0x00}
    }

    // -- Step 6: truncated frame (header+cmd only, length check should fail) --
    {
        uint8_t tx[3] = { 0x01, CMD_ID_DISPLAY_GRAY_2, 0x00 };  // 3 bytes, not 53
        uint8_t rx[3] = {0};
        spi_xfer(tx, rx, sizeof(tx));
        Serial.print("Truncated frame -> ");
        print_cipo("", rx);  // CIPO buffer unchanged from prev valid
    }

    // -- Step 7: parity-corrupted valid-length frame -------------------------
    {
        uint8_t tx[HEADER_SIZE + PAYLOAD_DISPLAY_GRAY_2] = {0};
        uint8_t rx[HEADER_SIZE + PAYLOAD_DISPLAY_GRAY_2] = {0};
        build_gray_2_cross(tx, sizeof(tx), 255,
                           CMD_PROTOCOL_V1, CMD_ID_DISPLAY_GRAY_2);
        // Flip a payload bit WITHOUT recomputing parity
        tx[HEADER_SIZE + 2] ^= 0x01;
        spi_xfer(tx, rx, sizeof(tx));
        Serial.print("Parity-corrupt frame -> ");
        print_cipo("", rx);  // CIPO buffer unchanged
    }

    Serial.println("--- iteration done ---");
    Serial.println();
    delay(1000);  // 1 Hz cadence
}

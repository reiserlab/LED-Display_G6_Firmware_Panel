// =============================================================================
// G6 panel firmware — panel-as-controller bench harness (Stage 1)
// =============================================================================
//
// This firmware turns a G6 panel into a stand-in SPI bus controller. It emits
// V1 wire-protocol frames to a second panel running the `panel/` peripheral
// firmware and reads back the 3-byte CIPO confirmation, printing both to USB
// serial.
//
// IMPORTANT WIRING NOTE — bitbang controller (not hardware SPI):
//   The panel's PCB hardwires GP32 to the J2 pin labeled "MOSI" (controller
//   perspective) and GP35 to the J2 "MISO" pin. The RP2350 hardware SPI
//   peripheral fixes pin roles: GP32 = spi0 RX (input), GP35 = spi0 TX
//   (output). That's correct for peripheral-role operation, but in controller
//   mode the SPI block would want GP32 as input-from-MISO and GP35 as
//   output-to-MOSI — opposite of how the PCB is wired. Using hw-SPI on the
//   controller would require a crossover cable.
//
//   To avoid the crossover-cable requirement, this firmware bitbangs SPI
//   in software on plain GPIOs. Pin roles can then be assigned freely:
//     MOSI_OUT = GP32  (drives the J2 "MOSI" wire to peripheral's GP32 RX)
//     MISO_IN  = GP35  (reads the J2 "MISO" wire from peripheral's GP35 TX)
//     SCK_OUT  = GP34  (drives the J2 "SCK" wire to peripheral's GP34 SCK)
//     CS_OUT   = GP33  (drives the J3 pin 5 CS wire to peripheral's GP33 CSn)
//   A straight-through J2↔J2 cable + J3pin5↔J3pin5 jumper works.
//
//   The peripheral panel still uses its hardware SPI block; spec-compliant SPI mode 3.
//
// Test sequence (1 Hz cadence):
//   1. COMM_CHECK with canonical payload 0..199
//   2. Gray_2 cross pattern (V1 Persistent 0x11), duty_cycle=192
//   3. Gray_16 gradient pattern (V1 Persistent 0x31), duty_cycle=128
//   4. Duty cycle sweep on Gray_16 Persistent
//   4b. V1 Oneshot burst (500 frames at ~500 Hz)
//   5. COMM_CHECK with one byte deliberately flipped (validation should fail)
//   6. Truncated frame (length check should fail)
//   7. Parity-corrupted frame
// =============================================================================

#include <Arduino.h>
#include <Streaming.h>
#include <hardware/gpio.h>
#include "constants.h"
#include "message.h"
#include "pattern.h"
#include "protocol.h"

// Start at ~250 kHz — easy on the bitbang loop, well within peripheral hw-SPI spec.
// Per-bit delay = roughly 2 µs total per bit (1 µs per half-period). The peripheral's
// hw-SPI peripheral handles this comfortably; spec target is 25 MHz so we have
// 100x margin for bench-test.
static constexpr uint32_t BITBANG_HALF_PERIOD_US = 2;  // -> ~250 kHz bit rate

// Pin assignments (controller perspective). With bitbang we are NOT bound by
// hardware-SPI funcsel pin roles, so we can map outputs to the GPIOs whose
// PCB traces go to the "MOSI" wire and "SCK" wire, and configure GP35 as an
// input to read the peripheral's MISO drive. See constants.cpp for the per-rev
// values of these macros (v0.2.1: 32/35/34/33; v0.3.1: 40/43/42/41).
#define CONTROLLER_MOSI_OUT_PIN  SPI_MOSI_PIN  // GP32 on v0.2.1
#define CONTROLLER_MISO_IN_PIN   SPI_MISO_PIN  // GP35 on v0.2.1
#define CONTROLLER_SCK_OUT_PIN   SPI_SCK_PIN   // GP34 on v0.2.1
#define CONTROLLER_CS_OUT_PIN    SPI_CS_PIN    // GP33 on v0.2.1

static inline void cs_high() { gpio_put(CONTROLLER_CS_OUT_PIN,  1); }
static inline void cs_low()  { gpio_put(CONTROLLER_CS_OUT_PIN,  0); }
static inline void sck_high(){ gpio_put(CONTROLLER_SCK_OUT_PIN, 1); }
static inline void sck_low() { gpio_put(CONTROLLER_SCK_OUT_PIN, 0); }

// SPI Mode 3 (CPOL=1, CPHA=1, MSB-first) bitbang.
// Idle: SCK = HIGH. Each bit:
//   - SCK falls (leading edge); controller drives MOSI bit
//   - half period
//   - SCK rises (trailing edge); controller samples MISO
//   - half period
static inline uint8_t bitbang_xfer_byte(uint8_t tx_byte) {
    uint8_t rx_byte = 0;
    for (int bit = 7; bit >= 0; bit--) {
        // Drive MOSI (controller shifts on first edge of cycle)
        gpio_put(CONTROLLER_MOSI_OUT_PIN, (tx_byte >> bit) & 1u);
        // First edge: SCK HIGH -> LOW
        sck_low();
        busy_wait_us(BITBANG_HALF_PERIOD_US);
        // Second edge: SCK LOW -> HIGH; both sides sample now
        sck_high();
        if (gpio_get(CONTROLLER_MISO_IN_PIN)) {
            rx_byte |= (1u << bit);
        }
        busy_wait_us(BITBANG_HALF_PERIOD_US);
    }
    return rx_byte;
}

// One SPI transaction: assert CS, exchange `len` bytes, deassert CS.
static void spi_xfer(const uint8_t *tx, uint8_t *rx, size_t len) {
    // CS setup
    cs_low();
    busy_wait_us(5);          // CS-setup time (peripheral parser reset)
    for (size_t i = 0; i < len; i++) {
        rx[i] = bitbang_xfer_byte(tx[i]);
    }
    busy_wait_us(5);
    cs_high();
    busy_wait_us(10);         // inter-transaction idle
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

    // Configure bitbang GPIOs (all SIO function; no hardware SPI peripheral).
    // Outputs: MOSI, SCK, CS — driven by the controller.
    // Input:   MISO — driven by the peripheral's hw-SPI TX pin.
    gpio_init(CONTROLLER_MOSI_OUT_PIN);
    gpio_init(CONTROLLER_SCK_OUT_PIN);
    gpio_init(CONTROLLER_CS_OUT_PIN);
    gpio_init(CONTROLLER_MISO_IN_PIN);
    gpio_set_function(CONTROLLER_MOSI_OUT_PIN, GPIO_FUNC_SIO);
    gpio_set_function(CONTROLLER_SCK_OUT_PIN,  GPIO_FUNC_SIO);
    gpio_set_function(CONTROLLER_CS_OUT_PIN,   GPIO_FUNC_SIO);
    gpio_set_function(CONTROLLER_MISO_IN_PIN,  GPIO_FUNC_SIO);
    gpio_set_dir(CONTROLLER_MOSI_OUT_PIN, GPIO_OUT);
    gpio_set_dir(CONTROLLER_SCK_OUT_PIN,  GPIO_OUT);
    gpio_set_dir(CONTROLLER_CS_OUT_PIN,   GPIO_OUT);
    gpio_set_dir(CONTROLLER_MISO_IN_PIN,  GPIO_IN);
    // Idle states: SCK = HIGH (CPOL=1), CS = HIGH (deasserted), MOSI = 0.
    sck_high();
    cs_high();
    gpio_put(CONTROLLER_MOSI_OUT_PIN, 0);

    // Banner — printed AFTER GPIO setup so the GPIO init can't stall pre-USB.
    delay(2000);                      // give USB CDC time to enumerate
    Serial.println("=== G6 panel_controller bench harness (bitbang SPI) ===");
    Serial.print("PANEL_REV: ");      Serial.println(PANEL_REV);
    Serial.print("MOSI->peripheral on GP");  Serial.println(CONTROLLER_MOSI_OUT_PIN);
    Serial.print("MISO<-peripheral on GP");  Serial.println(CONTROLLER_MISO_IN_PIN);
    Serial.print("SCK  on GP");         Serial.println(CONTROLLER_SCK_OUT_PIN);
    Serial.print("CS   on GP");         Serial.println(CONTROLLER_CS_OUT_PIN);
    Serial.print("bit half-period: "); Serial.print(BITBANG_HALF_PERIOD_US); Serial.println(" us");

    // Startup delay so the peripheral panel completes boot + FIFO prime before
    // our first transaction.
    delay(1000);
    Serial.println("setup() done; entering test loop");
}


// Helper: build a Gray_2 row+column cross pattern and serialize it.
// `protocol_version` selects which header byte (V1 = 0x01 only for now).
// `cmd_id` selects mode: 0x10 (Oneshot) or 0x11 (Persistent).
static void build_gray_2_cross(uint8_t *out, size_t out_len, uint8_t duty_cycle,
                               uint8_t protocol_version, uint8_t cmd_id) {
    Pattern pat;
    pat.set_gray_level(GrayLevel::Gray_2);
    pat.set_duty_cycle(duty_cycle);
    // Cross through center: row 10 and column 10 all ON
    for (size_t k = 0; k < PANEL_SIZE; k++) {
        pat.matrix()(10, k) = 1;
        pat.matrix()(k, 10) = 1;
    }
    Message msg;
    msg.from_pattern(pat, protocol_version);
    // Override cmd byte if caller wants the V1 Persistent opcode (0x11)
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
// `protocol_version` selects header byte; `cmd_id` selects 0x30 (Oneshot) or
// 0x31 (Persistent).
static void build_gray_16_gradient(uint8_t *out, size_t out_len, uint8_t duty_cycle,
                                   uint8_t protocol_version, uint8_t cmd_id) {
    Pattern pat;
    pat.set_gray_level(GrayLevel::Gray_16);
    pat.set_duty_cycle(duty_cycle);
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
    static uint8_t  duty_cycle_sweep = 0;
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

    // -- Step 2: Gray_2 cross V1 PERSISTENT (0x11), duty_cycle=192 --------------
    // Persistent: one send → display stays visible until next message. Easy
    // to verify visually at 1 Hz cadence (steps 2-4 each replace the prior
    // pattern, so the panel cycles through them).
    {
        uint8_t tx[HEADER_SIZE + PAYLOAD_DISPLAY_GRAY_2] = {0};
        uint8_t rx[HEADER_SIZE + PAYLOAD_DISPLAY_GRAY_2] = {0};
        build_gray_2_cross(tx, sizeof(tx), 192,
                           CMD_PROTOCOL_V1, CMD_ID_DISPLAY_GRAY_2_PERSIST);
        spi_xfer(tx, rx, sizeof(tx));
        print_cipo("Gray_2 cross PERSIST s=192", rx);
    }

    // -- Step 3: Gray_16 gradient V1 PERSISTENT (0x31), duty_cycle=128 ---------
    {
        uint8_t tx[HEADER_SIZE + PAYLOAD_DISPLAY_GRAY_16] = {0};
        uint8_t rx[HEADER_SIZE + PAYLOAD_DISPLAY_GRAY_16] = {0};
        build_gray_16_gradient(tx, sizeof(tx), 128,
                               CMD_PROTOCOL_V1, CMD_ID_DISPLAY_GRAY_16_PERSIST);
        spi_xfer(tx, rx, sizeof(tx));
        print_cipo("Gray_16 gradient PERSIST s=128", rx);
    }

    // -- Step 4: duty_cycle sweep (V1 Persistent gradient, one step per iter) --
    {
        uint8_t tx[HEADER_SIZE + PAYLOAD_DISPLAY_GRAY_16] = {0};
        uint8_t rx[HEADER_SIZE + PAYLOAD_DISPLAY_GRAY_16] = {0};
        build_gray_16_gradient(tx, sizeof(tx), duty_cycle_sweep,
                               CMD_PROTOCOL_V1, CMD_ID_DISPLAY_GRAY_16_PERSIST);
        spi_xfer(tx, rx, sizeof(tx));
        Serial.print("Gray_16 sweep duty_cycle=");
        Serial.print(duty_cycle_sweep);
        Serial.print(" -> ");
        print_cipo("", rx);
        duty_cycle_sweep = (duty_cycle_sweep + 13) & 0xFF;  // ~20 steps over 0..255
    }

    // -- Step 4b: V1 ONESHOT burst (0x10 streamed at ~500 Hz for 1 second) --
    // Oneshot semantics: panel displays for one scan, then idles. Without
    // streaming, the pattern would just flash once. We send the Gray_2 cross
    // 500 times in quick succession; if Oneshot is working, the peripheral should
    // show the cross continuously during this burst, then go dark after.
    {
        uint8_t tx[HEADER_SIZE + PAYLOAD_DISPLAY_GRAY_2] = {0};
        uint8_t rx[HEADER_SIZE + PAYLOAD_DISPLAY_GRAY_2] = {0};
        build_gray_2_cross(tx, sizeof(tx), 192,
                           CMD_PROTOCOL_V1, CMD_ID_DISPLAY_GRAY_2);
        Serial.println("V1 Oneshot burst: 500x at ~2ms cadence");
        for (int i = 0; i < 500; i++) {
            spi_xfer(tx, rx, sizeof(tx));
            // 2 ms gap = ~500 Hz "frame rate" from the controller side
            delayMicroseconds(1500);
        }
        print_cipo("Oneshot last CIPO", rx);
        // After this burst the peripheral should go dark (Oneshot timed out;
        // queue_drops_ may be nonzero in peripheral serial — many of the 500
        // frames will land while a previous frame is still on the queue).
        delay(1500);  // observe the peripheral go dark
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

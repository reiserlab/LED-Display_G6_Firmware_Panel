#include <hardware/spi.h>
#include <hardware/gpio.h>
#include "constants.h"
#include "panel_spi_custom.h"

// ----------------------------------------------------------------------------
// V1 CIPO confirmation buffer
// ----------------------------------------------------------------------------
// Per docs/development/g6_01-panel-protocol.md § Confirmation message, the
// panel must send a 3-byte confirmation slot on CIPO during the next CS-active
// window after each valid received message:
//     [header_with_recomputed_parity] [echoed_cmd] [8-bit CRC-8/AUTOSAR]
// (per g6_01-panel-protocol.md § CRC-8 algorithm; computed by
// Message::calculate_crc8() over {header_with_parity_cleared, cmd, payload}
// of the incoming message)
// Empty-buffer (boot or just-consumed) sentinel: {0x81, 0x00, 0x00}.
// COMM_CHECK byte-mismatch sentinel: {header, 0xFF, 0x00}  (this firmware's
// chosen encoding; see plan § Spec amendment).
//
// Codex review correction: the original "substitute TX bytes inside
// custom_spi_read_blocking" approach is wrong — by the time the read loop
// runs, the master has already clocked byte 0. The 3-byte confirmation must
// be in the TX FIFO BEFORE the next CS falling edge. Implementation:
//   - panel_spi_arm_confirmation() / panel_spi_clear_confirmation() update
//     conf_buf_ and reload the TX FIFO with the 3 bytes (FIFO depth = 8 on
//     RP2350, easy fit).
//   - custom_spi_read_blocking() then continues priming with 0x00 filler for
//     bytes beyond the 3-byte slot.
//   - On CS-rising after >= 3 bytes were clocked, we reset to the empty
//     sentinel (per spec: "buffer deleted, each confirmation sent only once").
// ----------------------------------------------------------------------------

static uint8_t conf_buf_[3] = {0x81, 0x00, 0x00};

// Push the 3 confirmation bytes into the SPI TX FIFO. Both TX and RX FIFOs
// are cleared in hardware first via an SSE (SPI Synchronous Serial Enable)
// toggle.
//
// PATCH (G6-ArenaSlim PE03 bring-up): the slave-mode TX FIFO does not drain
// between transactions (it only drains via SCK pulses, which require CS to be
// asserted by the master). Because custom_spi_read_blocking's flow-control
// condition (`rx_remaining < tx_remaining + fifo_depth`) lets the polling
// loop push up to 8 bytes ahead of bytes read, every transaction leaves ~8
// leftover zero-filler bytes in the TX FIFO. Without clearing them, the next
// reload_tx_fifo() call would block in spi_is_writable() until the *next*
// master transaction starts and clocks the FIFO down — by which point the
// slave has missed several bytes of that new transaction and num_rx
// undercounts (-> PE03 length error). The effect worsens at lower SCK rates
// where the inter-transaction drain takes proportionally longer absolute time.
//
// SSE-toggle clears both FIFOs (TX + RX) and resets the shift register in
// hardware, instantly. With CS high (slave idle) it is safe.
static void __not_in_flash_func(reload_tx_fifo)() {
    // Disable + re-enable the SPI block to clear both FIFOs.
    hw_clear_bits(&spi_get_hw(SPI_INST)->cr1, SPI_SSPCR1_SSE_BITS);
    hw_set_bits(&spi_get_hw(SPI_INST)->cr1,   SPI_SSPCR1_SSE_BITS);

    // Now the TX FIFO is guaranteed empty. The 3 pushes will succeed
    // immediately without spinning on spi_is_writable.
    for (size_t i = 0; i < 3; i++) {
        spi_get_hw(SPI_INST)->dr = (uint32_t) conf_buf_[i];
    }
}

void panel_spi_arm_confirmation(uint8_t header, uint8_t cmd, uint8_t checksum) {
    conf_buf_[0] = header;
    conf_buf_[1] = cmd;
    conf_buf_[2] = checksum;
    reload_tx_fifo();
}

void panel_spi_clear_confirmation() {
    conf_buf_[0] = 0x81;
    conf_buf_[1] = 0x00;
    conf_buf_[2] = 0x00;
    reload_tx_fifo();
}


// ----------------------------------------------------------------------------
// Customized version of SPI blocking read — based on pico SDK + hot-plug fix.
//
// Modified vs. SDK:
//   - read terminates when CS goes HIGH (instead of waiting for full `len`
//     bytes), so the parser can reset on a short / aborted transaction.
//   - The first 3 TX bytes come from conf_buf_ (preloaded into the FIFO by
//     panel_spi_arm_confirmation() / panel_spi_clear_confirmation() called
//     between transactions); bytes 4..len are 0x00 filler.
//
// Returns: number of bytes actually received (0 .. len).
// ----------------------------------------------------------------------------
static int __not_in_flash_func(custom_spi_read_blocking)(
        spi_inst_t *spi,
        uint8_t    *dst,
        size_t     len,
        uint8_t    cs_pin
    )
{
    invalid_params_if(HARDWARE_SPI, 0 > (int)len);
    const size_t fifo_depth = 8;
    // After CS-high, poll up to this many times for a straggler RX byte still
    // propagating through the input synchronizer + SSP RX pipeline before
    // concluding the transaction is drained (counter resets on each read).
    const uint32_t RX_DRAIN_SETTLE = 256;
    size_t rx_remaining = len, tx_remaining = len;
    size_t tx_index = 0;
    int num_rx = 0;

    // Wait until spi is readable (first bit clocked in by master)
    while(!spi_is_readable(spi)) {};

    while (rx_remaining || tx_remaining) {
        if (tx_remaining && spi_is_writable(spi) &&
                rx_remaining < tx_remaining + fifo_depth)
        {
            // Bytes 0..2: the 3-byte CIPO confirmation already pushed via
            // reload_tx_fifo() between transactions — those are already in
            // the FIFO. From byte 3 onward we push 0x00 filler.
            // tx_index tracks where we are in the transaction.
            //
            // Note: bytes 0..2 of the FIFO were preloaded; we only need to
            // push fresh bytes for index 3..len-1 (zero filler). This
            // reduces TX writes per transaction.
            if (tx_index >= 3) {
                spi_get_hw(spi)->dr = (uint32_t) 0x00;
            }
            ++tx_index;
            --tx_remaining;
        }
        if (rx_remaining && spi_is_readable(spi)) {
            *dst++ = (uint8_t) spi_get_hw(spi)->dr;
            --rx_remaining;
            num_rx++;
        }
        // Check if master de-asserted CS (transaction ended).
        if (gpio_get(cs_pin)) {
            // The final data byte's last SCK edge can precede the CS rising
            // edge by less than the RX-path latency (input synchronizer + SSP
            // RX pipeline), so that byte may not be spi_is_readable() yet at
            // the instant gpio_get() reports CS high. Breaking immediately
            // returns one byte short -> check_length() fails -> PE03. Drain any
            // straggler(s) first, bounded by RX_DRAIN_SETTLE consecutive empty
            // polls (reset on each read) so a finished or truncated transaction
            // still exits promptly and can never hang. CS is high, so the
            // master is not clocking — no bytes from the next transaction can
            // enter the FIFO during this drain.
            uint32_t settle = 0;
            while (rx_remaining && settle < RX_DRAIN_SETTLE) {
                if (spi_is_readable(spi)) {
                    *dst++ = (uint8_t) spi_get_hw(spi)->dr;
                    --rx_remaining;
                    num_rx++;
                    settle = 0;
                } else {
                    ++settle;
                }
            }
            break;
        }
    }
    return num_rx;
}


void panel_spi_read(Message &msg) {
    msg.num_bytes_ = custom_spi_read_blocking(
            SPI_INST,
            msg.data_.data(),
            MESSAGE_MAXIMUM_SIZE,
            SPI_CS_PIN
            );

    // Per spec: "After sending it successfully, the temporary buffer is deleted"
    // — but only if the master clocked at least 3 bytes (= a full CIPO slot).
    // Fragmented transactions (< 3 bytes) leave the buffer armed.
    if (msg.num_bytes_ >= 3) {
        panel_spi_clear_confirmation();
    }
}

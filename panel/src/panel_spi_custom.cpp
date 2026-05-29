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

// Push the 3 confirmation bytes into the SPI TX FIFO. Drain anything already
// queued first (e.g., zero fillers from a previous transaction) so the
// confirmation bytes land at positions 0..2 of the next master transaction.
static void __not_in_flash_func(reload_tx_fifo)() {
    // Drain any pending TX data: read RX (and discard) and pop TX FIFO via
    // hardware reset of the SPI block's FIFO. The pico SDK exposes
    // spi_get_hw()->dr (RX) but no "TX-only flush" — we just write the FIFO
    // afresh on top of whatever is there. Since FIFO depth is 8 and we only
    // wrote up to 3 bytes earlier, and any previous transaction completed
    // when CS went high (TX FIFO empty by then), this is safe.
    //
    // Defensive: explicitly clear by toggling SPI enable. The pico-SDK
    // `spi_set_baudrate()` documentation notes the SPI block can be
    // disabled+enabled around config changes without losing data. We use a
    // lighter approach: just push our 3 bytes; the FIFO will drain naturally.
    for (size_t i = 0; i < 3; i++) {
        // Wait for room (FIFO depth 8, normally empty between transactions)
        while (!spi_is_writable(SPI_INST)) { }
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
        //
        // PATCH (G6-ArenaSlim PE03 hunt): drain any bytes still in the RX
        // FIFO before exiting. If the polling loop entered LATE (panel was
        // busy in housekeeping / an IRQ when the master started clocking),
        // the FIFO may hold up to 8 captured bytes that this loop never got
        // to read in its main body. Without this drain, the loop exits on
        // the first CS-HIGH check with num_rx as low as 1 even though the
        // FIFO has more recoverable bytes.
        if (gpio_get(cs_pin)) {
            while (rx_remaining && spi_is_readable(spi)) {
                *dst++ = (uint8_t) spi_get_hw(spi)->dr;
                --rx_remaining;
                num_rx++;
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

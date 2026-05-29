#include <hardware/spi.h>
#include <hardware/gpio.h>
#include <hardware/dma.h>
#include <hardware/structs/spi.h>
#include <pico/time.h>
#include "constants.h"
#include "panel_spi_custom.h"

// ============================================================================
// PROTOTYPE: DMA-based SPI-slave RX (PE03 root-cause fix attempt)
// ============================================================================
// Replaces the software-polled custom_spi_read_blocking() that read the PL022
// RX FIFO one byte at a time on core 0. That approach lost bytes whenever core
// 0 entered the read loop late or stalled mid-burst: the 8-deep RX FIFO
// overran and num_bytes_ came back wrong (the classic n==8 signature) ->
// check_length() fails -> PE03.  See hypothesis.md (H1/H3/H4/H5).
//
// Here, byte capture is hardware-paced by two DMA channels driven off the SPI
// DREQs, so it is immune to core-0 processing latency *within* a burst:
//   - RX DMA: SPI DR -> msg.data_, paced by RX DREQ, count = MESSAGE_MAXIMUM.
//   - TX DMA: tx_buf_ -> SPI DR, paced by TX DREQ, count = MESSAGE_MAXIMUM.
//             tx_buf_[0..2] hold the armed CIPO confirmation; [3..] are 0x00
//             filler so the TX FIFO never underflows mid-frame.
//
// Framing is still derived from CS, but core 0 only polls CS *edges* (cheap),
// never per byte. The received length is read from the RX DMA's residual
// transfer_count after CS rises.
//
// The single remaining failure mode is missing a *whole* frame if core 0 is
// busy past an entire CS window — which produces a skipped update, NOT a PE03
// (we never arm mid-burst, so we never capture a partial frame). The durable
// fix for that is the fully IRQ-driven variant noted at the bottom of this
// file.
//
// NOTE: This is a bring-up prototype. Items marked [VERIFY] must be confirmed
// on hardware / against the RP2350 datasheet before this is trusted.
// ============================================================================

// CIPO confirmation slot + filler. Index 0..2 = {header, cmd, checksum};
// 3..MAX-1 = 0x00. Boot/empty sentinel is {0x81, 0x00, 0x00}.
static uint8_t tx_buf_[MESSAGE_MAXIMUM_SIZE] = {0x81, 0x00, 0x00};

static int  rx_dma_chan_ = -1;
static int  tx_dma_chan_ = -1;
static bool dma_ready_   = false;

// ----------------------------------------------------------------------------
// Lazy one-time DMA setup. Called from the public entry points so neither
// messenger.cpp nor the header needs to change. Safe to call repeatedly.
// ----------------------------------------------------------------------------
static void ensure_dma_init() {
    if (dma_ready_) return;

    rx_dma_chan_ = dma_claim_unused_channel(true);
    tx_dma_chan_ = dma_claim_unused_channel(true);
    dma_ready_   = true;
}

// ----------------------------------------------------------------------------
// Discard any residual RX bytes left in the FIFO and clear a sticky overrun,
// WITHOUT disabling the peripheral. Only legal while CS is idle (high).
//
// We deliberately do NOT toggle the SSE (enable) bit here. Disabling and
// re-enabling the PL022 under an idle-high MODE3 clock (CPOL=1/CPHA=1) injects
// a one-bit phase shift into the first frame after re-enable, which corrupts
// byte alignment and fails the parity check (observed as a PE02 glyph). The
// peripheral stays continuously enabled — matching the known-good alignment of
// the previous polled implementation — and we only drain leftover bytes by
// reading them, which is a no-op in the normal case (the prior transaction's
// RX DMA already consumed exactly num_bytes).
// ----------------------------------------------------------------------------
static void spi_drain_rx() {
    spi_hw_t *hw = spi_get_hw(SPI_INST);
    while (spi_is_readable(SPI_INST)) { (void)hw->dr; }
    hw->icr = SPI_SSPICR_RORIC_BITS;                // clear overrun latch
}

// ----------------------------------------------------------------------------
// Arm both DMA channels for one transaction of up to MESSAGE_MAXIMUM_SIZE
// bytes and start them together. RX writes into `rx_dst`; TX reads tx_buf_.
// ----------------------------------------------------------------------------
static void arm_dma(uint8_t *rx_dst) {
    spi_hw_t *hw = spi_get_hw(SPI_INST);

    // RX: read from DR (no increment), write to buffer (increment).
    dma_channel_config rc = dma_channel_get_default_config(rx_dma_chan_);
    channel_config_set_transfer_data_size(&rc, DMA_SIZE_8);
    channel_config_set_read_increment(&rc, false);
    channel_config_set_write_increment(&rc, true);
    channel_config_set_dreq(&rc, spi_get_dreq(SPI_INST, false));  // false = RX
    dma_channel_configure(rx_dma_chan_, &rc,
                          rx_dst,            // write addr
                          &hw->dr,           // read addr (SPI data reg)
                          MESSAGE_MAXIMUM_SIZE,
                          false);            // don't start yet

    // TX: read from tx_buf_ (increment), write to DR (no increment).
    dma_channel_config tc = dma_channel_get_default_config(tx_dma_chan_);
    channel_config_set_transfer_data_size(&tc, DMA_SIZE_8);
    channel_config_set_read_increment(&tc, true);
    channel_config_set_write_increment(&tc, false);
    channel_config_set_dreq(&tc, spi_get_dreq(SPI_INST, true));   // true = TX
    dma_channel_configure(tx_dma_chan_, &tc,
                          &hw->dr,           // write addr (SPI data reg)
                          tx_buf_,           // read addr
                          MESSAGE_MAXIMUM_SIZE,
                          false);            // don't start yet

    // Start both atomically. TX immediately pre-fills the 8-deep TX FIFO with
    // the confirmation bytes so byte 0 on CIPO is correct on the next CS edge.
    dma_start_channel_mask((1u << rx_dma_chan_) | (1u << tx_dma_chan_));
}

// ----------------------------------------------------------------------------
// Public API
// ----------------------------------------------------------------------------

void panel_spi_arm_confirmation(uint8_t header, uint8_t cmd, uint8_t checksum) {
    ensure_dma_init();
    tx_buf_[0] = header;
    tx_buf_[1] = cmd;
    tx_buf_[2] = checksum;
    // No FIFO write here: the TX DMA (armed in panel_spi_read) sources tx_buf_
    // for the next transaction. Bytes 3.. remain 0x00 filler.
}

void panel_spi_clear_confirmation() {
    ensure_dma_init();
    tx_buf_[0] = 0x81;
    tx_buf_[1] = 0x00;
    tx_buf_[2] = 0x00;
}

void panel_spi_read(Message &msg) {
    ensure_dma_init();

    // 1. Make sure we start from an idle bus so we never arm mid-burst (which
    //    would capture only a frame tail -> a false short count -> PE03).
    //    If we entered late and CS is already low, wait out that burst first.
    while (!gpio_get(SPI_CS_PIN)) { tight_loop_contents(); }

    // 2. Discard any residual RX (no SSE toggle — see spi_drain_rx), then arm.
    spi_drain_rx();
    arm_dma(msg.data_.data());

    // 3. Wait for the master to start (CS falling) then finish (CS rising).
    while (gpio_get(SPI_CS_PIN))  { tight_loop_contents(); }   // start
    while (!gpio_get(SPI_CS_PIN)) { tight_loop_contents(); }   // end

    // 4. Let the final byte propagate shift-reg -> RX FIFO -> DMA. The
    //    controller's cs_hold delay (~2.5 us at 10 MHz, Arena constants.h:65)
    //    already covers most of this; a small extra settle is cheap insurance.
    //    [VERIFY] 2 us is comfortably > FIFO drain time at sysclk.
    busy_wait_us(2);

    // 5. Received count = programmed length - residual transfer count.
    //    [VERIFY] On RP2350, reading transfer_count returns transfers REMAINING.
    uint32_t remaining = dma_channel_hw_addr(rx_dma_chan_)->transfer_count;
    size_t   received  = (remaining > MESSAGE_MAXIMUM_SIZE)
                             ? 0
                             : (size_t)(MESSAGE_MAXIMUM_SIZE - remaining);

    // 6. Stop both channels so they can be re-armed next call.
    dma_channel_abort(rx_dma_chan_);
    dma_channel_abort(tx_dma_chan_);

    msg.num_bytes_ = received;

    // Per spec: clear the armed confirmation only if the master clocked a full
    // 3-byte CIPO slot. Fragmented transactions leave the buffer armed.
    if (msg.num_bytes_ >= 3) {
        panel_spi_clear_confirmation();
    }
}

// ============================================================================
// NEXT STEP (not in this prototype): fully IRQ-driven, zero core-0 polling.
//   - Keep RX/TX DMA armed permanently.
//   - GPIO IRQ on CS rising: snapshot transfer_count -> received length, push
//     the message to a lock-free ring for core 0, abort+re-arm DMA, flush
//     FIFOs. Core 0 never spins on CS at all, so even whole-frame misses go
//     away. Requires moving the validity gate off the IRQ hot path.
// ============================================================================

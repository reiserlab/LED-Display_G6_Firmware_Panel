#include <hardware/spi.h>
#include <hardware/gpio.h>
#include <hardware/dma.h>
#include <hardware/structs/spi.h>
#include <pico/time.h>
#include "constants.h"
#include "panel_spi_custom.h"

// ----------------------------------------------------------------------------
// DMA-paced SPI peripheral RX/TX.
//
// Two DMA channels driven off the SPI DREQs move bytes in hardware, so
// reception is immune to core-0 processing latency within a burst (the polled
// predecessor lost bytes when core 0 entered the read loop late and the 8-deep
// RX FIFO overran). Framing is taken from CS edges — core 0 polls only the
// edges, never per byte — and the received length comes from the RX DMA's
// residual transfer_count.
//
//   - RX DMA: SPI DR -> msg.data_, paced by RX DREQ, count = MESSAGE_MAXIMUM.
//   - TX DMA: tx_buf_ -> SPI DR,   paced by TX DREQ, count = MESSAGE_MAXIMUM.
//             tx_buf_[0..2] = armed CIPO confirmation; [3..] = 0x00 filler so
//             the TX FIFO never underflows mid-frame.
//
// Remaining limitation: if core 0 is busy past a whole CS window the frame is
// skipped, not corrupted (we never arm mid-burst). The fully IRQ-driven variant
// noted at the bottom removes even that.
//
// [VERIFY] items must be confirmed on hardware / against the RP2350 datasheet.
// ----------------------------------------------------------------------------

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

    // Start from an idle bus so we never arm mid-burst (a partial-tail capture
    // would be a false short count). If we entered late with CS already low,
    // wait that burst out first.
    while (!gpio_get(SPI_CS_PIN)) { tight_loop_contents(); }

    spi_drain_rx();
    arm_dma(msg.data_.data());

    while (gpio_get(SPI_CS_PIN))  { tight_loop_contents(); }   // CS falling: start
    while (!gpio_get(SPI_CS_PIN)) { tight_loop_contents(); }   // CS rising: end

    // Let the final byte propagate shift-reg -> RX FIFO -> DMA. The controller's
    // cs_hold delay (~2.5 us at 10 MHz, Arena constants.h:65) covers most of it.
    // [VERIFY] 2 us comfortably exceeds FIFO drain time at sysclk.
    busy_wait_us(2);

    // [VERIFY] On RP2350, reading transfer_count returns transfers REMAINING.
    uint32_t remaining = dma_channel_hw_addr(rx_dma_chan_)->transfer_count;
    size_t   received  = (remaining > MESSAGE_MAXIMUM_SIZE)
                             ? 0
                             : (size_t)(MESSAGE_MAXIMUM_SIZE - remaining);

    dma_channel_abort(rx_dma_chan_);
    dma_channel_abort(tx_dma_chan_);

    msg.num_bytes_ = received;

    // Clear the armed confirmation only once the controller clocked a full
    // 3-byte CIPO slot; fragmented transactions leave it armed (per spec).
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

#include <hardware/spi.h>
#include <hardware/gpio.h>
#include <hardware/dma.h>
#include <hardware/structs/spi.h>
#include <pico/time.h>
#include "constants.h"
#include "panel_spi_custom.h"

// ----------------------------------------------------------------------------
// SPI peripheral: DMA-paced RX, direct-FIFO TX.
//
// RX is captured by a DMA channel off the SPI RX DREQ, so reception is immune
// to core-0 processing latency within a burst (the polled predecessor lost
// bytes when core 0 entered the read loop late and the 8-deep RX FIFO overran).
// Framing comes from CS edges — core 0 polls only the edges, never per byte —
// and the received length is read from the RX DMA's residual transfer_count.
//
//   - RX: SPI DR -> msg.data_ via DMA, paced by RX DREQ, count = MESSAGE_MAXIMUM.
//   - TX: the 3-byte CIPO confirmation (+ two 0x00 filler bytes) is written
//         straight into the TX FIFO before the transaction (no DMA). A full
//         frame clocks all five out, so the FIFO self-empties and leaves no
//         residue to shift the next reply. The filler keeps the CRC byte from
//         being last in the FIFO, where TX underflow would truncate its tail.
//
// (A previous revision used a TX DMA over-provisioned to MESSAGE_MAXIMUM_SIZE.
// It left up to 8 filler bytes in the FIFO between transactions, which clocked
// out first and shifted the confirmation off byte 0 — and the mid-burst DMA
// refill raced the clock and corrupted the trailing bytes. Direct FIFO writes
// avoid both.)
//
// Remaining limitation: if core 0 is busy past a whole CS window the frame is
// skipped, not corrupted (we never arm mid-burst). The fully IRQ-driven variant
// noted at the bottom removes even that.
//
// [VERIFY] items must be confirmed on hardware / against the RP2350 datasheet.
// ----------------------------------------------------------------------------

// Armed 3-byte CIPO confirmation {header, cmd, checksum}. Boot/empty sentinel
// is {0x81, 0x00, 0x00}.
static uint8_t tx_buf_[3] = {0x81, 0x00, 0x00};

static int  rx_dma_chan_ = -1;
static bool dma_ready_   = false;

// ----------------------------------------------------------------------------
// Lazy one-time DMA setup. Called from the public entry points so neither
// messenger.cpp nor the header needs to change. Safe to call repeatedly.
// ----------------------------------------------------------------------------
static void ensure_dma_init() {
    if (dma_ready_) return;
    rx_dma_chan_ = dma_claim_unused_channel(true);
    dma_ready_   = true;
}

// ----------------------------------------------------------------------------
// Discard any residual RX bytes and clear a sticky overrun. RX-only: the
// peripheral stays continuously enabled to preserve byte alignment (an SSE
// toggle does not reliably clear the FIFOs on this silicon anyway). A no-op in
// the normal case, since the RX DMA already consumed exactly num_bytes.
// ----------------------------------------------------------------------------
static void spi_drain_rx() {
    spi_hw_t *hw = spi_get_hw(SPI_INST);
#if SPI_DIAG
    // Diag builds: bound the drain so a bus that continuously clocks RX can't
    // wedge core 0 before the telemetry heartbeat runs.
    uint32_t t0 = time_us_32();
    while (spi_is_readable(SPI_INST)) {
        (void)hw->dr;
        if ((uint32_t)(time_us_32() - t0) > 5000u) break;
    }
#else
    while (spi_is_readable(SPI_INST)) { (void)hw->dr; }
#endif
    hw->icr = SPI_SSPICR_RORIC_BITS;                // clear overrun latch
}

// ----------------------------------------------------------------------------
// Write the 3-byte CIPO confirmation into the TX FIFO so it clocks out starting
// at byte 0 of the next transaction, followed by two 0x00 filler bytes. Must
// run while CS is idle (FIFO writable).
//
// The filler matters: the PL022 truncates the trailing bits of the *last* FIFO
// byte when it underflows, so the CRC must not be last (observed without
// padding: CRC 0x62 clocked out as 0x60). Two trailing zeros put the underflow
// past the confirmation slot. The shortest real frame is ERROR_DISPLAY = 5
// bytes, so all five queued bytes clock out on any valid frame and the FIFO
// self-empties — no residue to shift the next reply.
// ----------------------------------------------------------------------------
static void prime_tx_confirmation() {
    spi_hw_t *hw = spi_get_hw(SPI_INST);
#if SPI_DIAG
    // Diag builds: bound the FIFO-writable waits. On a quiet bus (CS idle high,
    // no SCK) the TX FIFO never drains, so successive primes would fill it and
    // this would spin forever — wedging core 0 before the heartbeat. Abort the
    // prime on timeout; RX (frame reception) is independent, and during real
    // streaming SCK drains the FIFO so this never trips. Only CIPO content on a
    // just-after-idle frame is affected, which the display path doesn't use.
    for (int i = 0; i < 3; i++) {
        uint32_t t0 = time_us_32();
        while (!spi_is_writable(SPI_INST)) {
            if ((uint32_t)(time_us_32() - t0) > 2000u) return;
            tight_loop_contents();
        }
        hw->dr = (uint32_t) tx_buf_[i];
    }
    for (int i = 0; i < 2; i++) {
        uint32_t t0 = time_us_32();
        while (!spi_is_writable(SPI_INST)) {
            if ((uint32_t)(time_us_32() - t0) > 2000u) return;
            tight_loop_contents();
        }
        hw->dr = (uint32_t) 0x00;
    }
#else
    for (int i = 0; i < 3; i++) {
        while (!spi_is_writable(SPI_INST)) { tight_loop_contents(); }
        hw->dr = (uint32_t) tx_buf_[i];
    }
    for (int i = 0; i < 2; i++) {
        while (!spi_is_writable(SPI_INST)) { tight_loop_contents(); }
        hw->dr = (uint32_t) 0x00;
    }
#endif
}

// ----------------------------------------------------------------------------
// Arm and start the RX DMA for one transaction of up to MESSAGE_MAXIMUM_SIZE
// bytes, writing into `rx_dst`.
// ----------------------------------------------------------------------------
static void arm_rx_dma(uint8_t *rx_dst) {
    spi_hw_t *hw = spi_get_hw(SPI_INST);
    dma_channel_config rc = dma_channel_get_default_config(rx_dma_chan_);
    channel_config_set_transfer_data_size(&rc, DMA_SIZE_8);
    channel_config_set_read_increment(&rc, false);
    channel_config_set_write_increment(&rc, true);
    channel_config_set_dreq(&rc, spi_get_dreq(SPI_INST, false));  // false = RX
    dma_channel_configure(rx_dma_chan_, &rc,
                          rx_dst,            // write addr
                          &hw->dr,           // read addr (SPI data reg)
                          MESSAGE_MAXIMUM_SIZE,
                          true);             // start now (waits on RX DREQ)
}

// ----------------------------------------------------------------------------
// Public API
// ----------------------------------------------------------------------------

void panel_spi_arm_confirmation(uint8_t header, uint8_t cmd, uint8_t checksum) {
    ensure_dma_init();
#if SPI_TX_STATIC
    // Diagnostic: ignore the real confirmation and emit a fixed, recognizable
    // pattern so a logic-analyzer CIPO capture isolates TX timing/alignment
    // from confirmation content. Build with -DSPI_TX_STATIC=1.
    tx_buf_[0] = 0xA5; tx_buf_[1] = 0x5A; tx_buf_[2] = 0xC3;
    (void)header; (void)cmd; (void)checksum;
#else
    tx_buf_[0] = header;
    tx_buf_[1] = cmd;
    tx_buf_[2] = checksum;
#endif
}

void panel_spi_clear_confirmation() {
    ensure_dma_init();
#if SPI_TX_STATIC
    tx_buf_[0] = 0xA5; tx_buf_[1] = 0x5A; tx_buf_[2] = 0xC3;
#else
    tx_buf_[0] = 0x81;
    tx_buf_[1] = 0x00;
    tx_buf_[2] = 0x00;
#endif
}

#if SPI_DIAG
// Copy the currently-armed 3-byte CIPO confirmation for diagnostics.
void panel_spi_debug_tx(uint8_t out[3]) {
    out[0] = tx_buf_[0];
    out[1] = tx_buf_[1];
    out[2] = tx_buf_[2];
}
#endif

void panel_spi_drive_response(const uint8_t *buf, size_t len) {
    ensure_dma_init();
    spi_hw_t *hw = spi_get_hw(SPI_INST);

    // Start from an idle bus so we drive from byte 0 of the next transaction.
    while (!gpio_get(SPI_CS_PIN)) { tight_loop_contents(); }
    spi_drain_rx();

    // Pre-load up to the 8-deep TX FIFO so byte 0 is shifted out on the first
    // SCK edge (matches prime_tx_confirmation's pre-CS-fall loading).
    size_t i = 0;
    while (i < len && i < 8 && spi_is_writable(SPI_INST)) {
        hw->dr = (uint32_t)buf[i++];
    }

    while (gpio_get(SPI_CS_PIN)) { tight_loop_contents(); }   // CS falling: start

    // Feed the rest as the FIFO drains, then 2 filler bytes so the PL022 TX
    // underflow can't truncate the tail of the final real byte.
    while (i < len) {
        while (!spi_is_writable(SPI_INST)) {
            if (gpio_get(SPI_CS_PIN)) goto done;   // controller ended early
            tight_loop_contents();
        }
        hw->dr = (uint32_t)buf[i++];
    }
    for (int k = 0; k < 2; ++k) {
        while (!spi_is_writable(SPI_INST)) {
            if (gpio_get(SPI_CS_PIN)) goto done;
            tight_loop_contents();
        }
        hw->dr = 0x00;
    }
done:
    while (!gpio_get(SPI_CS_PIN)) { tight_loop_contents(); }  // CS rising: end
    spi_drain_rx();
}

void panel_spi_read(Message &msg) {
    ensure_dma_init();

    // Start from an idle bus so we never arm mid-burst (a partial-tail capture
    // would be a false short count). If we entered late with CS already low,
    // wait that burst out first.
#if SPI_DIAG
    // Diag builds: bound this wait too, so a bus that idles/floats LOW (e.g. no
    // master, or between-frame level) can't wedge core 0 before the heartbeat.
    {
        uint32_t t0 = time_us_32();
        while (!gpio_get(SPI_CS_PIN)) {
            if ((uint32_t)(time_us_32() - t0) > 5000u) { msg.num_bytes_ = 0; return; }
            tight_loop_contents();
        }
    }
#else
    while (!gpio_get(SPI_CS_PIN)) { tight_loop_contents(); }
#endif

    spi_drain_rx();              // clear residual RX + overrun
    prime_tx_confirmation();     // load the 3-byte CIPO reply into the TX FIFO
    arm_rx_dma(msg.data_.data());

#if SPI_DIAG
    // Diag builds: don't spin forever when the master is idle/absent. Return
    // 0 bytes after a short CS-idle timeout so the messenger idle branch can
    // run the telemetry heartbeat + d/z commands (and so the instrument works
    // standalone). During real streaming CS toggles constantly, so this never
    // fires and reception stays production-identical.
    {
        uint32_t t0 = time_us_32();
        while (gpio_get(SPI_CS_PIN)) {                         // CS falling: start
            if ((uint32_t)(time_us_32() - t0) > 5000u) {      // 5 ms idle
                dma_channel_abort(rx_dma_chan_);
                msg.num_bytes_ = 0;
                return;
            }
            tight_loop_contents();
        }
    }
#else
    while (gpio_get(SPI_CS_PIN))  { tight_loop_contents(); }   // CS falling: start
#endif
#if SPI_DIAG
    // Diag builds: bound the CS-rising (end-of-burst) wait too. A real frame
    // transaction is ~tens of µs, so 5 ms never trips during normal streaming;
    // it only rescues a bus that fell and is held LOW (idle default, or a
    // continuous-CS master), so the heartbeat/commands still run. Drop the
    // (abnormal) partial capture as 0 bytes.
    {
        uint32_t t0 = time_us_32();
        while (!gpio_get(SPI_CS_PIN)) {                        // CS rising: end
            if ((uint32_t)(time_us_32() - t0) > 5000u) {
                dma_channel_abort(rx_dma_chan_);
                msg.num_bytes_ = 0;
                return;
            }
            tight_loop_contents();
        }
    }
#else
    while (!gpio_get(SPI_CS_PIN)) { tight_loop_contents(); }   // CS rising: end
#endif

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

    msg.num_bytes_ = received;

    // Clear the armed confirmation only once the controller clocked a full
    // 3-byte CIPO slot; fragmented transactions leave it armed (per spec).
    if (msg.num_bytes_ >= 3) {
        panel_spi_clear_confirmation();
    }
}

// ============================================================================
// NEXT STEP (not in this prototype): fully IRQ-driven, zero core-0 polling.
//   - Keep the RX DMA armed permanently.
//   - GPIO IRQ on CS rising: snapshot transfer_count -> received length, push
//     the message to a lock-free ring for core 0, re-arm RX DMA, re-prime the
//     TX FIFO. Core 0 never spins on CS at all, so even whole-frame misses go
//     away. Requires moving the validity gate off the IRQ hot path.
// ============================================================================

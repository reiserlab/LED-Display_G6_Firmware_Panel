#ifndef PANEL_SPI_CUSTOM_H
#define PANEL_SPI_CUSTOM_H
#include <Arduino.h>
#include "message.h"

// SPI read: blocks until a transaction completes (CS falling -> CS rising).
// On entry: ensures the TX FIFO is primed with the currently-armed CIPO
// confirmation (or the empty-buffer sentinel {0x81, 0x00, 0x00} if nothing
// armed). On exit (CS rising after >= 3 bytes), clears the armed confirmation.
void panel_spi_read(Message &msg);

// Arm a 3-byte CIPO confirmation: {header, cmd, checksum}. Called by
// Messenger::update() after dispatch on a VALID message. Staged for the next
// transaction's TX DMA so it's available before panel_spi_read runs again.
void panel_spi_arm_confirmation(uint8_t header, uint8_t cmd, uint8_t checksum);

// Reset the confirmation buffer to the empty-buffer sentinel
// {0x81, 0x00, 0x00}. Called: (a) at panel boot from Messenger::initialize()
// to prime the buffer before the controller's first transaction, (b) after a
// successful CIPO transmission (>= 3 bytes clocked, per spec).
void panel_spi_clear_confirmation();

#if SPI_DIAG
// Copy the currently-armed 3-byte CIPO confirmation (tx_buf_[0..2]) for the
// SPI_DIAG heartbeat, so the intended reply can be compared against the wire.
void panel_spi_debug_tx(uint8_t out[3]);
#endif

#endif

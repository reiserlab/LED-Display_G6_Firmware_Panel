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
// Messenger::update() after dispatch on a VALID message. Buffer is loaded
// into the TX FIFO immediately so it's available on the next master
// transaction (even before custom_spi_read_blocking runs again).
void panel_spi_arm_confirmation(uint8_t header, uint8_t cmd, uint8_t checksum);

// Reset the confirmation buffer to the empty-buffer sentinel
// {0x81, 0x00, 0x00}. Called: (a) at slave boot from Messenger::initialize()
// to prime the FIFO before the master's first transaction, (b) after a
// successful CIPO transmission (>= 3 bytes clocked, per spec).
void panel_spi_clear_confirmation();

#endif

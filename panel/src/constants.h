#ifndef CONSTANTS_H
#define CONSTANTS_H
#include <Arduino.h>
#include <hardware/spi.h>
#include "protocol.h"

#ifndef PANEL_REV
#error "PANEL_REV not defined. Build with -DPANEL_REV=21 (v0.2.1) or -DPANEL_REV=31 (v0.3.1) from platformio.ini."
#endif

#if (PANEL_REV != 21) && (PANEL_REV != 31)
#error "Unsupported PANEL_REV. Only 21 (v0.2.1) and 31 (v0.3.1) are valid."
#endif

// USB/Serial parameters
extern const uint32_t BAUDRATE;

// SPI peripheral instance (spi0 on v0.2.1, spi1 on v0.3.1) and pins
extern spi_inst_t *const SPI_INST;
extern const uint8_t SPI_SCK_PIN;
extern const uint8_t SPI_MOSI_PIN;
extern const uint8_t SPI_MISO_PIN;
extern const uint8_t SPI_CS_PIN;

// SPI clock speed (Hz)
extern const uint32_t SPI_SPEED;

// External trigger pin (GP45 on both revs; forward-looking for v3 Triggered/Gated modes)
extern const uint8_t EINT_PIN;

// EINT trigger polarity for the V1 EINT modes (Triggered 0x12/0x32, Gated
// 0x13/0x33). Default 0 = active-HIGH: Triggered advances one row per
// LOW->HIGH edge, Gated lights the panel while EINT is HIGH, and a pull-down
// keeps a disconnected line inactive. Build with -DEINT_ACTIVE_LOW=1
// (pico_v0*_eintlow envs in platformio.ini) to invert everything together:
// Triggered advances per HIGH->LOW edge, Gated lights while LOW, pull-up
// keeps a disconnected line inactive. For trigger sources whose line is
// asserted LOW (e.g. imaging systems with an active-low exposure output).
#ifndef EINT_ACTIVE_LOW
#define EINT_ACTIVE_LOW 0
#endif

// LED column and row pins (plain C arrays; matches Pico SDK conventions).
// Pattern matrix in pattern.h still uses Eigen for matrix math.
extern const uint8_t COL_PIN[PANEL_SIZE];
extern const uint8_t ROW_PIN[PANEL_SIZE];

// LED polarity: both v0.2.1 and v0.3.1 are NORMAL polarity (col HIGH + row LOW = ON).
// Differs from the v0.1 Janelia batch (which was reversed).
constexpr bool COL_ON_LEVEL = true;   // column HIGH = ON
constexpr bool ROW_ON_LEVEL = false;  // row LOW = ON

// Display parameters
extern const size_t DISPLAY_QUEUE_SIZE;
extern const uint8_t NUM_COLOR;

// Error-display timing (V1 panel error glyphs).
//   Duration: how long the panel shows an error glyph in Persistent mode.
//   Rate-limit: minimum elapsed wall-clock between successive raises; errors
//   inside this window are silently counted (heartbeat) but not displayed,
//   so a noisy bus doesn't starve the panel of valid commands.
// Plan: 1 s display, 5 s rate-limit. Spec minimum is 500 ms per
// g6_01-panel-protocol.md:394.
extern const uint32_t ERROR_DISPLAY_DURATION_US;
extern const uint32_t ERROR_RATE_LIMIT_US;

// Cross-core error-request queue depth. Messenger (core 0) enqueues a
// pending error slot index; Display (core 1) drains. A small fixed depth is
// fine because the rate-limit guarantees fewer than ~1 enqueue per error-
// display window.
extern const size_t ERROR_REQUEST_QUEUE_SIZE;


#endif

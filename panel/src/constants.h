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


#endif

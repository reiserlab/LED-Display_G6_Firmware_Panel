#include "constants.h"

// USB/Serial parameters
const uint32_t BAUDRATE = 115200;

// SPI clock speed (Hz). Peripheral-side is permissive; this is informational on
// the slave. Spec target is 25 MHz with margin; 30 MHz is the marginal ceiling.
const uint32_t SPI_SPEED = 30000000;

// EINT (external trigger) — GP45 on both v0.2.1 and v0.3.1.
const uint8_t EINT_PIN = 45;

#if PANEL_REV == 21
// G6 panel v0.2.1
//   Columns:  GP1-GP20 (20 contiguous)
//   Rows:     GP21-GP31 + GP36-GP44 (split by SPI0 block at GP32-35)
//   SPI:      SPI0 on GP32(MOSI/RX), GP33(CS), GP34(SCK), GP35(MISO/TX)
//   PSRAM CS: GP0 (via XIP_CS1n; set in platformio.ini -DRP2350_PSRAM_CS=0)
spi_inst_t *const SPI_INST = spi0;
const uint8_t SPI_SCK_PIN  = 34;
const uint8_t SPI_MOSI_PIN = 32;
const uint8_t SPI_MISO_PIN = 35;
const uint8_t SPI_CS_PIN   = 33;
const uint8_t COL_PIN[PANEL_SIZE] =
    {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20};
const uint8_t ROW_PIN[PANEL_SIZE] =
    {21,22,23,24,25,26,27,28,29,30,31,36,37,38,39,40,41,42,43,44};

#elif PANEL_REV == 31
// G6 panel v0.3.1
//   Columns:  GP0-GP19 (20 contiguous)
//   Rows:     GP20-GP39 (20 contiguous)
//   SPI:      SPI1 on GP40(MOSI/RX), GP41(CS), GP42(SCK), GP43(MISO/TX)
//   PSRAM CS: GP47 (via XIP_CS1n; set in platformio.ini -DRP2350_PSRAM_CS=47)
spi_inst_t *const SPI_INST = spi1;
const uint8_t SPI_SCK_PIN  = 42;
const uint8_t SPI_MOSI_PIN = 40;
const uint8_t SPI_MISO_PIN = 43;
const uint8_t SPI_CS_PIN   = 41;
const uint8_t COL_PIN[PANEL_SIZE] =
    {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19};
const uint8_t ROW_PIN[PANEL_SIZE] =
    {20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39};

#endif

// Display parameters
const size_t DISPLAY_QUEUE_SIZE = 5;
const uint8_t NUM_COLOR = 4;

// Error-display timing
const uint32_t ERROR_DISPLAY_DURATION_US = 1'000'000;   // 1 s display window
const uint32_t ERROR_RATE_LIMIT_US       = 5'000'000;   // 5 s minimum between displays
const size_t ERROR_REQUEST_QUEUE_SIZE    = 4;

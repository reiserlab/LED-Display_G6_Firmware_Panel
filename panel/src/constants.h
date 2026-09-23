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

// BCM base ON time (µs) — the duration of the weight-1 bit-plane at
// duty_cycle=255. A full-brightness row is 15 × base (Gray_16 planes 1+2+4+8,
// or the single weight-15 Gray_2 plane): 45 µs at the production 3.0 µs,
// sized for the free-running 1 kHz Persistent scan (20 rows × ~50 µs).
//
// For line-synchronous use with a resonant-scanning microscope the whole row
// must fit inside the scanner's turnaround gap, which is ~18 µs on the Bergamo
// (7.9 kHz bidirectional resonant, 63 µs line, 0.9 fill → line clock LOW for
// ~18.4 µs; measured 2026-09). Build with -DBCM_BASE_ON_US=1.0f
// (pico_v0*_eintlow_2p envs) so a duty=255 Gray_16 row is 15 µs + ~1 µs
// trigger→LED latency and stays inside that gap at every duty_cycle. Ships
// as a float literal so the bench selftest's runtime retune keeps working.
#ifndef BCM_BASE_ON_US
#define BCM_BASE_ON_US 3.0f
#endif

// Free-running Triggered mode (2P line-sync variant). Production V1 Triggered
// (0x12/0x32) is one-shot: 20 EINT edges consume the frame, then the panel is
// dark until the controller re-streams it. With the controller refreshing at
// 300 Hz and a 15.8 kHz line clock that lights the panel for only 20 lines
// (1.26 ms) of every 3.33 ms — a 300 Hz on/off envelope in the imaging data.
// -DTRIGGERED_WRAP=1 makes Triggered wrap 19→0 and keep going: exactly one row
// per EINT edge, forever, with the row counter preserved across re-streamed
// frames (a new frame swaps the pixel data between rows without restarting at
// row 0). The panel is then lit on every line and only during the trigger
// window; a stalled trigger source leaves it dark.
#ifndef TRIGGERED_WRAP
#define TRIGGERED_WRAP 0
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

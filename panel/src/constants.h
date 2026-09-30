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

// External trigger pin (GP45; Triggered/Gated modes)
extern const uint8_t EINT_PIN;

// EINT polarity (panel-fw v1.3.0). Triggered (0x12/0x32/0x52/0x62) fires one
// row per FALLING (HIGH->LOW) edge: its only source is the ScanImage line
// clock of the 2P rigs, which is LOW during the resonant scanner's turnaround
// gap — the window the display may light. Gated (0x13/0x33/0x53/0x63) is
// unchanged: lit while EINT is HIGH. A pull-down keeps a disconnected line
// dark in both modes. (Until v1.3.0 an EINT_ACTIVE_LOW build flag flipped both
// modes together; production was rising-edge Triggered.)
//
// Triggered mode free-runs: one row per edge, wrapping 19->0, forever, with
// the row phase kept across re-streamed frames (display.cpp, Display::update).
// The old one-shot rule (20 edges, then dark until the controller re-streams)
// put a 300 Hz on/off envelope on the imaging data at a 300 Hz refresh.

// BCM base ON time (µs) — the duration of the weight-1 bit-plane at
// duty_cycle=255. A full-brightness row is 15 × base (Gray_16 planes 1+2+4+8,
// or the single weight-15 Gray_2 plane).
//
//   BCM_BASE_ON_US            3.0 µs: Persistent, Oneshot, Gated and error
//                             glyphs. A 45 µs full-duty row, sized for the
//                             free-running 1 kHz scan (20 rows × ~50 µs).
//   BCM_TRIGGERED_BASE_ON_US  1.0 µs: Triggered only. The whole row must fit
//                             in the resonant scanner's turnaround gap (~18 µs
//                             on the Bergamo: 7.9 kHz bidirectional, 63 µs line,
//                             0.9 fill; measured 2026-09), so a duty=255
//                             Gray_16 row is 15 µs + ~1 µs trigger→LED latency.
//                             Brightness at equal duty is 1/3 of the other
//                             modes. The rig tests recommend duty <= 191.
//
// Float literals so the bench selftest's runtime retune ('b') keeps working.
#ifndef BCM_BASE_ON_US
#define BCM_BASE_ON_US 3.0f
#endif
#ifndef BCM_TRIGGERED_BASE_ON_US
#define BCM_TRIGGERED_BASE_ON_US 1.0f
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

#ifndef ISP_H
#define ISP_H

#include "message.h"

// ───────────────────────────────────────────────────────────────────────────
// In-System Programming (ISP) receiver — reflash this panel's firmware over SPI
// on command from the arena controller (host command g6-program-panel / 0xC8).
//
// The panel stages the incoming image in PSRAM, verifies it (CRC-32), writes it
// to a LittleFS file + an OTA command (PicoOTA), then reboots: the arduino-pico
// OTA stub (ota.o, linked at flash offset 0) copies the staged image into the
// app region at clean early-boot and boots it. (Directly reprogramming the
// running image at offset 0 from the app does not survive the reboot.)
//
// WIRE PROTOCOL (must stay in sync with the controller's IspController.{h,cpp}).
//   Panel-protocol v1 header (0x01/0x81), opcode block 0xE4–0xE9. Each command
//   is one CS-asserted transaction (the panel ingests + processes it and ARMS a
//   reply). The controller then issues a SECOND CS-asserted transaction in which
//   the panel drives the armed reply on CIPO via panel_spi_drive_response().
//   Reply framing: [status(1)][payload...][crc8]   (CRC-8/AUTOSAR over the
//   leading bytes; status 0 == OK).
//
// Requires a LittleFS region (board_build.filesystem_size > 0 in platformio.ini)
// for OTA staging. Verified end-to-end on hardware via g6-verify-panel (0xC9).
// ───────────────────────────────────────────────────────────────────────────
namespace Isp {

// Pre-allocate the PSRAM staging buffer at boot. Call once from setup so
// ISP_ENTER doesn't stall mid-handshake allocating it (the controller's
// phase-A → phase-B gap is short; a late panel misses the reply window).
void init();

// Dispatch an ISP-opcode message (0xE4–0xE9). Validates, processes, and arms
// the reply (except ISP_EXIT_REBOOT, which reboots and never returns).
void handle(Message &msg);

// True when a reply is armed and waiting to be clocked out by the controller.
bool response_pending();

// Drive the armed reply on CIPO (one transaction), then — if an ISP_COMMIT is
// due — perform the flash erase+program. Call once per update() while a reply
// is pending, in place of the normal command read.
void service_pending();

// Visual programming indicator (LAB-44). During staging the ISP receiver
// pushes a progress bar (central 10 rows) into the display queue; after an
// OTA install the freshly-booted image shows a smiley until first use.
//
// boot_indicator_check(): call ONCE from the production loop() (core 0, both
// cores in steady state — not from setup(); LittleFS ops idle the other
// core). Shows the smiley iff the just-flashed marker file exists.
void boot_indicator_check();

// notify_host_command(): call on the first valid host display command;
// retires the just-flashed marker so the smiley stays gone after power
// cycles. One-shot, cheap after the first call.
void notify_host_command();

}  // namespace Isp

#endif

#ifndef SCAN_CONSOLE_H
#define SCAN_CONSOLE_H
// LAB-286 — USB console commands for the bank-window scan mode.
//
//   B          print scan mode + window table
//   B0 / B1    legacy scan / bank windows
//   W          print the window table
//   W<t0>,<t1>,<t2>,<t3>   set the window table, ns at duty 255 (T0 violet,
//              T1 blue, T2 green, T3 yellow-orange), e.g. W4200,5500,12700,22500
//
// The letters are free in every panel console (bcmtest, the PR #30 color
// commands, SPI_DIAG, PSRAM selftest). Both consoles that run on core 0 call
// scan_console_command(): the bcmtest selftest and the pico_v031_bankwin
// production-path console below.

#include <Arduino.h>

// Handle one console line if it is a 'B' or 'W' command. Returns false (and
// prints nothing) for any other line.
bool scan_console_command(const String &line);

#if PANEL_CONSOLE
// pico_v031_bankwin: production SPI ingest plus a USB console. Call from the
// core 0 loop; it reads USB serial at most every 10 ms, assembles lines and
// handles B, W, S (frame-timing stats since the last S) and ? (help).
// Handling a line takes core 0 away from SPI reception for a moment, so a
// command typed while the arena is streaming can cost that panel one frame.
void panel_console_poll();
#endif

#endif // SCAN_CONSOLE_H

#ifndef ISP_LOGIC_H
#define ISP_LOGIC_H

#include <stdint.h>

#include "protocol.h"  // CMD_ID_* opcodes, PANEL_SIZE

// Arduino-free ISP logic: staging-protocol constants, the WRITE_PAGE bounds
// check, visual-indicator geometry (LAB-44), and the boot-indicator retire
// filter. Shared between the firmware (isp.cpp / messenger.cpp) and the
// host-side unit tests (test/test_isp_logic/), so this header must stay free
// of Arduino, pico-sdk, and Eigen includes.

namespace Isp {

// --- Staging protocol constants (MUST match the controller's IspController) --
constexpr uint32_t kStageMax    = 2u * 1024u * 1024u;  // <= RP2354 2 MiB app flash
constexpr uint16_t kPageBytes   = 256;                 // flash program granule (WRITE_PAGE)
constexpr uint32_t kSectorBytes = 4096;                // flash erase granule (reported in ENTER reply)

// WRITE_PAGE bounds check on the 24-bit page index. Checked on the index
// itself (not on idx * kPageBytes) so a huge index cannot wrap the 32-bit
// offset math and slip past the staging limit.
constexpr uint32_t kMaxPages = kStageMax / kPageBytes;
inline bool write_page_in_bounds(uint32_t idx) { return idx < kMaxPages; }

// --- Visual programming indicator (LAB-44) ----------------------------------
// While pages stream in, the panel shows a progress bar across the central 10
// rows (rows 5..14), filling left to right. The panel does not know the image
// total up front (only the controller does), so the bar is scaled to a nominal
// image size; a larger image simply wraps and refills, an indeterminate
// fallback for free. Real images (~96-140 KB -> ~380..550 pages) stay below
// nominal, so they fill near-linearly and never wrap.
constexpr uint32_t kNominalPages  = 576;  // ~144 KiB nominal image
constexpr int      kBarRowFirst   = 5;
constexpr int      kBarRowLast    = 14;
constexpr uint8_t  kIndicatorDuty = 128;

// Bar fill (0..PANEL_SIZE columns) for `pages` staged pages.
inline uint8_t progress_cols(uint32_t pages) {
  uint32_t filled = pages * PANEL_SIZE / kNominalPages;
  return (filled <= PANEL_SIZE) ? (uint8_t)filled
                                : (uint8_t)(filled % PANEL_SIZE);
}

// First content command retires the ISP boot-indicator flag (the post-flash
// smiley). COMM_CHECK, the error-glyph test command, and the ISP opcodes are
// not content, so they do not count.
inline bool retires_boot_indicator(uint8_t cmd_id) {
  return cmd_id != CMD_ID_COMMS_CHECK &&
         cmd_id != CMD_ID_ERROR_DISPLAY &&
         !(cmd_id >= CMD_ID_ISP_ENTER && cmd_id <= CMD_ID_ISP_EXIT_REBOOT);
}

// --- Indicator glyphs --------------------------------------------------------
// Row 0 = top (same Pattern orientation the predef glyphs use); '#' = lit.

// Shown by boot_indicator_check() on the first boot after an ISP flash.
// constexpr, not inline: works pre-C++17, and each referencing TU owning its
// own internal-linkage copy is fine (only isp.cpp and the tests use these).
constexpr const char *kSmiley[PANEL_SIZE] = {
    "....................",
    "....................",
    "....................",
    "....................",
    ".....##......##.....",
    ".....##......##.....",
    "....................",
    "....................",
    "....................",
    "....................",
    "...#............#...",
    "....#..........#....",
    ".....#........#.....",
    "......########......",
    "....................",
    "....................",
    "....................",
    "....................",
    "....................",
    "....................",
};

// Shown when ISP_COMMIT fails to stage the image (status 8): a sad smiley,
// the mirror of the success face, so a failed panel is as obvious at a
// glance as a successful one.
constexpr const char *kSadSmiley[PANEL_SIZE] = {
    "....................",
    "....................",
    "....................",
    "....#.#......#.#....",
    ".....#........#.....",
    "....#.#......#.#....",
    "....................",
    "....................",
    "....................",
    "....................",
    "......########......",
    ".....#........#.....",
    "....#..........#....",
    "...#............#...",
    "....................",
    "....................",
    "....................",
    "....................",
    "....................",
    "....................",
};

}  // namespace Isp

#endif

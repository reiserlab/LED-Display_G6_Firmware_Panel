#ifndef PSRAM_STORE_H
#define PSRAM_STORE_H
#include <stdint.h>
#include <stddef.h>
#include "pattern.h"
#include "protocol.h"

// PSRAM-backed frame store for the LAB-41/42 V2 demo.
//
// The panel keeps a small library of pre-rendered Gray_16 frames in its
// on-board 8 MB PSRAM. The controller then drives an animation with tiny V2
// "display PSRAM index N" commands (4-5 bytes) instead of streaming 201-byte
// pixel frames over the wire — that is the whole point of the PSRAM path.
//
// For the demo the frames are generated locally in firmware at boot
// (generate_demo); the over-the-wire upload path is the full V2 and is out of
// scope here.
//
// Each stored frame is exactly the V1 Gray_16 wire payload (200 nibble-packed
// pixels + 1 duty_cycle byte = PAYLOAD_DISPLAY_GRAY_16). Storing the canonical
// wire form means pack/unpack reuse the existing Message codec verbatim, so the
// host-side reference model (panel/tools/test_psram_demo.py) and the firmware
// agree byte-for-byte.
namespace psram_store {

    constexpr uint16_t FRAME_COUNT = 100;
    // 200 nibble-packed Gray_16 pixels (2 px/byte) + 1 duty_cycle byte. Equals
    // the runtime PAYLOAD_DISPLAY_GRAY_16 (201); derived from the constexpr
    // PANEL_SIZE so it can size the constexpr below.
    constexpr size_t   FRAME_BYTES = (size_t)PANEL_SIZE * PANEL_SIZE / 2 + 1;  // 201

    // Demo animation layout (see generate_demo):
    //   [0..39]   horizontal bar sweeping rows 0->19->0 (40 frames)
    //   [40..79]  vertical bar sweeping cols 0->19->0   (40 frames)
    //   [80..99]  drifting checkerboard                 (20 frames)
    constexpr uint16_t H_SWEEP_BEGIN = 0;
    constexpr uint16_t V_SWEEP_BEGIN = 40;
    constexpr uint16_t CHECKER_BEGIN = 80;

    // Allocate the PSRAM frame store via pmalloc(). Returns false if PSRAM is
    // unavailable (no chip / pmalloc failed); the caller should fail visibly
    // rather than boot into a silently-empty store.
    bool init();

    // True once init() has succeeded.
    bool ready();

    // Render the demo animation into the store. No-op (returns) if !ready().
    void generate_demo();

    // Number of frames currently programmed (0 until generate_demo runs).
    uint16_t count();

    // Pointer to the raw FRAME_BYTES record at `index`, or nullptr if the
    // index is out of range / store not ready / not yet generated.
    const uint8_t *frame_ptr(uint16_t index);

    // Read-back integrity check: re-derive every demo frame and compare it
    // byte-for-byte against what is stored in PSRAM. Proves the PSRAM write/
    // read path is intact. Used by the PSRAM_SELFTEST build.
    struct VerifyResult {
        uint16_t checked;     // frames compared
        uint16_t mismatched;  // frames that did not match
    };
    VerifyResult verify();

    // Decode frame[index] into `out` as a Gray_16 Pattern, applying display
    // `mode`. When duty_override >= 0 it replaces the stored duty_cycle
    // (used by the V2 0x60-0x63 explicit-duty commands); otherwise the stored
    // duty is kept (0x50-0x53). Returns false on out-of-range index / !ready /
    // ungenerated store.
    bool load(uint16_t index, Pattern &out, DisplayMode mode, int duty_override = -1);

}

#endif

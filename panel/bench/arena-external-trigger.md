# Arena-level external-trigger validation (Triggered + Gated via BNC)

**Date:** 2026-06-03 bench. **Result: PASS** — external triggering drives the arena's panels
in both Triggered and Gated modes through the production **BNC J4** input, up to the **8 kHz**
spec target. This was the last open validation item for the G6 arena + panel hardware.

## Setup

- **Arena:** Teensy 4.1, branch `mreiser/arena-trigger-validation` (off `origin/main` `acd5286`,
  the CIPO-fixed tip), built `teensy41-printf` (debug). Worktree `LED-Display_G6_Firmware_Arena-trigval`.
- **Panels:** v0.2.1 (`319A5199EE357F77`) `pico_v021_spidiag`; v0.3.1 (`D2A9A2161F5FA2EF`)
  `pico_v031_spidiag`. Both boot-generate the PSRAM catalog, run SPI ingest, expose `z`/`d`.
- **Stimulus:** AD3 **W1** (BNC adapter, DC-coupled, 0 Ω) → **arena BNC J4** (center+shield).
- **Host:** `LED-Display_G6_Firmware_Arena/scripts/stream_trigger_test.py` streams a host-built V1
  Gray_16 frame with cmd `0x33`(Gated)/`0x32`(Triggered) via `STREAM_FRAME` (no arena FW change for
  the *streaming* side). AD3 driven by `panel/tools/ad3_trigger.py`.

## The BNC input path (KiCad-traced, v1p1r7)

`W1 → BNC J4 center → U3.B (5V) → [U3 DIR=LOW ⇒ B→A] → U3.A (3.3V) → J30 pin2 → [J30 shunt] →
J30 pin1 → R216 (1 kΩ) → TNY.EINT → 74LVC2G17 fanout (3.3V) → R88–R97 → all 10 panels' EINT/GP45.`

- U3 = `SN74LVC1T45DBV` **bidirectional** translator (not the unidirectional part in the v1.1.2
  proposal). **DIR = Teensy pin 34** (`34_RX8`), **no pull** → firmware must drive it.
- **Required firmware:** drive **pin 34 LOW** (B→A). Added in `arena/src/main.cpp`
  `setupExternalTriggerInput()`. Without it the translator floats and the trigger never arrives.
- **Required hardware:** **J30 shunt installed** (routes U3.A → EINT). J3 is the *other* BNC (U2/D29).
- Fanout buffers are 3.3 V (74LVC2G17, VIH ~2 V) so a 3.3 V drive through R216's 1 kΩ into the
  high-Z inputs is ample. (J4/U3.B is 5 V-tolerant, so 0–5 V also works.)

## Results

- **Gated (0x33):** EINT HIGH → both panels all-on; LOW → dark; 3 Hz square → both blink in lockstep.
  Panels received `cmd_hist 0x33`, `reject_any=0`.
- **Triggered (0x32):** EINT held **HIGH with no edges → stayed DARK** (confirms edge-driven, not
  level); **8 kHz edges → bright** (20 edges/frame ≈ 400 fps); 1 kHz → lit but dimmer/shimmery;
  edges stop → dark. `cmd_hist 0x32`, `reject_any=0`.

## Notes / known artifacts

- **1 kHz shimmer:** the arena re-streams `0x32` at ~300 Hz (`STREAMING_FRAME` refresh) and each
  re-stream resets the 20-edge row counter. At 8 kHz a frame (2.5 ms) finishes inside the ~3.3 ms
  re-arm window → clean; at 1 kHz a frame (20 ms) is chopped → shimmer/beating. Cosmetic for this
  visual test. **Clean low-rate fix = single-shot Triggered emission** (arena mode-selector, pending).
- **AD3 gotcha:** W1 looked bipolar (±1.65 V) only because the BNC-adapter **Ch1 AC/DC coupling
  jumper was on AC**, stripping the DC. On DC it's a clean 0→3.3 V square. W1 offset works fine.
- **Panel power:** panels were USB-powered; unplugging panel USB crashes/darkens them. A reflash
  (BOOTSEL → `pico_v0NN_spidiag`) recovers them (confirmed alive: psramtest 100/100).

## Open / next

- **Instrumented Tier 2** (not yet): photodiode + AD3 scope for exact 20-edges=frame, trigger→LED
  latency (~1.5 µs expected), per-row LED-on window vs `duty_cycle`, clean 8 kHz no-missed-edges.
- **CIPO:** arena `[spi] CIPO …` dump reads all-zero on the `spidiag` panel build during display
  commands — separate investigation (spidiag-vs-production tx timing and/or known contention).
- **V2 PSRAM trigger path** + arena **mode selector** (single-shot Triggered) — Phase B remainder.
- **Commit/PR:** `stream_trigger_test.py`, `ad3_trigger.py`, and the `main.cpp` DIR-pin fix → PR
  against arena `main`.

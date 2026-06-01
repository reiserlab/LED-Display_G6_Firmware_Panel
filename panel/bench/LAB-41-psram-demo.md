# LAB-41 / LAB-42 — PSRAM demo (V2 display-from-PSRAM)

Phase-1 single-panel proof that the panel can store patterns in its on-board 8 MB PSRAM and
display them on a controller-issued **V2** command, with the wire carrying only a small frame
index instead of 201-byte pixel frames. Demo scope: frames are generated **locally in firmware
at boot** — the over-the-wire PSRAM *upload* path (full V2) is out of scope.

## TL;DR

- **Panel side (LAB-41) is implemented and builds clean** on both revs, plus the `_psramtest`
  and `_spidiag` diagnostic builds and the `panel_controller` bench driver. Host reference
  tests pass (7/7).
- **100-frame demo animation** generated on-device into PSRAM: H-bar sweep (idx 0–39),
  V-bar sweep (40–79), drifting checkerboard (80–99). 100 × 201 B ≈ 20 KB.
- **V2 protocol** (`0x50–0x53` implicit-duty, `0x60–0x63` explicit-duty) decoded, version-gated
  per command, dispatched to a PSRAM-index handler that reuses the existing Pattern→display path.
- **Arena side (LAB-42) NOT applied** — the `LED-Display_G6_Firmware_Arena` checkout was mid-merge
  with unresolved conflicts when this work landed; the exact changes are specified below for
  application on a clean tree. (LAB-42 is assigned to Frank.)

## V2 wire format (the shared contract)

Header byte: version 2 → `0x02` (parity 0) / `0x82` (parity 1). Parity + CRC-8/AUTOSAR are the
same algorithms as V1 (version-agnostic).

| Opcode      | Meaning                                  | Payload                         |
|-------------|------------------------------------------|---------------------------------|
| `0x50–0x53` | Display PSRAM index, mode in low nibble   | 2 B: uint16 LE index            |
| `0x60–0x63` | …with explicit duty_cycle                 | 3 B: uint16 LE index + 1 B duty |

Low nibble = display mode (0 Oneshot / 1 Persistent / 2 Triggered / 3 Gated), same as V1.
`0x5x` use the duty stored with the frame; `0x6x` override it. Demo uses **`0x51` (Persistent)**
so a late index holds rather than blinking dark. Out-of-range index → panel raises PE05.

This is modeled byte-for-byte in `panel/tools/test_psram_demo.py` (parity rule, CRC-8 check
value `0xDF`, payload sizes, LE index, Gray_16 nibble packing, and the 100-frame catalog).

## What changed (panel repo)

- `panel/src/protocol.{h,cpp}` — `CMD_PROTOCOL_V2`, V2 opcodes, payload sizes,
  `command_protocol_version()` (per-command version gate).
- `panel/src/message.cpp` — `check_protocol()` now compared against the command's required version
  (V1 regression-clean: V1 opcodes still require a V1 header).
- `panel/src/psram_store.{h,cpp}` — pmalloc-backed 100×201 B store; `generate_demo()`,
  `load()` (reuses the Message Gray_16 codec), `verify()` (read-back integrity).
- `panel/src/messenger.{h,cpp}` — `on_cmd_display_psram()` handler; V2 reception counters in
  the `SPI_DIAG` dump (`psram: cmds=… oor=… idx=[..] distinct=…/100`).
- `panel/src/main.cpp` — boot init+generate; `PSRAM_SELFTEST` console.
- `panel/platformio.ini` — `pico_v0NN_psramtest` envs (`-DPSRAM_SELFTEST=1`).
- `panel_controller/src/main.cpp` — `build_display_psram_index()` + a Step 8 that streams
  indices 0–99 (`0x51`) at ~30 fps.

## How to validate

**1. Single board — PSRAM integrity + local play (no SPI master).**
```
pio run -d panel -e pico_v031_psramtest -t upload      # or deploy.sh
panel/tools/monitor.sh
# expect: "PSRAM SELFTEST: 100/100 OK size=8388608 freeheap=..."
# type 'p' → bars sweep + checker drifts;  'D42' dumps slot 42;  'v' re-verifies
```

**2. Two boards — V2 over SPI (panel under test + panel_controller).**
```
pio run -d panel            -e pico_v031_spidiag  -t upload   # panel under test
pio run -d panel_controller -e pico_controller_v031 -t upload # bench driver
# Wire per panel_controller/README.md. Panel plays the 100-frame animation from PSRAM.
# On the panel's spidiag serial: 'z' then 'd' → cmd_hist shows 0x51,
#   psram: cmds=N oor=0 idx=[0..99] distinct=100/100, reject_any=0.
```

**3. Host reference tests.**
```
pytest panel/tools/test_psram_demo.py        # 7 passed
```

## LAB-42 — arena handoff (apply on a clean arena tree)

`LED-Display_G6_Firmware_Arena` was mid-merge (unmerged `src/SpiManager.cpp`, `src/constants.h`)
when LAB-41 landed, so these were **not** applied here. On a clean branch:

1. **`src/G6PanelProtocol.h`** — add `header_version_v2 = 0x02` / `…_with_parity = 0x82`, the
   `cmd_disp_psram_*` opcodes (0x50–0x53 / 0x60–0x63), `block_byte_count_psram = 4` /
   `…_duty = 5`, and `build_psram_index_block(block, index, cmd_id, duty)` that lays out
   `{0x02, cmd, idx_lo, idx_hi[, duty]}` and calls `stamp_header_parity(block, len)`.
2. **`src/SpiManager.cpp`** — relax the `transferFrame()` guard (currently rejects any block
   size ≠ gs2/gs16) to also accept the V2 block sizes (4 / 5). `transferPanelSet()` is already
   block-size-agnostic.
3. **`src/commands.h`** — add host opcodes, e.g. `DISPLAY_PSRAM_INDEX_CMD = 0x71` (payload:
   uint16 LE index) and `PSRAM_PLAY_CMD = 0x72` (payload: start, count, fps — arena auto-advances).
4. **`src/CommandProcessor.{h,cpp}`** — add `ArenaState::PSRAM_PLAY`; `buildPsramFrame(index,
   cmd_id)` fills `frame_buf_` with the 4-byte prefix + 20 identical V2 blocks (mirror
   `fillFrameBufferAllOn`), sets `block_byte_count_ = 4`; `servicePsramPlay()` advances the index
   at `frame_rate` (mirror `serviceOpenLoop`); arm the refresh timer to retransmit. Optionally set
   capability bitmap bit1 (`v2_local_storage`) in `controller_capability_bitmap`.

DoD: a controller-issued V2 command drives the (locally-loaded) panel to show its PSRAM frame;
`PSRAM_PLAY` loops 0–99 at the configured fps → satisfies the LAB-48 full-arena prerequisite.

## Status / open items

- Panel firmware + tests: **done**, builds green, host tests 7/7.
- On-hardware visual confirmation (single board play + two-board) and the spidiag 0–99 sweep:
  pending bench time.
- CIPO confirmation of the V2 echo on the real bus: inherits the LAB-39 caveat (panel CIPO not
  observed reaching the master on the single-panel bench) — re-check with the CIPO probe.
- Arena V2 (LAB-42): specified above; **not applied** pending a clean arena checkout.

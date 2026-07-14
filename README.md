# G6 Panel Firmware

Firmware for the Generation-6 modular LED-display **panels** (20×20, RP2354 /
RP2350 core). *In development.*

A panel runs as an **SPI peripheral**: the arena controller streams display
frames over SPI (MODE3), and the panel renders them on its 20×20 LED matrix.

- **Core 0** ingests SPI: a DMA-paced RX captures each frame, then the
  `Messenger` validity gate checks parity / length / protocol / opcode and
  either dispatches the frame or raises a `PEnn` error glyph.
- **Core 1** drives the display: binary-code-modulation (BCM) grayscale via PIO.

Wire-protocol reference: `docs/development/g6_01-panel-protocol.md` in the
[Modular-LED-Display](https://github.com/reiserlab) repo.

## Repository layout

| Path | What it is |
|---|---|
| `panel/` | The panel firmware — the SPI peripheral (the production target). |
| `panel_controller/` | Bench harness: turns one panel into a fake SPI controller to drive a second panel running `panel/`. See `panel_controller/README.md`. |
| `test_arena/` | Early standalone SPI sketch used during bring-up. |

## Hardware revisions

Selected at build time with `-DPANEL_REV` (set per PlatformIO env):

| Rev | `PANEL_REV` | SPI | Notes |
|---|---|---|---|
| v0.2.1 | `21` | SPI0, GP32–35 | PSRAM CS on GP0 |
| v0.3.1 | `31` | SPI1, GP40–43 | PSRAM CS on GP47; columns GP0–19, rows GP20–39 |

Pin tables: `docs/development/g6_02-led-mapping.md`.

## Toolchain

Build/flash tooling is provided through [pixi](https://pixi.sh), which installs
PlatformIO into the workspace:

```sh
pixi install
```

All commands below run through `pixi run …`; you can also invoke PlatformIO
directly once the environment is active.

## Build

```sh
pixi run platformio run -d panel -e pico_v031      # build v0.3.1 (or pico_v021)
```

### PlatformIO environments

| Env | Build flags | Purpose |
|---|---|---|
| `pico_v021` / `pico_v031` | `PANEL_REV` | Production firmware. |
| `pico_v021_spidiag` / `pico_v031_spidiag` | `+ SPI_DIAG=1` | Production + SPI/validity-gate **serial diagnostics**. Same SPI ingest — safe to deploy — but per-1000-message `Serial` prints run on core 0 and can cost the occasional frame. |
| `pico_v021_bcmtest` / `pico_v031_bcmtest` | `+ STAGE2_SELFTEST=1` | BCM-via-PIO visual self-test. **No SPI ingest — DO NOT DEPLOY** for bench testing; re-flash a production env first. |
| `pico_v031_twopiotimeouttest` | `+ TWOPIO_ROW_TIMEOUT_US=5` | Forced-fault repro for the two-PIO row-timeout recovery path (issue #21): every row burst times out, exercising the self-heal continuously. Same SPI ingest as production, but display timing is not representative — bench sessions only. Driven end-to-end by `tests/test_pr15_stuck_row_timeout.py` in [LED-Display_G6_Firmware_Arena](https://github.com/reiserlab/LED-Display_G6_Firmware_Arena). |
| `pico_v031_twopiotimeoutdiag` | `+ TWOPIO_ROW_TIMEOUT_US=5 SPI_DIAG=1` | The forced-fault repro plus the SPI_DIAG heartbeat, which adds a live pin-state line (`PINS r=<rows> c=<cols>`, bit i = `ROW_PIN[i]`/`COL_PIN[i]` level; rows active-LOW = ON) for observing the fault at the GPIO level. |

## Flash & monitor

The `deploy*` / `monitor*` pixi tasks target a board by **USB serial number**
(robust against shifting `/dev/ttyACM*` enumeration):

```sh
pixi run deploy31a         # build + flash production firmware to the v0.3.1 board
pixi run deploy31a-diag    # same, but the SPI_DIAG (serial diagnostics) build
pixi run monitor31a        # open the USB-serial monitor for that board
```

(`*21a` variants target the v0.2.1 board.)

> **These tasks are bound to two specific physical panels** (the serial numbers
> are hardcoded in `pixi.toml`) and will only act on those boards. To deploy to
> a **different** panel, find its serial with
> `ls /dev/serial/by-id/usb-Reiser_Lab_RP2354_20x20_Display_Panel_*`
> then either add a task in `pixi.toml` or call the script directly:
> ```sh
> bash panel/tools/deploy.sh <THAT_SERIAL> pico_v031
> ```
> A panel stuck in BOOTSEL won't expose its serial — flash it manually with
> `pixi run platformio run -d panel -e pico_v031 -t upload`.

### SPI diagnostics

With a `*_spidiag` build flashed, the panel prints terse histograms and the
last frame's validity-gate state every 1000 messages — SPI timing
(`gap`/`proc`/`spi` buckets), the `gate p/l/pr/cmd/cc` flags, leading bytes, and
a parity dump on a parity miss. Useful for diagnosing dropped/short frames and
parity mismatches. The instrumentation lives behind `#if SPI_DIAG` in
`panel/src/messenger.cpp`.

## License

MIT

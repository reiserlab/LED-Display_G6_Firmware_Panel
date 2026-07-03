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

`pixi run release` builds+packages the two production envs into `dist/`:

- `dist/g6-panel-<rev>.uf2` — for the `g6-flash` CLI / WebUSB flasher / GitHub Release.
- `dist/g6-panel-<rev>.bin` — the same firmware wrapped in a 32-byte ISP footer
  (`{magic, version, image_crc32, image_size}`) the arena controller validates before
  reflashing a panel over SPI (see the Modular-LED-Display repo's
  `docs/development/g6_03-controller.md` § Panel firmware update (ISP)). Copy the
  chosen `.bin` to the controller SD card as `/firmware/panel.bin` (the controller
  holds a single firmware at a time).
- `dist/manifest.json` — one entry per build, each with a `uf2: {file, sha256}`
  and/or `bin: {file, sha256}`.

This is exactly what CI publishes. `pixi run diag` does the same for the
`_bcmtest`/`_spidiag` envs above — bench/diagnostic builds staged into the same
`dist/`, but never published by CI.

`panel/tools/build_release.py` discovers both catalogs directly from
`panel/platformio.ini`: an env belongs to `release` if it `extends = common`, or to
`diag` if it instead `extends` another `pico_v*` env — so a new rev or variant needs
no change to `build_release.py`/`pixi.toml` to be picked up. `python
panel/tools/build_release.py --list` shows the current catalog.

**CAUTION:** bcmtest firmware has **no SPI ingest** — a panel ISP'd with a bcmtest
`.bin` can no longer be reflashed over SPI afterwards. Recover it via
`flash21-github-release`/`flash31-github-release` (USB) instead of a second ISP push.

## Flash & monitor

Flashing goes through `panel/tools/g6_flash.py` (`picotool`-based — see NOTE
below), not PlatformIO's own upload target. `flash21`/`flash31` flash EVERY
connected panel of a rev:

```sh
pixi run flash31                    # build the FULL release catalog, then flash all v0.3.1 panels
pixi run flash31-github-release     # flash the latest PUBLISHED release, no local build
```

(`*21`/`*21-github-release` variants target v0.2.1.) `flash21`/`flash31` build the
full release catalog first (`pixi run release`) and flash the resulting
`dist/g6-panel-<rev>.uf2` — the exact bytes `pixi run release`/CI would
publish, without needing to cut a release or have network access.
`flash21-github-release`/`flash31-github-release` skip the local build
entirely and flash the latest published release (just `picotool` + network
needed).

> To flash **one specific device** instead of every connected panel,
> build+package just that catalog entry then call `g6_flash.py` directly:
> ```sh
> python panel/tools/build_release.py --only g6-panel-v0.2.1   # or -bcmtest / -spidiag / etc.
> python panel/tools/g6_flash.py --rev v0.2.1 \
>     --uf2 dist/g6-panel-v0.2.1.uf2 --serial <THAT_SERIAL>
> ```
> Find a board's serial with `python panel/tools/g6_flash.py --list`. A panel
> stuck in BOOTSEL is **not** a problem — `g6_flash.py` flashes it directly —
> it just can't be targeted by `--serial` (a blank board exposes no serial),
> so target it by `--port` instead (same `--list` output shows connected
> ports).

To open a serial console on one panel (cross-platform — matches by USB
serial number via `pyserial`, then hands off to `pio device monitor`):

```sh
pixi run monitor -- --serial <THAT_SERIAL>
```

Find a board's serial with `python panel/tools/monitor.py --list`.

> **NOTE:** flashing needs `picotool` on PATH or in PlatformIO's package cache
> (`~/.platformio/packages/tool-picotool*/`, already present after any `pixi run
> release`/`diag`, since they build via `pio`). It's **not** a conda-forge package, so
> it isn't a pinned `pixi.toml` dependency — install it yourself only if neither
> location has it.

### SPI diagnostics

With a `*_spidiag` build flashed, the panel prints terse histograms and the
last frame's validity-gate state every 1000 messages — SPI timing
(`gap`/`proc`/`spi` buckets), the `gate p/l/pr/cmd/cc` flags, leading bytes, and
a parity dump on a parity miss. Useful for diagnosing dropped/short frames and
parity mismatches. The instrumentation lives behind `#if SPI_DIAG` in
`panel/src/messenger.cpp`.

## License

MIT

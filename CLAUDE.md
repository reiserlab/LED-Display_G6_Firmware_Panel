# CLAUDE.md — LED-Display_G6_Firmware_Panel

Guidance for Claude Code working in this repo: RP2350 (Pico 2 / RP2354B) firmware for
the 20×20 panels of the G6 LED arena.

## Architecture (orientation)

Dual-core RP2354B. **Core 0** = `Messenger` — SPI ingest on the PL022 hardware SSP in
**slave** mode (polled, custom blocking read). **Core 1** = `Display` — PIO-driven BCM.
Lock-free SPSC queues (`queue_t`) between them; the display can never starve SPI of CPU.

SPI wire format: **Mode 3** (CPOL=1, CPHA=1), MSB-first, 8-bit, CS-framed.
GS2 = 53-byte messages (cmd `0x10`), GS16 = 203-byte messages (cmd `0x30`).
CIPO is **shared** across panels on the arena bus → the panel drives a 3-byte
confirmation slot `{header, cmd, CRC-8}` only inside its own CS-active window.

Two hardware revs via `-DPANEL_REV` (see `platformio.ini`):
- **v0.2.1** (`pico_v021`): SPI0 on GP32–35, PSRAM CS GP0.
- **v0.3.1** (`pico_v031`): SPI1 on GP40–43, PSRAM CS GP47.

## Build & flash

Run from `panel/`. `pio` is at `/opt/homebrew/bin/pio`.

Environments:
- `pico_v021` / `pico_v031` — **production**.
- `pico_v021_spidiag` / `pico_v031_spidiag` — production + `-DSPI_DIAG=1`: silent in-RAM
  reception counters + per-command histogram + ring of the last 32 failures (`got` vs
  `expected`). Serial `z` = zero the window, `d` = dump. **Identical streaming timing to
  production** (counters are cheap RAM increments; no serial output while streaming).
- `pico_v021_bcmtest` / `pico_v031_bcmtest` — display self-test, **NO SPI ingest. Never deploy.**

Build: `pio run -e <env>`  (e.g. `pio run -e pico_v031_spidiag`)

### Flashing autonomously (BOOTSEL via 1200-baud touch) — no need to prompt the user

The earlephilhower core reboots into BOOTSEL when its USB CDC port is opened at 1200 baud.
**You may flash without asking the user to press the BOOTSEL button.** Sequence:

1. 1200-baud touch on the panel's CDC port (typically `/dev/cu.usbmodem1101`; the arena
   master enumerates separately, e.g. `…121699401`):
   ```python
   import serial, time
   s = serial.Serial("/dev/cu.usbmodem1101", 1200); s.setDTR(False); time.sleep(0.3); s.close()
   ```
2. Wait for `/Volumes/RP2350` to mount (poll up to ~10 s).
3. `cp -X panel/.pio/build/<env>/firmware.uf2 /Volumes/RP2350/`
4. Wait for the CDC port to re-enumerate (~a few seconds).

Use `/usr/bin/python3` (has pyserial). If `/Volumes/RP2350` is already present the board is
already in BOOTSEL — skip step 1. After flashing, verify with an idle `d` dump: under
`SPI_DIAG` the read has a 50 ms idle timeout, so `d`/`z` are answered even with the master
idle (the dump banner reports `PANEL_REV=21` or `=31` — use it to confirm the right build).

## SPI reliability benchmark (audio-cued contained burst)

The arena master (separate repo, `LED-Display_G6_Firmware_Arena`) has runtime SPI-clock
control + a free-running frames-sent counter. To measure panel-received vs master-sent:
`z` the panel window, have the operator clear the master counter and stream a fixed burst,
then `d`. `missed = sent − received`; `reject_any` = within-frame corruption. Synchronise
start/stop with audio cues (`afplay /System/Library/Sounds/{Glass,Basso}.aiff` + `say`) so
the panel's z..d window brackets the master's burst. ~30 s windows are plenty.

## Gotchas / do-not-break

- **The per-valid-frame DOUBLE SSE toggle is load-bearing.** `panel_spi_read()` clears the
  confirmation (toggle #1) and `Messenger::update()` arms it (toggle #2). Collapsing to a
  single reload was A/B-tested and **regressed 15 MHz from 0% to 2.4% byte-drops** — the
  extra SSP disable/enable keeps the marginal PL022 slave RX aligned. See the warning in
  `panel_spi_custom.cpp` `panel_spi_read()`. Do not "optimize" it away.
- **PE03 last-byte-drop** is fixed by draining the RX FIFO after CS-high (`RX_DRAIN_SETTLE`
  in `custom_spi_read_blocking`). Sub-`MESSAGE_MINIMUM_SIZE` runts are ignored (no glyph).
- **Reliability ceiling:** clean to **18 MHz** on **both** revs; sharp corruption cliff at
  **20 MHz** (~6.4%, `got=exp−1`, parity rides along) — inherent PL022 slave sampling,
  **rev-independent** (not pin-routing SI). Deployed clock is ~5 MHz (huge margin). Past
  ~18 MHz needs PL022+DMA, then a PIO+DMA SPI slave (see the plan in the SPI bench docs).

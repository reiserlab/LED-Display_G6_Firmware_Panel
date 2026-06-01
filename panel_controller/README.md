# panel_controller — bench harness

Turns a G6 panel into a fake SPI controller (controller) that drives a second
panel running the peripheral firmware in `../panel/`. Used for V1 wire-protocol
bench validation in the absence of an actual arena controller.

## Build

```sh
# v0.2.1 controller + peripheral pair
pio run -d panel_controller -e pico_controller_v021
pio run -d panel        -e pico_v021

# v0.3.1 controller + peripheral pair
pio run -d panel_controller -e pico_controller_v031
pio run -d panel        -e pico_v031
```

## Wiring pre-flight

**Before powering up anything**, verify the following on the panel schematics
(`reiserlab/LED-Display_G6_Hardware_Panel` v0.2.1 / v0.3.1):

1. **Direction:** On both panels, the inter-panel J2 header carries MOSI/MISO/SCK
   from the panel-MCU's perspective (controller mode and peripheral mode use the same
   pin names; the MCU's role is what flips). Controller MOSI/MISO/SCK pins ↔ peripheral
   MOSI/MISO/SCK pins, **straight-through** — no crossover.
2. **CS path:** Each panel hardwires J3 pin 5 → MCU CS0. For two-panel bench
   testing, wire controller J3 pin 5 to peripheral J3 pin 5 with a single jumper. The
   CS-shift behavior of J3↔J5 (vertical stacking topology) does NOT apply when
   using J3 on both sides.
3. **Power (single-USB setup):** Plug USB only into the **controller** panel.
   Power the peripheral from the controller via **J2 pin 5 (+5V) and J2 pin 4 (GND)**.
   This mirrors the arena topology (which routes +5V through J2 to fan out
   to multiple panels) and is electrically supported, but watch for two
   gotchas:
   - **USB-port current limit (~500 mA).** Two panels at high brightness
     (duty_cycle ≈ 255 with many LEDs lit) can pull > 500 mA and brown out.
     Bench sequence below uses moderate duty_cycle values; if you see USB
     dropouts, drop the duty_cycle sweep range or use a powered USB hub.
   - **No backfeed.** Only one panel may be USB-connected at a time. Plugging
     a second USB into the peripheral while +5V is also bridged via J2 would cross
     two USB power rails. **Do not do that.**
   If you do have two USB cables: independent USB on each + do NOT connect
   J2 pin 5 between panels (then there is no backfeed risk).
4. **Ground continuity:** wire J2 pin 4 (GND) through to the peripheral. In the
   single-USB setup this is the peripheral's only ground reference.

## Peripheral serial output

In the single-USB setup, **the peripheral's USB serial is not accessible** — the
peripheral runs without a host connection. All bench observations come from the
controller side: controller serial logs the CIPO bytes for each transaction, which is
sufficient to infer peripheral state for every test step in the sequence below.
Visual inspection of the peripheral's LEDs confirms display behavior.

If you need peripheral-side diagnostics (`msg_count`, `parity_ok`, `queue_drops`),
temporarily swap which panel gets the USB cable.

## Hardware pin reference

### v0.2.1 — SPI0 on GP32–35

| Header pin | Function | Controller GPIO | Peripheral GPIO |
|---|---|---|---|
| J2 pin 1 | MISO | GP35 (TX in controller mode) | GP35 (TX in peripheral mode) |
| J2 pin 2 | MOSI | GP32 | GP32 |
| J2 pin 3 | SCK  | GP34 | GP34 |
| J2 pin 4 | GND  | GND  | GND  |
| J2 pin 5 | +5V  | (USB-powered)      | **connect to controller J2 pin 5** (single-USB) |
| J3 pin 5 | CS0  | GP33 | GP33 |

### v0.3.1 — SPI1 on GP40–43

| Header pin | Function | Controller GPIO | Peripheral GPIO |
|---|---|---|---|
| J2 pin 1 | MISO | GP43 | GP43 |
| J2 pin 2 | MOSI | GP40 | GP40 |
| J2 pin 3 | SCK  | GP42 | GP42 |
| J2 pin 4 | GND  | GND  | GND  |
| J2 pin 5 | +5V  | (USB-powered)      | **connect to controller J2 pin 5** (single-USB) |
| J3 pin 5 | CS0  | GP41 | GP41 |

## Bench sequence (per iteration, ~1 Hz)

The controller loops through:

1. **COMM_CHECK** (canonical payload 0..199) — expect CIPO `[0x81 0x00 0x00]`
   on first run (peripheral empty buffer) or `[parity|0x01, prev_cmd, prev_crc]` thereafter.
   CRC byte is CRC-8/AUTOSAR per `g6_01-panel-protocol.md` § CRC-8 algorithm.
   Canonical 202-byte COMM_CHECK CRC = `0x8B`.
2. **Gray_2 cross**, duty_cycle=192 — visual: row+column-10 cross at high brightness.
3. **Gray_16 gradient**, duty_cycle=128 — visual: left-to-right brightness ramp at half intensity.
4. **Duty-cycle sweep** on Gray_16 gradient — brightness ramps each iteration.
5. **COMM_CHECK with one byte flipped** + follow-up COMM_CHECK — expect
   `[parity|0x01, 0xFF, 0x00]` on the follow-up (the COMM_CHECK-fail sentinel).
6. **Truncated frame** (3 bytes, length check fails) — CIPO unchanged from prev valid.
7. **Parity-corrupted frame** (no parity recompute) — CIPO unchanged.

Controller prints CIPO bytes to USB serial. Peripheral prints `msg_count / parity_ok /
length_ok / protocol_ok / cmd_ok / comm_check_ok / queue_drops` every 1000
messages.

## SPI clock

Starts at **1 MHz** — well below the marginal hardware ceiling. The spec
targets 25 MHz with margin; max is 30 MHz. Bump `CONTROLLER_SPI_HZ` in
`src/main.cpp` after the bench passes once.

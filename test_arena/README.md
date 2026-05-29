# test_arena — early controller test sketch

A minimal **SPI controller** sketch used during early bring-up to drive G6
panels before the production controller firmware existed. It runs on a
**Teensy 4.1** and clocks frames out to one or more panels running the
firmware in [`../panel/`](../panel).

> Superseded by the production controller firmware in the sibling
> `Arena-Firmware/` directory. Kept as a simple, dependency-light bring-up tool.

## What it does

- Configures SPI as the bus controller (MODE3, MSB-first, 25 MHz).
- Drives up to 4 chip-select lines (`SPI_CS_PINS`), one per panel.
- Generates a moving **cross** test pattern (a row + column that sweep back and
  forth across the 20×20 matrix), encodes it with the shared `Message` /
  `Pattern` classes as a Gray_2 frame, and transmits it to each panel every
  iteration.
- Logs the sweep position over USB serial (115200 baud).

It reuses the panel's `message.{cpp,h}` and `pattern.{cpp,h}` so the wire
encoding matches exactly what the panel firmware expects to decode.

## Build & flash

```sh
pixi run platformio run -d test_arena -e teensy41 -t upload
```

(Target board: Teensy 4.1, `teensy41` env in `platformio.ini`.)

## Notes

- Chip-select pins and SPI speed are compile-time constants at the top of
  `src/main.cpp` — edit there to match your wiring or to sweep the bus clock.
- This sketch is intentionally simple: no error handling, no CIPO/confirmation
  readback, no protocol versioning beyond a single Gray_2 frame type. Use the
  production controller firmware for anything beyond a basic link bring-up.

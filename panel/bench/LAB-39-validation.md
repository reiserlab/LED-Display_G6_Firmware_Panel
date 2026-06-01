# LAB-39 — PR #4 validation (PL022+DMA SPI), single panel, v021 + v031

Bench validation of Frank's PR #4 ("Debug timing", branch `debug-timing`) against the
LAB-39 DoD. Tested 2026-06-01 on the arena master + one panel of each rev, driving the
master over USB serial and counting received frames with a silent in-RAM diagnostic
(production-identical streaming timing).

## TL;DR

- **Reception is excellent: clean at 25 MHz on BOTH revs, reject ≈ 0.** This is the core
  win of the PR and it *clears the prior 20 MHz ceiling*.
- **The "PIO SPI" label is a misnomer** — the PR implements **PL022 peripheral + DMA**
  (DREQ-paced), the intermediate roadmap step, not a PIO SPI slave. (No PIO is used for
  SPI; the only PIO consumer remains the display column scanner.)
- **CIPO confirmation was NOT validated on this bench** — the panel's CIPO return path
  (shared bus, likely CS-gated buffers) doesn't reach the master here; a positive control
  proved the master reads `0x00` regardless of what the panel drives. Needs a bench probe.
- The **TX-FIFO-flush concern is therefore an unverified code-level hypothesis**, not a
  confirmed bug.

## Method

- Master: `LED-Display_G6_Firmware_Arena` release build; runtime `SET_SPI_CLOCK 25`,
  `STREAM_FRAME` a fixed full-on GS16/GS2 frame, free-running; `GET_FRAMES_SENT` = sent.
- Panel: `pico_v0NN_spidiag` carrying the ported **silent-counter** SPI_DIAG (`z` zero /
  `d` dump: `msgs`, `reject_any`, per-gate fails, last-32 fail ring). No serial while
  streaming → timing identical to production.
- Harness: `panel/tools/spi_reliability.py` drives both ends and self-brackets the window
  (`z` → baseline master counter → dwell → read master counter → `d`).
  `missed = sent − received`; `reject_any` = within-frame corruption.

## Results — RX reliability @ 25 MHz

| Rev  | Mode | Frames (recv) | reject_any | missed |
|------|------|--------------:|-----------:|-------:|
| v031 | GS16 | 38,890 | 0 (0.00%) | ≈0 (−2, bracket skew) |
| v031 | GS2  | 40,006 | 0 (0.00%) | ≈0 (−1) |
| v021 | GS16 | 38,885 | 0 (0.00%) | ≈0 (−1) |
| v021 | GS2  | 40,011 | 0 (0.00%) | 0 |

**Interpretation.** The earlier `spi-bringup-step0` "20 MHz corruption cliff (~6.4%,
rev-independent)" was the **polled** path's RX-FIFO overrun (core 0 can't drain the 8-deep
FIFO fast enough → `got = exp−1`), **not** an inherent PL022-slave sampling limit. The DMA
path drains the FIFO in hardware, so it sails past 20 MHz with reject ≈ 0 on both revs.
This refutes the pre-test worry that 25 MHz couldn't be clean without raising `clk_peri`.

## CIPO confirmation — deferred to a bench probe

The DoD's confirmation half could **not** be verified here:

- Streaming with the arena master's debug build (all-sets MISO capture), the master read
  `0x00` on CIPO across all 10 sets × both buses while the panel was provably receiving.
- **Positive control:** the panel was rebuilt to drive `0xA5` filler on CIPO; the master
  *still* read `0x00`. So the panel's CIPO output is not reaching the master's CIPO inputs
  (bus0 = Teensy pin 12, bus1 = pin 1) on this single-panel bench — most likely because the
  shared-CIPO return path goes through **CS-gated tri-state buffers** that aren't enabled/
  wired here. (Panel v031 CIPO out = GP43; v021 = GP35.)
- Consequence: the CIPO/confirmation path appears to have **never been exercised** on this
  bench, consistent with the PR's "works at 25 MHz" referring to RX/display only.

To validate CIPO: probe the CS-gated return buffers (or a logic analyzer directly on the
panel CIPO/CS/SCK pins). Until then, treat the TX-FIFO item below as unverified.

## PR #4 review

### Must-fix before merge
1. **`main.cpp`: `while(true){ messenger.update(); }`** in the production branch — debug
   scaffolding that starves the Arduino `loop()` housekeeping (yield / serialEvent / USB).
   Remove (or justify) before merge.
2. **TX FIFO is never flushed between transactions (unverified hypothesis).**
   `MESSAGE_MAXIMUM_SIZE = 300` but real frames are ≤ 203, so the TX DMA over-queues and
   ~8 prefetched filler bytes can linger in the PL022 TX FIFO (no SSE toggle, no TX drain).
   *Hypothesis:* this shifts the CIPO confirmation slot. **Not reproduced on this bench**
   (CIPO return path unprobed). Verify alongside the CIPO probe; if confirmed, add a
   per-frame TX-FIFO flush (the removed double-SSE toggle used to provide exactly this).

### Observations / should-fix
3. **Naming:** "PIO SPI" → it's PL022+DMA. Fix the issue/PR wording so the roadmap
   (PL022+DMA → true PIO+DMA) stays clear.
4. **Diagnostics:** the PR's `*_spidiag` prints serial *while streaming* (perturbs timing).
   The silent-counter `z`/`d` method used here is timing-neutral — recommend adopting it.
5. **Tooling:** `deploy.sh`/`monitor.sh` were Linux-only (`/dev/serial/by-id`). Added macOS
   support (resolve by USB serial via `panel_port.py` + 1200-baud BOOTSEL touch + UF2 copy).
   Note: macOS 15 FSKit msdos rejects `cp -X` to the RP2350 volume — use plain `cp`.
6. **Scope:** the `panel_master`→`panel_controller` rename + pixi addition are bundled with
   the SPI change — fine, just flagged for reviewer awareness.
7. **Branch divergence:** `debug-timing` does not contain `spi-bringup-step0`'s PE03 fix,
   `CLAUDE.md`, or `SPI-BRINGUP-SUMMARY.md` — reconcile before merge.

### Verdict
The reception path is a clear win — clean 25 MHz on both revs, clearing the polled cliff —
so the PL022+DMA direction is sound. Gate the merge on: (a) removing the `while(true)`
leftover; (b) verifying CIPO via a bench probe and adding a TX-FIFO flush if the shift is
real; (c) reconciling with the `spi-bringup-step0` fixes + test suite.

## Follow-up branch `mreiser/spi-diag-txfifo` (off merged main)

PR #4 was merged as-is (RX win) with the review above. The follow-up branch carries:
- the timing-neutral silent-counter `SPI_DIAG` (replaces PR #4's serial profiler),
- macOS support for `deploy.sh`/`monitor.sh` + `panel_port.py`,
- the `spi_reliability.py` / `cipo_capture.py` bench harness + this writeup,
- **the SSE-toggle TX-FIFO flush** (`spi_flush_tx_fifo()` in `panel_spi_read()`).

**SSE-flush RX re-validation @ 25 MHz (v0.2.1):** GS16 38,886 and GS2 40,007 →
`reject_any = 0`, `missed ≈ 0`. So the per-frame SSE toggle does **not** regress reception
(settles the PR-#4-vs-`spi-bringup` disagreement in favor of the toggle). Its effect on the
**CIPO slot** is still unconfirmed — that needs the bench probe below.

## Next steps
1. **CIPO bench probe** — enable/scope the CS-gated CIPO return buffers (or logic-analyzer
   the panel CIPO pin) to confirm the SSE flush actually lands the confirmation at CIPO
   bytes 0–2.
2. **Open the follow-up PR** (`mreiser/spi-diag-txfifo`) once CIPO is confirmed (or push now
   with the SSE flush flagged "RX-validated, CIPO pending probe").
3. Frank-side on a future PR: remove the `while(true)` leftover; correct the PIO/PL022 naming.

## Artifacts (branch `mreiser/spi-diag-txfifo`, off merged `main`)
- `panel/src/messenger.cpp`, `panel/src/panel_spi_custom.cpp` — silent-counter SPI_DIAG port
  + a diag-only CS-idle timeout in `panel_spi_read()`.
- `panel/tools/spi_reliability.py` — dual-ended reliability benchmark.
- `panel/tools/cipo_capture.py` — CIPO capture with in-window receive proof.
- `panel/tools/panel_port.py`, `deploy.sh`, `monitor.sh` — macOS support.
- Arena repo (`teensy41-printf`, uncommitted): all-sets MISO capture in `SpiManager.cpp`
  for the future CIPO probe.

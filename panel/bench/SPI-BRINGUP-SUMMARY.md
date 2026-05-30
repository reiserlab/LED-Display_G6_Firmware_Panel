# G6 panel SPI bring-up — investigation summary

**Date:** 2026-05-30 · **Hardware:** arena master (Teensy 4.1, G6-ArenaSlim) + single G6
panel · **Panel firmware:** branch `spi-bringup-step0` (PR #5) · **Author:** bring-up with
Frank Loesche (`floesche`).

This is the wrap-up of the first real-hardware SPI bring-up of the G6 arena: two merged
PRs, a root-caused-and-fixed flicker bug, a full clock/rate/rev reliability sweep, an
abandoned "optimization," and the resulting decision on the future high-speed path.

---

## 1. What was wrong, and what we shipped

| # | Problem | Fix | Status |
|---|---|---|---|
| PR #2 | Parity used `std::bitset<sizeof(uint8_t)>` = a **1-bit** set → counted only the LSB | `std::bitset<CHAR_BIT*sizeof(uint8_t)>` (full 8-bit popcount; spec-correct) | merged |
| PR #3 | `PE03`/`PE04` at ≤5 MHz: slave TX FIFO assumed empty between transactions | SSE toggle clears both FIFOs before re-loading the CIPO confirmation | merged |
| Flicker / PE03 | Display dimming = panel **rejecting** frames | see §2 | fixed (PR #5) |
| Core-0 stall | Blocking 11-line serial heartbeat every 1000 msgs could stall core 0 between transactions → missed leading bytes | non-blocking heartbeat (`availableForWrite` guard) | merged (`171d614`) |

## 2. Flicker / PE03 — root cause and fix

The flicker was the panel **rejecting** otherwise-good frames. `SPI_DIAG` (silent in-RAM
counters + a ring of the last 32 failures with `got` vs `expected`) showed: **parity never
failed alone — only length**, almost always `got = expected − 1` (one byte short).

**Root cause = last-byte drop.** `custom_spi_read_blocking()` broke out of its loop on
`gpio_get(cs_pin)` reading high *before* the final byte had cleared the RP2350 input
synchronizer (~4 sysclk) + the SSP RX pipeline → returned one byte short → `check_length()`
failed → PE03 → frame dropped → visible dimming. **Not** signal integrity.

**Fix** (`c35b0cc`, `panel_spi_custom.cpp`): after CS goes high, **drain the RX FIFO** for a
straggler byte (bounded by `RX_DRAIN_SETTLE` empty polls so it can never hang; CS is high so
no new bytes can arrive). Plus: **ignore sub-`MESSAGE_MINIMUM_SIZE` runts** (a 1–2-byte CS
glitch shouldn't flash a 3 s error glyph). Validated: GS2/200 Hz 10/24130 → **0/24101**,
GS16 **0/24117**, **0 rejects across 229,726 messages**.

## 3. Reliability sweep (the headline result)

Method: panel `SPI_DIAG` build + arena master with runtime SPI-clock control and a
free-running frames-sent counter. Audio-cued contained burst — `z` the panel, operator
clears the master counter + streams a fixed ~30 s burst, then `d`; `missed = sent − received`,
`corrupted = rejected`. GS16 (203 B/frame).

| SPI clock | rate | v0.3.1 received / corrupted | v0.2.1 received / corrupted |
|---:|---:|:---|:---|
| 5 MHz  | 200 Hz  | — | 6064 / **0** |
| 10 MHz | 200 Hz  | 3261 / **0** | — |
| 15 MHz | 200 Hz  | 3186 / **0** | 6724 / **0** |
| 15 MHz | 500 fps | 14713 / **0** | 15753 / **0** |
| 18 MHz | 200 Hz  | 3073 / **0** | 6908 / **0** |
| 18 MHz | 500 fps | 29144 / 858 (**2.9 %**) | — |
| 18 MHz | 1 kHz   | 52622 / 5144 (**9.7 %**) | — |
| 20 MHz | 200 Hz  | 3884 / 250 (**6.4 %**) | 6064 / 388 (**6.40 %**) |

**`missed = 0` in every run** (`sent == received`) — whole-frame *delivery* is robust even at
the cliff. The failures are within-frame byte corruption (`got = exp − 1`, sometimes −2/−3).
Parity fails are a ~30–34 % *subset* of length fails, never parity-only (a short frame
mismatches the header popcount about a third of the time).

### Findings

1. **Reliable through 18 MHz on both revs; sharp cliff at 20 MHz (~6.4 %).** Deployed ~5 MHz
   has ~3–4× margin and is rock-solid.
2. **The cliff is rev-independent → PL022, not board SI.** v0.2.1 (SPI0 GP32–35, interleaved
   between the row-driver halves — predicted *worse* by the crosstalk hypothesis) corrupts at
   the **same** clock, rate, and signature as v0.3.1 (20 MHz → 6.4 %, `got=202`). The
   pin-routing/crosstalk hypothesis is **falsified**; the ceiling is inherent **PL022
   SPI-slave sampling marginality** (input-synchronizer / shifter). The earlier impression
   that "v0.3.1 is more stable" was the last-byte-drop bug (now fixed), not a real rev delta.
3. **Cadence matters only near the cliff.** At 18 MHz, rate walks corruption up
   (0 % → 2.9 % → 9.7 % at 200 Hz / 500 fps / 1 kHz). At 15 MHz, **500 fps is still 0 %** —
   and notably the per-frame transaction is *longer* at 15 MHz than 18 MHz, so it leaves
   *less* inter-transaction slack yet stays clean. ⇒ the limit is per-byte sampling, **not**
   core-0 turnaround.
4. **Brightness/duty-cycle was not swept** — the only remaining open signal-integrity
   question, now low priority (both revs clean to 18 MHz at these patterns).

## 4. Abandoned: the "turnaround optimization"

Hypothesis: each valid frame does **two** TX-FIFO reloads — `panel_spi_read()` clears to the
sentinel (SSE toggle #1), then `Messenger::update()` arms the real confirmation a few µs
later (toggle #2) — and the intermediate clear looked like wasted work. Tried collapsing to a
single late arm-or-clear.

**A/B at 15 MHz / 500 fps, same warm bench, back-to-back: original 2-reload = 0/14713;
optimized 1-reload = 391/16086 (2.4 %).** Reverted, re-tested original → 0/14713 again
(thermal ruled out). **The double SSE toggle is load-bearing** — the extra SSP disable/enable
per frame keeps the marginal PL022 slave RX aligned transaction-to-transaction. Abandoned;
warning comment left in `panel_spi_custom.cpp` so it is not re-attempted.

## 5. Future high-speed path (gated, not started)

The 25–30 MHz spec aspiration is beyond the PL022 slave cliff. Order of attack (cheapest
first), per the plan and the Codex review:

1. **(optional) Brightness-vs-error bench** with a rail/ground probe — close the last SI
   question. Low priority.
2. **PL022 + DMA spike** — drive RX (and TX filler) via DMA + CS IRQ instead of the polled
   loop; may fix the per-byte latency without a from-scratch slave.
3. **PIO + DMA SPI slave** — only if 1–2 prove the PL022 shifter is the wall. PIO-SPI ceiling
   ≈ 25 MHz; levers: `INPUT_SYNC_BYPASS`, clock-recovery via a 2nd PIO, overclock sysclk.
   Must double-buffer RX, keep confirmation-arming in `Messenger` after CRC, tri-state CIPO
   when CS is high (shared bus), and serve both revs (relative-pin-compatible, GPIO base 16).

## 6. Artifacts & references

- **Panel firmware:** branch `spi-bringup-step0` (PR #5). Key files: `panel_spi_custom.cpp`
  (RX drain, double-toggle note, `SPI_DIAG` idle timeout), `messenger.cpp` (`SPI_DIAG`
  counters + runt-ignore), `platformio.ini` (`pico_vXX_spidiag` envs).
- **Controller firmware** (separate repo): `LED-Display_G6_Firmware_Arena` branch
  `runtime-spi-clock-and-frame-counter` — runtime SPI clock + frames-sent counter.
- **Controller-side ceilings + this sweep:**
  `Modular-LED-Display/docs/development/g6_performance-benchmarks.md`.
- **Bench protocol:** `panel/bench/handoff-spi-highspeed-bench.md`.
- **Open issue:** startup first-message PE03 (one-time at master init; separate from
  steady-state; GitHub issue #6).

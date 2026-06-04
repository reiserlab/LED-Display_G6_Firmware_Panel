# LAB-43 — v0.3.1 two-PIO (rows + columns) scanner

Port the test-firmware **PIOFULL** dual-PIO scanner into production for **v0.3.1 only**, to settle
the v0.2.1-vs-v0.3.1 hardware choice and quantify the CPU/jitter trade. v0.3.1's contiguous row
pins (GP20–39) and column pins (GP0–19) let *both* axes be driven by 20-bit PIO `OUT` programs +
DMA; v0.2.1's rows are split by the SPI0 block (GP21–31 + GP36–44) and cannot be, so it keeps the
single-PIO-columns + CPU-GPIO-rows path and is the **regression baseline**.

## TL;DR

- **Two-PIO scanner implemented** (`panel/src/display_scan_twopio.{h,cpp}`): row SM on **PIO1**
  (GPIOBASE 16 → GP20–39), column SM on **PIO0** (GPIOBASE 0 → GP0–19), each fed by its own DMA
  channel. Per-row arm+poll; the row's hold delay = Σ(column bit-plane durations)+overhead+margin,
  so the two SMs stay in lockstep with no per-plane CPU work and no IRQ handshake. Routed into all
  four display modes (`show()`→frame, `show_row()`→row covers Triggered + Gated).
- **v0.2.1 untouched:** the new TU is empty under `PANEL_REV!=31` and `build_src_filter`-excluded
  from the `pico_v021*` envs → v0.2.1 images are **byte-identical** to the pre-change baseline
  (verified by `cmp`). All 8 envs build clean.
- **Codex cross-review** done; hardened the fault path (DMA abort + self-heal re-prime + surfaced
  `twopio_timeouts` counter), made the completion poll start-race-proof (`transfer_count==0` gate),
  switched to `pio_set_gpio_base()`, added partial-init unwind + input/pin guards.
- **HARDWARE VALIDATED — visual (2026-06-03):** `pico_v031_bcmtest` on v0.3.1 board
  `D2A9A2161F5FA2EF` side-by-side with `pico_v021_bcmtest` on v0.2.1 board `319A5199EE357F77`. The
  v0.3.1 two-PIO panel renders the full autocycle (all-off / all-on / checker / Gray_16 gradient /
  duty staircase / Oneshot) **identically to the v0.2.1 baseline** — no flicker, ghosting, or
  mapping errors; zero `TWOPIO TIMEOUT`.
- **BENCHMARKED — the win is reclaimable core-1 headroom (~90% of frame), not raw scan time.**
  Free-running scan-completion jitter is < 4 µs on both (cycle-precise; v021 slightly tighter at
  full duty), so jitter is *not* a decided win; true 2P-sync edge jitter still needs a Saleae.

## Method

Both panels flashed with their `_bcmtest` (STAGE2_SELFTEST) build and driven over USB serial. Two
benchmark commands:

- **`t`** — scan-period sweep: Gray_2 all-on at duty {0,1,16,64,192,255}, 500 ms sample each,
  reports `scan-only` (core-1 time per frame, from `display_get_scan_stats`), period, min/max.
- **`k`** — reclaimable-headroom: injects a tunable per-row busy-wait (`g_bench_inject_us`,
  STAGE2_SELFTEST-only) that simulates "core 1 doing other work." On two-PIO it is placed **after
  `dma_start`** so it overlaps the autonomous burst; on the CPU-row path it lands at row start
  (there is no autonomous burst to overlap). Sweeps inject {0,10,20,40} µs/row at Gray_2 duty 255.

## Results

### scan time (`t`, Gray_2 all-on)

| duty | v031 two-PIO scan µs (avg/min/max) | v021 CPU-rows scan µs (avg/min/max) |
|----:|:---:|:---:|
| 0   | 17 / 17 / 17    | 17 / 17 / 17    |
| 1   | 16 / 16 / 17    | 19 / 19 / 19    |
| 16  | 70 / 70 / 70    | 72 / 72 / 72    |
| 64  | 240 / 240 / 241 | 241 / 241 / 242 |
| 192 | 691 / 691 / 691 | 693 / 691 / 694 |
| 255 | 915 / 915 / 915 | 915 / 915 / 916 |

Scan time is **identical** within 0–3 µs: it is dominated by the LED-on (duty) time, which is the
same BCM math on both revs — the scanner mechanism does not change it. Refresh ceiling is therefore
also identical (~1.09 kHz at full Gray_2 duty, ~4 kHz at duty 64), duty-bound on both.

### scan-completion jitter (`j`, DWT cycle counter, 6.67 ns/cyc @ 150 MHz)

The 1 µs `time_us_32` stats are too coarse for jitter; the `j` command times each frame's scan in
DWT cycles. Per-frame scan-completion jitter (max − min) over ~1000 frames:

| pattern | v031 two-PIO avg / jitter | v021 CPU-rows avg / jitter |
|---|---|---|
| Gray_2 duty 64  | 35990 cyc (239 µs) / 525 cyc = 3.5 µs  | 36283 cyc (241 µs) / 484 cyc = 3.2 µs |
| Gray_2 duty 255 | 137240 cyc (914 µs) / 548 cyc = 3.65 µs | 137611 cyc (917 µs) / 58 cyc = 0.39 µs |

**Correction to an earlier coarse reading:** the 1 µs sweep made v031 look like "0 µs spread", but
at cycle resolution the free-running scan-completion jitter is small for *both* (≤ ~3.7 µs, < 0.4 %
of the 1 ms frame) and **v031 two-PIO is NOT lower — at full duty v021 is tighter (0.39 µs vs
3.65 µs).** Cause: v031's `wait_burst_done` uses a 3-stage poll (DMA `transfer_count` → col SM PC →
row SM PC), each exiting on a poll-loop-iteration boundary (~27 cyc/row of detection granularity),
whereas v021's single per-plane IRQ-wait catches the PIO IRQ within a couple cycles (~3 cyc/row).
This is a completion-*detection* artifact on core 1, **not** the display's actual row/column edge
timing. **Caveat:** the jitter that matters for Triggered / 2P-sync is the *trigger→row-edge*
latency on a single row burst (PIO-clock-precise on both — the donor's "0.000 µs" regime), which
needs a Saleae capture and is NOT what this free-running 20-row traversal measures. So scan jitter
is **not** a decided win for either rev from this data.

### reclaimable core-1 headroom (`k`, Gray_2 all-on duty 255)

| inject (µs/row) | v031 two-PIO scan µs | frames / 0.5 s | v021 CPU-rows scan µs | frames / 0.5 s |
|---:|:---:|:---:|:---:|:---:|
| 0  | 914 | 498 | 917  | 499 |
| 10 | 913 | 498 | 1118 | 446 |
| 20 | 913 | 499 | 1318 | 378 |
| 40 | 914 | 498 | 1719 | 290 |

**v031 two-PIO: scan time is flat** — up to 40 µs/row (~800 µs/frame) of injected "other work" is
fully absorbed by overlapping the autonomous DMA burst, with **zero** impact on the display. The
per-row burst is ~45 µs, so **~45 µs/row ≈ 900 µs/frame (~90% of the frame) is reclaimable** core-1
time. **v021 CPU-rows: scan time rises 1:1** with injected work (+10→+200 µs, +20→+400, +40→+800)
and the frame rate collapses (499→290) — **~0 reclaimable**; core 1 must feed every bit-plane.

## Conclusion / decision input

The hypothesis "v0.3.1 frees CPU resources → higher performance" is **confirmed, with the precise
mechanism**: not a smaller scan time (duty-bound, identical) but **~90% of core-1 frame time made
reclaimable** for concurrent work (V2/PSRAM command handling, OTA, trigger logic, future features)
while the panel scans autonomously. The CPU-row path offers essentially none of that: core 1 *is*
the scanner. Free-running scan-completion jitter is a wash (both < 4 µs; v021 slightly tighter at
full duty — see above); true 2P-sync edge jitter is still unmeasured (needs Saleae).

| Dimension | v0.2.1 (CPU-rows) | v0.3.1 (two-PIO) |
|---|---|---|
| Visual correctness | baseline | identical |
| Scan time / refresh ceiling | ~915 µs / ~1.09 kHz | ~915 µs / ~1.09 kHz |
| Scan-completion jitter (free-run) | 0.4–3.2 µs | 3.5–3.7 µs (tie/slightly worse) |
| Core-1 reclaimable for other work | ~0 | **~900 µs/frame (~90%)** |
| Firmware complexity | simpler (shared path) | rev-specific scan TU |

**Pick v0.3.1** primarily if core 1 will do real work alongside a live display (the reclaimable-
headroom win). The Triggered / 2P-sync edge-jitter case is plausible but **not yet demonstrated** —
measure trigger→row-edge latency on the Saleae before relying on it. If the display is core 1's sole
job, the two are functionally equivalent and v0.2.1 is simpler.

## How to reproduce

```
# build + flash both bcmtest builds (system pio; pixi is linux-only here)
PIO=/opt/homebrew/bin/pio bash panel/tools/deploy.sh D2A9A2161F5FA2EF pico_v031_bcmtest
PIO=/opt/homebrew/bin/pio bash panel/tools/deploy.sh 319A5199EE357F77 pico_v021_bcmtest
# then over USB serial (115200): 't' = scan sweep, 'k' = reclaimable-headroom sweep
```

## Still open (not decision-critical)

- Precise jitter via Saleae on the row pins (µs-timer only resolves 0 vs 1–3 µs).
- Gray_16 (4-plane) scan/headroom — would widen the low-duty per-plane-overhead gap.
- Triggered/Gated functional pass via GP45 EINT (arena/BNC trigger path).
- SPI regression on production builds: `spi_reliability.py` reject_any=0 + CIPO capture +
  LAB-41/42 PSRAM playback (the Stage 0 CIPO gate + Stage B regression).

## Branch / commits

`mreiser/lab-43-v031-twopio` (off PR #9 = main + PR #7 CIPO + trigger tools; lab-41 PSRAM/V2 merged):
consolidation merge → two-PIO backend → codex hardening → (this) bench tooling + writeup.

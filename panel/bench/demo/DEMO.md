# LAB-43 live demo — "Claude measures real hardware"

A ~1-minute live demo: a plain-English request makes Claude drive a **function
generator (AD3 W1)**, an **SPI LED controller (Teensy arena)**, and a **photodiode
(AD3 Ch1)** to characterize a real LED panel's trigger response, then build an
**interactive dashboard**. Shows latency, jitter (external *and* the chip's internal
cycle-counter), and a brightness/on-time explorer the audience can drive.

---

## ▶︎ The prompt (paste this)

> **"Run the G6 panel live trigger-characterization demo. Read
> `panel/bench/demo/DEMO.md`, do the pre-flight check, then run the live
> measurement and open the interactive dashboard."**

Claude will: `run_demo.sh check` → confirm the photodiode is live → `run_demo.sh live`
(or `sweep`) → open `dashboard.html`. If anything is flaky on stage, say **"just use
the cached dashboard"** → `run_demo.sh cached` (instant, uses last-good data).

*Natural-language alternative (more "wow", same result):* "Characterize the panel's
external-trigger response live — measure trigger→LED latency and jitter with the
photodiode, sweep the brightness levels, read the panel's internal jitter, and build
me an interactive dashboard. Recipes + pre-flight are in panel/bench/demo/DEMO.md."

---

## ✅ Pre-flight (do this BEFORE the audience — this is where time gets lost)

1. **Arena = CLEAN build.** Flash the Teensy with `teensy41`, **NOT** `teensy41-printf`.
   The `-DDEBUG_SERIAL` build prints over USB and corrupts the command protocol
   (you'll see garbage / CE01). `pio run -d ../../../LED-Display_G6_Firmware_Arena -e teensy41 -t upload`
2. **Panels = production.** `deploy.sh D2A9A2161F5FA2EF pico_v031` (v0.3.1, the two-PIO
   panel) and/or `319A5199EE357F77 pico_v021`. Build with **system `pio`**, not pixi.
3. **Trigger wiring:** AD3 **W1 → arena BNC J4** (→ U3 → EINT). *Not DIO0.*
4. **Photodiode:** powered ON, on AD3 **Scope Ch1 — both `1+` and `1−`** (`1−` to the
   TIA ground; floating `1−` = no signal). Aimed at a lit panel.
5. Run **`bash panel/bench/demo/run_demo.sh check`** → expect `PHOTODIODE LIVE`.
   If not: fix #4 (power / `1−` / aim).

(Bench IDs: v031 panel `D2A9A2161F5FA2EF`, v021 panel `319A5199EE357F77`, arena Teensy
VID 0x16C0. python3.14 at `/opt/homebrew/bin/python3.14` for DWF.)

---

## Commands

```bash
cd /Users/reiserm/Documents/GitHub/LED-Display_G6_Firmware_Panel
bash panel/bench/demo/run_demo.sh check     # pre-flight (photodiode live?)
bash panel/bench/demo/run_demo.sh cached    # instant dashboard from cached data (safe fallback)
bash panel/bench/demo/run_demo.sh sweep     # LIVE: re-measure 10-duty on-time (~30s) + dashboard
bash panel/bench/demo/run_demo.sh live      # LIVE: sweep + latency/jitter (~45s) + dashboard
```

`dashboard.html` opens in the browser. Section 2 (brightness/on-time) is interactive —
type a **time budget** and **trigger rate**; it shows which `duty_cycle` levels fit.

---

## Measured numbers (2026-06-03, for talking points)

| measurement | value | how |
|---|---|---|
| trigger → LED latency | **1.15 µs** | photodiode time-of-flight from the W1 edge |
| timing jitter (external) | **115 ns RMS** (p2p ~0.5 µs) | spread of LED onset over ~100 triggers |
| scan jitter (internal) | **3.65 µs** (v031) / 0.39 µs (v021) | panel's own DWT cycle counter |
| per-row on-time vs duty | 2.7 µs (d16) → 44.8 µs (d255), ~linear | photodiode pulse FWHM |
| **on-time < 20 µs** | **duty_cycle ≤ 110** (rec **85 → 14.8 µs**) | for 2P sub-frame sync |

## Why it's impressive (narration)

- **Plain English → instruments.** No manual scope/gen fiddling — Claude orchestrates a
  signal generator, an SPI controller, and a photodiode from one sentence.
- **Real photons, not a sim.** The LED fires 1.15 µs after each trigger, ±115 ns — measured.
- **Inside *and* out.** Claude reads the external instrument (photodiode) *and* the chip's
  internal DWT timing register, and reconciles them.
- **Live → interactive report.** The audience sets a timing budget and instantly sees which
  brightness levels fit — from data measured seconds earlier.

## Files
```
demo/
  DEMO.md            this runbook
  run_demo.sh        orchestrator (check | cached | sweep | live)
  dashboard.html     the generated interactive page (Plotly)
  bin/
    stream.py        arena control (pyserial, system python3) — robust write-only
    cap_json.py      per-duty photodiode on-time capture (python3.14 + DWF)
    jit.py           latency + jitter capture (single-shot onset spread)
    check.py         pre-flight photodiode read
    dashboard.py     builds dashboard.html from data/*.json
  data/              cached last-good measurements (duty_*.json, lat_jit.json, internal_jitter.json)
```

## Gotchas (full list)
- Arena debug build (`-printf`) corrupts the host protocol → drive write-only + drain (stream.py does this). Use the clean `teensy41` build to avoid entirely.
- Trigger pin is **W1**, not DIO0 (ad3_trigger.py defaults to DIO0).
- AD3 scope is **differential** — connect `1+` *and* `1−`.
- Triggered needs the trigger rate high enough to scan all 20 rows before the arena re-arms (≥ ~4 kHz at 200 Hz refresh); 8 kHz is the demo default.
- on-time is a **runtime pattern parameter** (the duty byte in the Triggered command) — no firmware change to retune it.

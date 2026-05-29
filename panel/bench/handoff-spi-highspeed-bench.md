# High-Speed SPI Bench Protocol — arena-master bring-up

**Status:** DRAFT — written 2026-05-29, ahead of the arena master arriving on the bench.
**Hardware:** Saleae Logic Pro 8 (SPI decode + timing) + Digilent AD3 (analog rail probe) + production panel + **arena master** (real SPI controller; `panel_master` is retired).
**Goal:** Produce the two measurements that *gate* the SPI rework decision (see
`~/.claude/plans/please-find-the-2-iridescent-squirrel.md`, Steps 1–2):
- **Capture A** — the arena master's real **SCK clock** and **CS-high (inter-transaction) gap** distribution. Feeds every per-transaction-reset design decision.
- **Capture B** — **brightness vs. SPI-error** correlation at a fixed >10 MHz clock, with a rail/ground probe, to decide whether the >10 MHz failures are **signal/power integrity** (fix the board / rate) or the **PL022 shifter** (justifies PIO+DMA).

Uses the `instruments` skill helpers. **Two Python envs:** AD3 needs `/opt/homebrew/bin/python3.14` (DWF ctypes); Saleae uses system Python with `logic2-automation`. Run them as separate processes and merge results in analysis.

> **Context from the canonical spec** (`Modular-LED-Display/docs/development/`):
> - The **slim G4.1 controller currently clocks SPI at ~5 MHz** (`g6_07-arena-firmware-interface.md` line 178). The G6 SPI-clock target is an explicitly **unmeasured bring-up item** (`g6_03-controller.md` § "Timing measurements still needed" → "SPI clock + framing latency … must be measured"); `g6_01-panel-protocol.md` line 131's "up to 30 MHz" is **aspirational, not validated**. ⇒ Capture A's first job is to learn the arena master's *actual* clock. If it's ~5 MHz, PR #3 likely already covers the deployed system and the >10 MHz work is future-proofing.
> - **Topology:** 2 SPI buses (B0=P1–5, B1=P6–10), **20 CS lines (4 per panel column)**, with **SN74HCS08 column-buffer / fan-out chips** between the Teensy CS and the panel CS (~5–10 ns prop delay each). **CIPO is shared** on these topologies (`g6_03` line 258: broadcast forbidden on shared-CIPO to avoid MISO contention) → a PIO TX backend must tri-state CIPO. The extra buffering in the path is also a signal-integrity factor for Capture B.

---

## 1. Channel assignment (SPI pins differ by rev)

| Signal | v0.2.1 GPIO | v0.3.1 GPIO | Probe |
|---|---|---|---|
| SCK  | GP34 | GP42 | Saleae **D0** |
| CS   | GP33 | GP41 | Saleae **D1** |
| COPI (MOSI) | GP32 | GP40 | Saleae **D2** |
| CIPO (MISO) | GP35 | GP43 | Saleae **D3** |
| Panel logic/+5V rail (near MCU) | — | — | **AD3 Ch1+** (differential), Ch1− to **panel GND at the same point** |
| SCK time-reference tee (optional) | | | AD3 **Ch2+** (align droop to transactions) |

Probe the rail **at the panel**, not at the supply — we want to see droop/bounce the MCU's SPI pins actually reference. Differential Ch1 (across the local decoupling cap) captures ground bounce, not just rail sag.

---

## 2. Capture A — clock + CS-high gap (Saleae)

30 MHz SCK needs heavy oversampling for clean edges: run Saleae digital at **500 MS/s** (≤4 channels) — ~16 samples/SCK-bit at 30 MHz. Timer capture, a few seconds of steady master traffic.

```python
# system python; pip install logic2-automation; enable Logic 2 automation server
from saleae import automation
mgr = automation.Manager.connect(port=10430)
dev = automation.LogicDeviceConfiguration(
    enabled_digital_channels=[0, 1, 2, 3],     # SCK, CS, COPI, CIPO
    digital_sample_rate=500_000_000,            # 500 MS/s
    digital_threshold_volts=1.65,               # 3V3 logic
)
cap = mgr.start_capture(device_configuration=dev,
    capture_configuration=automation.CaptureConfiguration(
        capture_mode=automation.TimerCaptureMode(duration_seconds=3.0)))
cap.wait()
spi = cap.add_analyzer("SPI", label="arena", settings={
    "MISO": 3, "MOSI": 2, "Clock": 0, "Enable": 1,
    "Bits per Transfer": "8 Bits per Transfer",
    "Clock State (CPOL)": "Clock is High when inactive",     # CPOL=1
    "Clock Phase (CPHA)": "Data is Valid on Clock Trailing Edge",  # CPHA=1
    "Significant Bit": "Most Significant Bit First",
})
cap.export_data_table("/tmp/spiA_frames.csv", analyzers=[spi])
cap.export_raw_data_binary(directory="/tmp/spiA_raw/", digital_channels=[0, 1])
```

**Measurements (Python):**
- **SCK frequency:** from D0 raw edges — `1 / median(diff(rising_edges_seconds))`; also report min/max to catch master jitter.
- **CS-high gap histogram:** on D1, gap = each CS **rising** → next CS **falling**; report min / p1 / median / max. **The `min` (or p1) is the hard budget** any per-transaction reset path must beat.
- **Bytes/transaction & framing:** from the SPI analyzer table — confirm 3..300-byte frames decode and the CIPO confirmation slot `{header, cmd, CRC-8}` lands at bytes 0–2.

---

## 3. Capture B — brightness vs. SPI error sweep (AD3 rail + panel serial)

Fix the arena master at a chosen clock. Run the sweep first at **~5 MHz (the controller's actual rate today — the result that decides whether anything beyond PR #3 is even needed for deployment)**, then **10, 25, 30 MHz** as future-proofing toward the 30 MHz aspiration. At each clock, step `duty_cycle` through `{1, 32, 64, 128, 192, 255}` while the master streams a steady mix (e.g. Gray_16 patterns carrying that duty byte). At each step, record three things over a fixed window (e.g. 5 s):

1. **SPI error rate** — from the panel's serial heartbeat (`messenger.cpp` prints every 1000 msgs): take Δ(`err_displayed`+`err_suppressed`) / Δ`msg_count`, and the parity/length-OK flags. (Capture B is the motivation for the Step-0 task of also exposing a cumulative PE03/PE04 counter — easier to read than booleans.)
2. **Rail droop / ground bounce** — AD3 analog on Ch1.
3. *(optional, harder)* **Saleae SPI decode error count** for an independent error oracle.

**AD3 rail capture** (`/opt/homebrew/bin/python3.14`, raw ctypes per skill §1): for a per-step droop magnitude, **record mode** at single-channel **5 MHz** for the 5 s window catches the envelope (`V_nominal − min(Ch1)` = worst droop; std = bounce). Range ~500 mV around the rail, AC-ish. If you want to *see* droop coincident with a specific transaction, use **triggered single-shot** at 50–100 MHz with the **detector trigger on Ch1 falling below a droop threshold** (skill §1.4) and Ch2 = SCK tee for alignment — but the step-level envelope below is the decisive cheap test.

**Decision rule:**
- Error rate **climbs with duty_cycle** and droop/bounce events line up with bit errors → **signal/power integrity.** PIO won't fix marginal edges; pursue decoupling / termination / drive-strength / ground-return / lower rate first. (Strongly expected given v0.2.1's SPI pins are interleaved with the row drivers; compare v0.2.1 vs v0.3.1 here.)
- Error rate **flat vs. duty_cycle** but high at >10 MHz regardless → points at the **PL022 shifter**; do the PL022+DMA spike, then PIO+DMA if needed.

Run the **same sweep on both v0.2.1 and v0.3.1** — the rev delta is itself a strong SI signal.

---

## 4. Outputs

Per the existing convention, write to `panel/bench/results/<timestamp>-spi-highspeed/`:
- `captureA_frames.csv`, `captureA_raw/` + `captureA_measurements.json` (SCK freq, CS-gap stats)
- `captureB_sweep.csv` (rows: clock × duty × rev → error_rate, droop_mV, bounce_mV) + `captureB_plot.png` (error-rate & droop vs duty, per rev/clock)
- `serial.log` per step
- `SUMMARY.md` — the measured CS-high min/median, real clock, and the SI-vs-PL022 verdict that gates Steps 2–3.

---

## 5. Pre-flight checklist
- [ ] Arena master on the bus; panel running **production** firmware (SPI ingest, not `_bcmtest`).
- [ ] Saleae D0–D3 on SCK/CS/COPI/CIPO for the correct rev (§1); Logic 2 automation enabled.
- [ ] AD3 Ch1 differential across the panel-local rail cap; `python3.14` env verified (skill §1.1).
- [ ] Panel USB serial captured for the heartbeat counters.
- [ ] Run Capture A first (need the real clock/gap before interpreting B).
- [ ] Capture B on **both** revs, at 10/25/30 MHz.

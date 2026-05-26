# V1 Panel Firmware — Bench Testing Handover

**Target session:** Tuesday bench day
**Hardware:** Digilent Analog Discovery 3 (AD3) + Saleae Logic Pro 8 + photodiode + production panel (RP2354B, v0.3.1)
**Goal:** Automated validation of the V1 firmware feature-complete release (error codes + Triggered + Gated)
**Forward-looking:** This run's automation scripts should be factored into a reusable bench-test skill in a follow-up session.

---

## 1. Context for the next session

### What's being tested
V1 panel firmware now ships:
- **Existing**: COMM_CHECK (0x01), Oneshot/Persistent × Gray_2/Gray_16 (0x10, 0x11, 0x30, 0x31)
- **New this cycle**: Error display via predefined-pattern flash subsystem (0xC2), Triggered (0x12, 0x32), Gated (0x13, 0x33)
- **Above-spec additions**: PE01-PE05 auto-trigger from validity-gate failures; 5 s rate-limit; CIPO-unchanged-during-error-window; queue-delivered cross-core error path with full snapshot/restore

### Key documents to read first
1. `/Users/reiserm/.claude/plans/as-a-reference-the-peppy-puffin.md` — the implementation plan (both features). Read the "Known caveats / departures from spec" tables at the top of each section; those are the lines this testing must defend or revise.
2. `/Users/reiserm/Documents/GitHub/Modular-LED-Display/docs/development/g6_01-panel-protocol.md`:
   - § 0x12 / 0x13 / 0x32 / 0x33 (Triggered, Gated)
   - § Timing considerations for Triggered & Gated (`duty_cycle` dependence) — table values here are theoretical; Tuesday's job is to replace them with measured numbers
   - Line 363-394 (PE error display)
3. `panel/src/display.cpp` — comments inside `show_row()` summarize the operational model

### Repo state
Both repos have **uncommitted working trees**:
- `LED-Display_G6_Firmware_Panel`: all V1 firmware changes (error codes, Triggered, Gated)
- `Modular-LED-Display`: spec doc additions for error codes + timing considerations

Don't commit until bench validation passes. Both repos are on their primary working branches.

### Builds
Pre-built UF2s in the firmware repo:
- `panel/.pio/build/pico_v031/firmware.uf2` — production (SPI ingest enabled, no selftest commands)
- `panel/.pio/build/pico_v031_bcmtest/firmware.uf2` — selftest mode (USB serial commands, no SPI)
- `panel/.pio/build/pico_v021/firmware.uf2` — v0.2.1 panel rev (probably not needed Tuesday)

If source changes during the session, rebuild with `pio run -e <env>` from inside `panel/`.

---

## 2. Hardware setup

### AD3 (function generator + arbitrary stimulus)
**Role**: drive EINT (GP45 on panel) with controlled rising-edge bursts and level holds. Also useful for injecting noise or signal-conditioning tests.

- **W1** (wavegen) → panel EINT pin (GP45 — see [g6_02-led-mapping.md](/Users/reiserm/Documents/GitHub/Modular-LED-Display/docs/development/g6_02-led-mapping.md) for the connector pad mapping)
- **GND** → panel GND
- AD3 scope channels — **avoid**. They're noisy; defer to Saleae.

### Saleae Logic Pro 8 (digital + analog capture)
**Role**: ground-truth capture of EINT, row pins, optional SPI bus, and photodiode analog output. 8 digital + 8 analog channels at 500 MS/s digital / 50 MS/s analog. Stores to disk; Python can replay/measure.

Suggested channel assignment (USER TO CONFIRM PIN-TO-PAD MAPPING — varies by panel rev):

| Saleae | Signal | Notes |
|---|---|---|
| D0 | EINT (GP45) | Trigger source; sample at ≥1 MS/s for ns-level edge timing |
| D1 | One row pin (e.g., ROW[0] = GP20 on v0.3.1) | LOW = row ON; gives us "row drive window" timing |
| D2 | A second row pin (e.g., ROW[10] = GP30) | Lets us verify row sequencing across the panel without moving probes |
| D3-D6 | SPI MOSI/MISO/SCK/CS (if testing protocol-level Triggered/Gated via panel_master) | Optional |
| A0 | Photodiode amplifier output | LED-on ground truth; see below |

### Photodiode (the new piece)
**Role**: detect actual LED-on transitions independent of GPIO state, so we measure what the panel *displays*, not what the firmware *says* it displays.

- Single photodiode positioned over **one specific LED** (e.g., D50 = pixel[0,0] on the first row). Choice of pixel determines what test is meaningful — pick a row+col that exercises the row currently being tested.
- Amplifier: TIA (transimpedance) or a simple op-amp follower depending on what's available on the bench. Output to Saleae A0.
- Sample rate: 1-10 MS/s on Saleae analog is fine — LED edges are µs-scale, not ns.
- **Calibration step on Tuesday**: drive an all-on Persistent pattern at duty=255, then duty=0, and measure photodiode V_ON vs V_OFF. Pick a threshold (~50% between) for binary edge detection in analysis scripts.

### Panel — host serial connection
USB CDC at 115200 baud. macOS device name typically `/dev/cu.usbmodem*`. Python scripts should auto-detect; otherwise hard-code.

---

## 3. Automation strategy

### Available skill
The `instruments` skill (already in your skill library) provides Python wrappers around:
- AD3: wavegen patterns, scope, digital I/O — via `dwfpy` or the WaveForms SDK
- Saleae Pro 8: capture start/stop, channel config, export to CSV/sigrok — via the Saleae Python API

When you spawn the next session, invoke this skill explicitly so it loads the right helpers. Example trigger phrases that should activate it:
- "capture EINT with Saleae"
- "drive the AD3 to generate 20 rising edges"
- "measure the photodiode"

### Orchestration pattern (test = one Python function)
Each test routine should:
1. **Arm**: configure Saleae channels + trigger condition + sample rate
2. **Set panel state**: open serial, send selftest command (e.g., `T\n`), close serial
3. **Stimulate**: AD3 wavegen fires the planned EINT pattern
4. **Capture**: Saleae records (typically 100 ms - 1 s window)
5. **Analyze**: load capture, compute timing measurements, assert pass/fail
6. **Persist**: save raw capture + measurement dict to `panel/bench/results/<test_name>/<timestamp>/`

Pseudocode template:

```python
def test_triggered_polarity():
    """P0: confirm rising-edge of EINT fires the row, not falling."""
    saleae.set_channels(digital=[D0_EINT, D1_ROW0], analog=[A0_PHOTODIODE])
    saleae.set_trigger(channel=D0_EINT, edge="rising")
    saleae.set_capture_window(pre_ms=10, post_ms=10)
    saleae.arm()

    panel_serial_send("T\n")          # queue Triggered all-on Gray_2
    time.sleep(0.1)                    # let core 1 install the pattern

    ad3.wavegen_single_pulse(channel="W1", high_v=3.3, low_v=0.0,
                              rise_ns=10, width_ms=1)
    capture = saleae.wait_done()

    eint_rising = capture.digital[D0_EINT].rising_edges()[0]
    row_low = capture.digital[D1_ROW0].falling_edges()[0]   # LOW = row ON
    led_on = capture.analog[A0_PHOTODIODE].first_threshold_cross(V_THRESH)

    delay_row = row_low - eint_rising
    delay_led = led_on - eint_rising

    # PASS criteria
    assert 0 < delay_row < 5e-6,  f"row LOW should follow rising edge within 5 µs (got {delay_row*1e6:.2f} µs)"
    assert 0 < delay_led < 5e-6,  f"LED on should follow rising edge within 5 µs (got {delay_led*1e6:.2f} µs)"
    # spec line 852 warned of falling-edge fire from prototype; we explicitly reject that
    eint_falling = capture.digital[D0_EINT].falling_edges()
    if eint_falling and (led_on - eint_falling[0]) < (eint_rising - eint_falling[0]):
        fail("LED fired on FALLING edge first — possible ringing per spec line 852")

    save_result("triggered_polarity", capture, {"delay_row_us": delay_row*1e6,
                                                  "delay_led_us": delay_led*1e6})
```

The skill should provide `panel_serial_send`, `save_result`, channel constants, etc. — those are generic enough to be helpers.

### Repeat-and-summarize for sweeps
For tests that sweep a parameter (e.g., Triggered at 100 Hz / 1 kHz / 8 kHz / 22 kHz), wrap the test function and aggregate results into a single CSV + summary plot. Same for `duty_cycle` sweeps.

---

## 4. Test matrix (automation-prioritized)

### P0 — must pass before claiming V1 done

| # | Test | Stimulus | Capture | Pass criterion |
|---|---|---|---|---|
| 1 | **Firmware boots, predef blob valid** | Power-on + USB | Serial | `predef: ok, 200 slots (1994 B)` printed within 2 s of boot |
| 2 | **EINT polarity — rising-edge fires** | AD3 single rising pulse (1 ms width, 3.3 V) after `T` | Saleae D0 (EINT) + D1 (row[0]) + A0 (photodiode) | Row LOW + photodiode rise both follow EINT rising edge within 5 µs; no LED transition correlated with EINT falling edge |
| 3 | **Triggered consumption: 20 edges = 1 frame** | AD3 burst of 20 rising edges at 1 kHz after `T` | Saleae all rows + photodiode | Each edge produces one row drive (row LOW window); 20 edges total; panel dark after edge 21 |
| 4 | **Triggered at 8 kHz with operational duty_cycle** | Modify `T` cmd to push duty=85 (or panel_master 0x12 at duty=85), AD3 burst at 8 kHz | Saleae | Per-row LED-on window 12-18 µs (~10-15% of 125 µs); no edges missed across 100 bursts |
| 5 | **All glyphs render** | `e 0; e 1; e 2; e 3; e 4; e 5; e 100; e 50` via serial, one per ~1.5 s | Camera frame or photodiode array | Eyeball check (manual) — automated version requires multi-pixel sensor |

### P1 — would degrade UX

| # | Test | Stimulus | Capture | Pass criterion |
|---|---|---|---|---|
| 6 | **Gated brightness matches Persistent** | `g` (Gated checkerboard duty=192) with AD3 holding GP45 HIGH for 100 ms, then compare to Persistent at same pattern | A0 photodiode integration | Mean photodiode value during HIGH window within 10% of Persistent reference |
| 7 | **Gated drop latency** | AD3 generates HIGH (100 ms) → LOW step | Saleae D0 (EINT) + A0 (photodiode) | Photodiode drops below threshold within 60 µs of EINT falling (50 µs row-drive worst case + 10 µs margin) |
| 8 | **Error display interrupts + restores Triggered** | `T` + AD3 firing edges at 100 Hz; midway through (e.g., after 10 edges), inject `e 2` | Saleae all rows + photodiode | PE02 glyph visible for ~1 s; then Triggered resumes from edge 11 |
| 9 | **Error rate-limit (1 per 5 s)** | panel_master injects 10 bad-parity messages over 1 s | Saleae photodiode + serial heartbeat | Exactly 1 error glyph displayed; serial heartbeat shows `err_displayed:1, err_suppressed:9` |
| 10 | **CIPO unchanged during error window** | panel_master sends valid 0x10, captures CIPO; injects error; sends another 0x10 during error window, captures CIPO | Saleae SPI channels | Second CIPO capture identical to first |

### P2 — verify if time permits

| # | Test | Stimulus | Capture | Pass criterion |
|---|---|---|---|---|
| 11 | **Triggered frequency sweep** | AD3 sweeps 100 Hz → 50 kHz, 20-edge bursts | Saleae | First missed edge identifies the actual per-row drive limit; compare to spec table |
| 12 | **duty_cycle sweep — measure LED-on width** | duty=1, 16, 64, 85, 128, 192, 255 (via modified `T` command rebuilt each time, or panel_master) | Saleae A0 photodiode | Per-row LED-on width measured; update spec table with empirical values |
| 13 | **865 ns trigger-to-LED latency** (spec line 775) | AD3 single rising edge | Saleae D0 + A0 (high time resolution) | edge-to-photodiode-rise delay measured; expected ~1 µs (includes photodiode response time) |
| 14 | **EINT signal quality** (spec line 852) | AD3 sources rising edge; capture both source and panel-side EINT | Saleae D0 + D7 (panel-side EINT via probe) | No falling-edge ringing that would re-fire a row |
| 15 | **Cross-core soak** | panel_master streams mixed valid + invalid traffic for 60 s | Saleae photodiode + serial | No panel hangs; `frames_skipped` near 0; visible state matches commanded state |

---

## 5. Output format

Every test produces a directory under `panel/bench/results/<timestamp>-<test_name>/`:
- `capture.sal` — Saleae raw capture (re-openable in Logic 2)
- `capture.csv` — Saleae export for Python analysis
- `measurements.json` — computed timing values, pass/fail, parameters
- `plot.png` — matplotlib summary (waveforms + annotated thresholds)
- `serial.log` — panel USB serial output during the test

A top-level `panel/bench/results/summary-<timestamp>.md` aggregates pass/fail across all tests + key timing measurements (per-row drive at each duty, gate-drop latency, edge-to-LED latency).

---

## 6. Suggested file layout in this repo

```
panel/bench/
  handoff-v1-bench-testing.md      ← this file
  bench_test.py                    ← main entry: parses CLI args, runs requested tests
  bench/
    common.py                      ← panel_serial_send, save_result, channel constants
    fixtures.py                    ← AD3 setup, Saleae arm/trigger, photodiode calibration
    test_p0_polarity.py            ← test #2
    test_p0_consumption.py         ← test #3
    test_p0_8khz_duty.py           ← test #4
    test_p1_gated_brightness.py    ← test #6
    test_p1_gated_drop.py          ← test #7
    test_p1_error_interrupt.py     ← test #8
    test_p1_rate_limit.py          ← test #9
    test_p1_cipo_unchanged.py      ← test #10
    test_p2_freq_sweep.py          ← test #11
    test_p2_duty_sweep.py          ← test #12
  results/                          ← .gitignore'd
    .gitignore
```

Suggested CLI:
```
python3 panel/bench/bench_test.py --priority p0
python3 panel/bench/bench_test.py --test triggered_polarity
python3 panel/bench/bench_test.py --priority p1 --skip rate_limit
python3 panel/bench/bench_test.py --calibrate-photodiode    # one-time setup
```

The `--calibrate-photodiode` mode is essential — establishes V_ON / V_OFF / V_THRESH for the current setup before any other test trusts the analog channel.

---

## 7. Selftest serial commands (panel-side)

Already in firmware (build `pico_v031_bcmtest`):

| Cmd | Effect |
|---|---|
| `i` | Print boot banner |
| `?` or `h` | Help |
| `b<float>` | Set BCM base_T in µs (default 3.0) |
| `r<int>` | Set scan period in µs (default 1000) |
| `p<row>,<col>` | Light single pixel |
| `e<slot>` | Raise error glyph (slot 0=ERR, 1-5=PE01-05, 100=CE00) |
| `T` | Push Triggered all-on Gray_2 (currently hard-coded duty=255 — modify in main.cpp for sweeps) |
| `g` | Push Gated checkerboard (duty=192) |
| `t` | Scan-period timing benchmark (existing) |

**To inject duty_cycle-varied Triggered patterns for sweep tests**: either (a) modify the `T` handler in `panel/src/main.cpp` and rebuild, or (b) use panel_master to send real 0x12 commands with arbitrary duty_cycle bytes. Approach (b) is more realistic but requires panel_master to be on the SPI bus (production firmware needed, not bcmtest).

---

## 8. Skill extraction for future sessions

After Tuesday's session lands and the test scripts are working, the natural next step is to factor the orchestration pattern into a reusable skill. Candidate name: **`panel-bench-test`**.

Skill scope:
- Wrap the `instruments` skill (AD3 + Saleae) with panel-specific defaults (channel assignments, EINT pin location, photodiode threshold calibration flow)
- Provide a `BenchTest` base class with `arm()`, `stimulate()`, `capture()`, `analyze()`, `report()` hooks
- Standard report formats (JSON + plot + markdown summary)
- Convention for `results/` directory layout
- Pre-built test functions for the common assertions (edge-to-row latency, photodiode threshold cross, etc.)

The skill itself lives at `~/.claude/skills/panel-bench-test/` (or wherever the user prefers). The panel-specific calibration constants (channel assignments, EINT pin) live in `panel/bench/` as configuration; the skill consumes that config but doesn't hard-code it.

This Tuesday session should produce the working test scripts first; skill extraction happens after, when the patterns have stabilized.

---

## 9. Open questions for the bench session

These came up during implementation and need empirical answers Tuesday:

1. **EINT polarity on production hardware**: spec says rising-edge; prototype rig showed falling-edge fire due to ringing (spec line 852). Production board may or may not reproduce.
2. **Actual per-row drive time at each (duty_cycle, gray_level)**: the spec table values are derived from `base_T = 3 µs` and the BCM weight sum. Confirm against photodiode measurements; update the spec doc with empirical numbers.
3. **865 ± 17 ns trigger-to-LED latency** (spec line 775): re-confirm on production board (prototype measurement).
4. **Cross-core stability under mixed traffic**: implementation review found no race, but soak testing under realistic panel_master load is the real proof.
5. **EINT input impedance behavior with pull-down**: confirm the function-gen source impedance + pull-down combo doesn't degrade rising-edge sharpness. If it does, switch to floating + external pull (or just no pull) and document.

---

## 10. What to commit after Tuesday

Assuming P0 + P1 all pass:
- **Firmware repo** (`LED-Display_G6_Firmware_Panel`): commit the V1 feature-complete changes. Suggested split:
  - One commit for error-display subsystem (predef_patterns + 0xC2 + error state machine + selftest `e`)
  - One commit for Triggered + Gated (protocol surface + EINT + show_row factor + state machines + selftest `T` and `g`)
  - One commit for the test scripts under `panel/bench/`
- **Spec repo** (`Modular-LED-Display`): commit the timing-considerations addition + any empirical-numbers updates to the table
- Open PRs against both

If anything in P0 fails, do **not** commit firmware — fix and re-test.

---

## Quick checklist

- [ ] Read this doc + the implementation plan
- [ ] Confirm AD3 + Saleae + photodiode are connected per § 2
- [ ] Activate `instruments` skill
- [ ] Run photodiode calibration (`--calibrate-photodiode`)
- [ ] Run P0 tests; abort and fix if any fail
- [ ] Run P1 tests
- [ ] Run P2 tests if time permits
- [ ] Update the spec doc's timing table with empirical numbers
- [ ] Commit firmware + spec if all P0/P1 pass
- [ ] Open follow-up session to extract the `panel-bench-test` skill

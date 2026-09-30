# IR-red panel — draft BOM/layout changes for v0.4r2

Status: draft for review, 2026-09-19. Baseline is
`panel_rp2354_20x20_ir-red_v0p4/production/v0p4r1` in
`reiserlab/LED-Display_G6_Hardware_Panel` @ main (committed 2026-08-27,
"4-color and IR panels v0.4"). Companion analysis:
`hw-column-resistor-analysis.md`.

Goals: (1) checkerboard red/IR instead of stripes; (2) more red output for
CsChrimson stimulation while staying invisible to the fly; (3) column
resistors inside their power rating.

## Baseline (v0p4r1) as built

| item | as built |
|---|---|
| LED_T0, LED_T2 | Kingbright APHHS1005SURCK, hyper red 645 nm peak / 630 nm dom (LCSC C2852592) |
| LED_T1, LED_T3 | Inolux IN-S42CTQIR, 850 nm IR (LCSC C6879329) |
| R_T0…R_T3 (R9–R28) | 100 Ω 0201 50 mW, Yageo RC0201FR-07100RL (LCSC C77623), all four banks |
| red current / resistor power | ≈27 mA / 64–72 mW (128–144 % of rating) |
| IR current / resistor power | ≈32–33 mA / 91–99 mW (182–198 % of rating) |
| physical arrangement | T0+T2 on even columns, T1+T3 on odd columns → **vertical red/IR stripes** |

## Change 1 — checkerboard (BOM only)

Bank parity on the board is T0 = (even row, even col), T1 = (even, odd),
T2 = (odd, even), T3 = (odd, odd). Red/IR on T0+T2 gives columns; a
checkerboard needs the diagonal pairs:

| bank | v0p4r1 | **v0p4r2** |
|---|---|---|
| LED_T0 | red C2852592 | red C2852592 |
| LED_T1 | IR C6879329 | IR C6879329 |
| LED_T2 | red C2852592 | **IR C6879329** |
| LED_T3 | IR C6879329 | **red C2852592** |

No schematic, layout or firmware change. Consequences to note in docs/tools:
red = firmware channels 0 and 3 (`sch_col % 4 ∈ {0,3}`), IR = channels 1 and 2;
`panel_test.py led 0|3` drives red.

## Change 2 — column resistors: value and package

The 0201 footprint cannot carry even today's current within rating (see
analysis §2–3); the highest-rated 0201 is 63 mW. Move R9–R28 to **0402,
0.2 W high-power** parts. Per-bank values follow the new bank→colour map:

| bank (refs) | colour | value | part (primary / alternate) | I_peak | resistor power (full-field) | % of 0.2 W |
|---|---|---|---|---|---|---|
| R_T0 (R9, R13, R17, R21, R25) | red | **33 Ω** | Vishay CRCW040233R0FKEDHP / Panasonic ERJ‑PA2F33R0X | 65–72 mA | 126–152 mW | 63–76 % |
| R_T3 (R12, R16, R20, R24, R28) | red | **33 Ω** | same | | | |
| R_T1 (R10, R14, R18, R22, R26) | IR | **56 Ω** | CRCW040256R0FKEDHP / ERJ‑PA2F56R0X | 50–55 mA | 125–154 mW | 63–77 % |
| R_T2 (R11, R15, R19, R23, R27) | IR | **56 Ω** | same | | | |

Ranges cover "own colour only" (higher current, lower row-sink drop) to "red +
IR all on". LCSC stocks the CRCW‑HP series (e.g. C844715); confirm the 33 Ω
and 56 Ω codes are in stock at order time or use the Panasonic alternates
(Mouser/DigiKey, hand-placed or consigned).

Effect on output at the same panel timing (1 kHz, 45 µs row slot):

| | red photons vs v0p4r1 | IR photons vs v0p4r1 |
|---|---|---|
| 33 Ω red / 56 Ω IR | **≈2.5×** | ≈1.6× |
| 33 Ω red / 100 Ω IR (IR unchanged) | ≈2.5× | 1× (80 mW, 40 % of rating) |
| 22 Ω red / 56 Ω IR (aggressive) | ≈3.2× | ≈1.5× — row current 1.35 A, sink drop 0.74 V; pattern cross-talk grows; not recommended without bulk-cap change |

LED margins at the recommended point: red 72 mA is 39 % of the Kingbright
185 mA pulse rating (avg 7 mW vs 75 mW P_D); IR 55 mA is 79 % of the Inolux
70 mA *DC* rating and far below its pulse rating (avg 4 mW vs 140 mW P_D).

If IR brightness is already sufficient, choose the 100 Ω IR option: it halves
row current and sink drop, which also reduces the red channel's dependence on
whether IR is lit (red drops ~9 % from red-only to red+IR at 56 Ω, ~5 % at
100 Ω).

## Change 3 — layout (shared by all three v0.4 variants)

- Footprint R9–R28: `Resistor_SMD:R_0201_0603Metric` → `Resistor_SMD:R_0402_1005Metric`, same centre and rotation, bottom side.
- Pad-to-pad clearance after the swap is ≥0.14 mm everywhere (0.19–0.24 mm typical) — acceptable at JLCPCB's 0.127 mm minimum; nudge R21 if a 0.2 mm rule is wanted.
- Ten sites need a via or short track stub moved out from under the longer pads: **R10, R11, R12, R17, R20, R22, R24, R25, R26, R27** (worst: R27 via overlaps by 0.10 mm; R10/R12 a track by 0.06 mm). The other ten are clear.
- No component relocation. 0603 was evaluated and does **not** fit (pads land on the SOT‑23‑5 driver pins at all 20 sites).
- Run DRC at 0.127 mm copper clearance after the edits.

## Change 4 — optional, recommended if Change 2 is adopted

- **5 V bulk capacitance**: 18 × 10 µF 0402 (≈90 µF effective at 5 V bias) gives ≈0.6 V droop during a 45 µs, 1.15 A row pulse (all-on, red 33 Ω + IR 56 Ω). Add 2–4 × 22–47 µF (0805/1206 X5R, or a polymer cap) close to the column drivers U3–U22 to hold droop under ~0.2 V. Without this, brightness will sag a few percent across each pulse and vary with pattern content.
- 5 V supply average current rises only to ≈50 mA for the LED array (1.15 A × 4.5 %); no change to power entry or the arena bus.

## Unchanged

MCU, PSRAM, 40× UCC27517 drivers, decoupling, connectors, LED footprints (0402), all other resistors, firmware. The 660 nm LED question was investigated separately: no 660 nm part exists in 0402; the 645 nm Kingbright stays.

## Docs/tooling to update with this revision

- `Modular-LED-Display/docs/development/g6_02-led-mapping.md` § current-limit resistors: per-variant table (160 / 33·56 / 68·110 Ω), 0402‑HP package, and the bank→colour map for both v0.4 variants.
- `LED-Display_G6_Firmware_Panel/panel/tools/panel_test.py` help text: red = channels 0,3 on IR-red v0.4r2.

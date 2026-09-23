# Column current-limit resistors (R9–R28): dissipation, package options, PCB room

Status: analysis, 2026-09-19. Covers the three panel variants ordered from
`reiserlab/LED-Display_G6_Hardware_Panel` @ main:

| variant | directory | LEDs (bank T0 / T1 / T2 / T3) | column resistors |
|---|---|---|---|
| all-green production | `panel_rp2354_20x20_v0p3` (v0p3r1) | Starsealand XL0402YGC 570 nm ×4 | 160 Ω ×20 (Yageo RC0201FR-07160RL, C851657) |
| IR-red | `panel_rp2354_20x20_ir-red_v0p4` (v0p4r1) | red / IR / red / IR — Kingbright APHHS1005SURCK (C2852592), Inolux IN-S42CTQIR 850 nm (C6879329) | 100 Ω ×20 (Yageo RC0201FR-07100RL, C77623) |
| four-color | `panel_rp2354_20x20_four-color_v0p4` (v0p4r0, "G6_PBGY") | purple / blue / green / yellow — Yongyu YY0402PU / BL / GR / YE | 68 Ω (T0,T1,T2; C138127), 110 Ω (T3; C295716) |

All are 0201, 50 mW, ±1 % thick film.

## 1. Why the column resistor is the hot part

The matrix is scanned one schematic row at a time; an LED is on for at most
its row slot (45 µs of the 1000 µs frame at full brightness, i.e. 4.5 %). The
**column** resistor, however, conducts whenever *any* lit LED in its column is
in the active row. For a full-field pattern that is 20 rows × 45 µs = **90 %
of the time**. So the resistor sees ~20× the LED's duty, and its dissipation
is set by the peak current, not the LED's average.

Conduction fraction in general = (rows lit in that column / 20) × (45 µs / 50 µs) × (duty_cycle / 255). Full-field at duty 255 is the worst case; a quadrant stimulus is ~45 %; the IR bank used as camera illumination is full-field, continuously.

## 2. Operating points and dissipation per variant

Model: 5 V rail; UCC27517 column driver sourcing 1.3 Ω, row driver sinking
0.55 Ω carrying the whole row's current; LED V_F from datasheets (Kingbright
1.95 V @ 20 mA, Inolux 1.55 V @ 70 mA, Starsealand 1.95–2.30 V) with a small
dynamic resistance; Yongyu parts publish no specs, so V_F is assumed by
chemistry (InGaN purple/blue/green ≈ 3.2/2.95/2.95 V, AlInGaP yellow ≈ 2.05 V).
Resistor power = 0.9 × I² × R (full-field). Rating basis: 50 mW at 70 °C
ambient, derating linearly to zero at 155 °C.

| variant / pattern | bank | R | I_peak | row current | P_resistor | % of 50 mW |
|---|---|---|---|---|---|---|
| green, all-on | all | 160 Ω | 16.9 mA | 0.34 A | 41 mW | **82 %** |
| IR-red, red + IR all-on | red | 100 Ω | 26.7 mA | 0.59 A | 64 mW | **128 %** |
| | IR | 100 Ω | 31.8 mA | | 91 mW | **182 %** |
| IR-red, IR only (camera) | IR | 100 Ω | 33.2 mA | 0.33 A | 99 mW | **198 %** |
| IR-red, red only | red | 100 Ω | 28.2 mA | 0.28 A | 72 mW | **144 %** |
| four-color, all-on | purple | 68 Ω | 22 mA | 0.48 A | 30 mW | 59 % |
| | blue, green | 68 Ω | 25 mA | | 39 mW | 78 % |
| | yellow | 110 Ω | 24 mA | | 57 mW | **114 %** |

Reading: the green production panel is inside its rating with little margin;
the IR-red panel is over rating on both banks as built (the IR bank badly so,
because the 850 nm die's low V_F leaves 3.5 V across 100 Ω); the four-color
yellow bank is over. None of this is catastrophic — thick-film resistors
tolerate ~2× overload for a long time with drift and shortened life — but it
means **there is no room to raise current on the existing 0201 footprint.**
To stay at 50 mW the IR-red board would need ~130 Ω (red, 21 mA) and ~180 Ω
(IR, 19 mA) — i.e. *less* light than today.

The LEDs themselves are not the constraint: average LED power is 2–4 mW per die at these currents (4.5 % duty), and pulsed ratings are 185 mA (Kingbright), 500 mA (Inolux, ≤1 % duty), 30 mA (Starsealand).

## 3. Do higher-rated resistors exist in the same package?

| package | standard rating | highest available | notes |
|---|---|---|---|
| **0201** | 50 mW (1/20 W) | **63 mW** — Stackpole RMCP0201 (DigiKey/Mouser, not stocked at LCSC) | +26 % only; does not cover any of the over-rating cases above |
| 0402 | 62.5 mW (1/16 W) | **200 mW** — Vishay CRCW0402‑HP e3 (P70 = 0.2 W; LCSC stocks the series, e.g. C844715 = 10 kΩ; per-value stock must be checked), Panasonic ERJ‑PA2 (0.2 W, Mouser/DigiKey/RS) | 4× the 0201 rating; CRCW‑HP note: "specified power rating requires R_th ≤ 110 K/W" — i.e. reasonable copper on the pads |
| 0603 | 100 mW | 330 mW — CRCW0603‑HP | |
| 0805 | 125 mW | 500 mW — CRCW0805‑HP | |

Conclusion: **no 0201 part solves this.** 0402‑HP at 0.2 W is the smallest
package that gives real headroom (≈150 mW usable at 75 %).

## 4. PCB room for a larger footprint

Parsed from `panel_rp2354_20x20.kicad_pcb` in all three variant directories
(KiCad `version 20260206`). Findings:

- **The three layouts are identical**: 557 footprints each, zero position
  differences between ir-red, four-color and v0.3 green. One layout fix
  therefore applies to all three variants.
- R9–R28 are all `Resistor_SMD:R_0201_0603Metric`, **bottom side**, rotated
  ±90°, each 0.30–0.37 mm (courtyard gap) from its column driver's SOT‑23‑5
  (U3–U22) and typically 0.5 mm from the next driver. They sit in the driver
  cluster, not near the LED array.

Growth test — replace with KiCad `R_0402_1005Metric` (pads 0.54 × 0.64 mm at
±0.51 mm) or `R_0603_1608Metric` (pads 0.875 × 0.95 at ±0.79 mm) at the same
centre and rotation, and measure clearance to other footprints' pads, to
foreign B.Cu tracks and to foreign vias:

| footprint | pad‑to‑pad (worst / typical) | sites with foreign copper inside min clearance (0.127 mm) | verdict |
|---|---|---|---|
| 0402 | 0.14 mm (R21) / 0.19–0.24 mm | **10 of 20**: R10 (track −0.06, via 0.03), R11 (via 0.05), R12 (track −0.06, via 0.03), R17 (via 0.01), R20 (via −0.03), R22 (via −0.04), R24 (track 0.06), R25 (via 0.06), R26 (via −0.04), R27 (via −0.10). Clear: R9, R13, R14, R15, R16, R18, R19, R21, R23, R28 | **fits in place**; ten short track/via moves, none involving another part |
| 0603 | 0.00–0.08 mm at all 20 | all | **does not fit** — pads land on the driver pins; would need the 20 drivers moved |

(Negative numbers = overlap. Distances are copper-to-copper; JLCPCB's minimum is 0.127 mm, 0.2 mm is comfortable.)

So the footprint change is: **20 resistors, 0201 → 0402, in place, plus ~10
local re-routes of a via or a short track stub.** No component relocation.

## 5. Recommendation

1. **Change R9–R28 to 0402 footprints in the shared layout** (all three
   variants inherit it) and populate with 0.2 W high-power parts
   (CRCW0402‑HP or ERJ‑PA2). Even the green production panel benefits:
   41 mW becomes 20 % of rating instead of 82 %.
2. Values per variant (full-field, 75 % of 0.2 W as the ceiling):
   - **IR-red**: red 33 Ω (≈65–72 mA, 126–152 mW), IR 56 Ω (≈50–55 mA,
     125–154 mW) — or keep IR at 100 Ω (30 mA, 80 mW) if IR is already bright
     enough. See `hw-irred-v0p4r2-bom-changes.md`.
   - **green**: keep 160 Ω (no brightness request); the part change alone
     fixes the margin.
   - **four-color**: keep 68/110 Ω unless a brightness request comes; yellow
     goes from 114 % to 28 %.
3. If current is raised on the IR-red board, also check the 5 V bulk
   capacitance (18 × 10 µF 0402, ≈90 µF effective): at 1.15 A row current the
   45 µs pulse droops ≈0.6 V. A few 22–47 µF near the column drivers would
   hold that under 0.2 V.
4. Longer term, the right fix for brightness *and* pattern-independence is a
   constant-current column driver instead of resistor + gate driver; that is
   an architecture change, not a BOM change, and is out of scope here.

## 6. Not recommended

- Staying on 0201 and raising R to get back inside 50 mW — cuts light 20–40 %.
- 0603 or larger — requires moving the driver ICs.
- Two 0201 in parallel — no room, same issue.
- Moving the current limit to the row side — LED current would depend on how
  many LEDs in the row are lit.

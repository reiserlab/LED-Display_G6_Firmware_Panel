#!/usr/bin/env python3
"""pulse_sweep — time-averaged brightness vs LED pulse timing on a G6 panel.

Rig
---
  * One G6 panel flashed with a pico_v0*_bcmtest env (STAGE2_SELFTEST serial
    commands b/r/a/c), USB to the host. Current-limit resistors fixed; only the
    pulse timing (base_T, frame period, duty) is varied.
  * One arena_12-18 controller (Teensy 4.1) used purely as an I2C bridge
    (GET_I2C_SCAN 0xB0 / I2C_TRANSFER 0xB1), USB to the host, with the LAB-211
    light sensors on its Qwiic jack J2: TSL2591 (0x29), VEML7700 (0x10),
    AS7343 (0x39), optionally behind a PCA9548 mux. Sensors face the panel at
    fixed geometry; room light stable or dark.
  * Sensor drivers are imported from the arena repo branch
    claude/qwiic-i2c-validation-cac1ce (PR #58): tests/qwiic_sensors.py
    (Sampler) + tests/transport.py (SerialTransport). Point --arena-repo at a
    checkout of that branch; by default the main checkout and its worktrees
    are searched.

Per condition: send b<base_us>, r<period_us>, then the hold (a<pct> or
c<ch>,<pct>). The hold goes last because base_T is baked into the pattern
when it is pushed. Wait --settle, take --samples sensor rounds (~6 Hz, paced
by the AS7343 frame), write one row per sample to --out and one row per
condition to <out>_summary.csv. A reference condition (b=3, r=1000, pct=100,
same channel) is re-run every --ab-every conditions so drift can be divided
out; a dark reading (a0) opens the run. LED on-time per row per frame is
b x 15 x duty/255 (Gray_2, weight 15); on-fraction = that / r. Production
today is b=3, r=1000, pct=100 -> 45 us per 1000 us = 4.5 %.

Phase 1 — pct linearity at production timing, every channel:
    pulse_sweep.py --panel-serial 1C8D27443DD6020A --arena-port /dev/cu.usbmodem1234 \\
        --preset linearity --channel all --out phase1.csv

Phase 2 — constant 4.5 % on-fraction, pulse width 4.5 us .. 225 us:
    pulse_sweep.py --panel-serial 1C8D27443DD6020A --arena-port /dev/cu.usbmodem1234 \\
        --preset constduty --channel 2 --out phase2.csv

    pulse_sweep.py --preset fillpad --dry-run                    # print the plan only
    pulse_sweep.py --panel-port /dev/cu.usbmodemX --preset refresh --no-sensors
    pulse_sweep.py ... --conditions my_conditions.csv            # b_us,r_us,pct[,channel]
    pulse_sweep_plot.py phase2_summary.csv                       # analysis
"""

from __future__ import annotations

import argparse
import csv
import importlib
import statistics
import sys
import time
from dataclasses import dataclass, replace
from datetime import datetime
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from panel_test import BAUD, find_panels, pick_port, send  # noqa: E402

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit("pulse_sweep: pyserial not found (`pip install pyserial`).")

GRAY2_WEIGHT = 15
NUM_ROWS = 20
ROW_OVERHEAD_US = 1.0
B_MAX_US = 50.0
R_MIN_US, R_MAX_US = 100, 50000
REFERENCE_B, REFERENCE_R, REFERENCE_PCT = 3.0, 1000, 100
CHANNELS = ("0", "1", "2", "3", "all")

DEFAULT_ARENA_REPO = Path("/Users/reiserm/Documents/GitHub/LED-Display_G6_Firmware_Arena")
ARENA_BRANCH = "claude/qwiic-i2c-validation-cac1ce"
PJRC_VID = 0x16C0

TSL_CEILING = {100: 36863}
AS_SPECTRAL = ("F1_405", "F2_425", "FZ_450", "F3_475", "F4_515", "F5_550", "FY_555",
               "FXL_600", "F6_640", "F7_690", "F8_745", "NIR_855")

COND_COLUMNS = ["cond_idx", "tag", "is_reference", "preset", "b_us", "r_us", "pct", "channel",
                "duty_byte", "on_us", "on_fraction", "scan_est_us", "scan_overrun_est"]
SETTINGS_COLUMNS = ["tsl_gain", "tsl_atime_ms", "as_gain", "as_tint_ms", "as_full_scale"]
SENSOR_COLUMNS = (["tsl_full", "tsl_ir", "tsl_visible", "tsl_lux", "tsl_valid", "tsl_sat",
                   "veml_als", "veml_white", "veml_lux", "veml_sat"]
                  + [f"as_{k}" for k in AS_SPECTRAL]
                  + ["as_VIS", "as_FD", "as_asat_flags", "as_sat", "sat_any"])
SUMMARY_CHANNELS = (["tsl_full", "tsl_ir", "veml_als", "veml_white"]
                    + [f"as_{k}" for k in AS_SPECTRAL] + ["as_VIS"])
ROW_COLUMNS = ["timestamp", "t_s"] + COND_COLUMNS + ["sample_idx", "status"] + SETTINGS_COLUMNS + SENSOR_COLUMNS
SUMMARY_COLUMNS = (COND_COLUMNS + ["n", "status", "sat_count"] + SETTINGS_COLUMNS
                   + [f"{c}_{s}" for c in SUMMARY_CHANNELS for s in ("mean", "std", "cv")])


def pct_to_duty(pct: int) -> int:
    if pct <= 0:
        return 0
    return min(255, max(1, (pct * 255 + 50) // 100))


@dataclass(frozen=True)
class Condition:
    b_us: float
    r_us: int
    pct: int
    channel: str = "all"
    tag: str = "cond"
    preset: str = ""

    @property
    def duty_byte(self) -> int:
        return pct_to_duty(self.pct)

    @property
    def on_us(self) -> float:
        return self.b_us * GRAY2_WEIGHT * self.duty_byte / 255.0

    @property
    def on_fraction(self) -> float:
        return self.on_us / self.r_us

    @property
    def scan_est_us(self) -> float:
        return NUM_ROWS * (self.on_us + ROW_OVERHEAD_US)

    @property
    def scan_overrun_est(self) -> bool:
        return self.scan_est_us > self.r_us

    @property
    def is_reference(self) -> bool:
        return self.tag == "reference"

    def hold_cmd(self) -> str:
        if self.channel == "all" or self.pct == 0:
            return f"a{self.pct}"
        return f"c{self.channel},{self.pct}"

    def commands(self) -> list[str]:
        return [f"b{self.b_us:g}", f"r{self.r_us}", self.hold_cmd()]

    def validate(self) -> str | None:
        if not 0 < self.b_us <= B_MAX_US:
            return f"b={self.b_us:g} outside firmware range 0<b<={B_MAX_US:g}"
        if not R_MIN_US <= self.r_us <= R_MAX_US:
            return f"r={self.r_us} outside firmware range {R_MIN_US}..{R_MAX_US}"
        if not 0 <= self.pct <= 100:
            return f"pct={self.pct} outside 0..100"
        if self.channel not in CHANNELS:
            return f"channel={self.channel!r} not one of {CHANNELS}"
        return None

    def fields(self, idx: int) -> dict:
        return {"cond_idx": idx, "tag": self.tag, "is_reference": int(self.is_reference),
                "preset": self.preset, "b_us": f"{self.b_us:g}", "r_us": self.r_us, "pct": self.pct,
                "channel": self.channel, "duty_byte": self.duty_byte, "on_us": f"{self.on_us:.3f}",
                "on_fraction": f"{self.on_fraction:.6f}", "scan_est_us": f"{self.scan_est_us:.1f}",
                "scan_overrun_est": int(self.scan_overrun_est)}

    def label(self) -> str:
        return (f"{self.tag:9s} b={self.b_us:<5g} r={self.r_us:<6d} pct={self.pct:<3d} ch={self.channel:<3s} "
                f"on={self.on_us:7.2f}us frac={self.on_fraction * 100:6.3f}%"
                + ("  [scan overrun est.]" if self.scan_overrun_est else ""))


def reference_for(cond: Condition) -> Condition:
    return Condition(REFERENCE_B, REFERENCE_R, REFERENCE_PCT, cond.channel, "reference", cond.preset)


def dark_condition(preset: str) -> Condition:
    return Condition(REFERENCE_B, REFERENCE_R, 0, "all", "dark", preset)


LINEARITY_PCTS = (0, 1, 2, 5, 10, 20, 35, 50, 65, 80, 100)
# (60, 20000) would be the next 4.5 % pair but the firmware caps b at 50 us;
# (45, 15000) is the longest pulse that keeps exactly 4.5 % within the cap.
CONSTDUTY_PAIRS = ((0.3, 100), (0.75, 250), (1.5, 500), (3.0, 1000), (7.5, 2500), (15.0, 5000), (45.0, 15000))
FILLPAD_BS = (3.0, 3.2, 3.4, 3.6, 3.8, 4.0, 4.5)
REFRESH_RS = (500, 1000, 2000, 5000, 10000, 20000)


def preset_conditions(name: str, channel: str) -> list[Condition]:
    if name == "linearity":
        chans = list(CHANNELS) if channel == "all" else [channel]
        return [Condition(3.0, 1000, p, ch, preset=name) for ch in chans for p in LINEARITY_PCTS]
    if name == "constduty":
        return [Condition(b, r, pct, channel, preset=name)
                for pct in (100, 45) for b, r in CONSTDUTY_PAIRS]
    if name == "fillpad":
        return [Condition(b, 1000, 100, channel, preset=name) for b in FILLPAD_BS]
    if name == "refresh":
        return [Condition(3.0, r, 100, channel, preset=name) for r in REFRESH_RS]
    raise ValueError(f"unknown preset {name}")


def csv_conditions(path: str, channel: str) -> list[Condition]:
    out = []
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            out.append(Condition(float(row["b_us"]), int(float(row["r_us"])), int(float(row["pct"])),
                                 (row.get("channel") or channel).strip(), preset=Path(path).stem))
    return out


def drop_invalid(conds: list[Condition]) -> list[Condition]:
    kept = []
    for c in conds:
        why = c.validate()
        if why:
            print(f"pulse_sweep: skipping {c.label().strip()} — {why}", file=sys.stderr)
        else:
            kept.append(c)
    return kept


def build_plan(conds: list[Condition], ab_every: int) -> list[Condition]:
    """Interleave reference re-runs: before the first condition, at every
    channel change, and after every `ab_every` conditions; one closes the run."""
    plan = []
    since, last_ch = ab_every, None
    for c in conds:
        if ab_every and (since >= ab_every or c.channel != last_ch):
            plan.append(reference_for(c))
            since = 0
        plan.append(c)
        since += 1
        last_ch = c.channel
    if ab_every and conds:
        plan.append(reference_for(conds[-1]))
    return plan


class PanelError(RuntimeError):
    pass


class Panel:
    def __init__(self, port: str, reply_wait: float):
        self.ser = serial.Serial(port, BAUD, timeout=0.05)
        self.reply_wait = reply_wait
        time.sleep(0.3)

    def cmd(self, c: str) -> list[str]:
        replies = send(self.ser, c, self.reply_wait, echo=False)
        print(f"  > {c:<12s} < {' | '.join(replies) if replies else '(no reply)'}")
        if not replies:
            raise PanelError(f"{c}: no reply")
        for r in replies:
            if r.startswith("ERR:"):
                raise PanelError(f"{c}: {r}")
        return replies

    def apply(self, cond: Condition) -> None:
        for c in cond.commands():
            self.cmd(c)

    def safe(self, c: str) -> None:
        try:
            self.cmd(c)
        except (PanelError, serial.SerialException) as e:
            print(f"pulse_sweep: {e}", file=sys.stderr)

    def close(self) -> None:
        self.ser.close()


def arena_candidates(explicit: str | None) -> list[Path]:
    if explicit:
        return [Path(explicit).expanduser()]
    main = DEFAULT_ARENA_REPO
    return ([main] + sorted((main / ".claude" / "worktrees").glob("*"))
            + sorted(main.parent.glob(main.name + "-*")))


def find_arena_repo(explicit: str | None) -> Path:
    tried = arena_candidates(explicit)
    for p in tried:
        if (p / "tests" / "qwiic_sensors.py").is_file() and (p / "tests" / "transport.py").is_file():
            return p.resolve()
    sys.exit("pulse_sweep: no arena checkout with tests/qwiic_sensors.py found. Tried:\n  "
             + "\n  ".join(str(p) for p in tried)
             + f"\nCheck out branch {ARENA_BRANCH} (PR #58), e.g.\n"
             f"  git -C {DEFAULT_ARENA_REPO} worktree add ../LED-Display_G6_Firmware_Arena-qwiic origin/{ARENA_BRANCH}\n"
             "and pass it with --arena-repo.")


def load_arena_modules(repo: Path):
    sys.path.insert(0, str(repo))
    for name in [m for m in sys.modules if m == "tests" or m.startswith("tests.")]:
        del sys.modules[name]
    try:
        qs = importlib.import_module("tests.qwiic_sensors")
        tr = importlib.import_module("tests.transport")
    except Exception as e:
        sys.exit(f"pulse_sweep: cannot import sensor modules from {repo}: {e!r}\n"
                 f"(expected tests/qwiic_sensors.py + tests/transport.py from branch {ARENA_BRANCH})")
    for attr in ("Sampler", "NoQwiicJack", "TSL2591", "VEML7700", "AS7343"):
        if not hasattr(qs, attr):
            sys.exit(f"pulse_sweep: {repo}/tests/qwiic_sensors.py lacks {attr}; wrong branch?")
    return qs, tr


def find_arena_port(exclude: str | None) -> str:
    ports = [p for p in list_ports.comports() if p.device != exclude]
    named = [p.device for p in ports
             if any(s and ("Arena" in s or "Reiser" in s) for s in (p.manufacturer, p.product, p.description))]
    cands = named or [p.device for p in ports if p.vid == PJRC_VID]
    if len(cands) == 1:
        return cands[0]
    if not cands:
        sys.exit("pulse_sweep: no arena controller (Teensy) port found; pass --arena-port.")
    sys.exit("pulse_sweep: several Teensy ports; pick one with --arena-port: " + ", ".join(cands))


class SensorRig:
    def __init__(self, repo: Path, port: str, tsl_gain: str, tsl_atime: int, as_gain: float):
        self.qs, tr = load_arena_modules(repo)
        self.tsl_atime = tsl_atime
        self.transport = tr.SerialTransport(port)
        self.transport.open()
        try:
            self.sampler = self.qs.Sampler(self.transport, tsl_gain=tsl_gain, tsl_atime=tsl_atime, as_gain=as_gain)
        except self.qs.NoQwiicJack as e:
            self.transport.close()
            sys.exit(f"pulse_sweep: {e} (arena firmware built without the Qwiic jack?)")
        except RuntimeError as e:
            self.transport.close()
            sys.exit(f"pulse_sweep: {e} — arena firmware lacks the I2C bridge (0xB0/0xB1)?")
        self.primary = {}
        for dev, s in self.sampler.sensors:
            name = type(s).__name__
            if name in self.primary:
                print(f"pulse_sweep: extra {name} at {dev.where()} ignored (first one is logged)", file=sys.stderr)
            else:
                self.primary[name] = s
        if not self.primary:
            self.close()
            sys.exit("pulse_sweep: no TSL2591 / VEML7700 / AS7343 on the Qwiic bus.")
        found = ", ".join(f"{type(s).__name__} 0x{d.addr:02X} @ {d.where()}" for d, s in self.sampler.sensors)
        print(f"sensors: {found}  (round ~{self.sampler.period_s * 1000:.0f} ms)")
        missing = self.sampler.missing()
        if missing:
            print(f"pulse_sweep: missing sensors: {', '.join(missing)}", file=sys.stderr)

    def settings(self) -> dict:
        st = self.sampler.settings
        as_ = self.primary.get("AS7343")
        return {"tsl_gain": st["tsl_gain"], "tsl_atime_ms": st["tsl_atime"], "as_gain": f"{st['as_gain']:g}",
                "as_tint_ms": f"{as_.tint_ms:.2f}" if as_ else "", "as_full_scale": as_.full_scale if as_ else ""}

    def sample(self) -> dict:
        out = {}
        for dev, s, r in self.sampler.round():
            name = type(s).__name__
            if self.primary.get(name) is not s:
                continue
            if name == "TSL2591":
                ceiling = TSL_CEILING.get(self.tsl_atime, 0xFFFF)
                lux = r["lux_approx"]
                out.update(tsl_full=r["full"], tsl_ir=r["ir"], tsl_visible=r["visible"],
                           tsl_lux="" if lux is None else f"{lux:.2f}", tsl_valid=int(r["valid"]),
                           tsl_sat=int(r["full"] >= ceiling or r["ir"] >= ceiling or lux is None))
            elif name == "VEML7700":
                out.update(veml_als=r["als"], veml_white=r["white"], veml_lux=f"{r['lux_approx']:.2f}",
                           veml_sat=int(r["als"] >= 0xFFFF or r["white"] >= 0xFFFF))
            elif name == "AS7343":
                for k in AS_SPECTRAL:
                    out[f"as_{k}"] = r["spectral"][k]
                out.update(as_VIS=f"{r['vis']:.1f}", as_FD=f"{r['fd']:.1f}", as_asat_flags=r["asat_flags"],
                           as_sat=int(r["sat"] or r["sat_vis"] or bool(r["asat_flags"])))
        out["sat_any"] = int(any(out.get(k) for k in ("tsl_sat", "veml_sat", "as_sat")))
        return out

    def close(self) -> None:
        try:
            self.sampler.close()
        finally:
            self.transport.close()


def summarize(cond: Condition, idx: int, rows: list[dict], status: str, settings: dict) -> dict:
    out = cond.fields(idx)
    out.update(n=len(rows), status=status, sat_count=sum(int(r.get("sat_any") or 0) for r in rows), **settings)
    for ch in SUMMARY_CHANNELS:
        vals = [float(r[ch]) for r in rows if r.get(ch) not in (None, "")]
        if not vals:
            continue
        m = statistics.mean(vals)
        sd = statistics.stdev(vals) if len(vals) > 1 else 0.0
        out[f"{ch}_mean"] = f"{m:.3f}"
        out[f"{ch}_std"] = f"{sd:.3f}"
        out[f"{ch}_cv"] = f"{sd / m * 100:.3f}" if m else ""
    return out


class Sweep:
    def __init__(self, args, panel: Panel, rig: SensorRig | None):
        self.args, self.panel, self.rig = args, panel, rig
        self.t0 = time.monotonic()
        self.out = open(args.out, "w", newline="")
        self.rows = csv.DictWriter(self.out, ROW_COLUMNS)
        self.rows.writeheader()
        self.summary_path = Path(args.out).with_name(Path(args.out).stem + "_summary.csv")
        self.sumf = open(self.summary_path, "w", newline="")
        self.summary = csv.DictWriter(self.sumf, SUMMARY_COLUMNS)
        self.summary.writeheader()
        self.settings = rig.settings() if rig else {}

    def measure(self, cond: Condition, idx: int) -> None:
        print(f"[{idx}] {cond.label()}")
        status = "ok"
        try:
            self.panel.apply(cond)
        except PanelError as e:
            status = f"panel: {e}"
            print(f"pulse_sweep: {status} — condition skipped", file=sys.stderr)
            self.write_row(cond, idx, 0, status, {})
            self.summary.writerow(summarize(cond, idx, [], status, self.settings))
            self.flush()
            return
        time.sleep(self.args.settle)
        samples = []
        if self.rig is None:
            self.write_row(cond, idx, 0, status, {})
        else:
            for i in range(self.args.samples):
                try:
                    vals = self.rig.sample()
                except (RuntimeError, TimeoutError, OSError) as e:
                    vals, status = {}, f"sensor: {e}"
                    print(f"pulse_sweep: sample {i}: {e}", file=sys.stderr)
                else:
                    samples.append(vals)
                self.write_row(cond, idx, i, "ok" if vals else status, vals)
            if samples:
                sat = sum(s["sat_any"] for s in samples)
                brief = ", ".join(f"{k}={statistics.mean(float(s[k]) for s in samples):.0f}"
                                  for k in ("tsl_full", "veml_als", "as_VIS") if k in samples[0])
                print(f"      n={len(samples)} {brief}" + (f"  SAT x{sat}" if sat else ""))
        self.summary.writerow(summarize(cond, idx, samples, status, self.settings))
        self.flush()

    def write_row(self, cond: Condition, idx: int, sample_idx: int, status: str, vals: dict) -> None:
        row = {"timestamp": datetime.now().isoformat(timespec="milliseconds"),
               "t_s": f"{time.monotonic() - self.t0:.3f}", "sample_idx": sample_idx, "status": status}
        row.update(cond.fields(idx))
        row.update(self.settings)
        row.update(vals)
        self.rows.writerow(row)

    def flush(self) -> None:
        self.out.flush()
        self.sumf.flush()

    def close(self) -> None:
        self.out.close()
        self.sumf.close()


def print_plan(plan: list[Condition], preset: str) -> None:
    dark = dark_condition(preset)
    print(f"[0] {dark.label()}\n      " + "  ".join(dark.commands()))
    for i, c in enumerate(plan, 1):
        print(f"[{i}] {c.label()}\n      " + "  ".join(c.commands()))
    print("[end]     a0  b3  r1000")
    print(f"{len(plan)} conditions ({sum(c.is_reference for c in plan)} reference re-runs) + dark")


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="pulse_sweep", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    g = ap.add_argument_group("panel (bcmtest firmware)")
    g.add_argument("--panel-serial", metavar="S", help="USB serial number of the panel (see panel_test.py --list)")
    g.add_argument("--panel-port", metavar="DEV", help="panel serial device (overrides --panel-serial)")
    g.add_argument("--reply-wait", type=float, default=0.3, metavar="S", help="seconds to wait for a reply (default 0.3)")
    g = ap.add_argument_group("sensors (arena_12-18 I2C bridge)")
    g.add_argument("--arena-port", metavar="DEV", help="arena controller USB-CDC device (default: the one Teensy found)")
    g.add_argument("--arena-repo", metavar="PATH", help=f"arena checkout on branch {ARENA_BRANCH} "
                   f"(default: {DEFAULT_ARENA_REPO} or a worktree of it that has tests/qwiic_sensors.py)")
    g.add_argument("--tsl-gain", choices=("low", "med", "high", "max"), default="med", help="TSL2591 gain (default med = x25)")
    g.add_argument("--tsl-atime", type=int, choices=(100, 200, 300, 400, 500, 600), default=100, help="TSL2591 integration ms")
    g.add_argument("--as-gain", type=float, default=64, metavar="G", help="AS7343 gain 0.5..2048 (default 64)")
    g.add_argument("--no-sensors", action="store_true", help="drive the panel only; sensor columns left empty")
    g = ap.add_argument_group("sweep")
    x = g.add_mutually_exclusive_group(required=True)
    x.add_argument("--preset", choices=("linearity", "constduty", "fillpad", "refresh"))
    x.add_argument("--conditions", metavar="FILE.csv", help="columns b_us,r_us,pct[,channel]")
    g.add_argument("--channel", choices=CHANNELS, default="all", help="colour channel to hold (default all; "
                   "linearity with 'all' runs 0,1,2,3 and all)")
    g.add_argument("--settle", type=float, default=2.0, metavar="S", help="seconds after the hold before sampling (default 2)")
    g.add_argument("--samples", type=int, default=20, metavar="N", help="sensor rounds per condition (default 20)")
    g.add_argument("--ab-every", type=int, default=5, metavar="N",
                   help="re-run the reference (b=3 r=1000 pct=100) every N conditions; 0 disables (default 5)")
    g.add_argument("--out", default="results.csv", metavar="FILE", help="per-sample CSV; summary goes to <stem>_summary.csv")
    g.add_argument("--dry-run", action="store_true", help="print the command plan without opening any port")
    args = ap.parse_args(argv)

    if args.conditions:
        conds = csv_conditions(args.conditions, args.channel)
        preset = Path(args.conditions).stem
    else:
        conds = preset_conditions(args.preset, args.channel)
        preset = args.preset
    conds = drop_invalid(conds)
    if not conds:
        sys.exit("pulse_sweep: no valid conditions.")
    plan = build_plan(conds, args.ab_every)

    if args.dry_run:
        print_plan(plan, preset)
        return 0

    panel_port = pick_port(args.panel_serial, args.panel_port)
    rig = None
    if not args.no_sensors:
        repo = find_arena_repo(args.arena_repo)
        print(f"arena modules: {repo}")
        arena_port = args.arena_port or find_arena_port(panel_port)
        rig = SensorRig(repo, arena_port, args.tsl_gain, args.tsl_atime, args.as_gain)
        print(f"arena port: {arena_port}")
    try:
        panel = Panel(panel_port, args.reply_wait)
    except serial.SerialException as e:
        if rig:
            rig.close()
        sys.exit(f"pulse_sweep: cannot open panel {panel_port}: {e}")
    print(f"panel port: {panel_port}")

    sweep = Sweep(args, panel, rig)
    rc = 0
    try:
        sweep.measure(dark_condition(preset), 0)
        for i, cond in enumerate(plan, 1):
            sweep.measure(cond, i)
    except KeyboardInterrupt:
        print("\npulse_sweep: interrupted — blanking panel", file=sys.stderr)
        rc = 130
    finally:
        for c in ("a0", f"b{REFERENCE_B:g}", f"r{REFERENCE_R}"):
            panel.safe(c)
        panel.close()
        sweep.close()
        if rig:
            rig.close()
    print(f"wrote {args.out} and {sweep.summary_path}")
    return rc


if __name__ == "__main__":
    raise SystemExit(main())

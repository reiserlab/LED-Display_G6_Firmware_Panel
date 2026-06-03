#!/opt/homebrew/bin/python3.14
"""AD3 trigger generator + verifier for the arena EINT line (Phase A/B).

Drives the arena's external EINT input (jumper = direct-to-EINT -> TNY.EINT fanout
-> all panels' GP45) from AD3 **W1**, and optionally verifies on the scope.

REQUIRES homebrew python3.14 (system 3.9 crashes loading the DWF framework). See
the `instruments` skill. Wiring (default --source dio):
    DIO0 -> arena external EINT input  (GND common; ~330 ohm series R recommended)
            DIO is push-pull 0..3.3 V CMOS by hardware -> safe logic trigger.
    Ch1  -> photodiode over a lit pixel      (optional, for LED-on / latency)
    Ch2  -> tee on EINT (trigger reference)  (optional, edge count)
  NOTE: the W1 wavegen path (--source w1) is kept as a fallback, but on this bench
  W1's DC offset did not apply (measured bipolar +/-1.65 V) -> do NOT drive GP45 with
  W1 unless you confirm it is a clean 0..3.3 V unipolar square first.

Sub-commands
------------
  continuous   W1 square wave at --freq (default 8000 Hz), 0..3.3 V. Runs until
               Enter/Ctrl-C. Use for "lit when triggered" (Triggered, ~1-8 kHz for
               a bright frame) and Gated strobing.
  hold         W1 DC at --volts (default 3.3). Gated steady-ON (3.3) or OFF (0).
  burst        Exactly --edges rising edges at --freq, then idle low. For the
               clean "20 edges = 1 frame -> dark" Triggered demo. Verify the count
               with `capture` if exactness matters.
  capture      Scope-trigger on W1; record Ch2 (EINT) + Ch1 (photodiode); report
               measured edge count, period, and trigger->LED-onset latency.
  off          Stop both wavegen channels.

Examples
--------
    ./ad3_trigger.py hold --volts 3.3            # Gated ON
    ./ad3_trigger.py continuous --freq 8000      # 8 kHz sub-frame sync
    ./ad3_trigger.py burst --edges 20 --freq 1000
    ./ad3_trigger.py capture --freq 8000 --captures 50
"""

import argparse
import ctypes
import sys
import time

DWF = "/Library/Frameworks/dwf.framework/dwf"

# DWF enums
funcDC = 0
funcSquare = 2
AnalogOutNodeCarrier = 0
DwfStateDone = 2
acqmodeSingle = 0
trigsrcAnalogOut1 = 7
DwfAnalogOutIdleInitial = 2   # idle at the waveform's initial sample (low, with phase 0)


def _load():
    try:
        dwf = ctypes.cdll.LoadLibrary(DWF)
    except OSError as e:
        sys.exit(f"cannot load DWF ({e}); run with /opt/homebrew/bin/python3.14")
    v = ctypes.create_string_buffer(32)
    dwf.FDwfGetVersion(v)
    return dwf, v.value.decode()


def _open(dwf, config=1):
    n = ctypes.c_int()
    dwf.FDwfEnum(ctypes.c_int(0), ctypes.byref(n))
    if n.value == 0:
        sys.exit("no AD3 found (is WaveForms holding the device? `pkill -9 -f python3.14`)")
    h = ctypes.c_int()
    if dwf.FDwfDeviceConfigOpen(ctypes.c_int(0), ctypes.c_int(config), ctypes.byref(h)) == 0:
        sys.exit("FDwfDeviceConfigOpen failed")
    return h


def _w1_square(dwf, h, freq, vlow, vhigh, duty, run_s=None):
    """Configure W1 as a square wave. run_s=None -> continuous; else finite run then idle low."""
    ch = ctypes.c_int(0)
    off = (vhigh + vlow) / 2.0
    amp = (vhigh - vlow) / 2.0          # DWF amplitude is PEAK (Vpp = 2*amp)
    dwf.FDwfAnalogOutNodeEnableSet(h, ch, ctypes.c_int(AnalogOutNodeCarrier), ctypes.c_int(1))
    dwf.FDwfAnalogOutNodeFunctionSet(h, ch, ctypes.c_int(AnalogOutNodeCarrier), ctypes.c_byte(funcSquare))
    dwf.FDwfAnalogOutNodeFrequencySet(h, ch, ctypes.c_int(AnalogOutNodeCarrier), ctypes.c_double(freq))
    dwf.FDwfAnalogOutNodeAmplitudeSet(h, ch, ctypes.c_int(AnalogOutNodeCarrier), ctypes.c_double(amp))
    dwf.FDwfAnalogOutNodeOffsetSet(h, ch, ctypes.c_int(AnalogOutNodeCarrier), ctypes.c_double(off))
    dwf.FDwfAnalogOutNodeSymmetrySet(h, ch, ctypes.c_int(AnalogOutNodeCarrier), ctypes.c_double(duty))
    dwf.FDwfAnalogOutNodePhaseSet(h, ch, ctypes.c_int(AnalogOutNodeCarrier), ctypes.c_double(0.0))
    if run_s is not None:
        dwf.FDwfAnalogOutIdleSet(h, ch, ctypes.c_int(DwfAnalogOutIdleInitial))
        dwf.FDwfAnalogOutRunSet(h, ch, ctypes.c_double(run_s))
        dwf.FDwfAnalogOutRepeatSet(h, ch, ctypes.c_int(1))
    dwf.FDwfAnalogOutConfigure(h, ch, ctypes.c_int(1))


def _w1_dc(dwf, h, volts):
    ch = ctypes.c_int(0)
    dwf.FDwfAnalogOutNodeEnableSet(h, ch, ctypes.c_int(AnalogOutNodeCarrier), ctypes.c_int(1))
    dwf.FDwfAnalogOutNodeFunctionSet(h, ch, ctypes.c_int(AnalogOutNodeCarrier), ctypes.c_byte(funcDC))
    dwf.FDwfAnalogOutNodeOffsetSet(h, ch, ctypes.c_int(AnalogOutNodeCarrier), ctypes.c_double(volts))
    dwf.FDwfAnalogOutConfigure(h, ch, ctypes.c_int(1))


def _stop(dwf, h):
    for ci in (0, 1):
        dwf.FDwfAnalogOutConfigure(h, ctypes.c_int(ci), ctypes.c_int(0))


# --- Digital out (DIO) — native 0..3.3 V CMOS logic; the safe logic-trigger source.
# Preferred over W1 wavegen: push-pull 0/3.3 V by hardware (no offset to misconfigure,
# no negative excursion into GP45), and supports exact-N bursts via run/repeat. ---
def _dio_clock(dwf, h, pin, freq, run_s=None):
    """Square wave on DIO<pin>. run_s=None -> continuous; else exactly run_s of pulses, idle LOW."""
    hz = ctypes.c_double()
    dwf.FDwfDigitalOutInternalClockInfo(h, ctypes.byref(hz))
    half = max(1, int(round(hz.value / (2.0 * freq))))
    p = ctypes.c_int(pin)
    dwf.FDwfDigitalOutReset(h)
    dwf.FDwfDigitalOutEnableSet(h, p, ctypes.c_int(1))
    dwf.FDwfDigitalOutTypeSet(h, p, ctypes.c_int(0))     # DwfDigitalOutTypePulse
    dwf.FDwfDigitalOutIdleSet(h, p, ctypes.c_int(1))     # DwfDigitalOutIdleLow
    dwf.FDwfDigitalOutDividerSet(h, p, ctypes.c_int(1))
    dwf.FDwfDigitalOutCounterSet(h, p, ctypes.c_int(half), ctypes.c_int(half))  # low,high
    if run_s is not None:
        dwf.FDwfDigitalOutRunSet(h, ctypes.c_double(run_s))
        dwf.FDwfDigitalOutRepeatSet(h, ctypes.c_int(1))
    else:
        dwf.FDwfDigitalOutRunSet(h, ctypes.c_double(0.0))   # continuous
        dwf.FDwfDigitalOutRepeatSet(h, ctypes.c_int(0))
    dwf.FDwfDigitalOutConfigure(h, ctypes.c_int(1))
    return hz.value / (2.0 * half)                       # actual frequency


def _dio_static(dwf, h, pin, level):
    """Drive DIO<pin> to a constant 0 or 3.3 V (for Gated EINT level)."""
    mask = 1 << pin
    dwf.FDwfDigitalOutReset(h)
    dwf.FDwfDigitalIOReset(h)
    dwf.FDwfDigitalIOOutputEnableSet(h, ctypes.c_uint(mask))
    dwf.FDwfDigitalIOOutputSet(h, ctypes.c_uint(mask if level else 0))
    dwf.FDwfDigitalIOConfigure(h)


def _dio_stop(dwf, h):
    dwf.FDwfDigitalOutConfigure(h, ctypes.c_int(0))
    dwf.FDwfDigitalOutReset(h)
    dwf.FDwfDigitalIOReset(h)


def cmd_continuous(dwf, h, a):
    if a.source == "dio":
        f = _dio_clock(dwf, h, a.dio, a.freq)
        print(f"DIO{a.dio}: {f:.0f} Hz square 0..3.3 V CMOS — running. Enter to stop.")
        try:
            input()
        except (EOFError, KeyboardInterrupt):
            pass
        _dio_stop(dwf, h)
    else:
        _w1_square(dwf, h, a.freq, a.vlow, a.vhigh, a.duty)
        print(f"W1: {a.freq:.0f} Hz square {a.vlow}..{a.vhigh} V, duty {a.duty}% — running. Enter to stop.")
        try:
            input()
        except (EOFError, KeyboardInterrupt):
            pass
        _stop(dwf, h)
    print("stopped.")


def cmd_hold(dwf, h, a):
    level = 1 if a.volts >= 1.65 else 0
    if a.source == "dio":
        _dio_static(dwf, h, a.dio, level)
        print(f"DIO{a.dio}: held {'HIGH (3.3 V)' if level else 'LOW (0 V)'} — Enter to stop.")
        try:
            input()
        except (EOFError, KeyboardInterrupt):
            pass
        _dio_stop(dwf, h)
    else:
        _w1_dc(dwf, h, a.volts)
        print(f"W1: DC {a.volts} V — holding. Enter to stop.")
        try:
            input()
        except (EOFError, KeyboardInterrupt):
            pass
        _stop(dwf, h)
    print("stopped.")


def cmd_burst(dwf, h, a):
    run_s = a.edges / float(a.freq)     # N full periods -> N rising edges
    if a.source == "dio":
        f = _dio_clock(dwf, h, a.dio, a.freq, run_s=run_s)
        print(f"DIO{a.dio}: burst of {a.edges} edges @ {f:.0f} Hz ({run_s*1e3:.2f} ms), then idle LOW.")
        sts = ctypes.c_byte()
        t0 = time.time()
        while time.time() - t0 < run_s + 0.5:
            dwf.FDwfDigitalOutStatus(h, ctypes.byref(sts))
            if sts.value == DwfStateDone:
                break
            time.sleep(0.001)
        _dio_stop(dwf, h)
    else:
        _w1_square(dwf, h, a.freq, a.vlow, a.vhigh, a.duty, run_s=run_s)
        print(f"W1: burst of {a.edges} edges @ {a.freq:.0f} Hz ({run_s*1e3:.2f} ms), then idle low.")
        sts = ctypes.c_byte()
        t0 = time.time()
        while time.time() - t0 < run_s + 0.5:
            dwf.FDwfAnalogOutStatus(h, ctypes.c_int(0), ctypes.byref(sts))
            if sts.value == DwfStateDone:
                break
            time.sleep(0.001)
        _stop(dwf, h)
    print("burst done (verify exact count with `capture` if needed).")


def cmd_capture(dwf, h, a):
    import numpy as np
    # Start W1 first so the scope can trigger on it.
    _w1_square(dwf, h, a.freq, a.vlow, a.vhigh, a.duty)
    time.sleep(0.2)
    buf = 32768
    sr = a.rate
    # Ch1 = photodiode (small range), Ch2 = EINT reference (5 V range).
    dwf.FDwfAnalogInChannelEnableSet(h, ctypes.c_int(0), ctypes.c_int(1))
    dwf.FDwfAnalogInChannelEnableSet(h, ctypes.c_int(1), ctypes.c_int(1))
    dwf.FDwfAnalogInChannelRangeSet(h, ctypes.c_int(0), ctypes.c_double(a.pd_range))
    dwf.FDwfAnalogInChannelRangeSet(h, ctypes.c_int(1), ctypes.c_double(5.0))
    dwf.FDwfAnalogInAcquisitionModeSet(h, ctypes.c_int(acqmodeSingle))
    dwf.FDwfAnalogInFrequencySet(h, ctypes.c_double(sr))
    dwf.FDwfAnalogInBufferSizeSet(h, ctypes.c_int(buf))
    dwf.FDwfAnalogInTriggerSourceSet(h, ctypes.c_byte(trigsrcAnalogOut1))
    dwf.FDwfAnalogInTriggerAutoTimeoutSet(h, ctypes.c_double(2.0))
    dwf.FDwfAnalogInTriggerPositionSet(h, ctypes.c_double(buf * 0.95 / sr))

    b1 = (ctypes.c_double * buf)()
    b2 = (ctypes.c_double * buf)()
    edge_counts, latencies = [], []
    pre = int(buf * 0.05)
    t_us = (np.arange(buf) - pre) / sr * 1e6
    for _ in range(a.captures):
        dwf.FDwfAnalogInConfigure(h, ctypes.c_int(0), ctypes.c_int(1))
        sts = ctypes.c_byte()
        while True:
            dwf.FDwfAnalogInStatus(h, ctypes.c_int(1), ctypes.byref(sts))
            if sts.value == DwfStateDone:
                break
            time.sleep(5e-5)
        dwf.FDwfAnalogInStatusData(h, ctypes.c_int(0), b1, ctypes.c_int(buf))
        dwf.FDwfAnalogInStatusData(h, ctypes.c_int(1), b2, ctypes.c_int(buf))
        ch1 = np.ctypeslib.as_array(b1).copy()   # photodiode
        ch2 = np.ctypeslib.as_array(b2).copy()   # EINT
        thr2 = (ch2.min() + ch2.max()) / 2
        rises = np.where(np.diff((ch2 > thr2).astype(np.int8)) == 1)[0]
        edge_counts.append(len(rises))
        if len(rises) and (ch1.max() - ch1.min()) > a.pd_range * 0.1:
            thr1 = (ch1.min() + ch1.max()) / 2
            led = np.where(np.diff((ch1 > thr1).astype(np.int8)) == 1)[0]
            led = led[led >= rises[0]]
            if len(led):
                latencies.append(t_us[led[0]] - t_us[rises[0]])
    _stop(dwf, h)
    ec = np.array(edge_counts)
    print(f"window {buf/sr*1e3:.2f} ms @ {sr/1e6:.1f} MS/s, {a.captures} captures")
    print(f"edges/window: mean={ec.mean():.1f} min={ec.min()} max={ec.max()} "
          f"(expected ~{a.freq*buf/sr:.1f} @ {a.freq:.0f} Hz)")
    if latencies:
        la = np.array(latencies)
        print(f"trigger->LED-onset latency: mean={la.mean():.2f} us "
              f"min={la.min():.2f} max={la.max():.2f} (n={len(la)})")
    else:
        print("no photodiode transitions seen (check Ch1 wiring / pixel / --pd-range)")


def cmd_selfcheck(dwf, h, a):
    """Loopback sanity: drive the trigger, measure on scope Ch1 (wire source -> Ch1+, GND -> Ch1-)."""
    import numpy as np
    if a.source == "dio":
        f = _dio_clock(dwf, h, a.dio, a.freq)
        scope_trig = ctypes.c_byte(0)              # immediate (no analogout1 for DIO)
        src_label = f"DIO{a.dio}"
        mid = 1.65
    else:
        _w1_square(dwf, h, a.freq, a.vlow, a.vhigh, a.duty)
        scope_trig = ctypes.c_byte(trigsrcAnalogOut1)
        src_label = "W1"
        mid = (a.vhigh + a.vlow) / 2.0
    time.sleep(0.2)
    buf = 32768
    sr = min(max(a.freq * 200, 1e6), 12.5e6)   # ~200 samples/period, 1..12.5 MS/s
    dwf.FDwfAnalogInChannelEnableSet(h, ctypes.c_int(0), ctypes.c_int(1))
    dwf.FDwfAnalogInChannelEnableSet(h, ctypes.c_int(1), ctypes.c_int(0))
    dwf.FDwfAnalogInChannelRangeSet(h, ctypes.c_int(0), ctypes.c_double(5.0))
    dwf.FDwfAnalogInChannelOffsetSet(h, ctypes.c_int(0), ctypes.c_double(mid))
    dwf.FDwfAnalogInAcquisitionModeSet(h, ctypes.c_int(acqmodeSingle))
    dwf.FDwfAnalogInFrequencySet(h, ctypes.c_double(sr))
    dwf.FDwfAnalogInBufferSizeSet(h, ctypes.c_int(buf))
    dwf.FDwfAnalogInTriggerSourceSet(h, scope_trig)
    dwf.FDwfAnalogInTriggerAutoTimeoutSet(h, ctypes.c_double(2.0))
    dwf.FDwfAnalogInTriggerPositionSet(h, ctypes.c_double(buf * 0.45 / sr))
    dwf.FDwfAnalogInConfigure(h, ctypes.c_int(1), ctypes.c_int(1))
    sts = ctypes.c_byte()
    t0 = time.time()
    while time.time() - t0 < 3.0:
        dwf.FDwfAnalogInStatus(h, ctypes.c_int(1), ctypes.byref(sts))
        if sts.value == DwfStateDone:
            break
        time.sleep(5e-5)
    b1 = (ctypes.c_double * buf)()
    dwf.FDwfAnalogInStatusData(h, ctypes.c_int(0), b1, ctypes.c_int(buf))
    ch = np.ctypeslib.as_array(b1).copy()
    _dio_stop(dwf, h) if a.source == "dio" else _stop(dwf, h)
    vmin, vmax = float(ch.min()), float(ch.max())
    thr = (vmin + vmax) / 2
    above = (ch > thr).astype(np.int8)
    rises = np.where(np.diff(above) == 1)[0]
    meas_f = (sr / np.diff(rises).mean()) if len(rises) >= 2 else float("nan")
    duty = 100.0 * float(above.mean())
    print(f"{src_label} self-check — commanded {a.freq:.0f} Hz")
    print(f"  MEASURED: Vmin={vmin:.2f} V  Vmax={vmax:.2f} V  freq={meas_f:.1f} Hz  "
          f"duty~{duty:.0f}%  edges={len(rises)}  ({buf} samp @ {sr/1e6:.2f} MS/s)")
    if abs(meas_f - a.freq) / a.freq < 0.05:
        if a.source == "dio":
            print(f"  OK: clean square at commanded freq. {src_label} is push-pull 0..3.3 V CMOS "
                  f"by hardware — safe to wire to EINT (add a ~330 ohm series R for protection).")
        elif vmax - vmin >= 2.0:
            print(f"  OK: clean square. (Note: scope DC level may read shifted; confirm 0..3.3 V before EINT.)")
    elif vmax - vmin < 2.0:
        print("  WARNING: swing < 2 V — check source -> Ch1 wiring / GND.")


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="mode", required=True)

    def common(p):
        p.add_argument("--source", choices=("dio", "w1"), default="dio",
                       help="dio = AD3 digital pin (0..3.3V CMOS, safe — default); w1 = wavegen")
        p.add_argument("--dio", type=int, default=0, help="DIO pin index when --source dio")
        p.add_argument("--freq", type=float, default=8000.0, help="square-wave Hz")
        p.add_argument("--vlow", type=float, default=0.0, help="w1 only")
        p.add_argument("--vhigh", type=float, default=3.3, help="w1 only")
        p.add_argument("--duty", type=float, default=50.0, help="symmetry %% (w1 only)")

    pc = sub.add_parser("continuous"); common(pc)
    ph = sub.add_parser("hold"); ph.add_argument("--volts", type=float, default=3.3)
    ph.add_argument("--source", choices=("dio", "w1"), default="dio")
    ph.add_argument("--dio", type=int, default=0)
    pb = sub.add_parser("burst"); common(pb); pb.add_argument("--edges", type=int, default=20)
    psc = sub.add_parser("selfcheck"); common(psc)
    pcap = sub.add_parser("capture"); common(pcap)
    pcap.add_argument("--captures", type=int, default=50)
    pcap.add_argument("--rate", type=float, default=12.5e6, help="scope sample rate")
    pcap.add_argument("--pd-range", type=float, default=0.5, help="Ch1 photodiode range V")
    sub.add_parser("off")

    a = ap.parse_args()
    dwf, ver = _load()
    print(f"DWF v{ver}")
    h = _open(dwf)
    try:
        {"continuous": cmd_continuous, "hold": cmd_hold, "burst": cmd_burst,
         "selfcheck": cmd_selfcheck, "capture": cmd_capture,
         "off": lambda d, hh, aa: _stop(d, hh)}[a.mode](dwf, h, a)
    finally:
        dwf.FDwfDeviceClose(h)


if __name__ == "__main__":
    main()

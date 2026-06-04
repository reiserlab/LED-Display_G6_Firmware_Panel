#!/opt/homebrew/bin/python3.14
# Photodiode trigger->LED latency + JITTER: many single-shot captures, onset =
# first crossing of THR within 5us of the trigger (i.e. captures where t=0's
# edge hit a photodiode row). mean=latency, std=jitter. Saves clean avg waveform
# + onset list to /tmp/lat_jit.json.
import sys, time, ctypes, json
import numpy as np
sys.path.insert(0, "/Users/reiserm/Documents/GitHub/LED-Display_G6_Firmware_Panel/panel/tools")
from ad3_trigger import _load, _open, _w1_square, _stop
cd = ctypes.c_double; ci = ctypes.c_int; cb = ctypes.c_byte; TRIG_W1 = 7
THR = 0.20  # volts; ~30% of the ~0.65 V peak, well above ~0.03 V noise

dwf, ver = _load(); h = _open(dwf)
_w1_square(dwf, h, 8000, 0.0, 3.3, 50.0); time.sleep(0.3)
sr = 12.5e6; buf = 2048
dwf.FDwfAnalogInChannelEnableSet(h, ci(0), ci(1)); dwf.FDwfAnalogInChannelEnableSet(h, ci(1), ci(0))
dwf.FDwfAnalogInChannelRangeSet(h, ci(0), cd(2.0)); dwf.FDwfAnalogInAcquisitionModeSet(h, ci(0))
dwf.FDwfAnalogInFrequencySet(h, cd(sr)); dwf.FDwfAnalogInBufferSizeSet(h, ci(buf))
dwf.FDwfAnalogInTriggerSourceSet(h, cb(TRIG_W1))
dwf.FDwfAnalogInTriggerAutoTimeoutSet(h, cd(0.01)); dwf.FDwfAnalogInTriggerPositionSet(h, cd(0.0))
c = buf // 2; win = int(5e-6 * sr)
onsets = []; acc = np.zeros(buf); nacc = 0; b = (cd * buf)()
for _ in range(400):
    dwf.FDwfAnalogInConfigure(h, ci(0), ci(1)); s = cb(); t0 = time.time()
    while time.time() - t0 < 0.2:
        dwf.FDwfAnalogInStatus(h, ci(1), ctypes.byref(s))
        if s.value == 2: break
        time.sleep(1e-4)
    dwf.FDwfAnalogInStatusData(h, ci(0), b, ci(buf)); a = np.ctypeslib.as_array(b).copy()
    base = a[:c - 100].mean(); post = a[c:] - base
    idx = np.where(post[:win] > THR)[0]
    if len(idx):
        onsets.append(float(idx[0] / sr * 1e6)); acc += a; nacc += 1
_stop(dwf, h); dwf.FDwfDeviceClose(h)
on = np.array(onsets)
lat = float(on.mean()); jit = float(on.std()); lo = float(on.min()); hi = float(on.max())
print(f"valid={nacc}/400   latency(mean)={lat:.3f} us   JITTER(std)={jit:.3f} us   "
      f"min={lo:.3f}  max={hi:.3f}  p2p={hi-lo:.3f} us")
# clean averaged waveform from the valid (pulse-present) captures
avg = acc / max(1, nacc); base = avg[:c - 100].mean()
t_us = (np.arange(buf) - c) / sr * 1e6; mV = (avg - base) * 1e3
m = (t_us >= -3) & (t_us <= 12)
json.dump({"latency_us": round(lat, 3), "jitter_us": round(jit, 3),
           "min_us": round(lo, 3), "max_us": round(hi, 3), "n": nacc,
           "onsets_us": [round(x, 3) for x in onsets],
           "t": [round(float(x), 3) for x in t_us[m]], "v": [round(float(x), 2) for x in mV[m]]},
          open("/tmp/lat_jit.json", "w"))
print("saved /tmp/lat_jit.json")

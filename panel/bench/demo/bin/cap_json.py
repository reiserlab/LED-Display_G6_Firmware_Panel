#!/opt/homebrew/bin/python3.14
# Capture averaged photodiode waveform for the currently-streaming duty; save
# trace + metrics to /tmp/duty_<duty>.json.  argv[1]=duty.
import sys, time, ctypes, json
import numpy as np
sys.path.insert(0, "/Users/reiserm/Documents/GitHub/LED-Display_G6_Firmware_Panel/panel/tools")
from ad3_trigger import _load, _open, _w1_square, _stop
cd = ctypes.c_double; ci = ctypes.c_int; cb = ctypes.c_byte; TRIG_W1 = 7
duty = int(sys.argv[1])

dwf, ver = _load(); h = _open(dwf)
_w1_square(dwf, h, 8000, 0.0, 3.3, 50.0); time.sleep(0.25)
sr = 12.5e6; buf = 8192
dwf.FDwfAnalogInChannelEnableSet(h, ci(0), ci(1)); dwf.FDwfAnalogInChannelEnableSet(h, ci(1), ci(0))
dwf.FDwfAnalogInChannelRangeSet(h, ci(0), cd(2.0)); dwf.FDwfAnalogInAcquisitionModeSet(h, ci(0))
dwf.FDwfAnalogInFrequencySet(h, cd(sr)); dwf.FDwfAnalogInBufferSizeSet(h, ci(buf))
dwf.FDwfAnalogInTriggerSourceSet(h, cb(TRIG_W1))
dwf.FDwfAnalogInTriggerAutoTimeoutSet(h, cd(0.01)); dwf.FDwfAnalogInTriggerPositionSet(h, cd(0.0))
N = 250; acc = np.zeros(buf); b = (cd * buf)(); n = 0
for _ in range(N):
    dwf.FDwfAnalogInConfigure(h, ci(0), ci(1)); s = cb(); t0 = time.time()
    while time.time() - t0 < 0.2:
        dwf.FDwfAnalogInStatus(h, ci(1), ctypes.byref(s))
        if s.value == 2: break
        time.sleep(1e-4)
    dwf.FDwfAnalogInStatusData(h, ci(0), b, ci(buf)); acc += np.ctypeslib.as_array(b).copy(); n += 1
_stop(dwf, h); dwf.FDwfDeviceClose(h)

avg = acc / n; c = buf // 2; base = avg[:c - 300].mean()
t_us = (np.arange(buf) - c) / sr * 1e6; mV = (avg - base) * 1e3
segm = (t_us >= 0) & (t_us <= 110); ts = t_us[segm]; vs = mV[segm]
pk = float(vs.max()); pki = int(vs.argmax()); half = pk * 0.5
i = pki
while i > 0 and vs[i] > half: i -= 1
onset = float(ts[i])
l = pki
while l > 0 and vs[l] > half: l -= 1
r = pki
while r < len(vs) - 1 and vs[r] > half: r += 1
fwhm = float(ts[r] - ts[l])
# trace window -8..60 us, downsampled to ~360 pts
m = (t_us >= -8) & (t_us <= 60); tt = t_us[m]; vv = mV[m]
step = max(1, len(tt) // 360); tt = tt[::step]; vv = vv[::step]
json.dump({"duty": duty, "fwhm": round(fwhm, 2), "onset": round(onset, 2), "peak": round(pk, 1),
           "t": [round(float(x), 3) for x in tt], "v": [round(float(x), 2) for x in vv]},
          open(f"/tmp/duty_{duty}.json", "w"))
print(f"  duty={duty:>3}  on-time={fwhm:5.1f} us  latency={onset:4.2f} us  peak={pk:5.0f} mV")

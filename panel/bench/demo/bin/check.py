#!/opt/homebrew/bin/python3.14
"""Pre-flight: read AD3 Ch1 (photodiode) raw level. Run with the panel STREAMING
all-on (bright). peak > ~0.2 V => photodiode live. Else: power? Ch1 1- grounded?
aim? Requires homebrew python3.14 (DWF framework)."""
import sys, time, ctypes
import numpy as np
sys.path.insert(0, "/Users/reiserm/Documents/GitHub/LED-Display_G6_Firmware_Panel/panel/tools")
from ad3_trigger import _load, _open
cd = ctypes.c_double; ci = ctypes.c_int; cb = ctypes.c_byte
dwf, ver = _load(); h = _open(dwf)
sr = 2e6; buf = int(sr * 0.05)
dwf.FDwfAnalogInChannelEnableSet(h, ci(0), ci(1)); dwf.FDwfAnalogInChannelEnableSet(h, ci(1), ci(0))
dwf.FDwfAnalogInChannelRangeSet(h, ci(0), cd(5.0)); dwf.FDwfAnalogInAcquisitionModeSet(h, ci(0))
dwf.FDwfAnalogInFrequencySet(h, cd(sr)); dwf.FDwfAnalogInBufferSizeSet(h, ci(buf))
dwf.FDwfAnalogInTriggerSourceSet(h, cb(0)); dwf.FDwfAnalogInConfigure(h, ci(0), ci(1))
s = cb(); t = time.time()
while time.time() - t < 2:
    dwf.FDwfAnalogInStatus(h, ci(1), ctypes.byref(s))
    if s.value == 2: break
    time.sleep(1e-3)
b = (cd * buf)(); dwf.FDwfAnalogInStatusData(h, ci(0), b, ci(buf))
a = np.ctypeslib.as_array(b).copy(); dwf.FDwfDeviceClose(h)
pk = float(a.max())
print(f"Ch1 peak={pk*1000:.0f} mV  mean={a.mean()*1000:.0f} mV")
if pk > 0.2:
    print("  PHOTODIODE LIVE — ok to measure.")
else:
    print("  CHECK PHOTODIODE: powered? Ch1 1- tied to TIA ground (differential)? aimed at a lit panel?")

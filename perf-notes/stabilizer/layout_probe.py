"""Show where Stabilizer placed a few interpreter functions, before and after
running for a while (run with the Stabilizer-built python in retained mode).

  STABILIZER_CODE_MODE=retained ./python layout_probe.py [seconds]

Prints (completed epochs, exhausted flag, [(function, original entry, current copy)]).
"""
import ctypes, time, sys
lib = ctypes.CDLL(None)
lib.stabilizer_completed_epochs.restype = ctypes.c_uint64
lib.stabilizer_code_location.restype = ctypes.c_void_p
lib.stabilizer_code_location.argtypes = [ctypes.c_void_p]
names = ["PyLong_FromLong", "_PyEval_EvalFrameDefault", "PyObject_GetAttr", "PyDict_GetItem"]
def snap():
    out = []
    for n in names:
        try:
            a = ctypes.cast(getattr(lib, n), ctypes.c_void_p).value
        except AttributeError:
            out.append((n, None)); continue
        out.append((n, hex(a), hex(lib.stabilizer_code_location(a) or 0)))
    return lib.stabilizer_completed_epochs(), lib.stabilizer_retained_exhausted(), out
print(snap())
x = 0
t = time.time()
while time.time() - t < float(sys.argv[1] if len(sys.argv) > 1 else 2.2):
    x += 1
print(snap())

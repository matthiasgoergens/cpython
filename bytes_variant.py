import ctypes, subprocess, tempfile, pathlib
src = r'''#include <stdint.h>
#include <string.h>
#pragma pack(2)
typedef struct __attribute__((ms_struct)) { uint16_t a; uint32_t b; } Foo;
#pragma pack()
void foo_dump(Foo foo, unsigned char *out) { memcpy(out, &foo, sizeof foo); }
'''
d = pathlib.Path(tempfile.mkdtemp()); (d / "foo.c").write_text(src)
subprocess.run(["clang", "-O1", "-shared", "-fPIC", "-Wno-attributes", "-o", str(d / "foo.so"), str(d / "foo.c")], check=True)
class Bytes(ctypes.Structure):
    # same 6 bytes, but the under-aligned uint32 described as 4 bytes: nothing under-aligned
    _layout_ = "ms"; _pack_ = 2
    _fields_ = [("a", ctypes.c_uint16), ("b", ctypes.c_uint8 * 4)]
assert ctypes.sizeof(Bytes) == 6 and Bytes.b.offset == 2
lib = ctypes.CDLL(str(d / "foo.so"))
buf = (ctypes.c_ubyte * 6)()
lib.foo_dump.argtypes = [Bytes, ctypes.c_void_p]
lib.foo_dump(Bytes(), buf)
print("no crash:", bytes(buf).hex())

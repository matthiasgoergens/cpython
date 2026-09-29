"""ctypes: passing a packed struct with bit fields by value.  Usage:
repro.py PACK LAYOUT FIELDSPEC   e.g.  repro.py 2 ms "H:7,H:7,I"
Builds a C function that receives the struct by value and copies its bytes out."""
import ctypes, subprocess, sys, tempfile, pathlib
pack, layout, spec = int(sys.argv[1]), sys.argv[2], sys.argv[3]
C = {"B": ("uint8_t", ctypes.c_uint8), "H": ("uint16_t", ctypes.c_uint16), "I": ("uint32_t", ctypes.c_uint32), "Q": ("uint64_t", ctypes.c_uint64)}
fields, cdecl = [], []
for i, f in enumerate(spec.split(",")):
    code, _, bits = f.partition(":")
    cname, ctype = C[code]
    fields.append((f"f{i}", ctype, int(bits)) if bits else (f"f{i}", ctype))
    cdecl.append(f"  {cname} f{i}{':' + bits if bits else ''};")
attr = "__attribute__((ms_struct))" if layout == "ms" else ""
pragma = f"#pragma pack({pack})" if pack else ""
src = f"""#include <stdint.h>
#include <string.h>
#include <stddef.h>
{pragma}
typedef struct {attr} {{
{chr(10).join(cdecl)}
}} Foo;
size_t foo_size(void) {{ return sizeof(Foo); }}
void foo_dump(Foo foo, unsigned char *out) {{ memcpy(out, &foo, sizeof foo); }}
"""
d = pathlib.Path(tempfile.mkdtemp())
(d / "foo.c").write_text(src)
subprocess.run(["clang", "-O1", "-shared", "-fPIC", "-Wno-attributes", "-o", str(d / "foo.so"), str(d / "foo.c")], check=True)
ns = {"_layout_": layout, "_fields_": fields}
if pack:
    ns["_pack_"] = pack
Foo = type("Foo", (ctypes.Structure,), ns)
lib = ctypes.CDLL(str(d / "foo.so"))
lib.foo_size.restype = ctypes.c_size_t
assert lib.foo_size() == ctypes.sizeof(Foo), (lib.foo_size(), ctypes.sizeof(Foo))
obj = Foo()
buf = (ctypes.c_ubyte * ctypes.sizeof(Foo))()
lib.foo_dump.argtypes = [Foo, ctypes.c_void_p]
lib.foo_dump.restype = None
lib.foo_dump(obj, buf)
print("ok", ctypes.sizeof(Foo), bytes(buf).hex())

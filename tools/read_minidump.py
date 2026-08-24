"""Extract the exception and faulting module from a Windows minidump.

Remix's bridge overwrites its logs on relaunch, so after a crash the .dmp is often the only
surviving evidence. This reads the two streams that matter without needing WinDbg installed:
the exception record (what went wrong and where) and the module list (whose code that was).
"""
import struct, sys, datetime

PATH = sys.argv[1]
d = open(PATH, "rb").read()

sig, ver, nstreams, dir_rva = struct.unpack_from("<IIII", d, 0)
assert sig == 0x504D444D, "not a minidump"
ts = struct.unpack_from("<I", d, 20)[0]
print(f"{PATH}")
print(f"streams: {nstreams}   written: {datetime.datetime.utcfromtimestamp(ts)} UTC\n")

streams = {}
for i in range(nstreams):
    t, size, rva = struct.unpack_from("<III", d, dir_rva + i * 12)
    streams[t] = (size, rva)

EXCEPTION_STREAM, MODULE_LIST = 6, 4

def mdstring(rva):
    ln = struct.unpack_from("<I", d, rva)[0]
    return d[rva + 4: rva + 4 + ln].decode("utf-16-le", errors="replace")

modules = []
if MODULE_LIST in streams:
    _, rva = streams[MODULE_LIST]
    n = struct.unpack_from("<I", d, rva)[0]
    for i in range(n):
        o = rva + 4 + i * 108
        base, size, _chk, _tds, name_rva = struct.unpack_from("<QIIII", d, o)
        modules.append((base, size, mdstring(name_rva)))

CODES = {
    0xC0000005: "ACCESS_VIOLATION",
    0xC000001D: "ILLEGAL_INSTRUCTION",
    0xC0000094: "INTEGER_DIVIDE_BY_ZERO",
    0xC0000096: "PRIVILEGED_INSTRUCTION",
    0xC00000FD: "STACK_OVERFLOW",
    0x80000003: "BREAKPOINT",
    0xC0000374: "HEAP_CORRUPTION",
    0xE06D7363: "C++ EXCEPTION",
}

if EXCEPTION_STREAM in streams:
    _, rva = streams[EXCEPTION_STREAM]
    tid = struct.unpack_from("<I", d, rva)[0]
    code, flags, rec, addr, nparams = struct.unpack_from("<IIQQI", d, rva + 8)
    params = [struct.unpack_from("<Q", d, rva + 8 + 32 + i * 8)[0] for i in range(min(nparams, 4))]
    print("==== EXCEPTION ====")
    print(f"  thread   : {tid}")
    print(f"  code     : {code:#010x}  {CODES.get(code, 'unknown')}")
    print(f"  address  : {addr:#018x}")
    if code == 0xC0000005 and len(params) >= 2:
        kind = {0: "read", 1: "write", 8: "execute"}.get(params[0], f"op={params[0]}")
        print(f"  detail   : attempted to {kind} address {params[1]:#018x}")
        if params[1] < 0x10000:
            print("             -> near-null pointer dereference")
    owner = next((m for m in modules if m[0] <= addr < m[0] + m[1]), None)
    if owner:
        print(f"  in module: {owner[2]}  (base {owner[0]:#x}, +{addr - owner[0]:#x})")
    else:
        print("  in module: UNKNOWN - address is outside every loaded module")
else:
    print("no exception stream")

print(f"\n==== LOADED MODULES OF INTEREST ({len(modules)} total) ====")
for base, size, name in modules:
    low = name.lower()
    if any(k in low for k in ("d3d9", "remix", "nvremix", "sr3", "saints", "dxvk",
                              "nvapi", "dinput8", "amd", "nv")):
        print(f"  {base:#014x} +{size:<9x} {name}")

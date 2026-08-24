"""Static analysis of SaintsRowTheThird.exe: find where the engine calls D3D9.

Runtime patching needs to know which engine functions submit rendering work. The reliable way
in is the import address table: every D3D9 call the engine makes goes through a fixed IAT slot,
so scanning .text for indirect calls through those slots locates every call site, and the
function each one sits in is an engine render function worth naming.

Pure-Python PE parsing: no external dependencies, no disassembler install.
"""
import struct, sys, collections, re

PATH = sys.argv[1] if len(sys.argv) > 1 else \
    r"D:\SR3RTXREMIXCOMP\Saints Row 3\SaintsRowTheThird.exe"

data = open(PATH, "rb").read()
print(f"file: {PATH}\nsize: {len(data):,} bytes")

# ---- PE headers ----
pe = struct.unpack_from("<I", data, 0x3C)[0]
assert data[pe:pe+4] == b"PE\0\0", "not a PE"
machine, nsec = struct.unpack_from("<HH", data, pe + 4)
opt = pe + 24
magic = struct.unpack_from("<H", data, opt)[0]
is64 = magic == 0x20B
image_base = struct.unpack_from("<Q", data, opt + 24)[0] if is64 else struct.unpack_from("<I", data, opt + 28)[0]
print(f"machine: {machine:#06x} ({'x64' if is64 else 'x86'})  image base: {image_base:#x}"
      f"  sections: {nsec}")

# data directories: [1] = import table
dd = opt + (112 if is64 else 96)
imp_rva, imp_size = struct.unpack_from("<II", data, dd + 8)

sections = []
sec_off = opt + struct.unpack_from("<H", data, pe + 20)[0]
for i in range(nsec):
    o = sec_off + i * 40
    name = data[o:o+8].rstrip(b"\0").decode(errors="replace")
    vsize, vaddr, rsize, raddr = struct.unpack_from("<IIII", data, o + 8)
    sections.append((name, vaddr, vsize, raddr, rsize))
    print(f"  {name:8s} rva={vaddr:#010x} vsize={vsize:#010x} raw={raddr:#010x}")

def rva2off(rva):
    for _, va, vs, ra, rs in sections:
        if va <= rva < va + max(vs, rs):
            return ra + (rva - va)
    return None

# ---- imports ----
print("\n==== D3D9 IMPORTS ====")
iat = {}          # rva of IAT slot -> name
d3d9_slots = {}
off = rva2off(imp_rva)
while True:
    oft, tstamp, fwd, name_rva, first_thunk = struct.unpack_from("<IIIII", data, off)
    if not name_rva:
        break
    dll = data[rva2off(name_rva):].split(b"\0")[0].decode(errors="replace")
    thunk = oft or first_thunk
    t_off = rva2off(thunk)
    slot = first_thunk
    while True:
        val = struct.unpack_from("<I", data, t_off)[0]
        if not val:
            break
        if not (val & 0x80000000):
            n_off = rva2off(val)
            fname = data[n_off+2:].split(b"\0")[0].decode(errors="replace")
        else:
            fname = f"#{val & 0xFFFF}"
        iat[slot] = (dll, fname)
        if dll.lower().startswith("d3d9"):
            d3d9_slots[slot] = fname
        slot += 4
        t_off += 4
    off += 20

for dll in sorted({d for d, _ in iat.values()}):
    n = sum(1 for d, _ in iat.values() if d == dll)
    mark = "  <-- graphics" if dll.lower().startswith(("d3d", "dxgi")) else ""
    print(f"  {dll:28s} {n:4d} imports{mark}")

print(f"\nd3d9 IAT slots: {len(d3d9_slots)}")
for s, n in sorted(d3d9_slots.items()):
    print(f"  {image_base + s:#010x}  {n}")

# ---- find indirect calls through those slots: FF 15 <abs32> ----
print("\n==== CALL SITES INTO D3D9 (FF 15 = call dword ptr [addr]) ====")
text = next(s for s in sections if s[0] == ".text")
_, tva, tvs, tra, trs = text
blob = data[tra:tra+max(tvs, trs)]

wanted = {image_base + s: n for s, n in d3d9_slots.items()}
hits = collections.Counter()
sites = collections.defaultdict(list)
for m in re.finditer(rb"\xFF\x15", blob):
    p = m.start()
    if p + 6 > len(blob):
        continue
    target = struct.unpack_from("<I", blob, p + 2)[0]
    if target in wanted:
        va = image_base + tva + p
        hits[wanted[target]] += 1
        sites[wanted[target]].append(va)

if not hits:
    print("  none found - the engine likely resolves D3D9 dynamically, or wraps it")
for name, c in hits.most_common():
    ex = " ".join(f"{a:#x}" for a in sites[name][:6])
    print(f"  {c:5d}  {name:34s} e.g. {ex}")

print("\ntotal d3d9 call sites:", sum(hits.values()))

# ---- vtable-relative calls: the engine holds IDirect3DDevice9* and calls through it ----
# Device methods never appear in the IAT, so locate them by their vtable offset instead:
#   FF /r disp32  ->  call dword ptr [reg + offset]
SLOTS = {
    17: "Present", 41: "BeginScene", 42: "EndScene", 43: "Clear",
    44: "SetTransform", 49: "SetMaterial", 51: "SetLight", 53: "LightEnable",
    57: "SetRenderState", 65: "SetTexture", 67: "SetTextureStageState",
    81: "DrawPrimitive", 82: "DrawIndexedPrimitive", 83: "DrawPrimitiveUP",
    84: "DrawIndexedPrimitiveUP", 87: "SetVertexDeclaration", 89: "SetFVF",
    91: "CreateVertexShader", 92: "SetVertexShader", 94: "SetVertexShaderConstantF",
    100: "SetStreamSource", 104: "SetIndices",
    106: "CreatePixelShader", 107: "SetPixelShader", 109: "SetPixelShaderConstantF",
    37: "SetRenderTarget", 39: "SetDepthStencilSurface",
}
BY_OFF = {s * 4: n for s, n in SLOTS.items()}
REGS = {0x90: "eax", 0x91: "ecx", 0x92: "edx", 0x93: "ebx",
        0x95: "ebp", 0x96: "esi", 0x97: "edi"}

print("\n==== VTABLE CALL SITES (call dword ptr [reg+offset]) ====")
found = collections.Counter()
where = collections.defaultdict(list)
i = 0
while i < len(blob) - 6:
    if blob[i] == 0xFF and blob[i+1] in REGS:
        disp = struct.unpack_from("<I", blob, i + 2)[0]
        if disp in BY_OFF:
            nm = BY_OFF[disp]
            found[nm] += 1
            where[nm].append(image_base + tva + i)
            i += 6
            continue
    i += 1

for nm, c in found.most_common():
    ex = " ".join(f"{a:#x}" for a in where[nm][:5])
    print(f"  {c:5d}  {nm:26s} e.g. {ex}")
print("\ntotal vtable call sites:", sum(found.values()))

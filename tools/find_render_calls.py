"""Map the engine's D3D9 device call sites in SaintsRowTheThird.exe.

Why this exists: the exe imports only Direct3DCreate9 and four D3DPERF markers, so every device
method is reached through a COM vtable and none of them appear in the import table. Runtime
patching needs their addresses, so they have to be recovered from the code itself.

Three passes, in increasing order of trustworthiness:

  1. DIRECT   - `call dword ptr [reg + offset]`. Rare here: MSVC mostly does not emit this.
  2. LOADS    - `mov reg, [obj + offset]` where offset matches a vtable slot. High recall but
                MANY false positives: a vtable offset is just an integer, and struct field
                accesses collide with it constantly. Reported for reference only.
  3. CONFIRMED- a LOAD whose register is actually CALLed a few instructions later, with the
                register invalidated if anything overwrites it in between. This is the list
                worth patching against; everything else is a lead, not a fact.

Byte-pattern scanning was tried first and abandoned: x86 has no fixed instruction boundaries,
so a regex over .text invents hits (it found 162 "calls" whose displacements were random bytes)
and misses real ones. Capstone does a proper linear sweep.
"""
import struct, sys, collections
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

PATH = sys.argv[1] if len(sys.argv) > 1 else \
    r"D:\SR3RTXREMIXCOMP\Saints Row 3\SaintsRowTheThird.exe"
WINDOW = 12          # instructions a loaded pointer may live before being called

SLOTS = {
    17: "Present", 37: "SetRenderTarget", 39: "SetDepthStencilSurface",
    41: "BeginScene", 42: "EndScene", 43: "Clear",
    44: "SetTransform", 49: "SetMaterial", 51: "SetLight", 53: "LightEnable",
    57: "SetRenderState", 65: "SetTexture", 67: "SetTextureStageState",
    81: "DrawPrimitive", 82: "DrawIndexedPrimitive", 83: "DrawPrimitiveUP",
    84: "DrawIndexedPrimitiveUP", 87: "SetVertexDeclaration", 89: "SetFVF",
    91: "CreateVertexShader", 92: "SetVertexShader", 94: "SetVertexShaderConstantF",
    100: "SetStreamSource", 104: "SetIndices",
    106: "CreatePixelShader", 107: "SetPixelShader", 109: "SetPixelShaderConstantF",
}
BY_OFF = {s * 4: n for s, n in SLOTS.items()}
REGS = ("eax", "ecx", "edx", "ebx", "esi", "edi", "ebp")


def load_text(path):
    data = open(path, "rb").read()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    opt = pe + 24
    base = struct.unpack_from("<I", data, opt + 28)[0]  # PE32 ImageBase; +24 is BaseOfData
    nsec = struct.unpack_from("<H", data, pe + 6)[0]
    so = opt + struct.unpack_from("<H", data, pe + 20)[0]
    for i in range(nsec):
        o = so + i * 40
        if data[o:o+8].rstrip(b"\0") == b".text":
            vs, va, rs, ra = struct.unpack_from("<IIII", data, o + 8)
            return base, va, data[ra:ra + max(vs, rs)]
    raise SystemExit("no .text section")


def disp_of(op):
    """Displacement from a `[reg + 0xNN]` operand, or None."""
    if "[" not in op or "+" not in op or "]" not in op:
        return None
    try:
        return int(op[op.index("+") + 1:op.index("]")].strip(), 16)
    except ValueError:
        return None


base, tva, blob = load_text(PATH)
print(f"disassembling .text: {len(blob):,} bytes at VA {base + tva:#x}")

md = Cs(CS_ARCH_X86, CS_MODE_32)
md.skipdata = True

direct = collections.Counter()
loads = collections.Counter()
confirmed = collections.Counter()
sites = collections.defaultdict(list)

pending = {}          # reg -> (method, instruction index, address)
count = 0
for i, ins in enumerate(md.disasm(blob, base + tva)):
    count += 1
    op = ins.op_str
    dst = op.split(",")[0].strip()

    if ins.mnemonic == "call":
        d = disp_of(op)
        if d in BY_OFF:                       # pass 1: direct vtable call
            direct[BY_OFF[d]] += 1
            sites[BY_OFF[d]].append(ins.address)
        elif op.strip() in pending:           # pass 3: call through a loaded pointer
            method, idx, _ = pending[op.strip()]
            if i - idx <= WINDOW:
                confirmed[method] += 1
                sites[method].append(ins.address)
            pending.pop(op.strip(), None)

    elif ins.mnemonic == "mov" and dst in REGS:
        d = disp_of(op)
        if d in BY_OFF:                       # pass 2: method-pointer load
            loads[BY_OFF[d]] += 1
            pending[dst] = (BY_OFF[d], i, ins.address)
        else:
            pending.pop(dst, None)            # register reused for something else

    elif dst in REGS:
        pending.pop(dst, None)                # any other write invalidates it

    if len(pending) > 8:                      # keep the window small
        pending.clear()

print(f"instructions decoded: {count:,}\n")


def show(title, counter, note=""):
    print(f"==== {title} ({sum(counter.values())}) ====")
    if note:
        print(f"  {note}")
    for n, c in counter.most_common():
        ex = " ".join(f"{a:#x}" for a in sites[n][:5])
        print(f"  {c:6d}  {n:26s} {ex}")
    print()


show("PASS 1: DIRECT vtable calls", direct)
show("PASS 2: METHOD-POINTER LOADS", loads,
     "LEADS ONLY - vtable offsets collide with ordinary struct offsets")
show("PASS 3: CONFIRMED CALLS (load -> call, register still live)", confirmed,
     "this is the list to patch against")

draws = sorted(a for n in ("DrawIndexedPrimitive", "DrawPrimitive",
                           "DrawIndexedPrimitiveUP", "DrawPrimitiveUP")
               for a in sites.get(n, []))
if draws:
    print(f"==== DRAW-CALL CLUSTERS ({len(draws)} sites) ====")
    print("  adjacent draw calls usually belong to one submission routine")
    cluster, out = [draws[0]], []
    for a in draws[1:]:
        if a - cluster[-1] < 0x400:
            cluster.append(a)
        else:
            out.append(cluster)
            cluster = [a]
    out.append(cluster)
    out.sort(key=len, reverse=True)
    for c in out[:15]:
        print(f"  {len(c):3d} draws in {c[0]:#x}..{c[-1]:#x}")
    print(f"\n  {len(out)} distinct draw regions")

"""Rebuild customize_item.vpp_pc with the reshaped hair bundles and a patched asm.

customize_item.vpp_pc is an uncompressed 64-bit VPP: every entry is stored raw at a
0x1000-aligned offset. Rebuilding it is therefore a straight copy-through with a few
entries substituted, streamed so the 1.7 GB never has to be held in memory.

The .asm_pc records each container's compressed size, so every rebuilt bundle's new
size is patched in before the asm is written back. Everything else in the asm - and
every other entry in the packfile - is carried over byte for byte.

Usage:
    python pack_vpp.py <bundles_dir> <output.vpp_pc>
"""
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vpp2 import Pack                                     # noqa: E402
from asmpc import Asm                                     # noqa: E402

from paths import srttr_stock                              # noqa: E402

SRTTR = srttr_stock()
ALIGN = 0x1000
DIR_AT = 0x1000
ENTRY = 48
ASM = "customize_item.asm_pc"


def _up(v, a=ALIGN):
    return (v + a - 1) // a * a


def patch_asm(src_asm, manifest):
    """Update each rebuilt container's compressed size. Returns the new bytes."""
    a = Asm(src_asm)
    by = a.by_name()
    missing = []
    for bundle, info in manifest.items():
        key = bundle[:-len(".str2_pc")]
        c = by.get(key)
        if c is None:
            missing.append(key)
            continue
        struct.pack_into("<Q", a.d, c.off_comp, info["comp"])
    return a.bytes(), missing


def main():
    bundles_dir, out_path = sys.argv[1], sys.argv[2]
    manifest = json.load(open(os.path.join(bundles_dir, "manifest.json")))

    src = Pack(SRTTR)
    if src.compressed:
        raise SystemExit("expected an uncompressed source packfile")

    new_asm, missing = patch_asm(src.read(ASM), manifest)
    if missing:
        raise SystemExit(f"{len(missing)} rebuilt bundles are absent from {ASM}: {missing[:5]}")

    # replacement payloads: the rebuilt bundles, plus the patched asm
    repl = {ASM: new_asm}
    sizes = {ASM: len(new_asm)}
    for bundle in manifest:
        path = os.path.join(bundles_dir, bundle)
        repl[bundle] = path
        sizes[bundle] = os.path.getsize(path)

    names_blob = bytearray()
    name_offs = []
    for name, _, _, _ in src.items:
        name_offs.append(len(names_blob))
        names_blob += name.encode("latin1") + b"\0"

    index = bytearray()
    off = 0
    plan = []
    for (name, doff, usz, _), noff in zip(src.items, name_offs):
        size = sizes.get(name, usz)
        index += struct.pack("<6Q", noff, 0, off, size, 0xFFFFFFFFFFFFFFFF, 0)
        plan.append((name, doff, usz, size, off))
        off = _up(off + size)
    data_sz = off

    names_at = _up(DIR_AT + len(index))
    data_at = _up(names_at + len(names_blob))

    hdr = bytearray(DIR_AT)
    struct.pack_into("<I", hdr, 0x000, 0x51890ACE)
    struct.pack_into("<I", hdr, 0x004, 6)
    struct.pack_into("<I", hdr, 0x14C, 0)
    struct.pack_into("<Q", hdr, 0x158, len(src.items))
    struct.pack_into("<Q", hdr, 0x160, data_at + data_sz)
    struct.pack_into("<Q", hdr, 0x168, len(index))
    struct.pack_into("<Q", hdr, 0x170, len(names_blob))
    struct.pack_into("<Q", hdr, 0x178, data_sz)
    struct.pack_into("<Q", hdr, 0x180, 0xFFFFFFFFFFFFFFFF)

    fin = open(src.path, "rb")
    swapped = 0
    with open(out_path, "wb") as fo:
        fo.write(hdr)
        fo.write(index)
        fo.write(b"\0" * (names_at - fo.tell()))
        fo.write(names_blob)
        fo.write(b"\0" * (data_at - fo.tell()))
        for name, doff, usz, size, dst in plan:
            assert fo.tell() == data_at + dst, (name, fo.tell(), data_at + dst)
            if name in repl:
                payload = repl[name]
                if isinstance(payload, str):
                    with open(payload, "rb") as fh:
                        fo.write(fh.read())
                else:
                    fo.write(payload)
                swapped += 1
            else:
                fin.seek(src.data_at + doff)
                left = usz
                while left:
                    chunk = fin.read(min(1 << 22, left))
                    if not chunk:
                        raise SystemExit(f"short read on {name}")
                    fo.write(chunk)
                    left -= len(chunk)
            pad = _up(size) - size
            if pad:
                fo.write(b"\0" * pad)
    fin.close()
    print(f"wrote {out_path}  {os.path.getsize(out_path):,} bytes  "
          f"({swapped} entries substituted of {len(plan)})")


if __name__ == "__main__":
    main()

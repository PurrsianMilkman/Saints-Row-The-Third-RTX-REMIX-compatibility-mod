"""Map each custmesh_<hash>.str2_pc bundle to the asset names it contains.

The bundles are stored uncompressed and aligned inside the vpp, so their own
header/index/names blocks can be parsed in place without extracting anything.
"""
import struct, sys
sys.path.insert(0, r"C:/Users/Purrsian/AppData/Local/Temp/claude/C--Users-Purrsian/9bbd2cfa-f120-4f87-bc27-765b05b8f090/scratchpad")
from vpp2 import Pack, MAGIC, _up


def inner_names(buf):
    """Entry names of a v6 container held in `buf` (needs header+index+names only)."""
    if len(buf) < 0x1000 or struct.unpack_from("<I", buf, 0)[0] != MAGIC:
        return None
    c32 = struct.unpack_from("<I", buf, 0x154)[0]
    i32 = struct.unpack_from("<I", buf, 0x15C)[0]
    c64 = struct.unpack_from("<Q", buf, 0x158)[0]
    i64 = struct.unpack_from("<Q", buf, 0x168)[0]
    if c32 and i32 == c32 * 24:
        wide, align, dir_at, ent, count, idx_sz = False, 0x800, 0x800, 24, c32, i32
        nm_sz = struct.unpack_from("<I", buf, 0x160)[0]
    elif c64 and i64 == c64 * 48:
        wide, align, dir_at, ent, count, idx_sz = True, 0x1000, 0x1000, 48, c64, i64
        nm_sz = struct.unpack_from("<Q", buf, 0x170)[0]
    else:
        return None
    names_at = _up(dir_at + idx_sz, align)
    if names_at + nm_sz > len(buf):
        return None
    names = buf[names_at:names_at + nm_sz]
    out = []
    for i in range(count):
        o = dir_at + i * ent
        noff = struct.unpack_from("<Q" if wide else "<I", buf, o)[0]
        e = names.index(b"\0", noff)
        out.append(names[noff:e].decode("latin1"))
    return out


def scan(vpp_path, want, peek=0x40000):
    pk = Pack(vpp_path)
    f = open(vpp_path, "rb")
    hits = []
    for name, doff, usz, csz in pk.items:
        if not name.endswith(".str2_pc"):
            continue
        f.seek(pk.data_at + doff)
        buf = f.read(min(peek, usz))
        names = inner_names(buf)
        if not names:
            continue
        m = [n for n in names if want in n.lower()]
        if m:
            hits.append((name, sorted(m)))
    f.close()
    return pk, hits


if __name__ == "__main__":
    pk, hits = scan(sys.argv[1], sys.argv[2].lower())
    print(f"# {pk}  bundles containing {sys.argv[2]!r}: {len(hits)}")
    for bundle, names in sorted(hits, key=lambda h: h[1][0]):
        print(f"{bundle}")
        for n in names:
            print(f"      {n}")

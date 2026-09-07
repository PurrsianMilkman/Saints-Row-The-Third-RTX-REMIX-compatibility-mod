"""VPP_PC / STR2_PC v6 reader covering BOTH Saints Row: The Third variants.

Original (32-bit fields):  index at 0x800,  24-byte entries, 0x800 alignment.
Remastered (64-bit fields): index at 0x1000, 48-byte entries, 0x1000 alignment.

Both report version 6; they are told apart by whether index_size == count*24 (32-bit)
or count*48 (64-bit) under their respective header offsets.

32-bit header: flags@0x14C count@0x154 pkg@0x158 idx@0x15C names@0x160 data@0x164 comp@0x168
  entry: name_off, pad, data_off, usize, csize, pad          (u32 x6)
64-bit header: flags@0x14C count@0x158 pkg@0x160 idx@0x168 names@0x170 data@0x178 comp@0x180
  entry: name_off, pad, data_off, usize, csize, pad          (u64 x6)
"""
import os, struct, sys, zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lz4blk import framed, MAGIC1

MAGIC = 0x51890ACE


def _up(v, a):
    return (v + a - 1) // a * a


class Pack:
    def __init__(self, path):
        self.path = path
        self.f = open(path, "rb")
        hdr = self.f.read(0x1000)
        if struct.unpack_from("<I", hdr, 0)[0] != MAGIC:
            raise ValueError(f"not a vpp/str2 container: {path}")
        self.version = struct.unpack_from("<I", hdr, 4)[0]

        c32 = struct.unpack_from("<I", hdr, 0x154)[0]
        i32 = struct.unpack_from("<I", hdr, 0x15C)[0]
        c64 = struct.unpack_from("<Q", hdr, 0x158)[0]
        i64 = struct.unpack_from("<Q", hdr, 0x168)[0]

        if c32 and i32 == c32 * 24:
            self.wide = False
            self.align, self.dir_at, self.entry = 0x800, 0x800, 24
            self.count, self.idx_sz = c32, i32
            self.nm_sz = struct.unpack_from("<I", hdr, 0x160)[0]
            self.data_sz = struct.unpack_from("<I", hdr, 0x164)[0]
        elif c64 and i64 == c64 * 48:
            self.wide = True
            self.align, self.dir_at, self.entry = 0x1000, 0x1000, 48
            self.count, self.idx_sz = c64, i64
            self.nm_sz = struct.unpack_from("<Q", hdr, 0x170)[0]
            self.data_sz = struct.unpack_from("<Q", hdr, 0x178)[0]
        else:
            raise ValueError(f"unrecognised header: 32b({c32},{i32}) 64b({c64},{i64})")

        self.flags = struct.unpack_from("<I", hdr, 0x14C)[0]   # same offset in both layouts
        self.compressed = bool(self.flags & 1)
        self.condensed = bool(self.flags & 2)
        self.names_at = _up(self.dir_at + self.idx_sz, self.align)
        self.data_at = _up(self.names_at + self.nm_sz, self.align)

        self.f.seek(self.dir_at)
        idx = self.f.read(self.idx_sz)
        self.f.seek(self.names_at)
        names = self.f.read(self.nm_sz)

        self.items = []          # (name, data_off, usize, csize)
        for i in range(self.count):
            o = i * self.entry
            if self.wide:
                noff, _p0, doff, usz, csz, _p1 = struct.unpack_from("<6Q", idx, o)
                raw = 0xFFFFFFFFFFFFFFFF
            else:
                noff, _p0, doff, usz, csz, _p1 = struct.unpack_from("<6I", idx, o)
                raw = 0xFFFFFFFF
            end = names.index(b"\0", noff)
            self.items.append((names[noff:end].decode("latin1"), doff, usz,
                               None if csz == raw else csz))

    def __repr__(self):
        return (f"<Pack {os.path.basename(self.path)} {'64' if self.wide else '32'}-bit "
                f"flags=0x{self.flags:X} n={self.count}>")

    def names(self):
        return [it[0] for it in self.items]

    def _blobs(self):
        """All entry payloads, decoded. Only used for compressed containers."""
        if hasattr(self, "_b"):
            return self._b
        self.f.seek(self.data_at)
        raw = self.f.read()
        out = {}
        if self.wide and self.condensed:
            # One 0FEEDBEE-framed LZ4 block per entry, concatenated in index order;
            # the index offsets are in UNCOMPRESSED space, so walk the frames.
            off = 0
            for name, doff, usz, csz in self.items:
                data, used = framed(raw, off)
                if len(data) != usz:
                    raise ValueError(f"{name}: {len(data)} != {usz}")
                out[name] = data
                off += used
        elif self.wide:
            # Compressed but not condensed: each entry is its own frame, laid out at
            # running ALIGNED compressed offsets (the index offsets are uncompressed
            # space and cannot be used to find them).
            off = 0
            for name, doff, usz, csz in self.items:
                if csz is None:
                    out[name] = raw[off:off + usz]
                    off = _up(off + usz, self.align)
                    continue
                data, _used = framed(raw, off)
                if len(data) != usz:
                    raise ValueError(f"{name}: {len(data)} != {usz}")
                out[name] = data
                off = _up(off + csz, self.align)
        elif self.condensed:
            # Original: ONE zlib stream for the whole data block.
            whole = zlib.decompressobj().decompress(raw)
            for name, doff, usz, csz in self.items:
                out[name] = whole[doff:doff + usz]
        else:
            off = 0
            for name, doff, usz, csz in self.items:
                if csz is None:
                    out[name] = raw[doff:doff + usz]
                else:
                    out[name] = zlib.decompress(raw[off:off + csz])
                    off = _up(off + csz, self.align)
        self._b = out
        return out

    def raw_frames(self):
        """{name: (usize, framed_bytes)} without decompressing.

        Lets a rebuild carry unchanged entries across as their original LZ4 frames
        instead of recompressing them, which keeps rebuilt bundles near stock size.
        Only meaningful for the Remaster's compressed+condensed layout.
        """
        if not (self.wide and self.compressed and self.condensed):
            raise ValueError("raw_frames() only applies to 64-bit compressed bundles")
        self.f.seek(self.data_at)
        raw = self.f.read()
        out = {}
        off = 0
        for name, doff, usz, csz in self.items:
            m1, m2, cbytes, ubytes = struct.unpack_from("<4I", raw, off)
            if m1 != MAGIC1:
                raise ValueError(f"{name}: bad frame magic at {off}")
            size = 16 + cbytes
            out[name] = (ubytes, raw[off:off + size])
            off += size
        return out

    def read(self, name):
        """Bytes for one entry, by exact name."""
        for n, doff, usz, csz in self.items:
            if n == name:
                if not self.compressed:
                    self.f.seek(self.data_at + doff)
                    return self.f.read(usz)
                return self._blobs()[name]
        raise KeyError(name)


if __name__ == "__main__":
    p = Pack(sys.argv[1])
    pat = sys.argv[2].lower() if len(sys.argv) > 2 else ""
    hits = [it for it in p.items if pat in it[0].lower()]
    print(f"# {p}  matching={len(hits)}")
    for name, doff, usz, csz in sorted(hits):
        print(f"{usz:>10}  {name}")

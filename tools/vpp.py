"""Minimal VPP_PC / STR2_PC v6 reader for Saints Row: The Third.

The bundled tools/vpp_extract.py writes zero-length files for customize_player.vpp_pc, so this
reads the container directly. Both .vpp_pc and .str2_pc use the same v6 layout.

Header (offsets confirmed against the dry-run numbers this printed for customize_player):
    0x000  magic 0x51890ACE
    0x004  version (6)
    0x14C  flags        bit0 = compressed, bit1 = condensed
    0x154  index count
    0x158  package size
    0x15C  index size
    0x160  names size
    0x164  data size
    0x168  compressed data size

Index entries are 24 bytes each, starting at 0x800:
    0x00  name offset (into the names block)
    0x04  (unused/dir)
    0x08  data offset (relative to the data block)
    0x0C  (unused)
    0x10  uncompressed size
    0x14  compressed size (0xFFFFFFFF when stored raw)
"""
import os, struct, sys, zlib

MAGIC = 0x51890ACE
ALIGN = 2048
ENTRY = 24
DIR_AT = 0x800

def up(v, a=ALIGN):
    return (v + a - 1) // a * a

class Pack:
    def __init__(self, data):
        self.d = data
        assert struct.unpack_from("<I", data, 0)[0] == MAGIC, "not a vpp/str2 v6 container"
        self.version = struct.unpack_from("<I", data, 4)[0]
        self.flags   = struct.unpack_from("<I", data, 0x14C)[0]
        self.count   = struct.unpack_from("<I", data, 0x154)[0]
        self.idx_sz  = struct.unpack_from("<I", data, 0x15C)[0]
        self.nm_sz   = struct.unpack_from("<I", data, 0x160)[0]
        self.data_sz = struct.unpack_from("<I", data, 0x164)[0]
        self.comp_sz = struct.unpack_from("<I", data, 0x168)[0]
        self.compressed = bool(self.flags & 1)
        self.condensed  = bool(self.flags & 2)
        self.names_at = DIR_AT + up(self.idx_sz)
        self.data_at  = self.names_at + up(self.nm_sz)

    def entries(self):
        for i in range(self.count):
            o = DIR_AT + i * ENTRY
            # Verified against the raw bytes: name, pad, data offset, UNCOMPRESSED size,
            # COMPRESSED size (0xFFFFFFFF when stored raw), pad.
            name_off, _pad0, doff, usize, csize, _pad1 = struct.unpack_from("<6I", self.d, o)
            n = self.d.index(b"\0", self.names_at + name_off)
            name = self.d[self.names_at + name_off:n].decode("latin1")
            yield name, doff, usize, csize

    def read_all(self):
        """Every entry, in index order.

        A CONDENSED+COMPRESSED container (flags 0x…03, which is what .str2_pc uses) does not
        store a compressed offset per entry - the entry offsets are in UNCOMPRESSED space. The
        streams are simply concatenated in index order, so the compressed offset is the running
        sum of the compressed sizes. Walking them in order is the only way to find each one.
        """
        out = []
        whole = None
        if self.compressed and self.condensed:
            # ONE zlib stream for the entire data block, and the entry offsets index into the
            # DECOMPRESSED result. The header compressed size is the sum of the per-entry
            # compressed sizes, which is not the stream length - so feed the rest of the file
            # to a decompressobj and let it find its own end rather than trusting that number.
            whole = zlib.decompressobj().decompress(self.d[self.data_at:])
        comp_off = 0
        for name, doff, usize, csize in self.entries():
            if whole is not None:
                blob = whole[doff:doff + usize]
            elif self.compressed and csize != 0xFFFFFFFF:
                blob = zlib.decompress(self.d[self.data_at + comp_off:
                                              self.data_at + comp_off + csize])
                comp_off = up(comp_off + csize)
            else:
                blob = self.d[self.data_at + doff:self.data_at + doff + usize]
            out.append((name, blob, usize))
        return out

def extract(path, out):
    p = Pack(open(path, "rb").read())
    os.makedirs(out, exist_ok=True)
    n = 0
    try:
        items = p.read_all()
    except Exception as e:
        print("  FAIL", path, e); return 0, p.count
    for name, blob, usize in items:
        if len(blob) != usize:
            print("  SHORT", name, len(blob), "!=", usize); continue
        dest = os.path.join(out, name.replace("/", "_").replace("\\", "_"))
        open(dest, "wb").write(blob)
        n += 1
    return n, p.count

if __name__ == "__main__":
    got, tot = extract(sys.argv[1], sys.argv[2])
    print("%s: %d of %d files" % (os.path.basename(sys.argv[1]), got, tot))

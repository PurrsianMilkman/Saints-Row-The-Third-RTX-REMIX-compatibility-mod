"""PEG (GEKV v13) texture container reader for Saints Row: The Third.

A texture is a PAIR: the .cvbm_pc / .cpeg_pc holds the directory, the .gvbm_pc / .gpeg_pc holds
the pixels. Layout confirmed byte by byte against skin_pc_sb (128x128, format 400, frame_size
8192 - which is exactly 128*128/2, so format 400 is DXT1).

header, 24 bytes:
    +0x00 u32  'GEKV'
    +0x04 u32  version (13)
    +0x08 u32  dir block size   (== the .cvbm_pc file size)
    +0x0C u32  data block size  (== the .gvbm_pc file size)
    +0x10 u32  num bitmaps
    +0x14 u16  total entries
    +0x16 u16  align

entry, 72 bytes, first at +0x18:
    +0x00 u32  data offset into the .gvbm_pc
    +0x04 u32  (unused on disk)
    +0x08 u16  width          +0x0A u16 height
    +0x0C u16  bitmap format  +0x0E u16 palette format
    +0x10 u16  anim tiles w   +0x12 u16 anim tiles h
    +0x14 u32  frame count
    +0x18 u32  flags
    +0x1C u32  filename pointer (patched at runtime, zero on disk)
    +0x20 u16  palette size   +0x22 u8 fps   +0x23 u8 mip levels
    +0x24 u32  frame size
    ... pointers the game fixes up at load, zero on disk

Names are NUL-terminated strings at the end of the directory block, in entry order.
"""
import struct, io, os

# 72, not 56. A single-bitmap PEG cannot tell the two apart - 24 + 72 lands exactly where the
# name table starts either way - so the stride was only visible on a peg with three bitmaps in it,
# where the names came out as garbage until it was right.
ENTRY_STRIDE = 72

FMT = {400: ("DXT1", b"DXT1", 0.5), 401: ("DXT3", b"DXT3", 1.0), 402: ("DXT5", b"DXT5", 1.0),
       403: ("565", None, 2.0), 404: ("1555", None, 2.0), 405: ("4444", None, 2.0),
       406: ("888", None, 3.0), 407: ("8888", None, 4.0), 408: ("16_DUDV", None, 2.0),
       409: ("16_DOT3", None, 2.0), 410: ("A8", None, 1.0)}

class Peg:
    def __init__(self, cpu, gpu):
        self.d, self.g = cpu, gpu
        assert cpu[:4] == b"GEKV", "not a PEG"
        (self.version, self.dir_sz, self.data_sz, self.num,
         self.total, self.align) = struct.unpack_from("<IIIIHH", cpu, 4)
        self.entries = []
        for i in range(self.num):
            o = 0x18 + i * ENTRY_STRIDE
            doff, _p, w, h, bfmt, pfmt, aw, ah, frames, flags, _fn, psz, fps, mips, fsz = \
                struct.unpack_from("<IIHHHHHHIIIHBBI", cpu, o)
            self.entries.append(dict(off=doff, w=w, h=h, fmt=bfmt, frames=frames,
                                     mips=mips, size=fsz, flags=flags, name=None))
        # names: the strings that follow the entry table
        p = 0x18 + self.num * ENTRY_STRIDE
        while p < len(cpu) and cpu[p] == 0:
            p += 1
        for e in self.entries:
            if p >= len(cpu):
                break
            q = cpu.index(b"\0", p)
            e["name"] = cpu[p:q].decode("latin1")
            p = q + 1
            while p < len(cpu) and cpu[p] == 0:
                p += 1

    def to_dds(self, e):
        """Wrap one entry's pixels in a DDS so any normal decoder can read it."""
        name, fourcc, _bpp = FMT.get(e["fmt"], (str(e["fmt"]), None, 0))
        blob = self.g[e["off"]:e["off"] + e["size"]]
        if not blob:
            return None, name
        mips = max(1, e["mips"])
        flags = 0x1 | 0x2 | 0x4 | 0x1000 | (0x20000 if mips > 1 else 0)
        hdr = bytearray(128)
        hdr[0:4] = b"DDS "
        struct.pack_into("<I", hdr, 4, 124)
        struct.pack_into("<I", hdr, 8, flags)
        struct.pack_into("<I", hdr, 12, e["h"])
        struct.pack_into("<I", hdr, 16, e["w"])
        struct.pack_into("<I", hdr, 20, len(blob) if fourcc else e["w"] * 4)
        struct.pack_into("<I", hdr, 28, mips)
        struct.pack_into("<I", hdr, 76, 32)                      # pixelformat size
        if fourcc:
            struct.pack_into("<I", hdr, 80, 0x4)                 # DDPF_FOURCC
            hdr[84:88] = fourcc
        elif e["fmt"] in (407,):                                 # 8888 -> A8R8G8B8
            struct.pack_into("<I", hdr, 80, 0x41)                # RGB | ALPHAPIXELS
            struct.pack_into("<I", hdr, 88, 32)
            struct.pack_into("<IIII", hdr, 92, 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000)
        else:
            return None, name
        struct.pack_into("<I", hdr, 108, 0x1000)                 # caps: texture
        return bytes(hdr) + blob, name

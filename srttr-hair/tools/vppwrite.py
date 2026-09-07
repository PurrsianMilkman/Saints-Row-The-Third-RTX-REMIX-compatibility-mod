"""Writer for the SRTTR 64-bit VPP_PC / STR2_PC v6 container.

Mirrors vpp2.Pack. Verified against the shipped bundles: outside the fields written
here the whole 0x1000-byte header is zero.

    0x000 u32 magic 0x51890ACE      0x004 u32 version 6
    0x14C u32 flags                 0x158 u64 entry count
    0x160 u64 package size          0x168 u64 index size   (count * 48)
    0x170 u64 names size            0x178 u64 data size     (sum of uncompressed)
    0x180 u64 compressed size       (sum of per-entry frame sizes)

    index @0x1000, 48 B/entry: name_off, pad, data_off, usize, csize, pad  (u64 x6)
    names @align(index_end, 0x1000), NUL-terminated, index order
    data  @align(names_end, 0x1000)

flags 0x4803 = compressed | condensed: data_off is an offset into UNCOMPRESSED space
(the running sum of usize) and the LZ4 frames are simply concatenated, no alignment.
"""
import struct

MAGIC = 0x51890ACE
ALIGN = 0x1000
DIR_AT = 0x1000
ENTRY = 48
FLAGS_COMPRESSED_CONDENSED = 0x4803


def _up(v, a=ALIGN):
    return (v + a - 1) // a * a


def build(entries, compressor):
    """entries: list of (name, payload_bytes), in the order they should be indexed.

    compressor: callable(bytes) -> framed bytes. Returns the whole container.
    """
    return build_framed([(n, len(p), compressor(p)) for n, p in entries])


def build_framed(entries):
    """entries: list of (name, uncompressed_size, already_framed_bytes).

    Used when most payloads are being carried over unchanged and only need to be
    copied, not recompressed.
    """
    names_blob = bytearray()
    name_offs = []
    for name, _, _ in entries:
        name_offs.append(len(names_blob))
        names_blob += name.encode("latin1") + b"\0"

    frames = []
    data_off = 0
    index = bytearray()
    total_u = total_c = 0
    for (name, usize, fr), noff in zip(entries, name_offs):
        frames.append(fr)
        index += struct.pack("<6Q", noff, 0, data_off, usize, len(fr), 0)
        data_off += usize
        total_u += usize
        total_c += len(fr)

    idx_sz = len(index)
    nm_sz = len(names_blob)
    names_at = _up(DIR_AT + idx_sz)
    data_at = _up(names_at + nm_sz)

    hdr = bytearray(DIR_AT)
    struct.pack_into("<I", hdr, 0x000, MAGIC)
    struct.pack_into("<I", hdr, 0x004, 6)
    struct.pack_into("<I", hdr, 0x14C, FLAGS_COMPRESSED_CONDENSED)
    struct.pack_into("<Q", hdr, 0x158, len(entries))
    struct.pack_into("<Q", hdr, 0x160, data_at + total_c)
    struct.pack_into("<Q", hdr, 0x168, idx_sz)
    struct.pack_into("<Q", hdr, 0x170, nm_sz)
    struct.pack_into("<Q", hdr, 0x178, total_u)
    struct.pack_into("<Q", hdr, 0x180, total_c)

    out = bytearray(hdr)
    out += index
    out += b"\0" * (names_at - len(out))
    out += names_blob
    out += b"\0" * (data_at - len(out))
    for fr in frames:
        out += fr
    return bytes(out)

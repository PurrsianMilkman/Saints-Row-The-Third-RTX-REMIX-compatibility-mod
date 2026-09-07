"""Pure-python LZ4 block decompressor (no external deps).

Saints Row: The Third Remastered wraps each packfile entry in a 16-byte frame:
    0x00  u32 magic  0x0FEEDBEE
    0x04  u32 magic  0x00BADBEE
    0x08  u32 compressed payload size (frame header excluded)
    0x0C  u32 uncompressed size
followed by one LZ4 block.
"""
import struct

MAGIC1 = 0x0FEEDBEE
MAGIC2 = 0x00BADBEE
HDR = 16


def lz4_block(src, want=None):
    dst = bytearray()
    i, n = 0, len(src)
    while i < n:
        tok = src[i]; i += 1
        lit = tok >> 4
        if lit == 15:
            while True:
                b = src[i]; i += 1
                lit += b
                if b != 255:
                    break
        if lit:
            dst += src[i:i + lit]; i += lit
        if i >= n:
            break
        off = src[i] | (src[i + 1] << 8); i += 2
        if off == 0:
            raise ValueError("lz4: zero match offset")
        mlen = (tok & 0xF)
        if mlen == 15:
            while True:
                b = src[i]; i += 1
                mlen += b
                if b != 255:
                    break
        mlen += 4
        p = len(dst) - off
        if p < 0:
            raise ValueError(f"lz4: offset {off} past start at {len(dst)}")
        for _ in range(mlen):          # byte-wise: overlapping matches are legal
            dst.append(dst[p]); p += 1
        if want is not None and len(dst) >= want:
            break
    return bytes(dst)


def framed(buf, off=0):
    """Decode one 0FEEDBEE frame at `buf[off:]`. Returns (data, total_frame_bytes)."""
    m1, m2, csz, usz = struct.unpack_from("<4I", buf, off)
    if m1 != MAGIC1 or m2 != MAGIC2:
        raise ValueError(f"bad frame magic {m1:08X}/{m2:08X} at {off}")
    out = lz4_block(buf[off + HDR:off + HDR + csz], usz)
    if len(out) != usz:
        raise ValueError(f"lz4: got {len(out)} bytes, header says {usz}")
    return out, HDR + csz

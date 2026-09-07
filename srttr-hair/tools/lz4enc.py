"""LZ4 block compressor + 0FEEDBEE frame writer (pure python, no deps).

Produces the exact stream shape SRTTR's packfile loader expects:
    0x00  u32 0x0FEEDBEE
    0x04  u32 0x00BADBEE
    0x08  u32 compressed payload size (frame header excluded)
    0x0C  u32 uncompressed size
    0x10  LZ4 block

The matcher is a plain 4-byte hash table with a single candidate per slot. That is
weaker than the reference encoder, but every stream it emits is valid LZ4 - the
decoder is the game's, and it only cares about the token grammar.
"""
import struct

MAGIC1 = 0x0FEEDBEE
MAGIC2 = 0x00BADBEE
HDR = 16

MIN_MATCH = 4
LAST_LITERALS = 5          # LZ4 requires the last sequence to be literals only
MF_LIMIT = 12              # ...and no match may start within 12 bytes of the end
HASH_BITS = 16
MAX_OFFSET = 0xFFFF


def _emit_len(out, n):
    """Emit the 255-chained extra-length bytes for a length nibble that hit 15."""
    while n >= 255:
        out.append(255)
        n -= 255
    out.append(n)


def lz4_block(src):
    n = len(src)
    out = bytearray()
    if n < MF_LIMIT + LAST_LITERALS:
        # too short to match into - one all-literal sequence
        tok_lit = n
        out.append(min(15, tok_lit) << 4)
        if tok_lit >= 15:
            _emit_len(out, tok_lit - 15)
        out += src
        return bytes(out)

    table = [-1] * (1 << HASH_BITS)
    anchor = 0
    i = 0
    limit = n - MF_LIMIT - LAST_LITERALS

    while i <= limit:
        seq = src[i:i + 4]
        h = (int.from_bytes(seq, "little") * 2654435761 >> (32 - HASH_BITS)) & ((1 << HASH_BITS) - 1)
        ref = table[h]
        table[h] = i
        if ref < 0 or i - ref > MAX_OFFSET or src[ref:ref + 4] != seq:
            i += 1
            continue

        # extend the match forwards, stopping short of the mandatory last literals
        end = n - LAST_LITERALS
        m = 4
        while i + m < end and src[ref + m] == src[i + m]:
            m += 1

        lit = i - anchor
        tok = (min(15, lit) << 4) | min(15, m - MIN_MATCH)
        out.append(tok)
        if lit >= 15:
            _emit_len(out, lit - 15)
        out += src[anchor:i]
        out += struct.pack("<H", i - ref)
        if m - MIN_MATCH >= 15:
            _emit_len(out, m - MIN_MATCH - 15)

        i += m
        anchor = i

    lit = n - anchor
    out.append(min(15, lit) << 4)
    if lit >= 15:
        _emit_len(out, lit - 15)
    out += src[anchor:]
    return bytes(out)


def frame(data):
    """One complete 0FEEDBEE frame for `data`."""
    blk = lz4_block(data)
    return struct.pack("<4I", MAGIC1, MAGIC2, len(blk), len(data)) + blk

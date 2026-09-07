"""Build SRTTR-shaped hair textures from the SRTT originals.

SRTT hair packs its data unusually: the _dob diffuse is DXT1 with **no alpha at all**;
R and B carry an identical greyscale strand mask and G is pinned to 255. The shader
therefore takes opacity from R and uses a flat white diffuse that the player's chosen
hair colour tints. SRTTR instead expects a real 8-bit alpha (DXT5) plus an _arm map.

The conversion keeps SRTT's shading inputs exactly:
    alpha  <- the original R channel  (the real strand mask)
    RGB    <- flat white              (what G=255 meant)
so strand detail comes from the normal map, as it did in SRTT.

That makes the DXT5 colour half a constant, and the only real encoder needed is BC4
for the alpha half.
"""
import struct
from PIL import Image

# DXT5 colour half: c0 = c1 = white, all indices 0. In DXT5 the colour block is always
# read in 4-colour mode, so palette entry 0 is c0 and every texel comes out white.
WHITE_COLOUR_HALF = struct.pack("<HHI", 0xFFFF, 0xFFFF, 0)


def bc4_block(vals):
    """Encode 16 bytes (a 4x4 tile, row-major) as one BC4/DXT5-alpha block."""
    lo, hi = min(vals), max(vals)
    if lo == hi:
        return bytes([lo, hi, 0, 0, 0, 0, 0, 0])
    # 8-value mode: a0 > a1 gives endpoints plus 6 interpolants
    a0, a1 = hi, lo
    step = (a0 - a1)
    idx = 0
    for i, v in enumerate(vals):
        # palette: 0 -> a0, 1 -> a1, 2..7 -> a0 + k*(a1-a0)/7
        t = (a0 - v) * 7 // step if step else 0
        t = 0 if t < 0 else (7 if t > 7 else t)
        code = 0 if t == 0 else (1 if t == 7 else t + 1)
        idx |= code << (3 * i)
    return bytes([a0, a1]) + idx.to_bytes(6, "little")


def encode_alpha_dxt5(img):
    """img: 'L' mode. Returns DXT5 bytes with a constant-white colour half."""
    w, h = img.size
    px = img.load()
    out = bytearray()
    for by in range(0, h, 4):
        for bx in range(0, w, 4):
            vals = []
            for y in range(4):
                for x in range(4):
                    vals.append(px[min(bx + x, w - 1), min(by + y, h - 1)])
            out += bc4_block(vals)
            out += WHITE_COLOUR_HALF
    return bytes(out)


def mip_chain(img, levels):
    """Successive halvings, starting at `img`, stopping after `levels` entries."""
    out = [img]
    cur = img
    for _ in range(levels - 1):
        cur = cur.resize((max(1, cur.width // 2), max(1, cur.height // 2)), Image.BOX)
        out.append(cur)
    return out


def dxt5_with_mips(alpha_img, levels):
    return b"".join(encode_alpha_dxt5(m) for m in mip_chain(alpha_img, levels))


def dxt1_solid(rgb, w, h):
    """A flat DXT1 surface of one colour - used for the synthesised _arm map."""
    r, g, b = rgb
    c = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
    blk = struct.pack("<HHI", c, c, 0)
    return blk * ((w // 4) * (h // 4))


def dxt_size(fmt, w, h, levels):
    """Total bytes of a mip chain. 400=DXT1 (8B/block), 402=DXT5, 411=BC5 (16B/block)."""
    per = 8 if fmt == 400 else 16
    total = 0
    for i in range(levels):
        mw, mh = max(1, w >> i), max(1, h >> i)
        total += max(1, mw // 4) * max(1, mh // 4) * per
    return total

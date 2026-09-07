"""Minimal DXT1/DXT5/BC5 decoders -> PIL images. No numpy needed."""
import struct
from PIL import Image


def _c565(v):
    return ((v >> 11 & 31) * 255 // 31, (v >> 5 & 63) * 255 // 63, (v & 31) * 255 // 31)


def _blocks(w, h):
    for by in range(0, h, 4):
        for bx in range(0, w, 4):
            yield bx, by


def decode_dxt1(data, w, h):
    """DXT1. Returns (rgb_image, alpha_image) - alpha is the 1-bit punch-through."""
    rgb = Image.new("RGB", (w, h)); a = Image.new("L", (w, h), 255)
    pr, pa = rgb.load(), a.load()
    i = 0
    for bx, by in _blocks(w, h):
        c0, c1, bits = struct.unpack_from("<HHI", data, i); i += 8
        p0, p1 = _c565(c0), _c565(c1)
        if c0 > c1:
            pal = [p0, p1,
                   tuple((2 * p0[k] + p1[k]) // 3 for k in range(3)),
                   tuple((p0[k] + 2 * p1[k]) // 3 for k in range(3))]
            alp = [255] * 4
        else:
            pal = [p0, p1, tuple((p0[k] + p1[k]) // 2 for k in range(3)), (0, 0, 0)]
            alp = [255, 255, 255, 0]
        for y in range(4):
            for x in range(4):
                px, py = bx + x, by + y
                if px >= w or py >= h:
                    continue
                i2 = (bits >> (2 * (4 * y + x))) & 3
                pr[px, py] = pal[i2]; pa[px, py] = alp[i2]
    return rgb, a


def decode_dxt5(data, w, h):
    """DXT5. Returns (rgb_image, alpha_image) with full 8-bit alpha."""
    rgb = Image.new("RGB", (w, h)); a = Image.new("L", (w, h))
    pr, pa = rgb.load(), a.load()
    i = 0
    for bx, by in _blocks(w, h):
        a0, a1 = data[i], data[i + 1]
        abits = int.from_bytes(data[i + 2:i + 8], "little")
        if a0 > a1:
            at = [a0, a1] + [((7 - k) * a0 + k * a1) // 7 for k in range(1, 7)]
        else:
            at = [a0, a1] + [((5 - k) * a0 + k * a1) // 5 for k in range(1, 5)] + [0, 255]
        c0, c1, bits = struct.unpack_from("<HHI", data, i + 8); i += 16
        p0, p1 = _c565(c0), _c565(c1)
        pal = [p0, p1,
               tuple((2 * p0[k] + p1[k]) // 3 for k in range(3)),
               tuple((p0[k] + 2 * p1[k]) // 3 for k in range(3))]
        for y in range(4):
            for x in range(4):
                px, py = bx + x, by + y
                if px >= w or py >= h:
                    continue
                n = 4 * y + x
                pr[px, py] = pal[(bits >> (2 * n)) & 3]
                pa[px, py] = at[(abits >> (3 * n)) & 7]
    return rgb, a


def decode_bc5(data, w, h):
    """BC5/ATI2 (two DXT5-style alpha blocks: R then G). Returns an RGB image."""
    img = Image.new("RGB", (w, h)); p = img.load()
    i = 0
    for bx, by in _blocks(w, h):
        chans = []
        for c in range(2):
            e0, e1 = data[i], data[i + 1]
            bits = int.from_bytes(data[i + 2:i + 8], "little"); i += 8
            if e0 > e1:
                t = [e0, e1] + [((7 - k) * e0 + k * e1) // 7 for k in range(1, 7)]
            else:
                t = [e0, e1] + [((5 - k) * e0 + k * e1) // 5 for k in range(1, 5)] + [0, 255]
            chans.append((t, bits))
        for y in range(4):
            for x in range(4):
                px, py = bx + x, by + y
                if px >= w or py >= h:
                    continue
                n = 4 * y + x
                r = chans[0][0][(chans[0][1] >> (3 * n)) & 7]
                g = chans[1][0][(chans[1][1] >> (3 * n)) & 7]
                p[px, py] = (r, g, 255)
    return img


def size_of(fmt, w, h):
    if fmt == 400:
        return max(1, w // 4) * max(1, h // 4) * 8
    return max(1, w // 4) * max(1, h // 4) * 16     # DXT5 (402) and BC5 (411)

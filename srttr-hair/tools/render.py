"""Tiny z-buffered flat-shaded software rasteriser (PIL only) for mesh comparison."""
import math
from PIL import Image


def render(tris, verts, axes, bounds, size=520, bg=(18, 18, 22), tint=(1.0, 1.0, 1.0)):
    """axes: (h, v, depth) indices into the position tuple; depth is the view direction."""
    ai, bi, di = axes
    (lo_h, hi_h), (lo_v, hi_v) = bounds
    W = H = size
    pad = 0.06
    sc = (1 - 2 * pad) * size / max(hi_h - lo_h, hi_v - lo_v)
    cx = size / 2 - (lo_h + hi_h) / 2 * sc
    cy = size / 2 + (lo_v + hi_v) / 2 * sc

    img = Image.new("RGB", (W, H), bg)
    px = img.load()
    zbuf = [[-1e30] * W for _ in range(H)]

    for t in tris:
        p = [verts[i] for i in t]
        sx = [p[k][ai] * sc + cx for k in range(3)]
        sy = [-p[k][bi] * sc + cy for k in range(3)]
        sz = [p[k][di] for k in range(3)]
        # face normal in world space for shading
        u = [p[1][k] - p[0][k] for k in range(3)]
        v = [p[2][k] - p[0][k] for k in range(3)]
        n = (u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0])
        ln = math.sqrt(sum(c * c for c in n)) or 1.0
        n = [c / ln for c in n]
        # headlight + a little key light
        lam = abs(n[di]) * 0.75 + abs(n[0]) * 0.15 + abs(n[1]) * 0.10
        shade = 0.15 + 0.85 * lam

        minx, maxx = max(0, int(min(sx))), min(W - 1, int(max(sx)) + 1)
        miny, maxy = max(0, int(min(sy))), min(H - 1, int(max(sy)) + 1)
        if minx > maxx or miny > maxy:
            continue
        d = (sy[1] - sy[2]) * (sx[0] - sx[2]) + (sx[2] - sx[1]) * (sy[0] - sy[2])
        if abs(d) < 1e-9:
            continue
        for y in range(miny, maxy + 1):
            for x in range(minx, maxx + 1):
                w0 = ((sy[1] - sy[2]) * (x - sx[2]) + (sx[2] - sx[1]) * (y - sy[2])) / d
                w1 = ((sy[2] - sy[0]) * (x - sx[2]) + (sx[0] - sx[2]) * (y - sy[2])) / d
                w2 = 1 - w0 - w1
                if w0 < 0 or w1 < 0 or w2 < 0:
                    continue
                z = w0 * sz[0] + w1 * sz[1] + w2 * sz[2]
                if z > zbuf[y][x]:
                    zbuf[y][x] = z
                    px[x, y] = tuple(min(255, int(255 * shade * tint[k])) for k in range(3))
    return img

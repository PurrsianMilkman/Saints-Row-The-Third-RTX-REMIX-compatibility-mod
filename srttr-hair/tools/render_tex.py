"""Textured, alpha-tested z-buffered rasteriser - reproduces the in-game silhouette."""
import math
from PIL import Image


def render(tris, verts, uvs, axes, bounds, tex, alpha=None, athr=None,
           size=520, bg=(18, 18, 22)):
    ai, bi, di = axes
    (lo_h, hi_h), (lo_v, hi_v) = bounds
    pad = 0.06
    sc = (1 - 2 * pad) * size / max(hi_h - lo_h, hi_v - lo_v)
    cx = size / 2 - (lo_h + hi_h) / 2 * sc
    cy = size / 2 + (lo_v + hi_v) / 2 * sc

    tw, th = tex.size
    tp = tex.load()
    ap = alpha.load() if alpha is not None else None
    aw, ah = (alpha.size if alpha is not None else (0, 0))

    img = Image.new("RGB", (size, size), bg)
    px = img.load()
    zbuf = [[-1e30] * size for _ in range(size)]

    for t in tris:
        p = [verts[i] for i in t]
        q = [uvs[i] for i in t]
        sx = [p[k][ai] * sc + cx for k in range(3)]
        sy = [-p[k][bi] * sc + cy for k in range(3)]
        sz = [p[k][di] for k in range(3)]
        u = [p[1][k] - p[0][k] for k in range(3)]
        v = [p[2][k] - p[0][k] for k in range(3)]
        n = (u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0])
        ln = math.sqrt(sum(c * c for c in n)) or 1.0
        shade = 0.35 + 0.65 * abs(n[di] / ln)

        minx, maxx = max(0, int(min(sx))), min(size - 1, int(max(sx)) + 1)
        miny, maxy = max(0, int(min(sy))), min(size - 1, int(max(sy)) + 1)
        d = (sy[1] - sy[2]) * (sx[0] - sx[2]) + (sx[2] - sx[1]) * (sy[0] - sy[2])
        if abs(d) < 1e-9 or minx > maxx or miny > maxy:
            continue
        for y in range(miny, maxy + 1):
            for x in range(minx, maxx + 1):
                w0 = ((sy[1] - sy[2]) * (x - sx[2]) + (sx[2] - sx[1]) * (y - sy[2])) / d
                w1 = ((sy[2] - sy[0]) * (x - sx[2]) + (sx[0] - sx[2]) * (y - sy[2])) / d
                w2 = 1 - w0 - w1
                if w0 < 0 or w1 < 0 or w2 < 0:
                    continue
                z = w0 * sz[0] + w1 * sz[1] + w2 * sz[2]
                if z <= zbuf[y][x]:
                    continue
                uu = (w0 * q[0][0] + w1 * q[1][0] + w2 * q[2][0]) / 1024.0
                vv = (w0 * q[0][1] + w1 * q[1][1] + w2 * q[2][1]) / 1024.0
                if ap is not None and athr is not None:
                    if ap[int(uu * aw) % aw, int(vv * ah) % ah] < athr:
                        continue
                c = tp[int(uu * tw) % tw, int(vv * th) % th]
                zbuf[y][x] = z
                px[x, y] = tuple(min(255, int(c[k] * shade)) for k in range(3))
    return img

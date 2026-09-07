"""Project SRTTR hair vertices onto a smoothed SRTT target surface.

Only float3 positions change - vertex count, UVs, bone weights, LOD ranges, morph
data, textures and every file size stay exactly as SRTTR shipped them.
"""
import math
from collections import defaultdict


def closest_on_tri(p, a, b, c):
    ab = [b[i] - a[i] for i in range(3)]
    ac = [c[i] - a[i] for i in range(3)]
    ap = [p[i] - a[i] for i in range(3)]
    d1 = sum(ab[i] * ap[i] for i in range(3))
    d2 = sum(ac[i] * ap[i] for i in range(3))
    if d1 <= 0 and d2 <= 0:
        return a
    bp = [p[i] - b[i] for i in range(3)]
    d3 = sum(ab[i] * bp[i] for i in range(3))
    d4 = sum(ac[i] * bp[i] for i in range(3))
    if d3 >= 0 and d4 <= d3:
        return b
    vc = d1 * d4 - d3 * d2
    if vc <= 0 and d1 >= 0 and d3 <= 0:
        v = d1 / (d1 - d3) if d1 != d3 else 0.0
        return [a[i] + ab[i] * v for i in range(3)]
    cp = [p[i] - c[i] for i in range(3)]
    d5 = sum(ab[i] * cp[i] for i in range(3))
    d6 = sum(ac[i] * cp[i] for i in range(3))
    if d6 >= 0 and d5 <= d6:
        return c
    vb = d5 * d2 - d1 * d6
    if vb <= 0 and d2 >= 0 and d6 <= 0:
        w = d2 / (d2 - d6) if d2 != d6 else 0.0
        return [a[i] + ac[i] * w for i in range(3)]
    va = d3 * d6 - d5 * d4
    if va <= 0 and (d4 - d3) >= 0 and (d5 - d6) >= 0:
        w = (d4 - d3) / ((d4 - d3) + (d5 - d6))
        return [b[i] + (c[i] - b[i]) * w for i in range(3)]
    den = 1.0 / (va + vb + vc)
    v, w = vb * den, vc * den
    return [a[i] + ab[i] * v + ac[i] * w for i in range(3)]


class Surface:
    """Uniform-grid accelerated closest-point queries against a triangle soup."""

    def __init__(self, verts, tris, res=16):
        self.tris = [(verts[a], verts[b], verts[c]) for a, b, c in tris]
        self.mn = [min(v[i] for v in verts) for i in range(3)]
        mx = [max(v[i] for v in verts) for i in range(3)]
        self.n = res
        self.cell = [(mx[i] - self.mn[i]) / res + 1e-9 for i in range(3)]
        self.grid = defaultdict(list)
        for ti, (a, b, c) in enumerate(self.tris):
            lo = [int((min(a[i], b[i], c[i]) - self.mn[i]) / self.cell[i]) for i in range(3)]
            hi = [int((max(a[i], b[i], c[i]) - self.mn[i]) / self.cell[i]) for i in range(3)]
            for x in range(max(0, lo[0]), min(res, hi[0] + 1)):
                for y in range(max(0, lo[1]), min(res, hi[1] + 1)):
                    for z in range(max(0, lo[2]), min(res, hi[2] + 1)):
                        self.grid[(x, y, z)].append(ti)

    def closest(self, p):
        base = [int((p[i] - self.mn[i]) / self.cell[i]) for i in range(3)]
        step = min(self.cell)
        best, bd = None, 1e30
        for r in range(self.n):
            seen = set()
            for x in range(base[0] - r, base[0] + r + 1):
                for y in range(base[1] - r, base[1] + r + 1):
                    for z in range(base[2] - r, base[2] + r + 1):
                        if max(abs(x - base[0]), abs(y - base[1]), abs(z - base[2])) != r:
                            continue
                        seen.update(self.grid.get((x, y, z), ()))
            for ti in seen:
                q = closest_on_tri(p, *self.tris[ti])
                d = sum((q[i] - p[i]) ** 2 for i in range(3))
                if d < bd:
                    bd, best = d, q
            if best is not None and bd < (r * step) ** 2:
                break
        return best, math.sqrt(bd)


def standoff(verts, moved, k=0.25):
    """Keep a fraction `k` of each vertex's original distance from the target surface.

    A raw projection (k=0) puts every vertex exactly on the SRTT shell, which matches
    the silhouette but collapses the Remaster's many stacked hair cards into a single
    fused surface - measured faceting jumps from 15.9 to 25.9 degrees and the strand
    layering is lost. Smoothing the displacement field instead (the earlier approach)
    was worse on both counts: it left 4.3 mm of unstructured residual AND pushed
    faceting to 20.1.

    Keeping a quarter of the original stand-off compresses the layering onto the SRTT
    shell rather than flattening it: faceting comes out at 15.8, indistinguishable
    from stock, while 75% of the shape correction is applied.
    """
    return [tuple(moved[i][j] + (verts[i][j] - moved[i][j]) * k for j in range(3))
            for i in range(len(verts))]


def relax(verts, tris, moved, iterations=3, strength=0.5):
    """Laplacian relaxation applied only to the displacement field.

    Smoothing the *offsets* rather than the positions keeps SRTT's profile while
    removing the splayed facets a raw nearest-point projection leaves behind.
    """
    nb = defaultdict(set)
    for a, b, c in tris:
        nb[a].update((b, c))
        nb[b].update((a, c))
        nb[c].update((a, b))
    delta = [[moved[i][k] - verts[i][k] for k in range(3)] for i in range(len(verts))]
    for _ in range(iterations):
        new = [d[:] for d in delta]
        for i, d in enumerate(delta):
            links = nb.get(i)
            if not links:
                continue
            avg = [0.0, 0.0, 0.0]
            for j in links:
                for k in range(3):
                    avg[k] += delta[j][k]
            n = len(links)
            for k in range(3):
                new[i][k] = d[k] + strength * (avg[k] / n - d[k])
        delta = new
    return [tuple(verts[i][k] + delta[i][k] for k in range(3)) for i in range(len(verts))]

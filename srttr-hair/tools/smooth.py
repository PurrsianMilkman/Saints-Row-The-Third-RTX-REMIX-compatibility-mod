"""Weld / subdivide / smooth helpers used to turn a coarse SRTT hair mesh into a
clean target surface for reshaping the SRTTR mesh onto.

SRTT hair is ~700 triangles, so projecting the Remaster's 5-6k triangle mesh straight
onto it stamps SRTT's faceting into the result. Subdividing and Taubin-smoothing the
target first gives a surface with SRTT's *profile* but none of its faceting.

Hair meshes are open shells (every card has a free border), so boundary vertices are
smoothed only against their boundary neighbours - otherwise the card edges creep
inward and the silhouette shrinks.
"""
import math
from collections import defaultdict


def weld(verts, tris, eps=1e-6):
    """Merge coincident positions so smoothing propagates across UV seams."""
    q = 1.0 / eps
    lut = {}
    remap = []
    out = []
    for p in verts:
        k = (round(p[0] * q), round(p[1] * q), round(p[2] * q))
        i = lut.get(k)
        if i is None:
            i = len(out)
            lut[k] = i
            out.append(list(p))
        remap.append(i)
    wt = []
    for a, b, c in tris:
        a, b, c = remap[a], remap[b], remap[c]
        if a != b and b != c and a != c:
            wt.append((a, b, c))
    return out, wt, remap


def edge_map(tris):
    """edge -> list of adjacent triangle indices."""
    em = defaultdict(list)
    for ti, (a, b, c) in enumerate(tris):
        for u, v in ((a, b), (b, c), (c, a)):
            em[(u, v) if u < v else (v, u)].append(ti)
    return em


def boundary_vertices(tris):
    """Vertices on an edge used by exactly one triangle, plus their boundary links."""
    em = edge_map(tris)
    bset = set()
    blinks = defaultdict(set)
    for (u, v), ts in em.items():
        if len(ts) == 1:
            bset.add(u)
            bset.add(v)
            blinks[u].add(v)
            blinks[v].add(u)
    return bset, blinks


def neighbours(verts, tris):
    nb = defaultdict(set)
    for a, b, c in tris:
        nb[a].update((b, c))
        nb[b].update((a, c))
        nb[c].update((a, b))
    return nb


def subdivide(verts, tris):
    """One round of 4:1 midpoint subdivision."""
    verts = [list(p) for p in verts]
    mid = {}

    def m(u, v):
        k = (u, v) if u < v else (v, u)
        i = mid.get(k)
        if i is None:
            i = len(verts)
            verts.append([(verts[u][j] + verts[v][j]) * 0.5 for j in range(3)])
            mid[k] = i
        return i

    out = []
    for a, b, c in tris:
        ab, bc, ca = m(a, b), m(b, c), m(c, a)
        out += [(a, ab, ca), (ab, b, bc), (ca, bc, c), (ab, bc, ca)]
    return verts, out


def taubin(verts, tris, iterations=12, lam=0.53, mu=-0.55):
    """Taubin lambda/mu smoothing - low-pass without the shrinkage of plain Laplacian."""
    verts = [list(p) for p in verts]
    nb = neighbours(verts, tris)
    bset, blinks = boundary_vertices(tris)

    def pass_(factor):
        new = [p[:] for p in verts]
        for i, p in enumerate(verts):
            links = blinks[i] if i in bset else nb[i]
            if not links:
                continue
            n = len(links)
            avg = [0.0, 0.0, 0.0]
            for j in links:
                q = verts[j]
                avg[0] += q[0]; avg[1] += q[1]; avg[2] += q[2]
            for k in range(3):
                new[i][k] = p[k] + factor * (avg[k] / n - p[k])
        return new

    for _ in range(iterations):
        verts = pass_(lam)
        verts = pass_(mu)
    return verts


def build_target(verts, tris, levels=2, iterations=12):
    """Weld -> subdivide `levels` times -> Taubin smooth. Returns (verts, tris)."""
    v, t, _ = weld(verts, tris)
    for _ in range(levels):
        v, t = subdivide(v, t)
    v = taubin(v, t, iterations=iterations)
    return v, t


def mesh_stats(verts, tris):
    """Mean dihedral-angle deviation - a rough faceting score, in degrees."""
    em = edge_map(tris)
    norms = []
    for a, b, c in tris:
        p, q, r = verts[a], verts[b], verts[c]
        u = [q[i] - p[i] for i in range(3)]
        v = [r[i] - p[i] for i in range(3)]
        n = (u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0])
        ln = math.sqrt(sum(x * x for x in n)) or 1.0
        norms.append([x / ln for x in n])
    angs = []
    for ts in em.values():
        if len(ts) != 2:
            continue
        d = sum(norms[ts[0]][i] * norms[ts[1]][i] for i in range(3))
        angs.append(math.degrees(math.acos(max(-1.0, min(1.0, d)))))
    return sum(angs) / len(angs) if angs else 0.0

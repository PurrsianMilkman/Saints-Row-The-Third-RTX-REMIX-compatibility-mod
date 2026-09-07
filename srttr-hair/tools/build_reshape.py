"""Reshape SRTTR hair meshes onto the SRTT silhouette and repack the bundles.

For every hair style present in both games:

  1. read the SRTT mesh and build a smooth target surface from it
     (weld -> 2x midpoint subdivision -> Taubin smoothing), so the Remaster mesh
     picks up SRTT's *profile* without SRTT's 1.2k-triangle faceting;
  2. project every SRTTR vertex onto that surface;
  3. keep a quarter of each vertex's original stand-off, so the Remaster's stacked
     hair cards are compressed onto the SRTT shell rather than collapsed into it;
  4. write the new float3 positions back into the .gcmesh_pc IN PLACE.

Only positions change. Vertex count, stride, UVs, bone weights, index buffer, LOD
ranges, morph data, textures, materials and every uncompressed file size are byte
for byte what SRTTR shipped, so nothing downstream can go out of sync. The only
number that moves is each container's compressed size, which is patched into
customize_item.asm_pc.

Unchanged files are copied across as their original LZ4 frames rather than being
recompressed, so the rebuilt bundles stay close to stock size.

Usage:
    python build_reshape.py <out_dir> [--style NAME] [--limit N]
"""
import json
import math
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vpp2 import Pack                                     # noqa: E402
from vppwrite import build_framed                         # noqa: E402
from lz4enc import frame                                  # noqa: E402
from srmesh import Mesh                                   # noqa: E402
from smooth import weld, subdivide, taubin                # noqa: E402
from reshape import Surface, standoff                     # noqa: E402

from paths import SRTT, srttr_stock                        # noqa: E402

SRTTR = srttr_stock()

SUBDIV = 2          # midpoint subdivisions of the SRTT target
SMOOTH = 10         # Taubin iterations
STANDOFF = 0.25     # fraction of each card's original stand-off to keep


def style_of(names):
    for n in names:
        if n.endswith(".ccmesh_pc"):
            return n[:-len(".ccmesh_pc")]
    return None


def load_mesh(pack, bundle, style, scratch):
    tmp = os.path.join(scratch, "_b.str2_pc")
    with open(tmp, "wb") as fh:
        fh.write(pack.read(bundle))
    sub = Pack(tmp)
    cc = sub.read(style + ".ccmesh_pc")
    gc = sub.read(style + ".gcmesh_pc")
    return sub, cc, gc


def build_target(cc, gc):
    m = Mesh(cc, gc)
    v, t, _ = weld(m.positions(), m.tris())
    for _ in range(SUBDIV):
        v, t = subdivide(v, t)
    return Surface(taubin(v, t, iterations=SMOOTH), t, res=20)


def reshape_gcmesh(cc, gc, surf):
    """Returns (new_gcmesh_bytes, stats)."""
    m = Mesh(cc, gc)
    src = m.positions()
    tris = m.tris()
    proj = []
    dists = []
    for p in src:
        q, d = surf.closest(p)
        proj.append(q)
        dists.append(d)
    final = standoff(src, proj, k=STANDOFF)
    out = bytearray(gc)
    for i, p in enumerate(final):
        struct.pack_into("<3f", out, m.o_vtx + i * m.stride, *p)
    moved = [math.dist(src[i], final[i]) for i in range(len(src))]
    return bytes(out), {
        "verts": len(src),
        "tris": len(tris),
        "mean_mm": sum(moved) / len(moved) * 1000.0,
        "max_mm": max(moved) * 1000.0,
    }


def main():
    out_dir = sys.argv[1]
    only = None
    limit = None
    if "--style" in sys.argv:
        only = sys.argv[sys.argv.index("--style") + 1]
    if "--limit" in sys.argv:
        limit = int(sys.argv[sys.argv.index("--limit") + 1])
    os.makedirs(out_dir, exist_ok=True)
    scratch = os.path.join(out_dir, "_scratch")
    os.makedirs(scratch, exist_ok=True)

    here = os.path.dirname(os.path.abspath(__file__))
    index = json.load(open(os.path.join(here, "hair_index.json")))

    src_o, src_r = Pack(SRTT), Pack(SRTTR)
    orig_by_style = {}
    for bundle, names in index["orig"].items():
        s = style_of(names)
        if s:
            orig_by_style.setdefault(s, bundle)

    targets = {}          # style -> Surface
    reshaped = {}         # style -> (old_gcmesh, new_gcmesh, stats)
    manifest = {}         # bundle -> {"comp": n, "style": s}
    todo = sorted(index["rema"].items())
    done = 0
    for bundle, names in todo:
        style = style_of(names)
        if not style or style not in orig_by_style:
            continue
        if only and style != only:
            continue
        if limit and done >= limit:
            break
        t0 = time.time()
        if style not in targets:
            _, cc_o, gc_o = load_mesh(src_o, orig_by_style[style], style, scratch)
            targets[style] = build_target(cc_o, gc_o)
        surf = targets[style]

        tmp = os.path.join(scratch, "_r.str2_pc")
        with open(tmp, "wb") as fh:
            fh.write(src_r.read(bundle))
        sub = Pack(tmp)
        gname = style + ".gcmesh_pc"
        cc = sub.read(style + ".ccmesh_pc")
        gc = sub.read(gname)
        # every bundle for a style carries the same mesh, so reshape it once
        cached = reshaped.get(style)
        if cached is not None and cached[0] == gc:
            new_gc, st = cached[1], cached[2]
        else:
            new_gc, st = reshape_gcmesh(cc, gc, surf)
            reshaped[style] = (gc, new_gc, st)
        assert len(new_gc) == len(gc), "gcmesh size must not change"

        # carry every unchanged entry across as its original LZ4 frame
        raw = sub.raw_frames()
        entries = []
        for name, _, usz, _ in sub.items:
            if name == gname:
                entries.append((name, len(new_gc), frame(new_gc)))
            else:
                entries.append((name, raw[name][0], raw[name][1]))
        blob = build_framed(entries)
        with open(os.path.join(out_dir, bundle), "wb") as fh:
            fh.write(blob)
        comp = struct.unpack_from("<Q", blob, 0x180)[0]
        manifest[bundle] = {"style": style, "comp": comp}
        done += 1
        print(f"[{done:>3}] {bundle:<34} {style:<30} "
              f"{st['verts']:>6}v {st['tris']:>6}t  "
              f"move {st['mean_mm']:5.1f}/{st['max_mm']:5.1f} mm  "
              f"{time.time() - t0:5.1f}s", flush=True)

    with open(os.path.join(out_dir, "manifest.json"), "w") as fh:
        json.dump(manifest, fh, indent=1)
    print(f"\n{done} bundles rebuilt across {len(targets)} styles -> {out_dir}")


if __name__ == "__main__":
    main()

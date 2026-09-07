"""Render stock vs reshaped silhouettes for a set of styles, as a contact sheet.

Catches the failure mode the reshape is most prone to: a style whose accessory or
outlying geometry gets collapsed onto the hair surface by the projection.

Usage:
    python qa_sheet.py <bundles_dir> <out.png> [--styles a,b,c] [--worst N]
"""
import json
import os
import sys

from PIL import Image, ImageDraw

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vpp2 import Pack                       # noqa: E402
from srmesh import Mesh                     # noqa: E402
from render import render                   # noqa: E402

from paths import SRTT, srttr_stock                        # noqa: E402

SRTTR = srttr_stock()
TILE = 300


def style_of(names):
    for n in names:
        if n.endswith(".ccmesh_pc"):
            return n[:-len(".ccmesh_pc")]
    return None


def load(pack, bundle, style, scratch, name):
    tmp = os.path.join(scratch, name)
    with open(tmp, "wb") as fh:
        fh.write(pack.read(bundle))
    sub = Pack(tmp)
    return Mesh(sub.read(style + ".ccmesh_pc"), sub.read(style + ".gcmesh_pc"))


def main():
    bundles_dir, out_png = sys.argv[1], sys.argv[2]
    here = os.path.dirname(os.path.abspath(__file__))
    index = json.load(open(os.path.join(here, "hair_index.json")))
    manifest = json.load(open(os.path.join(bundles_dir, "manifest.json")))
    scratch = os.path.join(bundles_dir, "_scratch")
    os.makedirs(scratch, exist_ok=True)

    want = None
    if "--styles" in sys.argv:
        want = set(sys.argv[sys.argv.index("--styles") + 1].split(","))

    # one bundle per style
    per_style = {}
    for bundle, info in manifest.items():
        per_style.setdefault(info["style"], bundle)
    styles = sorted(want & set(per_style) if want else per_style)

    src_o, src_r = Pack(SRTT), Pack(SRTTR)
    orig_bundle = {}
    for bundle, names in index["orig"].items():
        s = style_of(names)
        if s:
            orig_bundle.setdefault(s, bundle)

    rows = []
    for style in styles:
        bundle = per_style[style]
        mo = load(src_o, orig_bundle[style], style, scratch, "_qo.str2_pc")
        mr = load(src_r, bundle, style, scratch, "_qr.str2_pc")
        new = Pack(os.path.join(bundles_dir, bundle))
        mn = Mesh(new.read(style + ".ccmesh_pc"), new.read(style + ".gcmesh_pc"))
        rows.append((style, mo, mr, mn))

    sheet = Image.new("RGB", (TILE * 3, (TILE + 22) * len(rows) + 4), (12, 12, 14))
    d = ImageDraw.Draw(sheet)
    for r, (style, mo, mr, mn) in enumerate(rows):
        allp = mo.positions() + mr.positions()
        def sp(i):
            v = [p[i] for p in allp]
            return min(v), max(v)
        bz, by = sp(2), sp(1)
        span = max(bz[1] - bz[0], by[1] - by[0], sp(0)[1] - sp(0)[0])
        def ctr(b):
            m = (b[0] + b[1]) / 2
            return (m - span / 2, m + span / 2)
        bz, by = ctr(bz), ctr(by)
        y = r * (TILE + 22) + 22
        for c, (lab, m, tint) in enumerate((
                ("SRTT", mo, (1.0, .85, .6)),
                ("SRTTR stock", mr, (.6, .8, 1.0)),
                ("SRTTR reshaped", mn, (.7, 1.0, .75)))):
            im = render(m.tris(), m.positions(), (2, 1, 0), (bz, by), size=TILE, tint=tint)
            sheet.paste(im, (c * TILE, y))
            d.text((c * TILE + 5, y - 15), f"{style}  {lab}", fill=(240, 230, 150))
    sheet.save(out_png)
    print(f"{len(rows)} styles -> {out_png}")


if __name__ == "__main__":
    main()

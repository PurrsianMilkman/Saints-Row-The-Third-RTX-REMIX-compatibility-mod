"""Rank a Remix capture's geometry by distance from the game camera.

Written for the free-fall capture: with the player in mid-air the nearest real terrain is
hundreds of units away, so anything within a few units of the camera is camera-attached by
definition rather than by inference. That removes the guesswork that made earlier passes
mis-identify terrain and props as unwanted shapes.

Identical meshes are grouped - the composite chain submits the same quad 15 times, and listing
it 15 times buries everything else.
"""
import sys, collections
from pxr import Usd, UsdGeom, UsdShade

path = sys.argv[1]
near_cut = float(sys.argv[2]) if len(sys.argv) > 2 else 25.0
stage = Usd.Stage.Open(path)
xf = UsdGeom.XformCache()

cam_pos = None
for prim in stage.Traverse():
    if prim.IsA(UsdGeom.Camera):
        cam_pos = xf.GetLocalToWorldTransform(prim).ExtractTranslation()
        break
if cam_pos is None:
    raise SystemExit("no camera prim in capture")
print(f"camera at ({cam_pos[0]:.1f}, {cam_pos[1]:.1f}, {cam_pos[2]:.1f})")


def material_of(prim):
    """Bound material, read from the relationship directly.

    ComputeBoundMaterial() is not used: Remix writes the binding without applying
    MaterialBindingAPI, so it emits a warning per mesh and buries the output.
    """
    rel = prim.GetRelationship("material:binding")
    if rel:
        targets = rel.GetTargets()
        if targets:
            return targets[0].name
    return "?"


groups = collections.defaultdict(lambda: {"n": 0, "dist": 1e30, "size": None,
                                          "verts": 0, "mat": "?", "centre": None})
total = 0
for prim in stage.Traverse():
    if not prim.IsA(UsdGeom.Mesh):
        continue
    pts = UsdGeom.Mesh(prim).GetPointsAttr().Get()
    if not pts:
        continue
    total += 1
    m = xf.GetLocalToWorldTransform(prim)
    lo = [1e30] * 3
    hi = [-1e30] * 3
    for p in pts:
        w = m.Transform(p)
        for k in range(3):
            lo[k] = min(lo[k], w[k])
            hi[k] = max(hi[k], w[k])
    centre = [(lo[k] + hi[k]) / 2 for k in range(3)]
    size = [hi[k] - lo[k] for k in range(3)]
    dist = sum((centre[k] - cam_pos[k]) ** 2 for k in range(3)) ** 0.5
    # Group on vertex count plus rounded size: same mesh submitted repeatedly.
    key = (len(pts), tuple(round(s, 1) for s in size))
    g = groups[key]
    g["n"] += 1
    g["verts"] = len(pts)
    if dist < g["dist"]:
        g["dist"] = dist
        g["size"] = size
        g["centre"] = centre
        # The mesh prim name carries Remix's geometry hash, which is the handle
        # rtx.* category lists key on - more directly useful than a material name.
        g["mat"] = f"{prim.GetName()} mat={material_of(prim)}"

rows = sorted(groups.values(), key=lambda g: g["dist"])
print(f"\n{total} mesh instances in {len(rows)} distinct shapes.")
print(f"Within {near_cut} units of the camera (camera-attached in free fall):\n")
print(f"{'copies':>6} {'dist':>7} {'verts':>6}  {'size (x,y,z)':<25} mesh hash / material")
shown = 0
for g in rows:
    if g["dist"] > near_cut:
        break
    shown += 1
    s = g["size"]
    print(f"{g['n']:6d} {g['dist']:7.2f} {g['verts']:6d}  "
          f"{s[0]:7.1f}{s[1]:8.1f}{s[2]:8.1f}   {g['mat'][:44]}")
if not shown:
    print("  (nothing this close - the shapes are further out than the cutoff)")

print(f"\nNext 10 shapes beyond {near_cut} units, for comparison:\n")
print(f"{'copies':>6} {'dist':>7} {'verts':>6}  {'size (x,y,z)':<25} mesh hash / material")
for g in [r for r in rows if r["dist"] > near_cut][:10]:
    s = g["size"]
    print(f"{g['n']:6d} {g['dist']:7.2f} {g['verts']:6d}  "
          f"{s[0]:7.1f}{s[1]:8.1f}{s[2]:8.1f}   {g['mat'][:44]}")

# SRTTR Hair Shape Fix

Saints Row: The Third Remastered re-authored every hair style with a fuller, puffier
silhouette than the 2011 original — the crown stands off the skull, the ponytails are
bulkier, and there is a spray of stray strand cards over the top of the head that the
original does not have. This project reshapes the Remaster's hair meshes back onto the
original game's silhouette.

**Only vertex positions change.** Vertex count, stride, UVs, bone weights, index
buffers, LOD ranges, morph data, textures, materials and every uncompressed file size
stay byte for byte as SRTTR shipped them, so nothing downstream can go out of sync.
The Remaster keeps its 1024² textures and its ~10× triangle density; it just stops
being puffy.

## What was wrong

Measured on `cf_hair_longhiponybangs` (the high ponytail with long bangs):

| | SRTT | SRTTR |
|---|---|---|
| Triangles (LOD0) | 1 246 | 10 751 |
| Vertices | 764 | 15 097 |
| Bounding box | 178 × 338 × 318 mm | 173 × 359 × 349 mm |
| Diffuse | 512² DXT1, **opacity in the R channel**, G pinned to 255 | 1024² DXT5 with a real alpha |
| Other maps | `_n`, `_flow` (anisotropic hair flow) | `_n` (BC5), `_arm` — **`_flow` dropped** |
| Alpha coverage @128 | 25 % | 60 % |
| Skeleton | `rig_pc`, 4250 B | byte-identical |

The bounding boxes are close, so it is the same hairstyle — the difference is
distribution: the Remaster's mask covers 60 % of the card area where the original
covers 25 %, and its geometry sits further off the skull, reaching 27 mm further
forward at the bangs.

## File formats

Everything below was derived by inspection and is implemented in `tools/`.

### VPP_PC / STR2_PC v6

Both games report version 6 but use different widths.

| | SRTT | SRTTR |
|---|---|---|
| Index offset | `0x800` | `0x1000` |
| Entry size | 24 B (u32 fields) | 48 B (u64 fields) |
| Alignment | `0x800` | `0x1000` |
| Compression | zlib | **LZ4** |

Header fields (SRTTR): count `0x158`, package size `0x160`, index size `0x168`,
names size `0x170`, data size `0x178`, compressed size `0x180`. `flags` is at
`0x14C` in **both** layouts — that one shared offset is easy to get wrong, and
reading it from the wrong place makes a compressed bundle look uncompressed.

Entry layout is the same field order in both: `name_off, pad, data_off, usize,
csize, pad`. `flags 0x4803` = compressed + condensed: `data_off` indexes
*uncompressed* space and the compressed blocks are simply concatenated.

### LZ4 framing (SRTTR only)

Each entry payload is one LZ4 block behind a 16-byte header:

```
0x00 u32 0x0FEEDBEE
0x04 u32 0x00BADBEE
0x08 u32 compressed payload size (header excluded)
0x0C u32 uncompressed size
0x10     LZ4 block
```

### Mesh (.ccmesh_pc + .gcmesh_pc)

The `.ccmesh_pc` holds texture names, a material block, a bone remap table and then
one or more mesh blocks, each starting with the u32 pair `(9, hash)` where `hash`
also opens the `.gcmesh_pc`. Geometry fields are u32 in SRTT and u64 in SRTTR.

The `.gcmesh_pc` is `hash`, then the index buffer, then the vertex buffer, then a
footer repeating the hash. **Both games use the same 32-byte vertex layout**:

```
+0x00 float3  position (object space, metres)
+0x0C 4 x u8  packed normal
+0x10 4 x u8  packed tangent
+0x14 4 x u8  bone weights (sum 255)
+0x18 4 x u8  bone indices (255 = unused)
+0x1C 2 x i16 UV, fixed point /1024
```

**The index buffer is a triangle STRIP**, not a list, with degenerate
(repeated-index) triangles stitching separate hair cards together. Reading it as a
list silently drops two thirds of the surface — the giveaway is a mesh full of
holes. Multi-part styles (hair+glasses, hair+chopsticks) carry a second vertex
buffer group, sometimes at a different stride, addressed by the same index buffer.

### .asm_pc (BEEFFEED)

Stream index: v11 in SRTT, v20 in SRTTR. Header is three count-prefixed string
tables (allocator, primitive and container type names), then one record per
container holding the container's **compressed size** and, per contained file, its
name, type and cpu/gpu sizes — stored twice, once in a flat size table and once in
the per-file records. Both copies must agree or the streaming system will not load
the container. `tools/asmpc.py` round-trips all 432 records of
`customize_item.asm_pc` byte for byte.

Note the level/stream asm files (`*_stream_grid`, `*_sr3_city`) use an extended
record layout this parser does not handle; the customize/character ones all parse.

### PEG v13 (GEKV)

`.cpeg_pc` is the directory, `.gpeg_pc` the pixels; 72-byte entries. Formats seen:
400 = DXT1, 402 = DXT5, 411 = BC5.

## Pipeline

For each style present in both games:

1. read the SRTT mesh, weld coincident vertices, subdivide 2×, Taubin-smooth —
   this yields a target with SRTT's *profile* but none of its 1.2k-triangle
   faceting (Taubin is used rather than plain Laplacian so the silhouette does not
   shrink; measured drift is under 0.5 mm);
2. project every SRTTR vertex onto that surface;
3. relax the *displacement field* so the projection cannot splay individual cards;
4. write the new positions back into the `.gcmesh_pc` in place;
5. repack the bundle, carrying every unchanged entry across as its original LZ4
   frame rather than recompressing it;
6. patch each container's new compressed size into `customize_item.asm_pc`;
7. rebuild `customize_item.vpp_pc` with the new bundles and asm.

## Layout

```
tools/
  vpp2.py         VPP/STR2 reader, both games
  vppwrite.py     SRTTR container writer
  lz4blk.py       LZ4 block decoder + 0FEEDBEE frame reader
  lz4enc.py       LZ4 block compressor + frame writer
  srmesh.py       ccmesh/gcmesh reader (strip decoding, LOD table)
  smooth.py       weld / subdivide / Taubin smoothing
  reshape.py      closest-point projection + displacement relaxation
  peg.py, dxt.py, texbuild.py   texture read/decode/encode
  asmpc.py        .asm_pc parser and in-place patcher
  build_reshape.py  per-style reshape and bundle repack
  pack_vpp.py       rebuild customize_item.vpp_pc
evidence/         rendered before/after comparisons
build/            generated bundles and packfile
```

## Building

```
python tools/build_reshape.py build/bundles          # reshape + repack all styles
python tools/pack_vpp.py build/bundles build/customize_item.vpp_pc
```

Then back up the stock `cache/customize_item.vpp_pc` and drop the new one in.
`tools/install.ps1` does both.

## Scope

68 hair styles are present in both games and get reshaped, covering all 142
Remaster hair bundles except those for `cf_hathair_clipperedshort` and
`cm_hathair_clipperedshort`, which are new in the Remaster and have no original to
match.

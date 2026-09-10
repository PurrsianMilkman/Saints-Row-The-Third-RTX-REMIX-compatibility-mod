# Start here

*Rewritten 2026-08-17 as a full handoff. State section rewritten 2026-08-30.*

## Resuming

Start Claude Code in `D:\SR3RTXREMIXCOMP` and say:

> Continue the SR3 RTX Remix project. Read docs/YOUR-INSTRUCTIONS.md, then docs/engine-map.md,
> then the last session of docs/worklog.md, before doing anything.

---

# THE CLOTH COLOUR FORMULA - CONFIRMED CORRECT ON EVERY GARMENT, 2026-09-10

Confirmed on screen by the user: shoes, wrist wraps, headwear, choker, earrings, armband,
bracelets, corset, backpack (stomach yellow), underwear (panel cyan, magenta cat, yellow eyes),
bra (fuchsia, yellow star on the left cup only, as in the game). There is exactly ONE generator
for the player's clothing, `ClothAlbedoUniform`; anything that changes colour must keep all of
those right. Every piece below is behind an ini switch.

## The formula, as implemented

    // 1. the three chosen colours: Diffuse_Color_a/b/c (c1/c2/c3), selected by the pattern
    //    texel's channels through THE PLAYER SHADER'S OWN CHAIN (ir_sr3pccloth_c ps[6]):
    //      add r5, -1, c3 ; mad r5 = blue*r5 + 1        -> lerp(1, C, blue)
    //      lrp r7 = lerp(r5, c2, green)
    //      lrp r5 = lerp(r7, c1, red)
    r,g,b   = gammaLUT[pattern texel]                                   // x^2.2
    colour  = lerp(lerp(lerp(1, C, b), B, g), A, r)                     // colourFromPatternTexelChain
    // used on the PER-TEXEL paths (mesh decal, AFFINE). The one-point/dominant `pick` still goes
    // through colourFromPatternTexel (weighted sum + desaturation branch, the NPC family's rule):
    // on a pure-channel texel the two agree exactly, and that is the only texel it is ever given.

    // 2. the game's Tint_color (c37 = 5,5,5, READ from the constant) and its tonemap:
    colour' = (Tint*colour) / (1 + Tint*colour)                         // Reinhard, per channel
    // clothColourCurve=1. A linear x2 (clothAlbedoPercent=200, still the fallback when the
    // switch is 0) keeps hue and could never turn (0.059,0.220,0.298) into cyan.

    // 3. times the diffuse, in LINEAR, encoded once; the diffuse's ALPHA rides through untouched
    albedo = LinearToSrgb( gammaLUT[diffuse] * colour' ),   alpha = diffuse.a

Diffuse_Color (c14) is (1,1,1,1) on every garment measured; the generator does not apply it.

## Where the pattern colour comes from, per garment - THE TAXONOMY, measured per draw

    ONE-POINT      uv1 constant across the draw: one pattern texel, one colour. Shoes, wraps,
                   headwear, choker, earrings, armband.
    AFFINE         uv1 = s*uv0 + o (the backpack: identity, residual 0.0). Resolved per texel in
                   the diffuse's space with no mesh.
    INDEPENDENT    two unrelated unwraps (bra, underwear). NOT a structural limit: the MESH
                   relates them. `BakeDecal` rasterises every triangle with a visible vertex into
                   the diffuse's space, interpolating uv1 barycentrically, and records the pattern
                   texel for each visible albedo texel (underwear 78.9% of visible texels, bra
                   91.4%; the rest keep the dominant colour, which is the background anyway).
                   clothMeshDecal=1.

## The cutout - the bra and the underwear are template meshes cut by their texture's alpha

Their diffuse maps are 63-90% transparent (DXT5 at draw time; every cm_bra_f_* and cm_unwr_f_*
in the game files agrees), and ps[6] ends `mul oC0, r3, c37` with r3.w straight from the
Diffuse_Map fetch; the game blends it (src=5 dst=6). Two thirds of the mesh landing on "black"
is the invisible part of the template, and it was the whole "black squares" report. The
generated texture carries the alpha; the dilation must leave it alone (colour only, for bilinear
edges); BeginFFP alpha-tests the converted draw at 128 when it binds such a texture, which is
also what Remix reads as "cutout". clothCutout=1. Check the ALPHA channel of a dump before
reasoning about its colour - and check that the dump writer kept it (it did not, for a day).

## Two surfaces on one island - tiles

The bra's two cups share one diffuse island with mirrored uv0 and want different images (the
star is on the left cup only). One texture cannot hold two answers for a texel, so the bake
records every disagreement as an edge between two TRIANGLES, colours that graph greedily (each
triangle takes the lowest tile holding nothing it disagrees with; non-conflicting triangles stay
in tile 0 - any tile is correct for them), and lays the generated texture out as tiles, each as
enough side-by-side COPIES of its image that a panel crossing the wrap seam never leaves its
region (bra: 4+2 copies, 6 texture widths). At bind time the skinned copy's u0 is shifted by
whole tiles per triangle, 18 seam vertices are duplicated behind the draw's own vertices with
the other tile's shift, a private INDEX32 triangle list references them, the draw hook swaps it
in for the one call, and the texture matrix is scaled by 1/width. Cut geometry keeps its
within-tile position and every tile carries the same alpha, so it stays invisible. Splits by
connectivity (one component) and by winding (257 same-winding conflicts) were both refused by
their own guards and are closed. clothDecalTiles=1.

## The things that are easy to get wrong here

1. **Mixed pattern texels.** The desaturation branch is the NPC family's; the player's shader
   has none. Cyan is B, white is A. Use the chain on any per-texel path.
2. **The colour curve is not a brightness knob.** A linear scale preserves hue; the game's
   cyan comes from saturation at x5. Reinhard on the MEASURED Tint, never a hard-coded 5.
3. **Alpha.** Keep it through every stage that rewrites pixels; test it on the converted draw.
4. **The diffuse texel goes to linear, the product is encoded once.** Two sRGB values
   multiplied and encoded is a different curve and reads dull.
5. **clothTintScale is `clothAlbedoPercent/100`**, and only the x2 fallback now. Never
   clothBrightness (already normalised at parse).
6. **A white or grey diffuse map is the detail layer**, shipped to be tinted. Not a bug.
7. **"Albedo is TEXCOORD0, pattern is TEXCOORD1" is not a rule** - see docs/cloth-uv-map.md.
   On the player family the albedo IS TEXCOORD0*1/1024 (vs[2]: `mul o6.xy, c1.x, v1`) and the
   pattern is TEXCOORD1*Pattern_Map_Tiling/1024 (`mul o2.xy, r2, c1.x`). Measured, per draw.
8. **The clamp registers are inactive** (ClampU1 = ClampV1 = 0 -> `cmp` takes the unclamped
   coordinate; U window [-512,513]). The game WRAPS. So does the conversion.

# STATE, 2026-09-07 - VERTEX CAPTURE IS OFF AND THE CHARACTER WORKS. READ THIS FIRST.

Anything below that says capture must be ON is superseded. The mechanism that made it possible:

## The one technique that unlocked all of this

**Remix's refusal is about the VERTEX shader, not the draw.** Its own message says so -
`Skipping draw call with shader usage as vertex capture is not enabled` - because vertex capture
exists to capture vertex-shader output. The pixel shader was never the problem.

So a draw re-issued with FIXED-FUNCTION VERTEX PROCESSING and THE GAME'S OWN PIXEL SHADER still
bound is a combination Remix executes with capture off. That single fact fixed three things:

| what | how |
|---|---|
| the HUD | `DrawPrimitiveUP` draws rebuilt as `D3DFVF_XYZRHW` quads; positions are already screen pixels, so it is a 28-byte field reorder |
| the character's skin | the atlas composites re-issued as a full-target quad with their own pixel shader kept - `4 done, 0 failed`, atlas means back to 182.7 and 169.6, IDENTICAL to the pre-breakage values |
| the menu video | proved the rule. Nulling the pixel shader made it greyscale, because Bink is Y/Cr/Cb across three stages and stage 0 alone is luma |

## Customisation, as it now stands

`ClothAlbedo()` used to refuse any material whose albedo was not the Pattern_Map itself, which
declined all fourteen of the player's items and left them showing raw untinted maps - white
bracelets, a white choker, a green beanie. Three garment shapes exist:

1. **pattern IS the albedo** - resolved in the pattern's own texture space. The corset. Always worked.
2. **separate diffuse + FLAT pattern** - one colour for the whole garment, multiplied into the
   diffuse in ITS texture space. No mesh, no uv assumptions. `ClothAlbedoUniform`. Fixed 2026-09-06.
3. **separate diffuse + VARYING pattern on a SECOND uv set** - genuinely needs the mesh to relate
   the two uv sets. Only the underwear. The CPU baker does this and its result is now registered
   into the same cache the game's own draw reads.

**There is ONE colour pipeline and both paths must use it**: pattern channels to linear through
`g_gammaLUT`, weighted SUM of A/B/C (not a lerp from white), `* clothTintScale`
(`clothAlbedoPercent/100`, currently 200), then `LinearToSrgb` once. Writing a second pipeline is
what made the bracelets the right hue and the wrong brightness.

## Traps paid for in this stretch

- **`clothBrightness` is already normalised at parse time.** Dividing by 100 again scaled every
  garment down by a hundred - `result mean 0.3 of 255`.
- **The cloth cache key must include the diffuse.** `ClothKey(pattern, col) ^ diffuse*prime`
  collapses to the pattern-space key when diffuse is null, so registering a UV-space bake there
  overwrote the corset and the shoes. Shoes went black; that is the collision, not a colour bug.
- **A white or grey diffuse map is not a bug.** It is the detail layer, shipped to be tinted.
- **PEG entry stride is 72 bytes**, and a single-bitmap PEG cannot reveal that - `24 + 72` lands
  on the name table either way. Only a three-bitmap PEG shows it.
- **`.str2_pc` is condensed**: the whole data block is ONE zlib stream and entry offsets index the
  DECOMPRESSED result. The header's compressed size is a sum, not a stream length.

## Where the assets actually live

    game-textures\      unpacked from the retail packfiles by tools/vpp.py + tools/peg.py
                        3,467 bitmaps, 697 items, with a README explaining the naming and recipe
    player-textures\    the player's own captured parts, pattern/diffuse/result per garment

## Open

*Updated 2026-09-07 evening.*

1. **The underwear AND THE BRA - they share the system.** Class 3 above. Three shortcuts are now
   closed by measurement, not opinion: the pattern is read at many texels; `uv1` is not affine in
   `uv0` (worst residual ~1550 of a ~1900 span); and from the shader itself the albedo is not even
   on TEXCOORD0 - `ir_at_sr3pccloth_bs[8]` samples s0 through a coordinate COMPUTED from TEXCOORD6
   while the pattern is s2 on TEXCOORD1. The deployed build reflects the real UV set per sampler
   and prints `UV SET PER SAMPLER`; **THAT RUN HAS NOT HAPPENED YET and is the next thing to read.**
2. **The UI**, parked at the user's request. In-game HUD invisible, sub-menu text and backgrounds
   missing; the menu video works. With capture off, anything passed through is not drawn - the
   pass census names what is being lost.
3. Clothes brightness overall - `clothAlbedoPercent` is the single knob now there is one pipeline.
4. The sky, lost to capture-off, and the frustum popping that comes with it.

**CLOSED since this section was written:** hair strand detail (the Dob_Map R channel), the
character copy (`remixApiCharacter=0` - it was silently un-submitted by the test-cube switch, and
everything that fixed the real character lives in the shared path instead), and the frozen-skinning
problem, which is moot while the API character is off.

---

# EARLIER STATE, 2026-09-04

The project has moved off the fixed-function conversion. Characters are described to RTX Remix
through its own programmatic API: we create the meshes, name the materials, and choose the
textures. The game renders untouched alongside.

## 1. The Remix API - proven, in use, and it changes everything

Enable it with `.trex/bridge.conf` -> `exposeRemixApi = True`. Without that,
`remixapi_InitializeLibrary` returns rc=11 NOT_INITIALIZED and the bridge log says exactly this.

`remixapi_InitializeLibrary` is exported by the 32-bit **bridge client** `d3d9.dll` - the one the
game loads, in our own process. Never `.trex\d3d9.dll`, and use GetModuleHandle, not LoadLibrary.

| what | state |
|---|---|
| CreateMesh / CreateMaterial / DrawInstance / CreateLight | WORK |
| SetupCamera | NOT implemented by this bridge. Not needed - the game's camera is used |
| dxvk_RegisterD3D9Device | NOT implemented and NOT needed - the API defaults to the most recently created device, which is the game's |

Contract details, all paid for:
- Transform is `float[3][4]`, translation in the LAST COLUMN. Same layout as SR3's objTM and its
  bone palette, so both copy across with memcpy.
- Base colour lives in `MaterialInfoOpaqueEXT::albedoConstant`, chained on pNext. The base
  `MaterialInfo` has NO albedo field.
- **`alphaTestType` mirrors VkCompareOp: 0 = NEVER. USE 7 = ALWAYS.** A zeroed extension struct
  makes geometry invisible while still emitting light.
- **The hash IS the identity.** Re-registering a changed material under an old hash is silently
  ignored, the OLD object is returned, and every return code still says SUCCESS. Destroying "the
  old one" then destroys the new one.
- Textures are FILE PATHS (`const wchar_t*`). Uncompressed BGRA8 DDS with a full mip chain works;
  DXT can be copied through unchanged with its fourCC.
- `MeshInfoSurfaceTriangles` holds POINTERS to the vertex and index arrays. They must OUTLIVE
  CreateMesh - locals are freed and the mesh renders as holes.
- D3DPT_TRIANGLESTRIP: SR3's character slots are STRIPS. prims+2 indices, not prims*3.

## 2. The character pipeline as it stands

One capture frame takes every part; builds drain afterwards, one per Present.

- 10 parts, ~22,000 triangles, each its own mesh with N surfaces (one per material slot).
- Vertices are the shim's CPU-skinned output; indices come from the game's IB, rebased absolutely
  as `(slotBase + idx) - (capBase + capMinIndex)`.
- Placement is objTM captured with the vertices. `remixApiCharacterOffset=3` stands the copy beside
  the real character so they cannot z-fight; it goes to 0 when the game's copy is removed.
- Materials are keyed on (texture, tint, alphaTestType, alphaRef, blend) and assigned PER SURFACE.
- Textures are dumped per SLOT, not per part.
- Hair uses `Hair_Spec_Color2` with NO texture: its Diffuse_Map is directional data, not colour.

## 3. Customisation - VERIFIED from the exe and the shaders

- **Colours are the 8-bit swatch divided by 255.** exe 0x008FCE60 / 0x00951A60: the engine sets
  Diffuse_Color_a/b/c by name then divides each byte by a constant that decodes to exactly 255.0.
  No linear conversion, no hidden scale.
- Material parameters are bound BY NAME (0x00951A60 -> 0xd9e8b0), so reflecting by name per draw -
  which ReflectShader already does - is correct by construction. Registers vary between variants
  (Pattern_Map s0 vs s2, Diffuse_Color c11 vs c14) and that was never a bug.
- The recipe, from ir_sr3pccloth_bs shader[6]:
      albedo = Diffuse_Map * Diffuse_Color * lerp(lerp(lerp(1, C, p.b), B, p.g), A, p.r)
  `r1.x` in that lerp is `c6.x = 1.0`, confirmed by tracing - so it is exactly the lerp it looks
  like.
- The `pow(c, 2.2)` in ir_at_sr3pccloth is that SHADER moving an sRGB constant into linear for its
  own lighting. It is NOT part of the colour's identity. Remix lights from an sRGB albedo, so the
  colour goes over AS AUTHORED.
- Runtime, from the stage probe: the Pattern_Map is a **32x32 UNIFORM selector**. The visible
  detail is the Diffuse_Map. Some garments have no diffuse at all - there `albedo stage ==
  pattern stage` and the lerp result IS the colour.
- `Pattern_Map` does NOT appear as a string in the exe. The texture-to-stage binding is data
  driven, which is why it needed a runtime probe and could not be read out of the code.

## 4. What is OPEN

1. **Clothes read too dark, and the recipe is not at fault.** Bake #2 computes (84 0 0) from
   colour 0.40 and diffuse 0.82 - exactly what the shader produces. So Remix lights our API mesh
   differently from how the game lit it. `clothBrightness` (a percentage, default 100) is a TUNING
   KNOB, not a fix. Whatever value looks right is itself a measurement of the lighting gap.
2. **The character is FROZEN.** `remixApiSkinning=0`. Handing Remix MeshInfoSkinning +
   INSTANCE_INFO_BONE_TRANSFORMS_EXT crashed its 64-bit server inside CreateMesh
   (0xC0000005 at d3d9.dll+0x780CE). The full 64-bone palette is now sent as identity-initialised
   to remove the out-of-range suspect, but it has not been retried.
3. **The copy stands beside the real character.** Removing the game's own copy is the last step.
4. Hair has no strand detail - flat colour only.

## 5. Routes that are CLOSED. Do not reopen.

- **GPU bake through the Remix device.** Rendering into our own target to capture what the pixel
  shader computes crashed Remix's server twice at the same address, even with the draw hidden
  inside an occlusion query. The hand-assembled vs_3_0 that maps UV to clip space IS valid - D3D9
  accepted it, 75 tokens - so the fault is Remix intercepting the render-target switch or the
  readback, not the shader.
- **Any texture tag that hides a vertex-captured draw.** Confirmed from Remix's own binary: it
  declines a draw for exactly three reasons - occlusion query, unsupported topology, no camera.
  There is no "ignore this draw". `ignoreTextures` VISUALISES as a pink/black checkerboard.
- **D3DPERF markers as pass identity.** All four D3DPERF imports have ZERO call sites.
- **The engine's render-pass classes as per-draw identity.** They run on the MAIN thread and record
  commands; the render thread executes them, so at a draw hook the stack holds the dispatcher.
- **GetRenderTargetData on the GAME's render targets.** Froze SR3 twice. On OUR OWN targets it
  works.

## 6. Method lessons that actually paid, this session

- **Log what a thing IS, by name, not what it looks like.** Printing each slot's albedo sampler
  found the hair in one line after several runs of guessing from screenshots.
- **When a symptom survives two hypotheses, print the data structure.** The holes cost four
  hypotheses from the picture; the slot dump solved it in one run with arithmetic
  (`start + prims + 2` tiled exactly -> triangle strips).
- **A budget that counts only successes is not a budget.** The bake's cap counted successes, so
  every failure retried forever and exhausted a 32-bit address space.
- **A guard placed on the wrong side of a condition disables more than intended.** Gating cloth
  RECOGNITION behind the switch meant to gate cloth TEXTURE REPLACEMENT.
- **Fixing wider than the fault breaks working things.** Applying Diffuse_Color_a to every cloth
  slot blacked the top, which had a real diffuse map and was already correct.
- **Read the bridge logs, not only remix-dxvk.log and our own.** bridge32.log carried the
  exposeRemixApi remedy from the first run that called the API.
- **Check which shader file is actually in use.** The cloth recipe was read from the wrong variant
  twice; there are 34 of them and their registers differ.

## 7. Deployed

    sr3-rtx.asi  b7d65adcdffc133d96da2a2b1cf4374d
    sr3-rtx.ini  52d61a050d633d2e29a8f95c09b9ea0a
    rtx.conf     521541b215897d4a264627fb24b0f56d
    bridge.conf  db55f8142db1db21eb4bdf256f5c4ae5   (exposeRemixApi = True)

    ffp=1 hiddenPassMode=2 screenSpaceMode=0 generateCloth=1 forceOcclusionVisible=1
    remixApi=1 remixApiCharacter=1 remixApiSkinning=0 bakeShaderAlbedo=0
    clothUseDiffuse=1 clothBrightness=100

Source backups in src/sr3-rtx/, one per change, named `.before-<what>`.

---

# EARLIER STATE, 2026-08-31

A mesh described entirely through Remix's programmatic API, with no D3D9 draw behind it, renders in
the path traced image wearing the game's own character texture. Verified 2026-08-31.

This changes what this project IS. Every hard problem here descended from one constraint - that
geometry could only reach Remix as a faked fixed-function draw or a vertex-capture reconstruction.
That constraint is gone. Do not spend further effort on: AlbedoRank and which sampler is the
albedo, the fixed-function 64-bone ceiling, SHORT2 texcoord conversion, hiding draws from Remix, or
deriving a camera for it. Those are all artefacts of the old constraint.

## How to use it (all verified, none of it needs re-deriving)
- Enable: `.trex/bridge.conf` -> `exposeRemixApi = True`. Without it InitializeLibrary returns
  rc=11 NOT_INITIALIZED and the bridge log tells you exactly this.
- `GetModuleHandleW(L"d3d9.dll")` -> `remixapi_InitializeLibrary`. That is the 32-bit BRIDGE
  CLIENT, in our own process. Never `.trex\d3d9.dll`, and never LoadLibrary.
- Do NOT call dxvk_RegisterD3D9Device (unimplemented) or SetupCamera (unimplemented). Neither is
  needed: the API uses the most recently created device - the game's - and the game's camera.
- CreateMaterial + CreateMesh once; DrawInstance EVERY FRAME from Present.
- Transform is `float[3][4]`, translation in the last column.
- Base colour lives in `MaterialInfoOpaqueEXT::albedoConstant`, chained via pNext. The base
  `MaterialInfo` has no albedo field at all.
- **`alphaTestType` mirrors VkCompareOp: 0 = NEVER. Set it to 7 = ALWAYS.** A zeroed extension
  struct makes geometry invisible while still emitting light.
- **The hash IS the identity.** Re-registering a changed material under an old hash is silently
  ignored, the old object is returned, and every return code still says SUCCESS.
- Textures are FILE PATHS (`const wchar_t*`). Uncompressed BGRA8 DDS with a full mip chain works;
  force alpha to 255 for X8R8G8B8 sources.
- Skinning: `MeshInfoSkinning` on the surface plus `INSTANCE_INFO_BONE_TRANSFORMS_EXT`, up to 256
  bones. SR3 needs 58.

## Next: step 3a
Submit ONE character mesh through the API alongside the existing path. A DOUBLED character is the
proof it landed. Only then (3b) turn vertex capture off and stop converting.

---

# STATE, 2026-08-30 (updated after the cameraOnly + block-walk run, 15:47-15:52)

## 0. The black world is NOT the skip rules. It is the camera.

This is the session's main result and it overturns four sessions of blame.

In `cameraOnly` the shim converts nothing, skips nothing, hides nothing - `Classify` returns
`PassThrough` ahead of every rule, and every skip/hide/convert counter in the report reads
`0/frame`. The shim removed **nothing** for ~10,800 frames and the world still went black at
certain angles. So the classification rules cannot be the cause. They never ran.

What the shim *does* do in that mode is set the camera - and it was setting **124,566 distinct
view/projection pairs over 10,800 frames, 11.5 per frame**: shadow cascades, the water reflection,
cubemap faces and the main view, pushed into `SetTransform` indiscriminately in draw order. Remix's
`CameraManager` then had to pick one to path trace with, and its own log shows the strain:

    warn: [RTX] CameraManager: FOV of a camera changed between frames
    info: Camera cut detected on frame 1416 / 2471 / 2508

At some viewing angles it settles on a camera whose frustum does not contain the scene.

**Fix built and deployed 2026-08-30: `cameraMainViewOnly=1`.** Accepts a camera only when all three
hold, each read from state the engine sets, none inferred from shader names or RT indices:
back-buffer-sized render target; perspective (not ortho) projection; positive view-rotation
determinant (a reflection mirrors and flips it). If it rejects every camera in a frame nothing is
set and Remix keeps the last one - a stale camera lags, it does not black out.

**The control ships with it.** The report lists every distinct camera the frame contains with RT
size, perspective/ortho, mirrored/upright, draw count and accepted/declined. A gate that picks the
WRONG camera and one that picks NONE both look like a black world from outside; only that listing
separates them. `cameraMainViewOnly=0` restores push-everything for an A/B. **UNVERIFIED - this
has not been run yet.**

## 0b. `cameraOnly` does NOT cause the 33 GB OOM

Retracted. The 2026-08-30 run held ~10,800 frames with no memory diagnostic in the Remix log at
all, and exited normally. The 33.5 GB was `cameraOnlyFloatUV` creating 6,252 float-UV buffers on
pass-through draws. Gated off, it is gone. Do not re-derive.

Performance in that mode: **frame 27.0 ms (37 fps), shim 0.49 ms = 1.8% of the frame**, against
~57% when converting. Converting nothing is nearly free; the rest is Remix and the game.

## 0c. The readback filter does not exist - closed by measurement

    0 GetRenderTargetData (ops 55/56/72) in 132,204,771 commands. 0 blocks contain a readback.

"A draw whose result the engine reads can never be skipped" is **true and vacuous**. The planned
`cameraOnly` filter has nothing to filter on. Corollary: the character atlas is not built by
readback but by 36 surface `LockRect` uploads, then sampled GPU-side - so the constraint on
skipping is GPU-side *sampling*, not CPU readback.

What replaced it: **797,612 `SetRenderTarget` (op 9), ~74 per frame**, decodable ahead of
execution. Every draw can carry the render target the engine itself names for it. That is the pass
identity every heuristic in this shim has been reconstructing. The block walk is validated -
200,839 blocks, 132M commands, **0 walks stopped early**.

Also corrected: `0x02E5D644` is the **previously** dispatched opcode, not the current one.

---

# EARLIER STATE, 2026-08-30 — read this whole section before touching anything

## 1. The premise the architecture was built on was WRONG

`docs/YOUR-INSTRUCTIONS.md` said, from 2026-08-17:

> "ffp=0 with capture on produces a rasterised game and no path tracing at all ... The
>  fixed-function conversion IS the path-traced world, entirely."

**That is false.** `ApplyTransforms` — the only thing that calls `SetTransform(D3DTS_VIEW/PROJECTION)`
for the scene — is invoked from exactly ONE place, inside the conversion path. So `ffp=0` removed
the geometry conversion **and the camera** together, and the test could not distinguish
"Remix needs our geometry" from "Remix needs a camera".

Remix reads the camera only from `SetTransform`, never from shader constants (shader-map.md). Given
a camera and vertex capture, **Remix path-traces SR3 with the shim converting nothing**. Confirmed
on screen 2026-08-29 via `cameraOnly=1`.

## 2. `cameraOnly` works and is not yet viable

It adds a camera (from c28 `projTM` fused VP and c48 `IR_World2View`) and touches no draw. The world
path-traces. But it skips nothing, so Remix builds geometry for **every** draw — and the run ended in

    err: Heap 1: 33536 MB allocated, 32970 MB used, 31571 MB total   (Vulkan OOM inside Remix)

So the architecture is right and **incomplete**: it needs filtering of draws Remix does not need,
without removing draws the ENGINE reads back. That is what the block scan (below) is for.

## 3. The character texture is SOLVED, and had TWO independent causes

SR3 bakes each character — skin, tattoos, clothing — into one 2048x1024 `X8R8G8B8` **DYNAMIC**
texture (`usage=0x200`, NOT a render target) by locking **each mip level's SURFACE**
(`IDirect3DSurface9::LockRect`, a different vtable from the texture's own method).

Both of these independently produce a black character:

| cause | why |
|---|---|
| `rtx.useVertexCapture = False` | ~~Remix declines the game's shader draws, so the composite never executes~~ **SOLVED 2026-09-06 - see STATE at the top. The composite is re-issued with fixed-function vertex processing and the game's own pixel shader, and Remix executes it.** |
| `hiddenPassMode=2` + `screenSpaceMode=2` | the SHIM skipped those same composite quads |

Fixing one while the other was broken is why every single-variable test came back negative for two
days. **Verified working:** capture ON + the composite quads surviving → the snoop captures the
atlas at `mean 182.7`, dumped and visually confirmed as the character with tattoos.

`atlasSnoop` snoops those surface writes and re-uploads through `UpdateTexture` (the path Remix
hashes). It is not currently needed for the bake itself, but it is the instrument that measures it.

## 4. `forceOcclusionVisible` is LOAD-BEARING — proven at instruction level

All four draw command handlers test one global byte, `0x03395EA4`, and skip the draw when set. It is
written at `0x0047CEBB` as `sete cl` from a visibility test at `0x0047C800`. **SR3 decides an object
is not visible and disables drawing with one flag.** Answering the occlusion query keeps it clear.
It is NOT a workaround for capture-off. Removing it on 2026-08-29 was wrong and was reverted.

## 5. The engine, reverse-engineered — see docs/engine-map.md

SR3 is a **command-buffer renderer with a dedicated render thread**. 74-opcode dispatch table at
`0x013509F8`; blocks are 16 KB pool chunks; per-opcode sizes extracted; `GetRenderTargetData` is a
render command (ops 55/56/72). The ring descriptors carry **no pass identity** — that question is
closed. A pass boundary is `SetRenderTarget` (op 9), which the shim already sees.

**Lookahead is built** (`scanCommandBlocks`): the block is fully built before execution, so a
readback is visible ahead of time. A block containing one may never have its draws skipped — rule 1
as a lookup rather than an inference.

## 6. Deployed and hash-verified, 2026-08-30

    sr3-rtx.asi  f9b98e8550f46104d2f2cf5a6a3e7573
    sr3-rtx.map  5d0caaabca8f98106c6205b78caa369f
    sr3-rtx.ini  b0fcafc70dcee0f515fe27f217fbcb3f
    rtx.conf     d7e9c7ec79bc7208166f00ba8a56565a
    dxvk.conf    61d3fb899588a0e703ee016ce96acbb8
    user.conf    8b361562e85c86ab88ab2fe61495b335

    ffp=1  cameraOnly=1  hiddenPassMode=2  screenSpaceMode=2  compositeToTexturePass=1
    forceOcclusionVisible=1  atlasSnoop=1  scanCommandBlocks=1  rtContentProbe=0
    rtx.useVertexCapture = True   rtx.enableRaytracing = True

**Backup:** `D:\SR3RTXREMIXCOMP-backup-2026-08-29` (project + game-state + all captures, 2.9 GB).
Known-good character config preserved as `configs/*.WORKING-character-texture.bak`.

## 7. The immediate next question

Read the `BLOCK SCAN` line from the last run. If `X blocks contain a READBACK` is meaningful and
few walks stopped early, that is the engine-derived filter `cameraOnly` needs: never skip a draw in
a block the engine reads back, skip what is left. If most walks stopped early the size table is
incomplete and the counts are lower bounds.

## 8. DO NOT RE-DERIVE THESE — they cost runs to learn

- `rtContentProbe` **froze the game twice**, the second time at one read per 120 frames — so cost
  was never the issue: Remix cannot service `GetRenderTargetData` on a surface it owns. Default OFF,
  do not enable.
- `d3d9.allowDiscard=False` and `d3d9.apitraceMode=True`: both verified parsed, both inert.
- Restoring the 08-18 `rtx.conf` wholesale: no effect. It is not a Remix configuration problem.
- With `d3d9.dll` renamed away the game renders correctly — the game, the save, the character mods
  and the display settings are all fine.
- `Tint_color` (c37) is a terminal exposure scale and must NOT be applied to albedo. `Diffuse_Color`
  is ~90% white on skinned draws.

## 9. Method failures that cost most of two days

| failure | instances |
|---|---|
| a measurement with no control | `usage=0x200` read as RENDERTARGET; blank `LockRect` readbacks read as "never written"; a black atlas hash read as a date correlation |
| right quantity, WRONG TIME | `compositeToTexturePass` measured in frames that could not contain a spawn; the fill probe hooked after the fill; the RT probe read during loading; the block listing reset before it printed |
| a two-variable space tested one variable at a time | capture and `hiddenPassMode` are independent; every test held one at its broken value, and produced a false "this is impossible" |
| a diagnostic with no ceiling | the RT probe froze the game twice |

The measurements that carried controls — the 846-file shader sweeps, `SelfTestCopyPath`, `ffp=0`,
the no-Remix A/B — were each right first time.

---
### What was fixed 2026-08-26..28, and the switch that reverts each

| symptom | cause | switch |
|---|---|---|
| detached car parts drifting with animation | shim read "skinned" from the vertex DECLARATION; the game reads it from the SHADER. `_s` variants declare no `dcl_blendindices` but share the declaration, so `boneReg` came back -1 and the code fell back to c52 - posing a part from whatever palette the last CHARACTER draw left there | `skinRequireBoneDecl=0` |
| heads misplaced, slightly low, wrong shape, "heads change" | SR3 morphs characters. `ir_sr3npcskinfull_mc`: `mad r1.xyz, v4, c0.x, v0` - `pos = v0 + v4/8192` from a SHORT4 delta in stream 2, applied BEFORE the bone blend. The shim read only `v0`, so every NPC wore the same base head | `applyMorph=0` |
| heads wrongly lit | the morph NORMAL is a delta too: `n = n_base + 2*n_morph`, not a replacement | (same path) |
| heads wrongly textured, z-fighting, swapping | the character G-buffer pass was converted with `Blend_Map` - a SPECULAR MASK - as its albedo, coincident with the correct material copy | `skipDeferredGBuffer=0` |
| duplicate coincident geometry generally | the same draw submitted twice; `dedupSkinned` existed, was correct, and was gated to skinned draws and switched OFF | `dedupAll=0` |

### Three hard-won rules, all of them broken at least once this week

1. **A draw whose RESULT the engine reads can never be SKIPPED, only hidden or passed through.**
   SR3's material pass reads the G-buffer through `IR_GBuffer_DSF_DataSampler` and
   `IR_LBufferSampler`. Skipping that pass renders the world black except the sky. This is written
   above `hiddenPassMode` in the source and was broken anyway.
2. **SR3's visible world geometry IS the G-buffer pass.** Terrain and large instanced meshes reach
   Remix only through it - `v=17601 Diffuse_MapSampler` - while the material pass is per-object.
   Any rule that stops converting that pass unconditionally turns the world black. Two builds did.
3. **Before shipping a rule that changes a disposition, measure how many draws it changes it FOR,
   and from what to what.** Not "how many draws match". A render-target rule that looked like 12
   draws/frame hid 707; a shader-output rule that measured 8 hid 693.

**Rule 1 has now been broken FOUR times.** The fourth: SR3 bakes each character - skin, tattoos,
clothing - into one 2048x1024 render target and samples it as that character's albedo. The quads
that fill it are screen-space with a rank-0 source, so `hiddenPassMode=2` SKIPPED them, the atlas
was never written, and 72 character sub-meshes were path-traced with a 100% black texture. Remix's
own captures date it exactly: the atlas reads mean 182.7 in every capture from 08-13 to 08-18 and
mean 0.0 on 08-23. Fixed by `compositeToTexturePass`, which passes through only composites whose
TARGET is not screen-sized (41 draws/frame; the 357 screen-sized ones are untouched).
| File | What it holds |
|---|---|
| **this file** | Current state, what works, dead ends, next step. **Read "Current state" and "The costliest failure of this project"** |
| `docs/worklog.md` | Session history, run by run. **The 2026-08-23..26 entry is the current front** |
| `re/shader_constants.csv` | 52,990 named constants across 7,276 shaders - **query it, don't re-derive.** It settles "which register holds X" in seconds |
| `re/shaders/` | 1,693 `.fxo_pc` files. `tools/fxo_disasm.py <file> <index>` disassembles them - **this is how the vehicle/character bone difference was found** |
| **`<game>/rtx-remix/logs/remix-dxvk.log`** | **Remix's own log. READ IT FIRST** - it names what it refused and why, and confirms whether a setting was parsed |
| `<game>/.trex/d3d9.dll` | Remix's renderer. `retools.search strings` on it is a searchable manual for every option |
| `<game>/rtx-remix/captures/*.usd` | Remix captures. **`pxr` is installed - open them directly** (see "Reading Remix captures") |
| `docs/sr2-fork.md` | The design being implemented and what is still to port |
| `docs/asset-replacement-plan.md` | Remastered asset replacement. **Note the 2026-08-26 correction: Remix DOES skin replacement meshes** |
| `docs/engine-map.md` | The exe's D3D9 call sites (unused by the shim now, still valid) |
| `docs/vibe-re-tools.md` | The Vibe-RE toolkit in `tools/vibe-re/` |
| `src/sr3-rtx/` | The shim + `build.ps1` |
| `configs/` | Versioned `sr3-rtx.ini`, `rtx.conf`, `dxvk.conf`, `user.conf` |

**Deployed and hash-verified 2026-08-28b** (masters in `build/` and `configs/`):

    sr3-rtx.asi  f9b98e8550f46104d2f2cf5a6a3e7573
    sr3-rtx.map  5d0caaabca8f98106c6205b78caa369f
    sr3-rtx.ini  b0fcafc70dcee0f515fe27f217fbcb3f
    rtx.conf     d7e9c7ec79bc7208166f00ba8a56565a
    dxvk.conf    61d3fb899588a0e703ee016ce96acbb8
    user.conf    8b361562e85c86ab88ab2fe61495b335

    ffp=1  convertSkinned=1  hiddenPassMode=2  forceOcclusionVisible=1  skinRigidSingleBone=1
    screenSpaceMode=2  compositeToTexturePass=1  rtx.useVertexCapture = True
    <- NO LONGER TRUE. Superseded 2026-09-06: capture is OFF and the character bake works.
       See the STATE section at the top of this file before acting on anything in this block.
    skinRequireBoneDecl=1  applyMorph=1  skipDeferredGBuffer=1  skipNonColourTargets=1
    dedupSkinned=1  dedupAll=1  skinRingMB=24  dumpFrame=60
    generateCloth=1  clothAlbedoPercent=200  diffuseColorProbe=1  compositeToTexturePass=0  rtAlbedoCopy=1
    rejectStaleBones=0  clampBonesToUpload=0  paletteSetupScope=0  vehicleBonesOff=0
    rtx.useVertexCapture = False
    rtx.geometryAssetHashRuleString = indices,texcoords,geometrydescriptor
    d3d9.maxEnabledLights = 64
    d3d9.allowDiscard = False   (2026-08-29: the character texture fault is NOT in the shim)

**Hash-verify these before the user runs.** `skinRigidSingleBone` must stay 1 - at 0 every vehicle
body/glass/panel draw is refused and cars do not render at all.

**`sr3-rtx.map` is deployed beside the .asi on purpose.** The shim writes `sr3-rtx-crash.dmp` on a
fault and nothing could resolve an address in it until 2026-08-28, because the build produced no
symbols. `build.ps1` now emits `/MAP`. To read a dump: the exception stream carries its OWN thread
context (the thread-list one is the handler's), and `tools/read_minidump.py` prints the exception;
resolve `module+offset` against the map by taking the greatest symbol RVA <= the offset. That
turned "the game crashes at random" into `memcpy(dst, NULL, 4176)` in one pass.

### Health baseline, 2026-08-28 (compare against this before believing a regression)

    frame 9600 | draws 2965/frame | FFP converted 938/frame (31.6%)
    ...of which 6.1 SKINNED draws/frame passed through by skipDeferredGBuffer
    MORPH APPLIED: 32.4 draws/frame skinned WITH the delta, 0 fell back
    DEDUP: 39.7 draws/frame hidden, 0.0/frame did not fit
    VB LOCK HOOK: snoop holds 2.0 MB of 96 MB, 0 refused, 0 allocations failed
    TIMING: frame 34.4 ms avg (29 fps) | shim 20.22 ms avg (58.8% of frame)

Conversion at ~31% is the number to watch: both times the world went black it had fallen to 11-16%.

Backups: `D:\SR3RTXREMIXCOMP-backup-2026-08-21` (project + live game-dir state + captures).

---

## What this is

A D3D9 ASI shim that converts Saints Row: The Third's shader-driven draws to **fixed function**
so RTX Remix can path-trace them. Ported from
[BRAGme/sr2-rtx-remix-proxy](https://github.com/BRAGme/sr2-rtx-remix-proxy). Without it, **Remix
does not path-trace SR3 at all** - measured 2026-08-17 by running with `ffp=0`, which produced a
completely unmodified rasterised game. The approach is validated; the remaining problems are
about what ELSE reaches Remix besides our converted geometry.

## Current state

### SOLVED (with the measurement that settled each)

| problem | cause | evidence |
|---|---|---|
| z-fighting / doubled world | the geometry prepass reached Remix | marker texture + one hash in `rtx.ignoreTextures` (see the correction below - this changed the colour, it did not hide it) |
| crash on fast movement | use-after-free: caches held D3D pointers with no `AddRef` | 21,600 frames without reproduction after the fix |
| black sky, "dish, cap and ring" | our own HUD demote rule was capturing the skybox | `ProbeSkyDraw` named it in one run |
| white surfaces | passes with unlisted samplers converting with no colour | the L-buffer property rule |
| frozen character animation | the skinned VB is STATIC bind pose; animation lives in c52 constants | CPU skinning port |
| decal/particle plane blocking the camera | `rl_particle_*` billboards built in the vertex shader | `Depth_bufferSampler` rule |
| every light sharing D3D9 slot 0 | Remix could not match a light to its previous-frame self | flashlight trail, hash churn |
| **the world renders as flat material colour** | **Remix cannot read SHORT2 texcoords and DISCARDS them** | `[rtx-interleaver] Unsupported texcoord buffer format (80)`; VkFormat 80 = `R16G16_SSCALED` = `D3DDECLTYPE_SHORT2`. Fixed by converting to a float2 stream |
| roads tiled far too densely | the uv scale came from `Normal_Map_Tiling`, but the texture bound is the DIFFUSE map | 443 shaders carry a Normal_Map tiling pair, only 44 carry a diffuse one. Tiling is now matched by name to the albedo map, or withheld |
| geometry hashes churning every frame | the asset hash rule included `positions`, which change every frame for anything animated | `geometryAssetHashRuleString = indices,geometrydescriptor`. User: NPC, player and window hashes all stable afterwards |
| NPC clothing has no customisation colour | SR3 builds it PER TEXEL from a mask and three constants, which no texture-stage arrangement can express | recipe read from `ir_sr3npcclothfull_c` [8]; generator verified byte-exact against its own output, 21 texel classes, 0 mismatches |
| cars render black | `ConstantAlbedo` wrote LINEAR constants into `TEXTUREFACTOR`, which Remix reads as sRGB | `Base_Paint_Color = (0.041, 0.008, 0.006)` -> byte (10,2,2) -> ~0.0012 linear. Encoded properly it is (58,29,26), a dark red car |

### OPEN, 2026-08-28, in priority order

Vertex capture is off and working, and **every reported character/vehicle geometry bug is closed**
(see the table in Resuming). What remains:

1. **PERFORMANCE - now the largest problem.** `shim 20.22 ms avg (58.8% of frame)` at 29 fps.
   Two concrete leads, both from the 2026-08-28 logs:
   - **The bind-pose cache thrashes.** `1,434 meshes decoded, 410 cached, 1 cache flushes` - it is
     re-decoding meshes it evicted, and every decode is a read-lock across the 32->64-bit bridge.
     Raising `kMaxBaseMeshes` is the obvious first experiment; measure decodes/frame, not just fps.
   - **The skin ring saturates.** `high water 24.00 MB, 0.33 wraps/frame` at `skinRingMB=24`. The
     ring's size is driven by the game's largest `firstVertex`, NOT by geometry volume - see "The
     skinning ring". The real fix is to stop `firstVertex` driving the write position by adjusting
     `BaseVertexIndex` on the draw instead; that would let the ring be ~2 MB.
   - Also unexamined: `dedupSkinned`/`dedupAll` hash 8 bone matrices + objTM + albedo per converted
     draw, ~1,000 times a frame.
2. **Character TINT - half closed 2026-08-28b, half awaiting one run.**

   **`Tint_color` (c37) is settled and needs no run: it must NOT be applied to albedo.** It is
   applied AFTER the fog lerp, at the very end of the shader. Swept all 846 disassembled files:
   1,681 of 1,944 declaring entries use it in the shader's FINAL instruction, 1,927 within three
   of the end, its only consumers are `mul` (2,025) and `rcp` (945), and the remaining 147 uses
   are all `.w` - car-glass opacity. The 945 reciprocals settle it: shaders divide BY it to undo
   it on a value read back from a buffer, which is done to an exposure scale and never to a
   material colour. The shim was already right; the comment justifying it said "(5.0,5.0,5.0) on
   every character material in the game", which was four draws in one frame and is contradicted
   by the vehicle draws in the same log reading 0.0. Corrected in the source.

   **The per-object albedo tint is a DIFFERENT constant: `Diffuse_Color`.** The character
   material pass does `texld r2, v4, s0` then `mul r2, r2, c0` - and `SetupTextureStages` binds
   the map with SELECTARG1/TEXTURE whenever one exists, so that multiply is dropped on every
   converted draw. Across the corpus `Diffuse_Color` scales a diffuse/blend/pattern sample in 215
   entries, `Base_Color` in 12, `Tint_color` in 1. It is already reflected at
   `colourConstRank = 100` and used only when NO map is bound.

   It has been observed **twice, both (1,1,1,1)**, by a probe that reports once per distinct
   first-sampler name and stops at 12. `diffuseColorProbe=1` now measures the DISTRIBUTION.
   **`NON-WHITE 0.00 draws/frame` closes the question with no code change**; a non-zero SKINNED
   figure is the missing tint and states how many draws a fix would affect.

   Also still worth asking the user: whether characters look correctly tinted at all now that
   `skipDeferredGBuffer` has removed the specular-mask copy. That was never re-checked.
3. **The UI.** Shader-drawn, never converted, so the HUD is absent with capture off. `Disp::Hide`
   cannot fix it - it sets `D3DTS_PROJECTION`, which **Remix never reads for a shader-driven
   draw**, and with capture off the draw is skipped before any UI classification. The fix is named
   in the code: convert HUD quads to fixed function so `orthographicIsUI` fires.
4. **The sky.** ~135 draws a frame, deliberately passed through, so absent with capture off.
   Converting it needs care: a 343-vertex dome one unit from the camera.
5. **Hair renders white.** `Hair_Spec_Color1/2` never captured - the probe's slots fill first.
6. **The player's clothing colour** - a different recipe needing TEXCOORD1 reconciled with TEXCOORD0.
7. **~12 draws a frame still have unreadable texcoords** - DYNAMIC source buffers, needs a per-draw ring.
8. **Crashes: unproven either way.** One 26,400-frame session with no crash after the thread-safety
   work, which is not proof. Four dumps predate all of it (2026-08-25). The diagnosed one - a null
   memcpy source from a cross-thread race - is fixed. Race 1 (deferred invalidation) and the bone
   palette bound have never been observed firing; they are hardening, not diagnosed fixes.
## Subsystems added 2026-08-26..28 - read before touching the skinned path

### The morph stream (character customisation)

SR3 stores every character's face and body as a base mesh plus a per-character DELTA in a second
vertex stream. `ir_sr3npcskinfull_mc` (`m` = morph):

    def c0, 0.000122070313, 2, -1, 3          c0.x = 1/8192
    mov r1.xyz, v4                            v4 = dcl_position1, SHORT4, stream 2 offset 0
    mad r1.xyz, r1, c0.x, v0                  pos = v0 + v4/8192, BEFORE the bone blend
    mad r0.yzw, v5.xxyz, c0.y, c0.z           n_morph = v5*2-1  (dcl_normal1, UBYTE4N, offset 8)
    mad r0.yzw, r0, c0.y, r1.xxyz             n = n_base + 2*n_morph   <- a DELTA, not a swap

Swept before shipping: **384 of 384** skinned shaders that read the morph stream define the 1/8192
scale; the 52 that declare `dcl_position1` without it are all `rl_particle_*`, which are not
skinned. `ShaderInfo::usesMorph` gates it from the dcl stream.

**The morph buffer is DYNAMIC** - one 5 MB buffer, stride 12, one contiguous block per character,
and the STREAM OFFSET selects the character. It must never be read-locked. It is SNOOPED through
the machinery the instance streams already use, and the slice for a draw is COPIED OUT under the
lock (`SnoopCopy`) rather than borrowed - see below.

### The game locks vertex buffers from more than one thread

Proven, not assumed: a crash dump caught `memcpy(dst, NULL, 4176)` in `Hook_VBUnlock`, and the
source can only become null between the null test and the memcpy two lines later if another thread
cleared it. Measured rate afterwards: **272 buffer locks a frame** off the render thread.

Consequences, all now handled, and all of which had been unguarded since the snoop was written:

- `Hook_VBLock` used to call `InvalidateBaseMeshes` inline - erasing from a map and `Release()`ing
  buffers while the render thread held a `const BaseMesh*` into it for a whole skinning loop.
  Invalidations are now QUEUED and drained on the render thread at the top of each draw.
- Nothing may hold a pointer into the snoop cache across a draw. `SnoopCopy` copies out under one
  critical section. **Do not reintroduce an accessor that returns `c.data.data()`.**
- Do NOT pre-size those buffers to their declared size to get pointer stability. That was tried:
  ~2,390 instanced draws a frame across many buffers, each reserving its full size twice over,
  exhausted a 32-bit address space and killed the game with an unhandled `bad_alloc`.
- A shim must degrade, not abort its host. Growth paths are capped (96 MB total) and wrapped in
  `try`/`catch`; "This application has requested the Runtime to terminate it in an unusual way" is
  `std::terminate`, not an access violation.

### The deferred G-buffer pass

SR3 draws every object twice. The G-buffer pass writes `oC0`/`oC1`/`oC2` (normals and data, no
colour); the material pass writes `oC0` only, ending `mul oC0, r2, c37` with `Tint_color`, and
samples `IR_LBufferSampler`. `ShaderInfo::rtCount` counts distinct COLOROUT registers.

**Do not generalise from that.** Two builds turned the world black doing so:

- Terrain's G-buffer shader is structurally IDENTICAL to the head's, dead colour sample included,
  yet **the world's only converted copy is that pass** (`v=17601 Diffuse_MapSampler`). The
  material pass is per-object and does not carry terrain or large instanced meshes.
- What differs is what the game BINDS at that stage - a real colour map for the world, a specular
  mask (`Blend_Map`) for characters - and whether a material-pass copy exists at all.
- So the rule is gated on **skinned**, applies at the END of `Classify`, and returns
  **`Disp::PassThrough`** - never `HiddenDisp`, because `hiddenPassMode=2` means SKIP and the
  engine reads this pass back.

### Reading a crash dump

`build.ps1` emits `/MAP` and the map is deployed beside the .asi. The exception stream carries its
OWN thread context - the thread-list one is the handler's and will mislead you. Resolve
`module+offset` by taking the greatest symbol RVA <= the offset. A build differing only in PE
header bytes still resolves correctly; check with a byte diff before trusting it.

---

## Capture-off: how it was unblocked, 2026-08-23

The engine collapse under `useVertexCapture = False` was never Remix. **SR3 does its own GPU
occlusion culling and reads the depth prepass back**, and the rule was already written in this
codebase above `hiddenPassMode`: *"a draw whose RESULT the engine reads can never be skipped, only
hidden."* Capture-off IS a global skip.

Measured, same area, camera y~147:

    capture ON   5330 draws/frame        capture OFF  1426 draws/frame

73% of the GAME's own submission gone - matching the 77% collapse recorded for `hiddenPassMode=2`.
Objects clipped by a frustum plane survived, because an occlusion test on a clipped bounding box is
unreliable and engines skip the query for those. That detail is what identified the mechanism.

**The fix needed nothing from Remix.** Occlusion queries are D3D9 objects and we are the D3D9
layer. `forceOcclusionVisible=1` answers `D3DQUERYTYPE_OCCLUSION` readbacks with 2^20 visible
pixels. Measured **243.9 queries/frame, all answered**. Only occlusion queries -
`D3DQUERYTYPE_EVENT` is a frame-pacing fence. The real query is never drained, because
`D3DGETDATA_FLUSH` would stall across the bridge.

**Consequences:** `hiddenPassMode=2` became viable, which deleted the marker subsystem entirely -
**3886 marked draws and 14,319 SetTexture calls a frame, and the magenta permanently.** The cost is
that the game no longer culls anything, so more geometry is submitted than it would normally draw.

## Config layers and precedence

Parse order from the Remix log: **`dxvk.conf`, then `rtx.conf`, then `user.conf` LAST.** `user.conf`
wins, and it is what the Remix in-game menu rewrites - the menu has silently dropped hand-edited
keys from `rtx.conf` before. **`dxvk.conf` is not rewritten by the menu** and is the safest home
for hand-authored settings.

| file | holds |
|---|---|
| `dxvk.conf` | `d3d9.maxEnabledLights = 64` |
| `rtx.conf` | `useVertexCapture=False`, `geometryAssetHashRuleString=indices,texcoords,geometrydescriptor`, `enableAlwaysCalculateAABB=True`, `useBuffersDirectly=False`, `antiCulling.light.enable=True` |
| `user.conf` | upscaler/DLSS, `enableReplacementAssets=True` - **menu-owned, expect rewrites** |

## The geometry hash rule - settled

| rule | stable across animation? | unique per part? |
|---|---|---|
| `positions,indices,geometrydescriptor` | **no** - CPU skinning changes positions every frame | yes |
| `indices,geometrydescriptor` | yes | **no** - parts sharing an index buffer collide |
| **`indices,texcoords,geometrydescriptor`** | **yes** | **yes** |

Our skinning copies UVs from the bind pose untouched, so they never churn, and different parts have
different UVs. User-confirmed stable. **Keep this one.** Note Remix's own text: the asset hash rule
is for *"sampling from replacements and doing USD capture"*, not for placing geometry.

## Lights - the 8-slot cap was ours to raise

`d3d9.maxEnabledLights` is a DXVK option that fills `caps.MaxActiveLights` and defaults to 8, while
the game injects ~27 lights a frame. D3D9 silently ignores `LightEnable` past the cap and which
lights lose changes per frame. The shim already supported 64 and was simply being told 8. Set in
`dxvk.conf`; confirmed `device reports MaxActiveLights = 64`. **No code change was needed.**

## Fixed-function vertex blending is NOT available

    MaxVertexBlendMatrices = 4, MaxVertexBlendMatrixIndex = 8

Four influences per vertex is right, but only **nine matrices are addressable and SR3 needs 64**.
So the palette cannot be handed to Remix through fixed function, and CPU skinning stays.

## CORRECTION: Remix DOES support skinned replacement geometry

The claim "Remix cannot skin a replacement mesh" in `asset-replacement-plan.md` is **wrong**. Remix
1.5.2 ships `gpu_skinning`, `performSkinning`, `RtxGeometryUtils::dispatchSkinning`, full
`UsdSkelBindingAPI` read paths, a `ReadBoneTransform` graph node, a `skeletons/` capture directory,
and `rtx.limitedBonesPerVertex`, whose text is explicit: *"Limit the number of bone influences per
vertex **for replacement geometry**."* This removed the strongest argument for forking dxvk-remix.
The catch: it needs the DRAW to carry bones, which CPU skinning does not supply, and FF vertex
blending cannot supply them either (nine matrices).

## Reading Remix captures - tooling exists now

`pxr` (USD python bindings) is **already installed** (`Python312/Lib/site-packages/pxr`). Captures
are binary `PXR-USDC`:

```python
from pxr import Usd, UsdGeom, Gf
st = Usd.Stage.Open("capture_....usd")
# /RootNode/meshes/mesh_<hash>/mesh   points, faceVertexIndices
# /RootNode/instances/inst_<hash>_N   xformOp:transform  (name encodes the mesh hash)
# /RootNode/lights  /RootNode/Looks  /RootNode/cameras/Camera
```

A capture records the **ray-traced scene** - anything Remix rasterises does not appear. It is a
single frame, so it cannot show motion; two captures with a static camera can be diffed by mesh
hash. This also unblocks the hash-to-asset bridge for asset replacement.

## Established engine facts (measured, trust these)

**Frame structure** - textbook Volition inferred lighting, read off `sr3-rtx-frame.log`:

```
1280x720  G16R16          geometry / DSF prepass   (pixel shaders sample NOTHING)
1280x720  A16B16G16R16F   material pass            (carries the real diffuse map)
400x288   A16B16G16R16F   x3  low-res light buffers
1280x720  A8R8G8B8        back buffer - written ONLY by the composite quad
```

**Every mesh is submitted twice**: once to the prepass with a sampler-less shader, once to the
material pass with its diffuse map. Confirmed by a state probe catching the identical vertex and
primitive counts in both.

**Constant registers** (`docs/shader-map.md`): `projTM` c28 = VIEW*PROJ (4 regs), `objTM` c32 =
world (3 regs), `IR_World2View` c48 = view (3 regs), `Bone_weights` c52, 3 regs per bone.

**UV formula**, from disassembling `ir_bbsimple2_decal_s`:
`uv = raw * tiling / 1024`. The 1/1024 is a literal; the tiling factors are uniforms whose
REGISTERS DIFFER BETWEEN SHADERS, so they must be read from each shader's CTAB.

**Instance transform**: three `float4` rows declared as `POSITION` usage indices 2/3/4, in
**stream 4**, same row-major 3x4 layout as objTM. Instance counts are usually 1 but **can be
10,000** - a draw with count > 1 cannot be converted (fixed function has one world matrix).

**Projection is finite**: near=0.15, far=5000 (Q=1.00003). SR2's infinite-far problem does not
exist here.

**Camera**: 3-4 distinct cameras per frame. The main one is identified by matching the back
buffer's aspect ratio and not sitting at the world origin.

**Skinning** (from `debug_diffuse_only_c.fxo_pc` shader [0], cross-checked against all 882
shaders declaring `Bone_weights` in `re/shader_constants.csv`, which agree on reg 52 count 192):

```
pos' = ( SUM_i  w_i * Bone[idx_i] ) * float4(pos, 1)  /  SUM_i w_i
```

- palette at **c52**, `row_major float3x4`, **3 registers per bone, 64 bones** (192 registers);
- **objTM is applied AFTER the blend**, so skinning runs in OBJECT space and the existing
  WORLD/VIEW/PROJECTION path places the result unchanged. An older note in this file claimed
  objTM "means nothing" for skinned meshes - that was wrong;
- four influences, weights **not** pre-normalised (the shader divides by their sum);
- `BLENDWEIGHT` is **UBYTE4** - bytes summing to 254-255, not 1.0 - and index **255 is a sentinel
  for "no influence"**, always paired with weight 0. `255*3 = 765` addresses far past the palette,
  so influences must be rejected on WEIGHT, never trusted by index;
- the skinned vertex buffer is **STATIC** (usage 0x8 WRITEONLY, no DYNAMIC bit) and holds the
  bind pose. All animation is in the c52 constants, which is why Remix cannot see it.

**Skinned vertex declaration** (both variants): `float3` POSITION @0, `ubyte4n` NORMAL @12,
`ubyte4n` TANGENT @16, `ubyte4` BLENDWEIGHT @20, `ubyte4` BLENDINDICES @24, `short2` TEXCOORD @28.
A third variant carries **BLENDINDICES with no BLENDWEIGHT** - rigid single-bone attachment.

**Alpha cutouts are done INSIDE the pixel shader**, not by render state. From `tree_s.fxo_pc`:

```
float Alpha_Threshold;   // c41
texkill r0
```

**403 pixel shaders** do this - the whole `ir_at_*` family (`at` = alpha test), foliage, decals,
windows, cloth. `D3DRS_ALPHATESTENABLE` is never involved, so a rule testing only the render state
cannot see them and hands them opaque alpha, rendering every leaf card as a solid rectangle.

### Property rules that identify a pass without knowing its name

Each was checked against all 7,276 shaders BEFORE being written, because each one HIDES draws and
hiding a real surface makes it invisible. The safety argument in every case is that the category
which could be wrongly hidden is empty:

| property | means | count | real surfaces at risk |
|---|---|---|---|
| samples `IR_LBufferSampler` | produces visible colour (inferred lighting) | - | - |
| no albedo map + no colour constant + **no** L-buffer | G-buffer fill, not a material | 962 | **0** |
| samples `IR_GBuffer_Normals` | screen-space pass reading existing geometry | 30 | **0** (22 light volumes, 4 AO, 2 screen-space decals) |
| samples `Depth_bufferSampler` | `rl_particle_*` billboard built in the vertex shader | 29 | **0** |

**The sampler-name spellings are distinct and that distinction is load-bearing:**
`Depth_bufferSampler` is the particle system (29 shaders), `IR_GBuffer_DepthSampler` is what
**water** uses (40), `Depth_mapSampler` is projectors (5). A rule written against "samples any
depth buffer" would have deleted all five water shaders.

**A sampler name identifies a FAMILY, not a file.** `Decal_diffuse_mapSampler` appears in eleven
shaders; only two are the screen-space decals. Identifying a shader by one sampler name produced a
rule that never fired.

---

## The hiding problem - the 2026-08-18 conclusion was WRONG, corrected 2026-08-21

Remix reconstructs any draw we do NOT convert from vertex-shader output, so every unconverted pass
renders alongside our converted one. That part still holds. What was wrong is the fix.

**The marker texture never hid anything.** `rtx.ignoreTextures` is not a hide. Remix's own string:

    rtx.ignoreTextures
      "These textures will be ignored when attempting to determine the desired textures from a
       draw to use for ray tracing."

    (material description)
      "Runtime will not render any objects using an ignored material.
       RTX Remix will render with a PINK AND BLACK CHECKERBOARD."

So a marked draw is drawn as a pink-and-black checkerboard, which at any distance reads as magenta.
Every marked prepass draw in this game has been rendering as that checkerboard the whole time. What
looked like "duplicate world gone, z-fighting solved" in run 19 was the duplicate changing colour,
not disappearing.

**This is the origin of the magenta characters and the NPC head z-fighting**, both of which cost
runs 55-66 to trace back here.

### What was tried against it, and failed

| attempt | result |
|---|---|
| clear all 8 texture stages on marked draws | magenta stayed |
| put that clear on the sampler-less rule, where 85 of 86 skinned prepass draws actually go | magenta stayed |
| `rtx.hideInstanceTextures`, whose text is literally "hidden from rendering" | magenta stayed |
| confirm the marker hash in the Remix menu | it was already the selected texture - the hash was right all along |

All four were correctly implemented against the correct hash. **No texture-tag mechanism suppresses
a vertex-captured draw.** There is no configuration with vertex capture ON and no duplicates, and
looking for one is a dead end.

### `rtx.useVertexCapture = False` is the only mechanism that works

Remix's own compatibility message names it:

    [RTX-Compatibility-Info] Skipping draw call with shader usage as vertex capture is not enabled.

With it off, every draw still using shaders is skipped outright - the marked prepass, the composite
chain, the auxiliary cameras. Confirmed by the user: **the magenta is gone.**

Vertex capture has been on since session 1, when it was the only thing putting geometry in the
scene. It is a leftover: Remix describes it as being for "games using simple vertex shaders that
still also set the fixed function transform matrices", and SR3 sets no fixed-function transforms,
which is the entire reason this shim exists.

### The cost, measured

`ffp=0` with capture on produces a rasterised game and **no path tracing at all** - Remix's capture
button does nothing because there is no ray-traced scene. So:

**The fixed-function conversion IS the path-traced world, entirely.** Vertex capture was never
providing it; it was adding a second, shader-derived copy of everything passed through or marked.

Turning capture off therefore loses only what the shim does not convert. From a real frame:

    979 CONVERT     the path-traced world
    2765 MARK       2489 prepass + 242 composite + 22 auxiliary camera + 7 billboards + 3 dupes
      50 PASS       every one of them the sky

Everything in MARK *should* be absent, and capture-off gives that for free. What is genuinely lost:

1. **the sky** - 50 draws a frame, deliberately passed through so `rtx.skyBoxTextures` can tag it;
2. **the UI** - shader-drawn, never converted, so the HUD disappears entirely;
3. and a **stability bug** appears: objects enter the view frustum and are dropped immediately.

The first two are ordinary conversion work. The third is unmeasured and is what the run-67 probe
exists to settle - it instruments whether converted instanced draws read instance-transform bytes
the game actually wrote, or bytes left over from an earlier fill of the buffer.

**Ruled out for the stability bug already:** `rtx.antiCulling.object.enable` (verified parsed by
Remix, popping unchanged), `rtx.enableCulling` (front/back-face only), and de-instancing the
converted draws (`deinstanceConverted`, popping unchanged, reverted).

---

## Untextured materials - the white was a SECOND PREPASS, not a material

> **Superseded 2026-08-19 by the L-buffer rule** (see "Property rules" above). The
> sampler-name list this section describes could only ever recognise samplers already
> known to the shim, so any unlisted one - `Dob_Map` on hair, `baseSampler` on the
> customisation blend - still converted white. The property test replaced the list and
> took the genuinely-blank population from 20/frame to 0. This section is kept for the
> reasoning that got there.

*Rewritten 2026-08-18. The previous version of this section asked where sampler-less materials get
their colour and quoted 1,983 such shaders. Both the question and the number were wrong: the
corpus has 307 sampler-less pixel shaders, and they were never the white population.*

The white draws are **415 pixel shaders that DO declare samplers, but only utility ones** - 236
sample `IR_Stipple_Pattern_2D` alone, 179 sample it plus `Normal_Map`. None names a colour map, so
`rankAlbedo` unbinds stage 0 and they converted white.

Disassembly says what they are. In `ir_bb_tod_window_bs.fxo_pc`, shader **[6]** samples
`Normal_Map` (s0) and `IR_Stipple_Pattern_2D` (s11), decodes a tangent-space normal (`*2-1`,
normalise) and writes specular power, with no colour anywhere; shader **[8]** of the *same file*
is the material pass, carrying `Diffuse_Map`, `Decal_Map`, `Specular_Map`, `IR_LBuffer` and
`Tint_color`. Two shader indices, the two halves of inferred lighting.

**So the prepass signature is "samples no surface colour AND samples the IR stipple", not "samples
nothing".** We were converting the normal prepass and painting it white, coincident with the
correctly textured material pass. `skipUntextured` now hides both shapes.

Marking these is safe for a register reason, not a hopeful one: **stage 0 holds the normal map,
while the stipple driving the dithered discard is at s11 and is never touched**, so the depth the
pass writes - and the engine's occlusion culling that reads it back - is unchanged.

Still true and still worth knowing: stage 0 holds a Diffuse map 804 times across the game's pixel
shaders but a **normal map 903 times**, and Remix reads stage 0 as albedo. `rankAlbedo` exists to
stop those rendering as tangent-space green/orange - the "yellow geometry" identified early on.

---


## Remix's own binary is the documentation - use it before guessing

`.trex/d3d9.dll` registers every option by name with its description as an adjacent string, so it
is a searchable manual for the runtime:

```
cd tools/vibe-re
py -3 -m retools.search "<game>/.trex/d3d9.dll" strings -f <keyword>
```

**This is the single most productive tool in the project.** It found the texcoord format bug, the
`ignoreTextures` semantics, the terrain-baker requirement, the anti-culling option and the
vertex-capture behaviour. Every one of those had previously been guessed at, and most of the
guesses were wrong. Read the option before flipping it.

What it has established so far:

| option | what it actually does |
|---|---|
| `rtx.ignoreTextures` | ignored when choosing a draw's texture; **Remix draws an ignored material as a pink and black checkerboard**. Not a hide |
| `rtx.hideInstanceTextures` | "hidden from rendering, but not totally ignored... allowing for the hidden objects to still appear in captures". Does NOT suppress a vertex-captured draw |
| `rtx.useVertexCapture` | injects code into the game's vertex shader to capture output. OFF makes Remix skip every draw that still uses shaders. **The only mechanism that removes unconverted draws** |
| `rtx.geometryAssetHashRuleString` | default `positions,indices,geometrydescriptor` - includes positions, so animated meshes have a new asset identity every frame and no replacement can attach. Set to `indices,geometrydescriptor` |
| `rtx.geometryGenerationHashRuleString` | default `positions,indices,texcoords,geometrydescriptor,vertexlayout` |
| `rtx.terrainBaker.material.replacementSupportInPS_fixedFunction` | terrain baking does not apply to FIXED FUNCTION draws without it. Every draw this shim makes is fixed function, so `rtx.terrainTextures` was inert |
| `rtx.displacement` | full POM: `RaymarchPOM` and `QuadtreePOM`, `displaceIn`/`displaceOut`. This is the route for the parallax windows |
| `rtx.antiCulling.object` | extends the lifetime of objects leaving the frustum. Verified parsed; did not fix the stability bug |
| `rtx.enableCulling` | front/back-face culling only, not instance culling |

### The Remix API exists and is reachable, but is recreate-only

The 32-bit bridge client `<game>/d3d9.dll` exports `remixapi_InitializeLibrary` and
`remixapi_RegisterCallbacks`, gated behind `exposeRemixApi = True` in a `bridge.conf` that does not
exist yet. Its full command set is Create/Destroy pairs plus `DrawInstance`, `SetConfigVariable`,
and two stubs (`CreateD3D9`, `RegisterDevice`, both logging "Not yet supported"). The interface is
22 slots with 12 filled.

**There is no mesh update entry point anywhere**, so animated geometry means destroy-and-create per
mesh per frame across the 32-to-64-bit bridge. Good for static replacement geometry and lights;
wrong for CPU-skinned characters. It also settles the fork question: a fork was only ever worth
considering for per-texel material maths, and runtime texture generation reaches that from outside.

---

## Colour space: anything we hand Remix must be stored in the space Remix reads it in

This has now caused two separate bugs and will cause more.

Remix reads an 8-bit albedo texture, and `D3DRS_TEXTUREFACTOR`, as **sRGB**. The shim computes in
**linear** - the game's shaders do their maths there, and `pow(x, 2.2)` inside them is an
sRGB-to-linear conversion. Writing a linear value into either without encoding applies gamma twice.

| where | symptom | fix |
|---|---|---|
| generated clothing textures | "clothes are really dark" - 0.5 became 0.22 | encode with `pow(v, 1/2.2)` before writing the texel |
| `ConstantAlbedo` -> `TEXTUREFACTOR` | "cars are black" - `Base_Paint_Color (0.041, 0.008, 0.006)` became byte (10,2,2), about 30x too dark | same encode in `byte()` |

**And the reverse trap:** a constant at or above 1.0 in every channel is not a colour, it is an
exposure factor. `Tint_color` measures **(5.0, 5.0, 5.0) on every character material** - clamping
it to 255 painted 72 draws a frame pure white while counting them as rescued. It is now rejected.
But it is **not** uniform across the game: vehicle materials measure `Tint_color` at 0.009-0.022,
where it IS a real value. Do not generalise a constant's meaning from one material family.

---

## SR3's material recipes, read from the disassembly

**NPC clothing** (`ir_sr3npcclothfull_c` [8], 20 shader entries where the pattern IS the albedo):

    sum  = p.r + p.g + p.b
    dev  = |p.r-sum/3| + |p.g-sum/3| + |p.b-sum/3|
    test = sum - (dev*165.016495 + 256)/255
    test <  0 -> p.r^2.2*Diffuse_Color_a + p.g^2.2*Diffuse_Color_b + p.b^2.2*Diffuse_Color_c
    test >= 0 -> saturate((p - 0.372549) * 1.59375) ^ 2.2

The channels are gamma-2.2 WEIGHTS, not plain masks, and `test` is a chromaticity SELECTOR that
sends achromatic texels down a desaturated branch - which is how trim and skin escape the
customisation colours. Implemented and verified byte-exact. Pattern maps measure 32x32 to 512x512,
DXT1 and DXT5, and the read-lock on `D3DPOOL_DEFAULT` works under DXVK.

**Player clothing** (`ir_at_sr3pccloth_c` [8], 14 entries) is a DIFFERENT recipe:

    layer  = lerp(lerp(lerp(1, Diffuse_Color_c^2.2, p.b), Diffuse_Color_b^2.2, p.g),
                                                          Diffuse_Color_a^2.2, p.r)
    albedo = Diffuse_Map * Diffuse_Color * layer

A layered mask over a full-resolution diffuse, with the pattern on a **second texture coordinate
set** (`v1`, with `ClampU1`/`ClampV1`). Not implemented - it needs the two UV sets reconciled.

**Skin** binds the right texture already: a 2048x1024 uncompressed sheet, the composited character
face and body the customisation system builds at runtime. Its Sphere_Maps are a neutral lit sphere,
an environment term, not skin tone.

---

## Skinning facts settled 2026-08-20

- **`baseVertex` is always 0** on skinned draws. Threading it through the decode, the cache key and
  the ring offset was therefore INERT - the counter added to falsify that fix did so. The bounds
  check added alongside it stands on its own merits.
- **`skinRigidSingleBone` works now.** Refusals went 22-39/frame to **0** and nothing vanished. All
  four BLENDINDICES bytes are identical on those draws, so index component 0 was never a guess, and
  the palette is written 3-6 draws earlier under the same objTM, so it belongs to the draw.
- **Refusals were never the double-draw.** They reached 0 and the head z-fighting survived.
- **`dedupSkinned` is ON and works.** Its key needed a FOURTH component - the bound albedo - after
  the index range, the pose and the position. Analysing the key against a real frame BEFORE
  enabling it is what caught that: of 68 groups it would merge, one carried three different
  textures on one mesh at one position, and two of those three would have been dropped.

---

## The skinning ring - sized by the game's vertex indices, not by our geometry

`SkinAndBind` writes converted vertices into one `D3DPOOL_DEFAULT | D3DUSAGE_DYNAMIC` vertex
buffer. Two properties of it are easy to get wrong and both have now cost a run:

**1. Its size is set by the game's largest `firstVertex`, not by how much we skin.**

    const UINT base = firstVertex * stride;
    if (g_skinRingPos < base) g_skinRingPos = base;

The write position is dragged up to the game's own vertex index so the game's index buffer can be
reused verbatim - no index copy, no rebasing. Measured high water is **15.24 MB**, which at 32-byte
vertices is ~480,000 vertices; we only ever write ~6.8 MB of actual geometry a frame. So do not
reason about the ring's size from vertex counts. Read `high water` out of the report.

The only reason the position is forced up is that `SetStreamSource`'s offset (`g_skinRingPos -
base`) cannot be negative. `DrawIndexedPrimitive` takes a `BaseVertexIndex` that the hook currently
forwards unchanged; adjusting it for converted skinned draws removes the constraint and the ring
drops to ~2 MB. **That is the real fix and it has not been made** - it is a change on the hottest
path.

**2. A DISCARD renames the WHOLE buffer, so a bigger ring is not a free bigger ring.**

The ring resets at every Present, so the first skinned lock of each frame is `D3DLOCK_DISCARD`.
The driver must hand back a fresh allocation of the entire buffer and blocks if its pool has none
free. At `skinRingMB=64` this made the game unplayable in a busy street - shim time went from
12-18 ms to a 20-65 ms sawtooth while the draw count moved only ~20%.

So the two costs pull against each other: too small and it wraps mid-frame, too large and the
per-frame rename stalls. `24` is the current compromise (57% over the measured high water).

**The report tells you which one you are paying:**

    SKIN RING: N MB, high water H MB, W wraps/frame, D discards/frame
    SKIN RING LOCK: X ms/frame discarding, Y ms/frame appending, worst single lock Z ms

`appending` is the control - the same call on the same path without the rename. If `discarding`
dwarfs it, the size is the cost. If both are ~0, the ring is exonerated and the shim's time is
CPU skinning volume (211,312 vertices/frame across 54 draws), which is a different problem.

**Do not infer the ring's cost from the shape of the frame times.** It was timed for exactly that
reason: the sawtooth fits a driver stall, and it also fits a street filling up with NPCs.
---

## Changes that were MEASURED WORSE and reverted

Every one of these looked correct when written. They are recorded with the symptom that killed
them so the same reasoning is not repeated from scratch.

| change | symptom it caused | why it was wrong |
|---|---|---|
| `rtx.orthographicIsUI` + substituting the UI transform | UI vanished at some camera angles | `perspective=0` in the log means "failed the perspective test", not "is orthographic". The matrix was neither. |
| `tintFallbackAlbedo` - modulate a fallback map by `Tint_color` | character and NPC clothing went dark | `Pattern_Map` channels are MASKS selecting between `Diffuse_Color_a/b/c`. Multiplying a mask by a tint multiplies two things the shader never multiplies. |
| `skinRigidSingleBone` - skin BLENDINDICES-only meshes | clothing items vanished | Those meshes are most likely not bone-driven at all, so skinning moved them off camera. |
| `rtx.enableAlwaysCalculateAABB` | no effect | Every skinned draw shares one objTM (they are pieces of ONE character), so there was no instance ambiguity to fix. |
| sampler state to WRAP/LINEAR | no effect | The inherited state was ALREADY WRAP + LINEAR. Proven inert by the probe shipped alongside it. |
| mesh albedo cache "stop growing at the cap" | world progressively lost textures | At the cap no NEWLY streamed mesh could ever be protected. A bounded cache needs an eviction policy; "stop accepting" is not one. |

### The dedup lesson - FIVE attempts, and it is on now

Dropping a skinned draw whose geometry was already converted this frame. Each fix was correct as
far as it went and each left out one more thing that distinguishes an object:

| attempt | key | what it wrongly merged |
|---|---|---|
| 1 | buffer + vertex range | every material sub-range of one 7,977-vertex mesh |
| 2 | + index range + triangle count | every NPC sharing a garment |
| 3 | + bone palette | every NPC sharing a garment AND a pose (bones are in OBJECT space, so identical poses hash identically) |
| 4 | + objTM | - |
| 5 | + **the bound albedo** | three material layers on one head, at one position, in one pose - two of the three would have been dropped |

**An object is geometry AND pose AND position AND the texture it is drawn with.** Drop any one and
you merge things that are not the same.

**`dedupSkinned=1` since 2026-08-20**, and the fifth attempt is the only one that was analysed
against a captured frame BEFORE being switched on: "of the 68 groups this key would merge, how many
carry different textures?" - one, which is what added the albedo. It collapses exactly 5 draws a
frame, all but one at the player's own position, which matches the original "my character is drawn
twice". It does NOT fix the NPC head z-fighting; that was vertex capture.

### Counters are not evidence that something works

Three times a healthy-looking number described completely broken behaviour:

- `lights 46.8/frame` while **every light shared D3D9 slot 0**;
- `no-albedo materials 41/frame` while that counter was actually counting rank-0 draws INCLUDING
  the ones the constant rule had already rescued;
- every albedo counter reading fine while the surface sampled one texel - they record that a
  POINTER was non-null, never what it points at.

When a counter and the screen disagree, the counter is measuring the wrong thing.

---


## The costliest failure of this project: inventing a definition of "correct"

Sessions 22-26 spent five runs on the car-part drift and produced five wrong fixes. Three of them
failed the same way - **a metric I invented, measuring legitimate behaviour as a defect:**

1. *"a rigid part should end up centred on its own origin"* - **wrong.** `bone[13]` is a 22-degree
   rotation plus `(0, 1.121, -1.440)` and the mesh is authored at the origin: the bone legitimately
   **places the part on the car**. An offset result is normal.
2. *"bones written at different draw indices belong to different objects"* - **wrong.** One
   object's palette arrives as several `SetVertexShaderConstantF` calls, which naturally span
   several draw indices.
3. *"geometry that moves while objTM is static is drifting"* - **wrong.** It captured a destroyed
   car's suspension settling to rest: `bone[0]` translation decaying `0.117 -> 0.109 -> 0.097 ->
   0.087 -> 0.072 -> 0.000` over consecutive frames. Correct animation, correctly read.

Each one produced a confident diagnosis, a fix, an ini key with a long justification, and a wasted
run. **A falsifier only helps if the thing it falsifies is anchored to something outside your own
reasoning.** The measurements that held up were all anchored externally:

- the **shader disassembly** (`tools/fxo_disasm.py`) - what the game actually computes
- the **device's own constants** (`GetVertexShaderConstantF` read back and compared - 649,925
  comparisons, 0 mismatches) - not our mirror of them
- the **shader constant table corpus** (`re/shader_constants.csv`) - all 882 `Bone_weights`
  declarations at c52, all 2357 `objTM` at c32, settled in seconds
- **the game with the shim disabled** - which halved the search space in one run

### When you have burned two runs on hypotheses, stop and ask for an A/B

The single most valuable experiment of the session was the user's, not the agent's: run the game
**vanilla**, and run it **with Remix but with our .asi disabled**. Both were clean. That proved the
drift requires our shim and dissolved a paradox that had survived five probes - because with vertex
capture Remix reads the game's ALREADY-TRANSFORMED vertex output, so it is identical to the game by
construction, whereas we recompute that transform.

It should have been proposed after the second failed hypothesis, not the fifth.

### Read the code you already have before instrumenting

The occlusion-culling breakthrough was sitting in a comment above `hiddenPassMode` the whole time -
*"a draw whose RESULT the engine reads can never be skipped, only hidden"* - and the 73% collapse
matched the 77% already recorded there. Sessions 62-67 blamed Remix instead.

Likewise, `objTM`'s existing guard already described the exact failure mode later chased for the
palette, and `re/shader_constants.csv` could have killed the "wrong bone register" theory before it
was ever built.

### Verify a parser against real data before shipping it

The `dcl_blendweight` scan shipped with a bug caught only because it was re-implemented in Python
and run against the real bytecode first: **a comment block (CTAB is one, and it sits immediately
after the version token) carries its length in bits 16-30, not the 24-27 field instructions use.**
Reading the wrong field walks into the middle of the constant table and never reaches the dcls. It
would have shipped as a silent no-op and cost another run.

## Dead ends - do not retry without new information

- **`rtx.ignoreTextures` on game textures to remove shapes.** The shapes are ordinary world
  geometry drawn in the wrong pass, so they carry as many materials as the world does. Removing
  two only changed the shape of the artefact.
- **Skipping the composite quad.** It is the only draw that writes the back buffer, so the image
  freezes at a healthy 56 fps.
- **Skipping the prepass.** Breaks the engine's culling (above).
- **`albedoRank == 0` as a prepass test.** It also matches real materials whose only texture is a
  normal map; using it skipped ~2,400 real draws/frame including 17,601-vertex terrain. The
  prepass signature is an **empty sampler list**, not "no colour sampler".
- **Any texture-tag route to suppressing an unconverted draw.** `rtx.ignoreTextures` visualises
  rather than hides, `rtx.hideInstanceTextures` does not apply to vertex-captured draws, and
  clearing all eight texture stages changes nothing. The marker hash is confirmed correct. There is
  no configuration with `rtx.useVertexCapture` ON and no duplicate copies.
- **`rtx.antiCulling.object` for the capture-off instability.** Verified parsed by Remix; the
  popping was unchanged.
- **De-instancing converted draws for the same instability.** Every converted instanced draw
  already carries exactly one instance; resetting the frequency changed nothing. Reverted.
- **Reading a Remix `.usd` capture with naive string extraction.** The token table is LZ4-compressed
  inside the USDC container, so it reports 2 meshes where a scene has thousands. Either use a real
  USD library or do not use captures as evidence.
- **The DX9 tracer from the Vibe-RE toolkit.** SR3 statically imports five symbols from `d3d9.dll`
  and resolves `Direct3DCreate9Ex` at runtime; the tracer exports one. It cannot load.
- **The static D3D9 PE scanners.** SR3 is data-driven - declarations and register assignments come
  from `.fxo_pc` files and packfile tables, not immediates. `find_skinning.py` reports no skinning
  in a game with 221 skinned shader files.
- **Absolute `determinant < 0` for mirrored passes.** Whether SR3's view matrix preserves
  handedness is unverified; an absolute test would reject every draw if it does not. The current
  test compares against the frame's first camera instead.

---


- **Fixed-function vertex blending to hand Remix a bone palette.** `MaxVertexBlendMatrixIndex = 8`;
  SR3 needs 64. Closed by one caps line at device creation.
- **`rejectStaleBones` / `clampBonesToUpload` / `paletteSetupScope`** - three attempts to decide
  which palette "belongs" to a draw. All reverted; see the worklog. The palette IS read correctly
  (649,925 device comparisons, 0 mismatches), so ownership was never the defect.
- **`rtx.useBuffersDirectly = False`** - verified parsed by Remix; the drift was unchanged. Buffer
  lifetime is not the cause.
- **`rtx.enableInstanceDebuggingTools = True`** ("disables temporal correlation for instances") and
  **`upscalerType = 0` + `useDenoiser = False`** (all temporal reprojection off) - drift unchanged.
  Temporal handling is not the cause.
- **`rtx.enableAlwaysCalculateAABB = True`** - retried deliberately, because the session-31 revert
  happened when only ONE character was ever in the probe frame so the option had nothing to work
  with. Retried with multiple characters and vehicles present: no change.
- **`positions` in the geometry asset hash rule** - unique per part, but churns every frame because
  CPU skinning rewrites positions. Use `indices,texcoords,geometrydescriptor`.
- **The bind-pose cache invalidation** (`InvalidateBaseMeshes` on VB write-lock) reported **0
  invalidations** - the game never refills those buffers. Kept because it is correct by
  construction, but it fixes nothing and must not be credited for anything.

## The magenta: nine mechanisms, one hit. What that cost and why

Runs 55-66 chased one symptom - the player rendered magenta, NPC heads z-fighting - through nine
proposed causes:

| run | proposed cause | how it died |
|---|---|---|
| 55 | refused rigid draws pass through and get reconstructed | refusals reached 0, symptom stayed |
| 57 | duplicate submissions the dedup key was too small to catch | key fixed and working, symptom stayed |
| 58 | Remix categorises a draw from leftover texture stages | cleared all 8, magenta stayed |
| 59 | that clear was on the wrong prepass rule | 85 of 86 draws use the other rule; fixed it, magenta stayed |
| 60 | `ignoreTextures` visualises rather than hides; use `hideInstanceTextures` | magenta stayed |
| 61 | **vertex capture resurrects every unconverted draw** | **magenta GONE** - the one that hit |
| 62 | converted draws never reach the path tracer at all | `ffp=0` gave no path tracing whatsoever, so they ARE all of it |
| 63 | Remix drops culled objects; enable anti-culling | verified parsed, popping unchanged |
| 64 | instancing destabilises the fixed-function scene | de-instanced, popping unchanged, reverted |

**What would have shortened it:** reading Remix's description of `ignoreTextures` on day one. The
whole marker mechanism - built in session 18, believed working since - rested on assuming that
"ignore" meant "hide". One `retools.search strings` call would have said otherwise, and runs 58, 59
and 60 would not have happened.

### Counters going up is not the same as the intended population being covered

Run 58 added the wide stage clear and the counters moved exactly as predicted - marked draws 2,330 a
frame, SetTexture calls up from ~5,800 to 8,552. All of that extra work was landing on composite
quads and a single stipple draw, because the character prepass takes a different rule. The change
tested nothing and the numbers said it was working.

This is the third time a healthy-looking counter has described broken behaviour - see also
`lights 46.8/frame` while every light shared slot 0, and a no-albedo counter that also counted the
draws it had rescued.

### When argument fails twice, dump the data and look at it

The clothing generator was wrong twice on reasoning. Making the shim write the decoded pattern and
the generated result out as raw RGB, converting them to PNG and **looking**, settled it in one run -
and then a numerical check of 21 texel classes against the shader's own arithmetic proved the
generator byte-exact, which no amount of staring at code would have.

The same move works for "which texture holds the colour": `charTexDump` writes every named stage of
a character material, and one look showed the hair strands are in `Dob_Map` and the thing named
`Diffuse_Map` is directional data.

### Verify a setting was PARSED before concluding it did nothing

`rtx.antiCulling.object.enable` and `numberOfFramesToKeepObjects` were guessed names. Checking
`remix-dxvk.log` confirmed Remix read both, which is what made "the popping continued" a clean
negative instead of an ambiguous one.

### Write the falsifier into the change

Two fixes this session shipped with a counter whose whole job was to prove them wrong, and one of
them did: `skinned draws with a non-zero baseVertex: 0` established that the baseVertex fix was
inert, which would otherwise have been quietly credited for the heads improving in the same build.

### Analyse a risky change against real data before enabling it

`dedupSkinned` had failed four times. The fifth attempt was run against a captured frame first -
"of 68 groups this key would merge, how many carry different textures?" - and the answer was one,
which added the missing key component before it could break anything. That analysis took minutes
and is the only reason the setting is on.

---

## Method notes that cost real time

**Measure before fixing.** The "freeze" was diagnosed as a stall and fixed twice before anyone
timed it. It was running at **56 fps** - not a stall at all. One timing build settled in minutes
what two builds of reasoning did not.

**One classifier per decision.** `ShouldDemoteToUI` and `Classify` both decided the disposition
of the same draw and disagreed, turning real geometry into UI overlays. The pre-fork build died
of exactly this with ~20 interacting switches.

**A setting is a hypothesis that is not yet settled.** Once measured, it belongs in the code with
the measurement beside it and its switch deleted. Four were removed on 2026-08-17 with evidence
recorded in `configs/sr3-rtx.ini`.

**Cap the work, not just the log.** `MeasureWorldExtent` capped its output at 20 lines but kept
locking a vertex buffer on every converted draw - ~1,400 locks/frame feeding a diagnostic that
had stopped printing.

**Never read-lock a dynamic buffer.** Instance data is captured by snooping the game's own
`Lock`/`Unlock` instead.

**Any container holding a D3D9 interface pointer must AddRef it.** The mesh albedo cache did not,
and it holds textures across the streamer evicting them - a use-after-free that crashed the game
on fast traversal, and that silently substitutes textures when a freed address is recycled.
Anything cached across frames needs a reference or it is a dangling pointer waiting for the
streamer.

**`rtx.conf` is written by Remix, not by us.** When the user saves in the Remix UI, the game's
copy becomes authoritative - pull it back to `configs/` rather than overwriting it. Remix's own
sign convention is the correct one; a hand-written `0x4FAE...` should have been `-0x4FAE...` and
probably never matched.

**Verify deploys by hash, every time.** A locked `.asi` fails silently while the `.ini` succeeds;
`build.ps1` once reported OK for a stale binary after a failed compile (fixed - it deletes the
output first now).

**Capture analysis is currently unreliable.** `tools/capture_near.py` and two ad-hoc scripts
disagreed about the same file - one reported 15 quads at distance 1.0, another nothing closer
than 1.99. Reconcile them before quoting capture geometry. The "48 near-camera meshes -> 0"
result was built on this and should not be trusted.

---

## Things only the user can do

- Report what the screen looks like, ideally from **free cam moved away from the player**.
- Say when the game is open or closed - a running game locks `sr3-rtx.asi`.
- Tag textures in the Remix menu (Alt+X) -> Game Setup, then **Save**. Remix writes the hash in
  its own format, which is the only reliable way to get one.

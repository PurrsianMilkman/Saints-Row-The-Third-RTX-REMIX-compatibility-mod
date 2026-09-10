# Changelog

## v0.1.5 — 2026-09-10

**The player's clothing colours are correct on every garment.** Confirmed on screen, piece by
piece: shoes, wrist wraps, headwear, choker, earrings, armband, bracelets, corset, backpack,
underwear, and the bra with its star on the left cup only, exactly as the game has it.

This is the release where the cloth recipe stopped being "close" and started being *right*, and it
took four separate mechanisms to get there. Every one is behind its own ini switch.

### The colour chain, and why brightness knobs never worked

Earlier releases scaled cloth albedo with a percentage and called the remainder a lighting gap.
Both halves of that were wrong.

The player's shader has its own chain — `lerp(lerp(lerp(1, C, blue), B, green), A, red)` — which is
**not** the weighted sum plus desaturation branch used by the NPC family. And the game's
`Tint_color` is (5, 5, 5), applied through a **Reinhard tonemap**, `x/(1+x)` per channel.

That distinction is the whole thing. A linear ×2 preserves hue and could never turn
(0.059, 0.220, 0.298) into the cyan the game shows; saturation at ×5 does exactly that. The
tonemap now reads the tint from the constant rather than assuming it. `clothColourCurve=1`.

### The template garments were being cut by alpha all along

The bra and the underwear are template meshes whose diffuse maps are **63–90% transparent**. Two
thirds of the mesh lands on what looked like "black squares" — that is the invisible part of the
template, not a colour bug.

The generated texture now carries the alpha through untouched, the dilation leaves it alone, and
the converted draw is alpha-tested at 128, which is also what Remix reads as a cutout.
`clothCutout=1`.

A day was lost here to a tool rather than the engine: **the DDS writer fabricated alpha 255**, so
every dump looked opaque and the cutout was invisible in the evidence. Check the channel your
viewer is actually showing you.

### Independent UV sets are related by the mesh

Some garments put the pattern on a second UV set that has no arithmetic relationship to the first.
That was previously treated as a structural limit. It is not — **the mesh relates them.**

`BakeDecal` rasterises every triangle with a visible vertex into the diffuse's space, interpolating
the second UV set barycentrically, and records the pattern texel for each visible albedo texel.
Coverage: 78.9% of visible texels on the underwear, 91.4% on the bra; the remainder keep the
dominant colour, which is the background anyway. `clothMeshDecal=1`.

The full taxonomy, measured per draw rather than assumed: **ONE-POINT** (the second UV set is
constant across the draw — one texel, one colour), **AFFINE** (a scale and offset from the first),
and **INDEPENDENT** (two unrelated unwraps).

### Two surfaces on one island

The bra's two cups share a single diffuse island with mirrored UVs, and want *different* images —
the star belongs on the left cup only. One texture cannot hold two answers for one texel.

So the bake records every disagreement as an edge between two triangles, colours that conflict
graph greedily, and lays the generated texture out as **tiles** — each with enough side-by-side
copies that a panel crossing the wrap seam never leaves its region. At bind time the skinned copy's
u is shifted by whole tiles per triangle, seam vertices are duplicated with the other tile's shift
behind the draw's own vertices, a private 32-bit index list references them, and the draw hook
swaps it in for that one call. `clothDecalTiles=1`.

Splitting by connectivity and by winding were both tried first, and both were refused by their own
guards — one component, 257 same-winding conflicts. Those routes are closed.

### Also

`docs/cloth-uv-map.md` records which UV set feeds which sampler per shader, because
*"albedo is TEXCOORD0, pattern is TEXCOORD1" is not a rule* — it is measured per draw. It also
records that the clamp registers are inactive at runtime: the game wraps, and so does the
conversion.

`tools/cloth_uv_table.py` produces that table from the shader corpus.

### Known issues

- **The UI.** The HUD's draws are found and rebuilt, but the **in-game HUD is still not visible**
  and sub-menu text and backgrounds are missing. The menu video works. See the correction on
  v0.1.4 below — this was reported as fixed and was not. Parked for now.
- **The sky**, still passed through and so still absent with vertex capture off.
- **Other outfits.** Every mechanism above is general, but only one outfit is confirmed on screen.
  A garment needing more than 4 tiles, or with a visible panel wider than 12 texture widths, is
  refused by its own guard and falls back to a single tile.
- Frustum popping.

---

## v0.1.4 — 2026-09-07

> **Correction, 2026-09-10: the HUD is not back.** This entry and the v0.1.4 release notes said it
> was. What was actually true is that the HUD's draws are now *found* and *rebuilt* — a whole D3D9
> entry point, `DrawPrimitiveUP`, had never been hooked, so 13–30 draws a frame had been invisible
> to the shim since the fork — and the rebuild fires 16.6 times a frame. But **the in-game HUD is
> still not visible and sub-menu text and backgrounds are still missing.** The menu video does
> work.
>
> The counter said the rebuild happened. That is the *process*, not the *result*, and reporting it
> as the result was the mistake. The character-skin fix and the technique below are unaffected and
> were verified on screen.

**The character's skin is back**, and so are the player's clothing colours. Both came from one
observation.

### The technique

**Remix's refusal is about the vertex shader, not the draw.** Its own message says so —
`Skipping draw call with shader usage as vertex capture is not enabled` — because vertex capture
exists to capture *vertex-shader* output. The pixel shader was never the problem.

So a draw re-issued with **fixed-function vertex processing** while **the game's own pixel shader
stays bound** is a combination Remix executes happily with vertex capture off. That single fact
fixed three separate things:

| what | how |
|---|---|
| **the HUD** | `DrawPrimitiveUP` draws rebuilt as `D3DFVF_XYZRHW` quads. Positions are already screen pixels, so it is a 28-byte field reorder. 21.9 draws/frame rebuilt. |
| **the character's skin** | the atlas composites re-issued as a full-target quad with their own pixel shader kept — `4 done, 0 failed`, and the atlas means came back to 182.7 and 169.6, identical to their pre-breakage values |
| the menu video | proved the rule. Nulling the pixel shader made it greyscale, because Bink is Y/Cr/Cb across three stages and stage 0 alone is luma. |

### Fixed: the player's clothing was untinted

`ClothAlbedo()` refused any material whose albedo was not the `Pattern_Map` itself, which declined
**all fourteen** of the player's items — white bracelets, a white choker, a green beanie. There are
three garment shapes, and only the first was handled:

1. **pattern *is* the albedo** — resolved in the pattern's own texture space. Always worked.
2. **separate diffuse + flat pattern** — one colour for the whole garment, multiplied into the
   diffuse in its texture space. No mesh needed, no UV assumptions. Fixed in v0.1.3.
3. **separate diffuse + varying pattern on a second UV set** — genuinely needs the mesh to relate
   the two UV sets. Only the underwear. The CPU baker handles it and its result is now registered
   into the same cache the game's own draw reads.

A fourth case turned out to exist and is now handled too: a pattern that *varies* but which a given
garment samples at a **single point**, which needs no mesh at all.

There is now **one** colour pipeline, and both paths use it: pattern channels to linear through a
gamma LUT, weighted **sum** of the three colour constants (not a lerp from white), scaled, then
converted to sRGB once. Writing a second pipeline is what previously made the bracelets the right
hue at the wrong brightness.

### Fixed: hair has strand detail

`hairStrandsFromDob=1` modulates the hair colour with the `Dob_Map`'s strand detail, which adds the
variation a flat constant never could. The `Dob_Map` is directional data rather than colour, which
is why it took a while to find the right way to use it.

### Changed: the Remix-API character path ships off

`remixApiCharacter=0`. The game's own character now renders correctly through the technique above,
so the API copy is not needed — which retires **both** the duplicate character and the frozen one
that v0.1.2 and v0.1.3 shipped with. The API path itself is unchanged and still available; its
skinning crash is unresolved, but nothing in the shipped configuration reaches it.

### Two sub-projects, both new

**`engine-control/`** — a standalone `sr3-engine.asi` that hooks **the engine's own
render-command dispatch table** at `0x013509F8` instead of the D3D9 device vtable. SR3 is a
command-buffer renderer: a producer thread writes command blocks into a ring and a render thread
dispatches each through a 74-entry function-pointer table, which sits in writable `.data`. Hooking
there puts our code inside the engine, one level above D3D9, where a command is still a command
rather than six loose arguments — instead of reconstructing engine intent from samplers and render
target formats, which has blacked out the world three separate times. It shares no state with
`sr3-rtx.asi` and either can be removed without affecting the other.

**`srttr-hair/`** — an unrelated mod that reshapes *Saints Row: The Third Remastered*'s hair meshes
back onto the 2011 game's silhouette. The Remaster re-authored every style fuller and puffier, with
a spray of stray strand cards over the crown; on one measured style it went from 1,246 triangles to
10,751, and alpha coverage from 25% to 60%. Only vertex positions change — vertex counts, stride,
UVs, bone weights, index buffers, LOD ranges, morph data, textures and materials all stay byte for
byte as SRTTR shipped them. Its tools are versioned here; its game data is not.

### Known issues

**The sky is now the only headline gap** — ~14 draws a frame, still passed through, so still absent
with vertex capture off.

Also open: the underwear (garment class 3 above), overall cloth brightness — which remains a
difference in how Remix lights these meshes rather than an error in the recipe — and the ~11
draws/frame whose texcoords cannot be cached.

---

## v0.1.3 — 2026-09-07

Mostly groundwork for asset replacement, plus a cloth fix.

### The game's own texture library is now readable

Two new readers, written because the existing tooling could not do it:

- **`tools/peg.py`** — reads SR3's PEG (`GEKV` v13) texture containers. A texture is a *pair*: the
  `.cvbm_pc` / `.cpeg_pc` holds the directory and the `.gvbm_pc` / `.gpeg_pc` holds the pixels.
  The layout was confirmed byte by byte against a known texture (128×128, format 400,
  `frame_size` 8192 — exactly 128×128/2, which is what makes format 400 DXT1).
- **`tools/vpp.py`** — reads `VPP_PC` / `STR2_PC` v6 containers directly. The bundled
  `vpp_extract.py` wrote zero-length files for `customize_player.vpp_pc`; this one does not.

Together they extract **3,467 bitmaps** from the retail packfiles — 443 from
`customize_player.vpp_pc` and 3,024 from `customize_item.vpp_pc` — at full resolution, with alpha
channels split out where they carry real data (on this engine alpha often carries the cut-out and
is invisible in an RGB view).

That is the last missing piece for authoring replacements: the shim can already name the texture a
draw is using, and now the shipped source art for that name can be found and read.

**The extracted textures are not in this repository and never will be** — they are Volition/Deep
Silver art. `game-textures/` and `player-textures/` are gitignored. Regenerate them from your own
copy of the game with the two readers above.

### Fixed: garments with a flat pattern map came out wrong

SR3's `Pattern_Map` is often a 32×32 **uniform** selector — it picks the customisation colour and
carries no detail at all, with the visible detail living in the `Diffuse_Map`. The generator was
treating the pattern as the source of detail regardless, so those garments lost theirs.

`clothUniformFromDiffuse=1` detects a flat pattern and composes the garment as
`diffuse × one colour` instead. The frame report names each one it catches:

```
CLOTH UNIFORM #N: diffuse WxH * one colour (r g b) from a flat pattern
```

The cloth bake also now writes what it *read* — the pattern and diffuse it used, with the pattern's
first texel — so a wrong result can be traced to a wrong input rather than guessed at.

### New, and deliberately off: `uvScaleFromShader`

The UV divide has been the literal 1/1024 read out of the disassembly. This reads it instead from
each vertex shader's own `def` constant, per shader, and reports when the value is ambiguous. It is
`0` by default because the hardcoded value is correct everywhere measured so far; the switch exists
so the assumption can be tested rather than trusted.

### Changed

- `clothDump` and `charTexDump` are back **off**. They write `.raw` dumps into the game directory
  and were left on during the texture investigation.
- `remixApiTestCube` off — the API test cube has done its job.

### Known issues

Unchanged from v0.1.2. Still **no sky** and **no HUD**; the Remix-API character still stands beside
the game's own copy, is still frozen, and clothes still read darker than they should — that last one
remains a lighting difference rather than a recipe error.

---

## v0.1.2 — 2026-09-04

The architecture changed, and a conclusion that stood for weeks turned out to be wrong.

### The founding premise was disproven

Every earlier release said the fixed-function conversion **was** the path-traced world, on the
strength of an A/B: set `ffp=0`, the shim converts nothing, and the world comes back rasterised.

**That A/B had no control.** The only code publishing a **camera** to Remix lived inside the
conversion path, so `ffp=0` removed the camera and the conversion at the same time — and Remix
cannot trace anything without a camera. Two variables moved and one was credited.

`cameraOnly=1` settles it: publish the camera, convert nothing, and the world path-traces. What
Remix needed was the camera. The conversion is how the draw list is filtered down to something
Remix can afford to build — unfiltered, it builds geometry for every draw and exhausts Vulkan
memory at 33.5 GB.

### Characters are now built through Remix's programmatic API

Rather than dressing character draws up as fixed function and hoping Remix reconstructs them
correctly, the shim now describes them to Remix directly — creating the meshes, naming the
materials and choosing the textures — while the game renders untouched alongside.

Ten parts, ~22,000 triangles, with correct geometry, placement, per-slot textures, customisation
colours and hair.

This needs `exposeRemixApi = True` in `.trex\bridge.conf`, which is a **new install step**.
Without it `remixapi_InitializeLibrary` returns `rc=11 NOT_INITIALIZED`.

Contract details that cost a run each, recorded so nobody pays twice:

- `alphaTestType` mirrors `VkCompareOp`, so **0 means NEVER** — a zeroed extension struct makes
  geometry invisible while it still emits light. Use 7 (ALWAYS).
- **The hash is the identity.** Re-registering a changed material under an old hash is silently
  ignored and returns the *old* object, with every return code still reporting SUCCESS.
- `MeshInfoSurfaceTriangles` holds *pointers*; the arrays must outlive `CreateMesh` or the mesh
  renders as holes.
- SR3's character slots are **triangle strips** — `prims + 2` indices, not `prims * 3`.
- `SetupCamera` and `dxvk_RegisterD3D9Device` are unimplemented, and unnecessary: the API defaults
  to the game's own device and camera.

### Fixed: the black character texture

SR3 composites each character into a single 2048×1024 **dynamic** texture by locking each mip
surface individually. Two independent causes each blacked it out — running with vertex capture
off, and the shim skipping the composite quads.

### Customisation colours, verified from the executable

Colours are the 8-bit swatch **divided by 255** — read out of the exe at `0x008FCE60`, where the
divisor decodes to exactly 255.0. No linear conversion and no hidden scale. Parameters are bound
**by name**, so registers differing between shader variants (`Pattern_Map` at s0 or s2,
`Diffuse_Color` at c11 or c14) was never a bug. The `Pattern_Map` turns out to be a 32×32 uniform
selector; the visible detail comes from the `Diffuse_Map`.

### The renderer is reverse-engineered

`docs/engine-map.md` now documents SR3's command buffer, its render thread, a 74-opcode dispatch
table with per-opcode sizes, and the engine's own draw kill-switch at `0x03395EA4` — which is the
mechanism that makes `forceOcclusionVisible` load-bearing rather than a workaround.

### Known issues

Still missing: **the sky** and **the HUD**.

New, all specific to the API character pipeline:

- **The API character is frozen.** Handing Remix `MeshInfoSkinning` crashed its 64-bit server
  inside `CreateMesh`, so skinning is off.
- **Clothes read too dark.** The recipe is proven correct against both the exe and the shader, so
  the fault is in how Remix lights these meshes. `clothBrightness` is a tuning knob, not a fix.
- **The API character stands 3 units beside the game's own**, deliberately, so the two cannot
  z-fight while the pipeline is built. Removing the game's copy is the last step.
- Hair has flat colour with no strand detail — the colour itself is now correct.

### Routes closed — do not reopen

- **GPU bake through the Remix device.** Crashed Remix's server twice at the same address, even
  with the draw hidden inside an occlusion query.
- **Any texture tag that hides a vertex-captured draw.** Confirmed from Remix's own binary: it
  declines a draw for exactly three reasons — occlusion query, unsupported topology, no camera.
- **`GetRenderTargetData` on the game's render targets.** Froze SR3 twice. On our own targets it
  is fine.

---

## v0.1.1 — 2026-08-26

The car-part drift is fixed. That was the bug that consumed the most sessions on this project, and
it was found by disassembling the game's shaders rather than by running the game.

### Fixed

**Car parts and glass rendered in the wrong place and moved in rhythm with character animation.**

A knocked-off bumper, panel or pane of car glass would render somewhere it wasn't and drift around
in time with whatever the player or a nearby NPC was doing. The game's own physics position was
always correct — only the path-traced copy moved.

The cause was ours. The shim decided whether a draw was skinned from the **vertex declaration**;
the game decides it from the **shader**. Vehicle materials ship two variants over the *same mesh*
and the *same declaration*:

```
ir_sr3cardiffusespec_g_v    dcl_position / dcl_normal / dcl_blendindices
                            dp4 r0.x, c52[a0.x], r1     <- one bone, then objTM

ir_sr3cardiffusespec_g_s    dcl_position / dcl_normal   <- no blendindices
                            dp4 r1.x, c32, r0           <- objTM alone, no palette
```

For the `_s` variant the shim asked the shader where the bone palette lived, got "nowhere"
(`boneReg = -1`), and fell back to register c52 — **posing the car part with whatever the last
character draw had left in the bone palette.** That is every reported symptom at once: the part
moves but doesn't deform (one bone, one index), parts move *together* (they share the stale
palette), the movement follows character animation, and it never happens in the vanilla game.

Skinning is now gated on the shader actually declaring a `BLENDINDICES` input, read from the
bytecode. Swept all 1,693 shader files first: **zero** shaders declare `Bone_weights` without
`dcl_blendindices`, so the gate cannot stop something that genuinely skins.

Confirmed fixed by four probes going silent without being touched — `FOREIGN BONE` 20 reports → 0,
`DISPLACED SKIN` 16 → 0, `SKIN DISPLACEMENT` worst 2.1 units moved → 0.0 moved, `DRIFT` 0.0 moved.

**This also fixes intact windshields and door glass moving with the camera**, which had been listed
as a separate open issue. It was the same bug — intact glass draws through the same pair of shader
variants.

**Vehicles were skinned with four bones where the game uses one.** The disassembly showed vehicle
shaders use a single unweighted bone and character shaders use four weighted ones. The shim was
reading the vertex declaration, which still carries `BLENDWEIGHT` bytes that the vehicle shader
ignores — so it blended four bones and pulled three bone indices out of bytes the game never reads.
The blend form now comes from the shader's own `dcl` instructions.

**The CPU-skinning ring buffer was too small and was wrapping mid-frame.** It was 8 MB; measured
high water during real gameplay is 15.2 MB. Now 24 MB, exposed as `skinRingMB`. (A 64 MB trial made
the game unplayable and was reverted — 24 MB is the tested value.)

**Geometry asset hashes collided between car parts.** `indices,geometrydescriptor` was stable but
gave the same hash to different parts sharing an index buffer. Now
`indices,texcoords,geometrydescriptor` — stable *and* discriminating, because the shim's skinning
copies UVs from the bind pose untouched so they never churn.

### Changed

- `rtx.conf`: `rtx.enableAlwaysCalculateAABB = True`, `rtx.useBuffersDirectly = False`.
- New `sr3-rtx.ini` switches, all defaulting to off, kept as one-line A/B tests rather than deleted:
  `rejectStaleBones`, `clampBonesToUpload`, `paletteSetupScope`, `vehicleBonesOff`.
  `skinRequireBoneDecl=0` restores the old (broken) fallback with no rebuild, which is the A/B for
  the headline fix.

### Corrected

Two things the documentation previously asserted that turned out to be wrong:

- **Remix does support skinned replacement geometry.** `docs/asset-replacement-plan.md` said it
  could not. Remix 1.5.2 has `gpu_skinning`, `dispatchSkinning`, `UsdSkelBindingAPI` read paths and
  a `rtx.limitedBonesPerVertex` option whose own text says "for replacement geometry". This removes
  the strongest argument for forking dxvk-remix.
- **Fixed-function vertex blending is not a route.** The device reports
  `MaxVertexBlendMatrices = 4` with only nine addressable matrices; SR3 needs 64. CPU skinning
  stays.

### Known issues

Unchanged from v0.1.0: **no sky**, **no HUD**, hair renders white, player clothing colour is wrong
(NPC clothing is correct).

Removed from the list: windshield and door glass moving with the camera — fixed above.

New, and now the largest open problem after the sky and the HUD:

- **Shim time grows across a session**, from ~24% to ~44% of frame time, with worst-case stalls of
  ~300 ms. It tracks skinned-geometry volume (211k → 354k skinned vertices/frame), so it may just
  be a busier district, but the stalls are visible.
- The skinning ring saturates at its 24 MB size with ~0.19 wraps/frame. Deliberately not raised:
  wraps are now demonstrably harmless, the lock costs 0.01 ms/frame, and the unexplained 64 MB
  freeze argues against churning this while the game works.

---

## v0.1.0 — 2026-08-24

First public release.

A D3D9 ASI shim that converts Saints Row: The Third's shader-driven draws to fixed function so RTX
Remix can path-trace them. Without it, Remix does not path-trace SR3 at all — the engine never
calls `SetTransform`, so Remix had no camera and was rasterising the whole game.

Included at this point: matrices recovered from vertex-shader constant registers, CPU skinning for
characters, SHORT2 texcoords converted to a float2 stream, per-texel NPC clothing colour verified
byte-exact against the shader, correctly encoded car paint, the occlusion-query hook that stopped
objects being culled out of existence, and 64 light slots.

Known gaps at release: no sky, no HUD, white hair, wrong player clothing colour, camera-locked
glass, car parts drifting, and no performance work.

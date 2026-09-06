# Changelog

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

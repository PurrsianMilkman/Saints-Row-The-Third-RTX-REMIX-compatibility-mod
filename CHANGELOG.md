# Changelog

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

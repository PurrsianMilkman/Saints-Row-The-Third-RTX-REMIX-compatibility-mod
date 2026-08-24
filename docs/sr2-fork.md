# Porting the Saints Row 2 Remix proxy to Saints Row: The Third

*2026-08-16. Source: [BRAGme/sr2-rtx-remix-proxy](https://github.com/BRAGme/sr2-rtx-remix-proxy)
(MIT), itself derived from xoxor4d/remix-comp-base, adapted by Kim2091.*

The project was restarted around that design. Everything built before it was read is deleted —
see "What was thrown away" at the end.

## Why it applies

Same studio, same engine lineage, same problem: Remix path-traces **fixed-function** geometry
natively and has to *reconstruct* anything drawn through shaders from vertex-shader output.
Every recurring defect in this project traces to that reconstruction — meshes duplicated at
mirrored positions, quads welded to the camera, stale albedo, wrong UVs.

How closely the two engines match is worth recording, because it means their findings transfer
rather than merely inspire:

| | Saints Row 2 | Saints Row: The Third |
|---|---|---|
| Bone palette register | c52 | c52 |
| Registers per bone | 3 | 3 |
| Camera matrix | **fused** W·V·P at c4–c7 | `projTM` V·P at c28, world at c32 |
| View matrix | not supplied | **supplied**, `IR_World2View` c48 |
| Far plane | infinite (Q = 1) | to be confirmed — the shim now counts substitutions |

SR3 is the easier of the two. SR2 must *analytically decompose* a fused matrix into V and P;
we get V handed to us, so `P = inverse(V) · projTM` and there is nothing to guess.

## What the port takes

### 1. The conversion itself

Null both shaders, push WORLD/VIEW/PROJECTION through `SetTransform`, bind real albedo to
stage 0, draw, restore. Nothing is reconstructed, so nothing can be reconstructed wrongly.

### 2. The finite far-plane substitution — ~~the highest-value line~~ **does not apply to SR3**

> "SR2 uses an infinite-far projection (Q ≈ 1.0); **Remix's CameraManager rejects that and
> falls back to the wrong camera.** Substitute a finite far so Remix can lock onto a valid
> (near, far) pair." — `ffp_state.cpp`

**Measured 2026-08-16: SR3 does not do this, and the first version of this section was wrong.**

The projection reads `_33 = 1.00003`, `_43 = -0.15`. That is not an infinite far plane — it is
exactly what `near = 0.15, far = 5000` produces (`5000/(5000-0.15) = 1.0000300`). The initial
threshold of `_33 ≥ 0.999` matched it, so the shim "substituted" a numerically identical matrix
on every converted draw and reported 1,504 fixes per frame that changed nothing.

The check now requires an implied far beyond 10⁶ units, which admits only a genuinely unbounded
projection. It is kept because it costs nothing and the counter would reveal an auxiliary pass
that does use one — but **SR3's camera was never the problem, and no symptom is explained by
it.** The lesson is the project's oldest one: a threshold copied from another game is an
assumption, and the arithmetic took two minutes to check.

### 3. Alpha handling — a bug we would have shipped

> "A real cutout is identified by a non-zero alpha-ref — SR2 leaves alpha-test enabled with
> ref 0 on nearly all opaque world draws, and those must stay TFACTOR-opaque or Remix reads
> the sub-1.0 texture alpha as translucency and **every wall goes X-ray**."

Our previous `BeginFFP` set `ALPHAARG1 = D3DTA_TEXTURE` unconditionally. Now only a genuine
cutout (alpha test on **and** ref > 0) takes texture alpha; everything else is opaque.

### 4. Colour op — another correction

SR2 uses `COLOROP = SELECTARG1` from `D3DTA_TEXTURE`. Ours used `MODULATE` against
`D3DTA_DIFFUSE`, multiplying in a vertex-colour term the game meant for its own shading —
darkening everything and baking lighting into the albedo Remix reads. FFP lighting is also
switched off (`D3DRS_LIGHTING = FALSE`) for the same reason.

### 5. Render-target textures are never albedo

`ffp_state` keeps a set of textures created with `D3DUSAGE_RENDERTARGET` and refuses to use
one as base colour, falling back to the **largest** authored texture bound on any stage —
largest, not first, because a tiny detail map leaves Remix a featureless surface it renders as
a near-mirror. This is directly on-point for us: the camera-blocking planes were textured with
render-target hashes (`4FAE8190C113287B`, `CBA54AB254F72ECE`), and "surfaces flickering through
unrelated images" is what binding a per-frame buffer as albedo looks like.

### 6. The state-call catastrophe, avoided in advance

SR2's own profiling, on a 4070 Ti at ~21 FPS with the GPU at **37%**:

| Call | Per frame | Draws/frame |
|---|---|---|
| `SetRenderState` | 6,460 | ~530 |
| `SetTextureStageState` | 4,304 | |
| `SetTexture` | 4,029 | |

~28 state changes per draw, and **the game was not the source — their own FFP conversion was.**
Every call crosses a 32→64-bit process boundary as IPC. SR3 submits roughly **three times**
SR2's draw count, so repeating this would have been far worse than merely slow.

Built in from the start here: a shadow cache over `SetRenderState` / `SetTextureStageState` /
`SetSamplerState` that drops redundant writes and answers reads locally, plus dirty-tracked
transforms (three `SetTransform` per *change*, not per draw). The log reports dropped calls per
frame. Invalidated every Present, because state-block `Apply` bypasses our hooks.

### 7. Do not re-submit via `DrawIndexedPrimitiveUP`

> "Persistent dynamic VB/IB used instead of DrawIndexedPrimitiveUP. **DrawIndexedPrimitiveUP
> triggers a null-ptr crash in the Remix bridge server.**" — `renderer.hpp`

Relevant when skinning lands. Our own 2026-08-15 crash was an `ACCESS_VIOLATION` reading `0x20`
inside `.trex\d3d9.dll` — the same shape of failure.

## What we keep that SR2 does not have

**CTAB sampler names.** SR3's shaders carry a constant table naming every sampler, so base
colour is chosen by *name* (`Diffuse` > `Decal_Map` > `Pattern_Map` > `Blend_Map` > `Grime` >
`base_sampler` > `Illumination`) rather than by size. Stage 0 holds a Diffuse map 804 times and
a **normal map 903 times** across the game's pixel shaders; ranking is what stops ~900
materials being shaded with a tangent-space normal map. Where a shader has no colour map at
all, stage 0 is left **empty** — untextured white is wrong, but it will not masquerade as
surface detail.

**Property-based pass selection.** `skipDepthOnly` drops draws with
`D3DRS_COLORWRITEENABLE == 0`, which is what the inferred-lighting depth prepass is. The old
build selected passes by render-target *index*, and those indices are not stable.

## Status, 2026-08-17

**The approach is validated.** Running with `ffp=0` (shim inert) produces a completely
unmodified rasterised game, so Remix does not path-trace SR3 without this conversion.

Landed: per-draw transforms, instancing via the instance stream, the UV formula, light
injection, the mesh albedo cache, a state shadow that keeps bridge traffic down, and a
four-way draw disposition. See `docs/YOUR-INSTRUCTIONS.md` for current state, the unsolved
"hiding problem", and the dead ends not to retry.

## Still to port

1. **Skinning** (`src/comp/modules/skinning.cpp`). Characters are excluded until CPU skinning
   re-submits them pre-transformed. Bone layout is identical to SR2's, so this should port
   closely. Use a persistent dynamic VB/IB, not `DrawIndexedPrimitiveUP`.
2. **Vertex expansion.** SR2 expands to a fixed 32-byte FFP vertex, decoding `half`, `ubyte4n`
   and `short2` fields the fixed-function pipeline cannot read correctly. SR3 stores UVs as
   `short2` on most meshes (stride 20/24), which FFP reads as raw integers — UVs in the
   thousands. A texture matrix (`uvScaleDenom`) is the cheap fix if a single scale works;
   expansion is the general one. **Measure from the `vertex layout` log lines first.**
3. **Mesh albedo cache.** SR2 caches each mesh's first-seen albedo and rebinds it after the
   streamer evicts a wall's unique texture to a shared atlas. We have the same symptom.
4. **The Remix API** (`remix_api.cpp`, `custom_lights.cpp`). They inject lights through the
   Remix API directly rather than `SetLight`, which needs `exposeRemixApi = True` in
   `bridge.conf`. We still use `SetLight`; it works, but the API path is strictly better.

## What was thrown away

Deleted outright rather than defaulted off, because each was a heuristic compensating for bad
reconstruction and each caused a measured regression of its own:

`demoteCameraMismatch`, `demoteLightVolumes`, `demoteViewSpheres`, `skipCompositePasses`,
`skipLightVolumes`, `skipCameraMismatch`, `skipSkinned`, `skipProcedural`, `skipScreenSpace`,
`cullBlankScreenQuads`, `cullStaleAlbedoDraws`, `cullFullscreenQuads`, `collapseTarget`,
`skipTargets`, `screenSpaceAsUI`, `skinnedAsUI`, `proceduralAsUI`, `publishWorld`,
`oneCameraPerFrame`, `aspectFilter`, the degenerate-collapse vertex shader (`GetKillVS`), the
render-target index inventory, caller-region attribution, the `mode=0/1` split and the runtime
engine-map verifier.

The engine map itself survives in [engine-map.md](engine-map.md) and `tools/`; nothing was
learned that is lost. Previous source and config: `docs/evidence/pre-sr2-fork/`.

One deliberate consequence: **there is no longer a per-frame "main camera" latch.** Each
converted draw carries its own exact transforms, so a shadow or reflection pass is placed
correctly instead of being reconstructed against a camera that was not its own. That removes
the need for the aspect filter, the origin rejection and the one-camera-per-frame latch
together — SR2 works this way and lets Remix's own camera selection decide.

# SR3 shader map — reverse-engineering results

*2026-08-13. Produced by `tools/vpp_extract.py` + `tools/fxo_scan.py` from
`packfiles/pc/cache/shaders.vpp_pc`. Raw data: `re/shader_constants.csv` (52,990 rows).*

**Headline: SR3 uploads world, view and projection as three separate, named matrices at fixed
constant registers, and every light type is a named shader with its parameters in known
registers.** Both blockers of this project are now addressable without touching the game binary's
logic — only reading it.

## What was extracted

| | |
|---|---|
| Packfile | `shaders.vpp_pc`, VPP_PC **version 6**, 1693 entries, 9 MB → 35.6 MB |
| Extracted | 1693 files: **844 DX9 (`.fxo_pc`)**, 847 DX11 (`.fxo_pc_dx11`), 2 misc |
| Parsed | **7,276 DX9 shaders** (21,462 vs_3_0 + 31,528 ps_3_0 constant entries) across the 844 |
| Failures | 1 file (`rl_light_sampling_stenciltest`) — trivial edge case, ignorable |

Constant **names survive** in every shader's CTAB, so nothing here is guesswork — these are
Volition's own identifiers. The `IR_` prefix throughout confirms the research finding that this is
**Inferred Rendering** (Kircher & Lawrance, SIGGRAPH 2009).

## Container formats (both newly documented here)

**VPP_PC v6** — header fields at fixed offsets: `0x14C` flags (`0x4801`), `0x154` file count,
`0x158` package size, `0x15C` index size, `0x160` names size, `0x164` uncompressed data size,
`0x168` compressed data size. Directory at `0x800`, **24 bytes/entry**
(`nameOffset, ?, dataOffset, size, compressedSize, ?`); `dataOffset` indexes the *uncompressed*
stream. Names table and data section each start 2048-aligned.
**Gotcha:** each block is a zlib stream **with the adler32 trailer stripped**, padded to 2048.
Decode as raw deflate (`wbits=-15`) from byte 2, or every file fails.

**`.fxo_pc`** — magic `EE A1 42 4B`, then several compiled DX9 shaders (technique variants)
concatenated, each with an intact `CTAB`. Scanning for the `CTAB` fourcc is sufficient; the
container's own tables never need decoding.

## 1. Camera matrices — the fix that turned path tracing on

> **Confirmed in-engine 2026-08-13.** `projTM` is not a projection matrix despite the name: the
> vertex shaders compute `clipPos = projTM · worldPos` directly, with no view matrix between, so
> **`projTM` is a fused view-projection** and `IR_World2View` is the separate pure view matrix.
> The true projection is `inverse(IR_World2View) · projTM`. The ASI also proved the engine calls
> `SetTransform` **zero** times, which is why Remix had no camera and rasterized everything.

Register assignments are **completely invariant** — same slot in every one of thousands of shaders:

| Constant | Register | Size | Meaning | Shaders |
|---|---|---|---|---|
| `projTM` | **c28** | 4 regs (4×4) | projection matrix | 3398 |
| `objTM` | **c32** | 3 regs (4×3 affine) | object→world | 2357 |
| `IR_World2View` | **c48** | 3 regs (4×3 affine) | **world→view** | 2203 |
| `eyePos` | c41 | 1 | camera position | 2133 |
| `Bone_weights` | c52 | 192 regs | skinning palette (64×3) | 882 |

All vertex-shader constants, no exceptions.

**Why this matters.** Remix reads `worldToView`/`viewToProjection` from
`SetTransform(D3DTS_VIEW/PROJECTION)` and *never* from shader constants
(`D3D9Rtx::processRenderState()`). SR3 renders through shaders, so wherever it doesn't also set
the fixed-function transforms, Remix has no camera — exactly the Shaundi's-loft symptom.

Consequences:
- **World/view/projection are separate, not a fused WVP** ⇒ `rtx.fusedWorldViewMode` should stay
  `None`; the fused-matrix theory is dead.
- A hook on `SetVertexShaderConstantF` can watch c28/c32/c48, rebuild the matrices (expanding the
  4×3s to 4×4) and call `SetTransform` — handing Remix a valid camera *by construction*, in every
  location, with no heuristics. This is the Phase 3 camera module, and it is now fully specified.

## 2. The lighting pipeline — the "no lights" fix

Every light type is its own named shader drawing a volume into the irradiance buffer, with all
parameters in pixel-shader constants:

| Shader file | Light type | Key constants |
|---|---|---|
| `ir_light_point.fxo_pc` (+`_tex`) | point | `IR_Light_Pos` c0, `IR_Light_Color` c3, `IR_Light_Info` c4 |
| `ir_light_spot.fxo_pc` (+`_tex`, `_noshadows`) | spot | `IR_Light_Pos` c0, `IR_Light_Dir` c1, `IR_Light_Color` c13, `IR_Light_Info` c14, `IR_Spot_Info` c15 |
| `ir_light_directional.fxo_pc` (+`_amb_only`, `_direct_only`) | **sun** | `IR_Light_Pos` c0 (direction), `IR_Light_Color` c2, `V_ambient_render` c28 |
| `ir_light_tube.fxo_pc` (+`_tex`) | tube/area | as point, plus tube endpoints |
| `ir_light_local_ambient.fxo_pc` | ambient probe | — |

Shared: `IR_Light_Inv_Proj_TM` (vs c60, 4 regs), `IR_GBuffer_{Lighting,Normals,Depth}Sampler`
(ps s12/s13/s14), `IR_Similarity_Data` (ps c45).

### Light parameter semantics — decoded from the disassembly

Recovered by disassembling the light pixel shaders with `tools/fxo_disasm.py`
(`D3DXDisassembleShader` out of the in-box `d3dx9_43.dll`, no SDK needed). The relevant
instructions and what they prove:

**Distance attenuation** (identical in point and spot; `c4` here is `IR_Light_Info`):
```asm
mad_pp r1.xyz, v1, -r0.w, c0   ; light vector  = IR_Light_Pos - surfacePos   -> c0 is a POSITION
rcp_pp r0.w, r0.w              ; r0.w = distance
add_pp r0.w, r0.w, -c4.y       ; distance - Info.y
add_pp r0.x, -c4.y, c4.z       ; Info.z - Info.y
rcp_pp r0.x, r0.x
mul_sat_pp r0.x, r0.x, r0.w    ; t = saturate((distance - Info.y) / (Info.z - Info.y))
add_pp r0.x, -r0.x, c5.w       ; 1 - t
pow_pp r1.z, r0.x, c4.x        ; attenuation = pow(1 - t, Info.x)
```
⇒ **`IR_Light_Info` = (falloff exponent, inner radius, outer radius, –)**, giving
`attenuation = pow(saturate(1 - (d - inner)/(outer - inner)), exponent)`.
**`outer radius` is the light's range** — exactly what `D3DLIGHT9.Range` needs.

**Spot cone** (`c1` = `IR_Light_Dir`, `c15` = `IR_Spot_Info`):
```asm
dp3_pp r0.z, -r1, c1           ; cosAngle to the spot axis
add_pp r0.z, r0.z, -c15.y      ; cosAngle - Spot.y
mul_sat_pp r0.z, r0.z, c15.z   ; * Spot.z   -> saturated cone falloff
```
⇒ **`IR_Spot_Info.y` = cos(outer cone angle)**, **`.z` = 1/(cos inner − cos outer)**. So
`outerAngle = acos(Spot.y)` and `innerAngle = acos(Spot.y + 1/Spot.z)` — i.e. `D3DLIGHT9.Phi`
and `.Theta` are directly recoverable.

**Directional / sun**: `dp3_pp r0.w, r0, c0` uses `IR_Light_Pos` straight as an N·L direction —
no position subtraction, no distance term — confirming that for this shader **`IR_Light_Pos.xyz`
is a direction**, with `IR_Light_Color` (c2) the sun colour and `IR_light_back_color` (c1) a
rim/translucency term.

**Coordinate space caveat**: these are *view-space* quantities (the shader reconstructs surface
position from the depth buffer along a view ray). D3D9 lights are world-space, so the light module
must transform by the inverse of `IR_World2View` (c48) — which the camera module already tracks.

### Resulting D3DLIGHT9 translation

| D3DLIGHT9 field | Source |
|---|---|
| `Type` | which `ir_light_*` shader is bound |
| `Position` | `IR_Light_Pos.xyz` × inverse(`IR_World2View`) |
| `Direction` | `IR_Light_Dir.xyz` (spot) / `IR_Light_Pos.xyz` (directional), rotated to world |
| `Diffuse` | `IR_Light_Color.rgb` |
| `Range` | `IR_Light_Info.z` (outer radius) |
| `Attenuation0/1/2` | fit to `pow(1 - t, IR_Light_Info.x)`; Remix least-squares-fits the curve anyway |
| `Theta` / `Phi` | `acos(IR_Spot_Info.y + 1/IR_Spot_Info.z)` / `acos(IR_Spot_Info.y)` |

**Consequence.** Position, direction, colour, falloff and cone angle for every light in the scene
are readable at draw time. A hook can translate each into a `D3DLIGHT9` and call
`SetLight`/`LightEnable`, which stock Remix converts into real ray-traced lights
(`RtxContext::addLights`) — no Remix API, no runtime fork, and it works over the 32-bit bridge.
`ir_light_directional` is the sun, which is the single highest-value light to inject first.

## 3. G-buffer / render-target structure (bears on the "looks rasterized" question)

The G-buffer samplers (`IR_GBuffer_DepthSampler`, `..._NormalsSampler`, `..._LightingSampler`,
`IR_GBuffer_DSF_DataSampler` — 1329 shaders) and `IR_LBufferSampler` (1325 shaders) confirm the
inferred-rendering flow: **depth+DSF prepass → low-res light buffer → material pass sampling it.**
Material shaders read `IR_LBufferSampler` and `IR_Pixel_Steps` (1989 shaders) to fetch their
lighting from that offscreen buffer.

This is consistent with Remix's per-frame log line
`Found a draw call to a non-primary, non-raytraced render target. Falling back to rasterization`,
and with the user's report that the world looks rasterized. **Phase 0's controlled A/B test is
still what settles it** — this is corroboration, not proof.

## Open questions for the next pass

1. Do the world material shaders write to the back buffer or to an offscreen target? (Answerable
   from the extracted bytecode + a frame capture.)
2. Which specific shader draws the final composite — the candidate for `raytracedRenderTarget`
   tagging.
3. ~~`IR_Light_Info` / `IR_Spot_Info` component layout~~ — **done**, see above.
4. Skybox shaders (`rfg-skybox-*`) → the texture hashes to tag as Sky.
5. `IR_Spot_Info.x` and the `.w` components of `IR_Light_Info`/`IR_Light_Color` are still
   unaccounted for (possibly intensity or shadow-map indices) — harmless to ignore initially.

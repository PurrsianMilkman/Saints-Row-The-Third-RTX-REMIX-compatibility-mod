# Start here

*Rewritten 2026-08-17 as a full handoff. Current-state sections updated 2026-08-26.*

## Resuming

Start Claude Code in `D:\SR3RTXREMIXCOMP` and say:

> Continue the SR3 RTX Remix project. Read docs/YOUR-INSTRUCTIONS.md and the last session of
> docs/worklog.md before doing anything.

**State in one paragraph, 2026-08-26.** Vertex capture is OFF and working - the blocker that
dominated four sessions is solved by answering the game's own occlusion queries (`forceOcclusionVisible`),
because SR3 reads its depth prepass back for GPU culling and capture-off is a global skip. That also
let `hiddenPassMode=2` become viable, deleting the marker subsystem and the magenta entirely. Lights
are fixed (`d3d9.maxEnabledLights = 64` in `dxvk.conf`). **The open bug is that knocked-off car parts
and car glass are rendered in the wrong place and move with player/NPC animation, and NPC heads are
wrong.** It requires our .asi - the vanilla game is clean, and so is Remix with the shim disabled.
Five fixes have been attempted and reverted; our arithmetic is verified correct against the device
and the disassembly. **Read "The costliest failure of this project" before proposing a sixth.**

**The drift is FIXED as of 2026-08-26** - confirmed by the user and by `FOREIGN BONE` and
`DISPLACED SKIN` both going to zero reports, with `NO BONE DECL: 15.3 draws/frame` showing the gate
doing the work. The
shim decided "this draw is skinned" from the vertex DECLARATION. The game decides it from the
SHADER, and car body/glass ship two variants over the same mesh and the same declaration:
`ir_sr3cardiffusespec_g_v` declares `dcl_blendindices` and reads `c52[v.x*3]`; the `_s` variant
declares neither and places by `objTM` alone. On the static variant `boneReg` came back -1 and the
code **fell back to c52**, posing a detached part from whatever palette the last character draw had
left there. `skinRequireBoneDecl=1` gates skinning on `dcl_blendindices`. `skinRequireBoneDecl=0`
restores the old behaviour with no rebuild, if this ever needs re-proving.

The ring is **eliminated**: `SKIN RING LOCK: 0.01 ms/frame discarding` and `0.02 wraps/frame`, so
neither its cost nor its recycling explains anything. It did need raising from 8 MB (it genuinely
wrapped) - see "The skinning ring" below.

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

**Deployed and hash-verified 2026-08-26** (masters in `build/` and `configs/`):

    sr3-rtx.asi  6beeadac4395526ac5e0c88f482ffee6
    sr3-rtx.ini  25a85afe2c47c4afbcc90a492d0647b7
    rtx.conf     0872ecaa9ff251c4b34a71759cb7ca8d
    dxvk.conf    1c745b9305a5954d75994fa3408325c5
    user.conf    8b361562e85c86ab88ab2fe61495b335

    ffp=1  convertSkinned=1  hiddenPassMode=2  forceOcclusionVisible=1  skinRigidSingleBone=1
    skinRingMB=24  skinRequireBoneDecl=1  generateCloth=1  clothAlbedoPercent=200
    dedupSkinned=0  rejectStaleBones=0  clampBonesToUpload=0  paletteSetupScope=0  vehicleBonesOff=0
    rtx.useVertexCapture = False
    rtx.geometryAssetHashRuleString = indices,texcoords,geometrydescriptor
    d3d9.maxEnabledLights = 64

**Hash-verify these before the user runs.** `skinRigidSingleBone` must stay 1 - at 0 every vehicle
body/glass/panel draw is refused and the cars do not render at all.

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

### OPEN

**`rtx.useVertexCapture = False` is now IN PLACE and working** - the blocker that dominated
sessions 18-21 is solved (see "Capture-off: how it was unblocked"). What remains:

1. **Car parts and glass drift - THE open bug.** A knocked-off bumper or car window is rendered in
   the wrong place and moves in rhythm with player or NPC animation, varying with what is on
   screen. The mesh is correct and undeformed; the game's own physics position does **not** move.
   **It requires our .asi** - absent in the vanilla game AND absent with the shim disabled while
   Remix runs. Five fixes attempted and reverted; our arithmetic is verified correct against the
   device (649,925 constant comparisons, 0 mismatches) and against the disassembly. Full account in
   the worklog under "The car-part drift". Current suspect: the CPU-skinning ring buffer.
2. **NPC heads are wrong** - same shape of defect (right mesh, wrong owner), likely the same cause.
3. **The UI.** Shader-drawn, never converted, so the HUD is absent with capture off. `Disp::Hide`
   cannot fix it - it sets `D3DTS_PROJECTION`, which **Remix never reads for a shader-driven
   draw**, and with capture off the draw is skipped before any UI classification. The fix is named
   in the code: convert HUD quads to fixed function so `orthographicIsUI` fires.
4. **The sky.** ~135 draws a frame (not the 50 recorded earlier), deliberately passed through, so
   absent with capture off. Converting it needs care: a 343-vertex dome one unit from the camera.
5. **Hair renders white.** `Hair_Spec_Color1/2` never captured - the probe's slots fill first.
6. **The player's clothing colour** - a different recipe needing TEXCOORD1 reconciled with TEXCOORD0.
7. **~12 draws a frame still have unreadable texcoords** - DYNAMIC source buffers, needs a per-draw ring.

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

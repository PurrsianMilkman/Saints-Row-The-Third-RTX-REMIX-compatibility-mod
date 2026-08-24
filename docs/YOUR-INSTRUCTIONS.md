# Start here

*Rewritten 2026-08-17 as a full handoff. Everything needed to resume is on disk.*

## Resuming

Start Claude Code in `D:\SR3RTXREMIXCOMP` and say:

> Continue the SR3 RTX Remix project. Read docs/YOUR-INSTRUCTIONS.md, docs/sr2-fork.md and the
> last two sessions of docs/worklog.md before doing anything.

| File | What it holds |
|---|---|
| **this file** | Current state, what works, dead ends, next step. **Read "The hiding problem" first** - it corrects a conclusion that stood for three days and cost nine runs |
| `docs/sr2-fork.md` | The design being implemented and what is still to port |
| `docs/worklog.md` | Session history, run by run. **Runs 55-67 are the current front** |
| `docs/engine-map.md` | The exe's D3D9 call sites (unused by the shim now, still valid) |
| `re/shader_constants.csv` | 52,990 named constants across 7,276 shaders - query it, don't re-derive |
| `re/shaders/` | 1,693 `.fxo_pc` files. `tools/fxo_disasm.py <file> <index>` disassembles them |
| `docs/evidence/` | Every run's log, the frame dump, captures, the pre-fork source |
| **`<game>/rtx-remix/logs/remix-dxvk.log`** | **Remix's own log. READ IT FIRST on any "Remix does not show X" question** - it names what it refused and why. It is what found the texcoord format bug after three correct shim-side disproofs |
| `<game>/.trex/d3d9.dll` | Remix's renderer. `retools.search strings` on it is a searchable manual: every option name, its documentation, and the full `VK_FORMAT_*` table in enum order |
| `docs/asset-replacement-plan.md` | **The plan for Remastered asset replacement** - the two substitution points, what is already unblocked, and the two cheap experiments that decide the rest |
| `docs/vibe-re-tools.md` | The Vibe-RE toolkit in `tools/vibe-re/` - static/dynamic RE, D3D9 tracer, and a `dx9-ffp-port` skill describing this exact task |
| `src/sr3-rtx/` | The shim + `build.ps1` |
| `configs/` | Versioned `sr3-rtx.ini` and `rtx.conf` |

Backup of the whole project (minus the 11G game dir): `D:\SR3RTXREMIXCOMP-backup-2026-08-16`.

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

**Everything below now hangs off one decision: `rtx.useVertexCapture` must be turned OFF** (see
"The hiding problem"), and three things block that.

1. **Stability - the blocker.** With capture off, objects enter the view frustum and are dropped
   immediately. Cause unmeasured. Ruled out: `antiCulling.object` (verified parsed, no change),
   `rtx.enableCulling` (front/back-face only), de-instancing converted draws (no change, reverted).
   The run-67 probe measures the leading suspect: converted instanced draws take their world matrix
   from a **snooped copy** of the game's instance buffer, refreshed at **0.7 writes a frame against
   ~377 instanced draws**, and nothing has ever checked whether the bytes read were actually
   written.
2. **The UI.** Shader-drawn, never converted, so the HUD vanishes with capture off. Already
   imperfect with capture on - lingering UI planes mid-air at some angles.
3. **The sky.** 50 draws a frame, the only pass-through population. Converting it needs care: a
   343-vertex dome one unit from the camera, and treating it as ordinary lit geometry is what
   produced the black sky in session 14.

Also open, and independent of the above:

4. **Hair renders white.** Neither of its textures holds the colour - the `Dob_Map` is white strands
   on a green field and the `Diffuse_Map` is smooth directional data - and every character colour
   constant measures (1,1,1). `Hair_Spec_Color1/2` are the remaining candidates and have never been
   captured, because the probe's twelve slots fill with other materials first.
5. **The player's clothing colour.** A different recipe from the NPC one:
   `albedo = Diffuse_Map * Diffuse_Color * layer(pattern)` with the pattern on a SECOND texture
   coordinate set. Folding them into one texture needs TEXCOORD1 to be a fixed transform of
   TEXCOORD0; the probe that measures this has never fired.
6. **~11 draws a frame still have unreadable texcoords** - their source vertex buffer is DYNAMIC,
   so the per-buffer UV conversion cannot cache them. Needs a per-draw ring.
7. **Windshield and door glass move with the camera.** Untouched.

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

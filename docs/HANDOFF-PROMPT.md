<!-- CONTEXT-GUARD:RESUME-BEGIN -->
# RESUME NOTE (context guard, rewritten 2026-09-22 19:30) - read this first
Full detail for this session is in docs/worklog.md under "SESSION 2026-09-21/22". Do not re-derive it.

## Deployed right now - all hash-verified
    Saints Row 3/sr3-rtx.asi      18d0e9da21b5d3efac8a87276f443f3b   680,960 bytes
    Saints Row 3/sr3-rtx.ini      8b6f1963a12b645bd588ab3e835f95a3   154 keys   (UNRUN tuning, see (b)1)
    Saints Row 3/sr3-rtx.map      795ff6ce8791fa9334ea592df9f26d95
    Saints Row 3/.trex/d3d9.dll   fc092dea17cee5759fe7c376354679a2   190,476,288 = OUR FORK BUILD 4
      shipped backup beside it:   d3d9.dll.shipped-2026-06-04  f4a02738e096c57e897e5ba0af854628
    Saints Row 3/rtx.conf         961908874723560340690287f471e2a1
      = original 81249e23 + `rtx.profiler.memory.enable = True` + `rtx.useBuffersDirectly = False`
        + `rtx.enableIndexBufferMemoization = False`. Backups: rtx.conf.before-memory-profiler (81249e23),
        rtx.conf.before-usebuffersdirectly (9e476baa). The REPO configs/rtx.conf (88a5ecf4) is NOT live.
    Fork patches, each its own diff in C:\remix-fork\: staging-release.diff (memory leak),
      skinning-normal-format.diff (normals), identity-pinning.diff (dormant),
      build-environment-workarounds.diff. Build: `. .\build_common.ps1` then
      `meson compile -C _Comp64Release` in C:\remix-fork\dxvk-remix, then COPY
      _Comp64Release\src\d3d9\{d3d9.dll,.exp,.lib,.pdb} to _output\ (meson install is blocked).
    newest source backup: src/sr3-rtx/sr3rtx.cpp.before-bake-keys
    Shim build: powershell -NoProfile -ExecutionPolicy Bypass -File src/sr3-rtx/build.ps1
    Deploy: copy build/sr3-rtx.asi + .map + configs/sr3-rtx.ini into "Saints Row 3/", then md5-verify.
    THE GAME AND NvRemixBridge.exe MUST BE CLOSED FIRST - locked-file copies fail silently and cost three
    wasted test runs. Write the ini/source with PYTHON only: PowerShell Set-Content adds a BOM.

## (a) FIXED AND CONFIRMED BY THE USER THIS SESSION
- The overlay (raster frame replacing the world, path tracing stopping): `mainCameraScreenSizedOnly=1`.
- Stretched bracelet/glasses lens: `skinFFRestageBone0=1`.
- Car windows/parts floating with player and NPC animation: `skinFFFollowShaderInfluences=1`.
- Rigid attachments on the GPU: `skinFFRigidDecl=1` (declarations 8 made / 0 failed, was 3/7).
- The 25 GB leak and its ~8 minute crash: our fork's staging-release fix + `enableIndexBufferMemoization
  = False`. 428 x 32 MB blocks / 13.7 GB -> 5 blocks / 160 MB; host RAM 13.82 -> 0.60 GB.
- Hard/faceted shading on every GPU-skinned character: our fork's skinning normal-format fix.
- Performance: frame 28.7 ms / 35 fps (was 43.5 / 23), WORST FRAME 44 ms (was 1201), skin 8.22 ms (was
  15), morph refusals 0.1/frame (was 28.7), 0 bake rebuilds (was 7,699). User: "about two times".

## (b) IN FLIGHT / NEXT, in order
1. **UNRUN tuning already deployed:** `morphBakeMaxMB` 24 -> 64 (the bake was at its cap: 3,451 built vs
   87 live, 3,364 evicted) and `morphProbe=0`, `assetHashProbe=0` (both had answered; ~2.3 ms/frame).
   Next run should show far fewer evictions and a lower skin/probes time. No user check needed beyond fps.
2. **MISPLACED BUILDINGS - unattributed, the top open correctness bug.** Rare, sticks a few seconds,
   "slightly angle and location dependent", seen while flying. Shim-side captures CANNOT see it (the
   shim's own data is correct; frame-4 analysis found only legitimate at-origin draws - 1,065 instanced
   plus 64 world-space-shader draws - and no object moving in the 60-frame ring record).
   THE CONTROL RUN that settles it: restore the SHIPPED runtime (copy .trex\d3d9.dll.shipped-2026-06-04
   over .trex\d3d9.dll) AND remove the `rtx.enableIndexBufferMemoization = False` line from rtx.conf,
   then fly the same route ~5 min, accepting that the leak and its crash come back for that run.
   Still misplaced there -> our changes are not the cause. Gone -> the staging release is the suspect.
3. **Character colours: body skin and head do not match.** Parked by the user earlier, explicitly to come
   back to. Probably the most visible remaining fault now that shading is fixed.
4. **Decal flicker in Remix's Geometry Hash view.** PROVEN not to be a hash change (see worklog 8). Ask
   whether the whole decal changes colour at once or speckles between two colours, which separates a
   coplanar decal/wall fight from anything else. 91 hashes are already in rtx.decalTextures.
5. **Restore Remix's index memoization properly:** give each memoized index copy its own dedicated
   host-visible TRANSFER_SRC buffer (like allocVertexCaptureBuffer) instead of carving it from
   RtxStagingDataAlloc, then set `rtx.enableIndexBufferMemoization` back to True.
6. **Report both Remix bugs upstream** (both are in NVIDIA's shipped build AND in origin/main
   HEAD 471db69f): the unreleased staging acquire, and the skinning kernel's float-only normal read.
7. **Stutter work, not started:** first-time per-buffer conversions (uv decode, bind-pose decode, skin
   side buffers, index scans) run on the game's render thread when content streams in. Worst shim spike
   was 219 ms. Move them to a worker thread. (The 1201 ms frame was the F9 ring write, not this.)
8. **Known latent bug, reported not fixed:** GameCapturer::captureMeshNormals (rtx_game_capturer.cpp:674)
   reads raw skinned normals as floats - USD captures of skinned meshes export garbage normals.

## (c) KEY FACTS worth keeping (numbers; the rest is in the worklog)
- Remix's "Geometry Hash" debug view = the ASSET hash = indices + texcoords + geometrydescriptor. Remix
  derives the vertex range from the INDICES and rebases them, and hashes texcoords only at referenced
  vertices, so the shim's window/tightening does NOT affect it.
- The morph delta is repacked, never edited in place (8,735 of 8,735 changes followed a DISCARD); a
  character's block can stay byte-identical for ~10,000 frames. That is why the content-hash key works.
- Bridge client AddRef/Release are LOCAL atomics; the returned count is the public count; device bindings
  do NOT count. GetDesc/GetLevelDesc are answered locally (sendReadOnlyCalls=false), so old "GetDesc is a
  round trip" comments in the shim are stale.
- Remix clears its scene at the menu, but DXVK keeps empty chunks (high-water) and only frees them when
  the Remix menu (Alt+X) is opened/closed - Task Manager lags any memory fix until then.
- Memory profiler: `rtx.profiler.memory.enable = True`, then Alt+X -> Development -> Memory Profiler ->
  Sample Memory -> "Write to Log"; parse with scratchpad/memparse2.py.
- Useful current counters: `MORPH BAKE`, `MORPH PROBE`, `GPU SKIN`, `GPU SKIN refused`, `ORPHAN SWEEP`,
  `LIVE:`, `STREAM-0 OFFSET`, `PROFILE ms/frame`, `TIMING: frame`, and ` bake=`/` ffskin ` in the F9 dump.

## (d) RULED OUT - do not re-test
coplanar z-fight as the decal-DISTORTION cause; draws blinking; albedo churn; UV scale error;
Decal_Map_Offset; rtx.decalTextures as a fix; opacity micromaps; sampler address mode; the FIX A stripe
route; dedup dropping decals; Disp::Hide via screenSpaceMode; the out-of-window theory for decals; the
early-injection theory for the overlay (0 of 2,999 frames); identity pinning as a fix for
skinning/overlay/decals; the shim's tightening affecting Remix's hash; the stream-0 offset as the
bracelet cause (0.0/frame) or the decal cause (0.1/frame); the Remix API as the memory leak
(remixApiCharacter=0, CreateLight never called); shim Get* calls leaking references (all Release);
the morph bake as the hard-shading cause (it persisted with morphBake=0); rigid draws as the
hard-shading cause (persisted with skinFFRigidDecl=0).

## (e) PENDING FROM THE USER / RULES
- Nothing is waiting on the user right now except choosing the next item from (b): the lead recommended
  the character colours (b3), with the misplaced buildings (b2) as the alternative.
- RULES: Fable 5.1 plans, Sonnet 5 codes, Opus 5 takes over a task Sonnet fails, ask before Fable on the
  biggest coding jobs, disassembly is Fable's. `D:\Project Crreish\TEAM A` is READ ONLY. Every
  behavioural change gets an ini switch. Patch scripts must be exact-match and ABORT unless the anchor
  matches once; an anchor spanning kept code must repeat it verbatim; after patching diff g_settings
  use-counts against the backup. Never commit game assets. Fork work only under C:\remix-fork.
<!-- CONTEXT-GUARD:RESUME-END -->

# RESUME PROMPT - Saints Row 3 RTX Remix

Continue the SR3 RTX Remix project at `D:\SR3RTXREMIXCOMP`.

## THE ONE THING TO DO FIRST

**2026-09-21 RUN RESULT - THE OVERLAY IS A LOST CAMERA, NOT AN EARLY TRIGGER. This supersedes the
injection-trigger theory below.** The user: "some of the ui is fixed. but the world gets replaced by
the raster render overlaying it and while the overlay is happening, the path tracing STOPS."

    INJECT PROBE/CONTROL: trigger preceded the final composite 0.000%, no shim trigger 100.000%
    remix-dxvk.log: [RTX-Compatibility-Info] Trying to raytrace but not detecting a valid camera.
    remix-dxvk.log: [RTX] CameraManager: FOV of a camera changed between frames

No shim draw EVER fired Remix's injection early (2,999 frames). The early-trigger mechanism is real
in the source but is NOT what the user sees. On the bad frames Remix has NO VALID MAIN CAMERA, so it
renders no path-traced image at all and the game's raster frame is what remains. NEXT: find why the
camera the shim publishes (ApplyTransforms, D3DTS_VIEW/PROJECTION on converted draws) is rejected
at some angles: read dxvk-remix's CameraManager (source at C:\remix-fork\dxvk-remix) for what makes
a Main camera valid, and diff the cam()/proj() fields of a bad frame dump against a good one. Keep
injectControl=1 for now: the user reports some UI got better with it.

**GPU SKINNING WORKS PARTLY:** 14.9 draws/frame hardware-skinned (43,683 verts/frame). Refused:
12.8-15.3/frame morph delta (deliberate in v1), 12.6-14.8/frame DECLARATION CLONE FAILED
("declarations: 3 made, 5 failed" - a real bug to fix), 0.3/frame cloth remap. Whether characters
LOOK right with it has not yet been reported by the user: ASK.


**Two things are in flight. Read both before acting.**

    Saints Row 3/sr3-rtx.asi  3f1a8f309bb9fac7dd54eb126bf1944a   680,448 bytes  (+ bake delta-hash per-frame memo; morphBake=0, morphProbe=1; UNRUN)  DEPLOYED, UNRUN
    Saints Row 3/sr3-rtx.ini  79cfd9df783bdb93ca9ec44867b344c6   154 keys (morphBake=1 SET 2026-09-22, UNRUN)
    Saints Row 3/rtx.conf     81249e23c4a929ea89d0b27f39b9d4e9   still UNCHANGED
    newest source backup: src/sr3-rtx/sr3rtx.cpp.before-inject-control

### 1. THE NEXT RUN tests three things at once, all visually separable

  - `skinViaFixedFunction=1`: GPU SKINNING. Read the startup MaxStreams line (if the weight stream
    COLLIDES with the uv stream the feature cannot run at all), then the GPU SKIN line (0 draws means
    the refusal breakdown beside it names the excluding test), THEN look at the characters. Research
    confirmed against Remix's source that our side stream matches its requirements exactly: FLOAT3
    weights, UBYTE4 indices, usage index 0, INDEXEDVERTEXBLENDENABLE, D3DVBF_3WEIGHTS, palette in
    WORLDMATRIX(0..255); Remix hashes BEFORE skinning (d3d9_rtx.cpp:680 vs processSkinning at 684).
  - `injectProbe=1 injectControl=1`: THE OVERLAY FIX. Read the INJECT PROBE/CONTROL line: the
    percentage of frames where the trigger 'followed' the final composite should approach 100.
  - `blockEngineOutputToScreen=1`: the earlier invariant, still on. It CAUGHT the right draws
    (Base_sampler + Bloom_stage_0 + Lut_sampler_2d, the final tonemap, 2560x1440 into the back
    buffer) but passed them through raw, and the overlay still happened: see below for why.

### THE OVERLAY, EXPLAINED FROM REMIX'S OWN SOURCE (tag remix-1.5.2, 2026-09-21)

The standing premise "a pass-through shader draw is invisible because Remix refuses it" is only
HALF true. `internalPrepareDraw` opens with

    if (m_rtxInjectTriggered) { return skipDrawCallsPostRTXInjection() ? Ignore
                                                                       : PreserveDrawCallAndItsState; }

(d3d9_rtx.cpp:572-576). BEFORE the injection trigger a shader draw is genuinely ignored. AFTER it,
EVERY draw in the frame is rasterized raw on top of the path-traced image, until EndFrame. The
trigger (isRenderingUI, d3d9_rtx.cpp:555-568) fires on the first FIXED-FUNCTION draw with
PROJECTION._44 == 1 and depth write off on a back-buffer-sized target. Shader draws are Ignored
before that test (425-427), so **only the shim's own draws can fire it.** At some camera angles one
of ours fired BEFORE the game's final composite, and the composite then painted the raster frame over
the path tracing. This also explains the recorded dead end "skipping the composite freezes the image":
if the trigger fires while the screen-sized HDR target is bound, Remix blits INTO that target and the
composite carries it to the back buffer. It is conditional on an early trigger, not absolute.

The fix is the GTA IV mod's own technique on STOCK Remix (gta4-rtx renderer.cpp:3141-3196,
manually_trigger_remix_injection): suppress early triggers from pretransformed shim draws by setting
PROJECTION._44 off 1.0 for that draw alone (RHW ignores transforms, so nothing visible changes), and
after the final composite draw an invisible 0.01-pixel fixed-function quad on the back buffer to fire
the trigger at the right moment. A free diagnostic if it ever needs confirming:
`rtx.skipDrawCallsPostRTXInjection = True` (overlay AND HUD vanish = early trigger).

### 2. THE REMIX FORK FOR IDENTITY PINNING - the user chose "build it now, in parallel"

**BUILT 2026-09-21, SUCCESSFULLY, AND DELIBERATELY NOT DEPLOYED.** No Vulkan SDK was needed (headers
are a submodule, vulkan-1.lib is committed to the repo). 34-line patch in 3 files:
`C:\remix-fork\identity-pinning.diff`. Two build-environment fixes kept separate in
`build-environment-workarounds.diff` (vcvarsall needed a full path because this machine sets
NoDefaultCurrentDirectoryInExePath=1; one unrelated test app's werror scoped off). Built with
VS2022/MSVC 14.44, not the README's tested VS2019. Packman's shared cache is `C:\packman-repo` (1.7 GB).

    runtime  C:\remix-fork\dxvk-remix\_output\d3d9.dll        190,472,192  aa45d02acd7d3886df20b10bf80ef268
             + 38 companions (usd, NRD, NRC, DLSS, Reflex, XeSS, rtxio)
    bridge   ...\bridge\_output\d3d9.dll                          868,352  a5dee49d115eb3a14ab72af64706c380  (x86 client)
             ...\bridge\_output\NvRemixLauncher32.exe             138,240  492bcfaea5fceeaf000e443df994971f
             ...\bridge\_output\.trex\NvRemixBridge.exe         1,114,624  6d5db58122cf503d21df469b8b80eb29  (x64 server)

**DEPLOY ORDER, one variable per run:** (1) the pending run on STOCK Remix (GPU skinning + overlay);
(2) back up `Saints Row 3\.trex\` and the game-dir bridge files, swap in the self-built runtime AND
bridge together with NO shim change, and confirm parity with stock; (3) only then write the shim side
(RS 42 DecalStatic per draw by shader family, RS 150 for the character atlas).

Originally built by an agent under `C:\remix-fork\` from tag `remix-1.5.2` (the shipped
`remix-1.5.2+68edea01` is NOT a public commit, so the tag is the closest match). **NOT DEPLOYED, and
must not be deployed without backing up `Saints Row 3\.trex\` first.** The patch, saved as
`C:\remix-fork\identity-pinning.diff`, ports ONLY RS 150 / RS 42 / RS 220 from
`xoxor4d/dxvk-remix` branch `game/gta4_atmos10_nr2` (the branch gta4-rtx actually ships). The bridge
now lives inside dxvk-remix (`bridge/`) and is built from the same tag so the halves pair.

**What pinning actually is, SOURCED, and what it will not fix** (told to the user before they chose):

    RS 150  overrides the LEGACY MATERIAL hash only (rtx_materials.h updateCachedHash), a 32-bit
            value widened to 64. NOT the geometry hash, NOT the texture hash.
    RS 220  SPLITS an identity by a seed (the opposite of pinning); wins over RS 150.
    RS 42   seeds InstanceCategories per draw (DecalStatic, Sky, Particle, Ignore...).
    It does NOT fix: skinning churn (GPU skinning does), the overlay (injection order does), or the
    decal layers. What it buys: per-draw DecalStatic tagging by SHADER FAMILY instead of a hash list,
    a character-atlas identity that survives restarts (render targets are hashed from a GLOBAL
    CREATION COUNTER, d3d9_common_texture.cpp:347-353), and identity splitting.
    Shim side (NOT yet written): SetRenderState(150, hash) / (42, bits) per draw, save and restore.

### 3. THE BUILDINGS - two mechanisms, and one earlier plan that WOULD HAVE FAILED

  - **A second texture stage does NOT work on stock Remix.** `colorTextures[1]` becomes only
    `OpaqueMaterialData::secondaryTexture`, which the path tracer reads ONLY FOR THE EYE IRIS
    (opaque_surface_material_interaction.slangh ~498). So the decal layer of `ir_bbstandard` (211
    draws/frame drawing only their base texture) must be a SEPARATE BLENDED DRAW: same geometry,
    decal texture on stage 0, stage 0 TEXCOORDINDEX set to the decal's set (Remix uploads only
    textureStages[firstStage][TEXCOORDINDEX]), SRCALPHA/INVSRCALPHA, zwrite off.
  - **ir_sr3fauxinterior, the striped windows** (INFERRED, never yet measured): its "diffuse" is an
    INTERIOR ATLAS the game samples through a view-dependent offset clamped to one cell
    (ClampU/ClampV/Room_Depth); we sample it flat across the whole facade, so the atlas tiles
    endlessly. It is the ONLY shader with Room_Depth, ClampU and ClampV, so target a probe on that,
    and **do NOT reject instanced draws at the origin** (the last probe threw 16 of 24 away that way).
  - The graffiti decal-only draws at Angel's Gym (-220.18 14.20 143.66) were finally MEASURED and are
    PERFECT on input: correct DXT5 image, real alpha 0..255, 1/1024 scale, exactly one repeat.

### STILL WANTED FROM THE USER

  1. After the next run: do characters look right with GPU skinning, and is the overlay gone?
  2. Does the flicker persist while standing completely still? (never answered)

## HARD RULE: THE TEAM A SPECS ARE A SOURCE (2026-09-14)

`D:\Project Crreish\TEAM A` holds 30 `spec-*.md` cleanroom specifications of Saints Row: The
Third plus `HANDOFF.md` (the spec index) and `tools/` (Ghidra scripts and dumps). **Consult it
first for anything a specification could answer. READ ONLY - never write anywhere under
`D:\Project Crreish`.** Start at `spec-format-inventory.md`. Most relevant here:
`spec-vertex-format.md` (per-vertex channels; texcoords are signed 16-bit fixed point, scale
1024 - the same 1/1024 the shim's texture matrix applies), `spec-geometry-format.md`,
`spec-texture-format.md`, `spec-fxo-format.md`, `spec-lua-bindings.md`. Confidence labels there
are real: "CONFIRMED - empirical" is a fact, "inferred" is a lead. If a spec disagrees with what
the shim measures, say so rather than silently preferring one.

**The rule covers the RE ARTEFACTS too**, all verified present: `tools\ghidra_projects\SR3.gpr`
(+ `SR3.rep`) is a Ghidra project with `SaintsRowTheThird.exe` imported, analysed and carrying
~80 bookmarks on the key addresses; `tools\ghidra_12.1.3_PUBLIC\support\analyzeHeadless.bat`
is a full Ghidra 12.1.3 (drive it from PowerShell - the space in "Project Crreish" breaks Bash
quoting - and note a headless run fails if another session holds the project open);
`tools\scripts\*.java` is 378 post-scripts; `tools\*.txt` is **345 already-dumped outputs, so
grep those FIRST** before running anything; `tools\gp_*\` are per-topic Ghidra projects.
Per the model policy, anything that actually READS disassembly goes to a Fable agent.

## MODEL POLICY - A HARD RULE FROM THE USER (2026-09-14)

1. **Fable 5.1 = planning only.**
2. **Sonnet 5 = coding.** Every implementation agent starts there.
3. **Opus 5 takes over the REMAINDER of a task Sonnet fails**, not just the failed step.
4. **Ask before putting Fable on the biggest tasks.**
5. **Disassembly is Fable's, no permission needed** - machine code, PE internals, `.fxo_pc`
   shader bytecode. This is the one exception to rule 1, and it has already paid twice: a Sonnet
   lens built a whole finding on a premise ("the world path keeps the game's pixel shader") that
   the Fable disassembly disproved the same day.

Also standing: every behavioural change gets an ini switch; back the source up as
`sr3rtx.cpp.before-<what>` before editing; patch with exact-match Python scripts that ABORT
unless the anchor matches exactly once; never commit game assets.

## THE LIVE PROBLEM: DECALS RENDER WRONG

The user, with a screenshot and recorded gameplay (2026-09-14): *"the random polygons are the
decals. they are not rendering properly... they are not random. they are the decals."* The
screenshot shows flat quads over a building facade carrying a **fine repeating stripe pattern**
instead of their image, sitting where the building's windows are. This merges two symptoms
tracked separately for days: the intermittent "random stretched polygons" and the sign/logo
flicker are ONE fault.

### IN FLIGHT - two fixes, built and DEPLOYED, NOT yet run

Full derivation in `docs/worklog.md` 2026-09-14 entries (7). Source backups
`sr3rtx.cpp.before-decal-uvset`, `sr3rtx.cpp.before-decl-dup`.

**FIX A `decalUvFromDeclaredSet=1`** - the decal's coordinates come from vertex TEXCOORD**1** in
several families whose vertex shaders declare NO usage-index-0 input at all:

    ir_at_sr3decalonly_{s,bs}         VS[0]-[4]
    ir_sr3diffcol_normal_decal_{s,bs} VS[2]-[5]
    ir_at_decalonly_cuberef_*         VS[5]
    ir_sr3fauxinterior_{s,bs}         VS[0],[1]   <- FAKE WINDOW INTERIORS on facades

19 of 3,725 VS blobs read texcoord1 with no texcoord0. The shim forced `D3DTSS_TEXCOORDINDEX=0`
and `FloatUVDeclaration` retargeted only usage-index 0, so those draws were textured from
whatever index 0 held. `ReflectShader` now records `declaresTexcoord0` / `lowestDeclaredTexcoord`
from VERTEX shader input dcls only (guarded by `isVertexShader = (tokens[0]>>16)==0xFFFE`;
a pixel shader's `dcl_texcoordN` are interpolators, a different thing). `DecalUvSourceSet()`
(~8849) returns 1 when the VS declares texcoord1 and not texcoord0. `UvBufferFor` reads
`texcoord1Offset/Type`, `UvKey` gained a 6th `set` parameter, `g_uvDecls` became `[2]` indexed by
set, and the clone relabels the set-1 element AS usage-index 0. When the declaration also carries
a real TEXCOORD0 element it is displaced to the lowest free index 1-7 (counted) rather than
colliding; if none is free the clone is abandoned and the draw falls back.

**FIX B `decalAlphaFromTexture=1`** - `ir_sr3decalonly`, `ir_sr3diffcol_normal_decal` and
`ir_sr3simple_distfield_decal` declare no `Alpha_Threshold`, so `shaderCutout` (~6731) never
fired and `ALPHAARG1` was `TFACTOR` = alpha 1.0. Their pixel shaders compute
`alpha = decal.a x Decal_Map_Opacity x Tint_color.a` and the game submits them with real
SRCALPHA/INVSRCALPHA (`blend=1(5/6)` in the dumps), so a decal that should fade onto the wall was
drawn as a FILLED RECTANGLE of its texture - the user's *"a texture that is on top of the
buildings already existing texture"*, verbatim. Now `ALPHAARG1 = TEXTURE` when the albedo sampler
is Decal-named and the draw is blended BY ITS FACTORS.

**New counters to read on the next run** (report lines `SHORT2 texcoords converted` and
`albedo: moved off stage 0`):

    FIX A decal texcoord1 set: N/frame served, N/frame wanted but unavailable,
          N declarations displaced a colliding texcoord0 element
    FIX B decal alpha from texture N/frame

A non-zero "served" is the fix firing. A non-zero "wanted but unavailable" is a mesh that has no
usable set 1 - **this class used to be INVISIBLE**: `InstallFloatUV` returned false at its first
condition with nothing counted, which is why the report said "failures: 0 convert" for weeks
while these draws reached Remix with no texcoords at all
(`[rtx-interleaver] Unsupported texcoord buffer format (80)` in `rtx-remix/logs/remix-dxvk.log`).

### THE CAPTURE AT THE SPOT - OBTAINED, AND IT NARROWS THE FIX

`Saints Row 3\sr3-rtx-frame-2.log`, frame 4635, camera (-187.6 6.2 142.0), taken with both
switches OFF so it records the BROKEN state. Six draws, all at ONE position (-220.2 14.2 143.7):

    775-780 CONVERT v=28336 p=2|4 zw=0 zt=1 zf=4 bias=0 blend=1(5/6) pull=0
            vs[proj][obj] ps='Decal_MapSampler' rank=90 low='Decal_MapSampler'@0
            tex0=alb@0  uv=2  inst=0

  - TRUE decals (decal map at the LOWEST register), correctly chosen as the albedo (rank 90).
  - `blend=1(5/6)` = SRCALPHA/INVSRCALPHA: **genuinely blended**. `zw=0`: no depth write.
  - `inst=0` + `vs[proj][obj]`: placed by objTM, NOT the instance stream.
  - `uv=2` matches the disassembled `ir_sr3decalonly_*` exactly (`texld r1, v2, s0`), and that
    family builds interpolator 2 from **vertex TEXCOORD0** - the set the shim already converts.

**So FIX A does NOT apply to these draws.** It stays in for `ir_at_sr3decalonly`,
`ir_sr3diffcol_normal_decal` and `ir_sr3fauxinterior`, none of which appear in this frame.
**FIX B applies to exactly these six**: in that frame 20 converted draws are genuinely blended
and 6 of them have a Decal-named albedo. With the switch off, `cutout` is false (no
`Alpha_Threshold`) so `ALPHAARG1 = TFACTOR` = alpha 1.0, and under 5/6 that is `1*src + 0*dst`,
i.e. FULLY OPAQUE - a decal that should fade onto the wall covers it. The 3% pull test moved this
same population (`zw=0` satisfied the old noWrite rule), which is why the user saw the texture
"slightly spaced from the wall".

**Prediction to test:** with `decalAlphaFromTexture=1` those six stop being opaque rectangles.
If they still look wrong, the next instrument is dumping the decal texture together with the
min/max of the converted coordinates for one of those six draws.

### STILL WANTED FROM THE USER

1. Whether the flicker persists while standing **completely still** (separates temporal from
   camera-driven).
2. Whether anything on a wall looks **worse** since `rtx.decalTextures` went in - if so, a sign
   OBJECT is in that list and must come out.

### DEFERRED, recorded so they are not lost

  - **distfield colour.** `ir_sr3simple_distfield_decal_s [2]`: `smoothstep(0.45,0.55,alpha)` for
    coverage, RGB entirely from the constant `Decal_Map_Color`; the texture's own RGB is NEVER
    read. Converted with no alpha test, signage renders as an opaque rectangle of a channel the
    game never displays. Fix: `COLORARG1 = TFACTOR = Decal_Map_Color` plus a 0.5 alpha test.
  - **The second decal layer on Diffuse+Decal walls** (`ir_bbstandard`, `ir_bbsimple2_decal`):
    the diffuse wins the albedo NAME rank (100 vs 90) so the decal layer is never bound at all.
    Needs a second texture stage on texcoord set 1 with its own `D3DTS_TEXTURE1` matrix and
    `BLENDTEXTUREALPHA`.
  - Dropped colour multipliers `Decal_Color x Object_instance_params`, `Diffuse_Color`,
    `Tint_color`, `Self_Illumination`.
  - `Decal_Map_TilingU_2` shares the base name `Decal_Map` and overwrites the primary pair's
    register in `ReflectShader` (~1420-1428). Cars only (`ir_sr2carpaint1_*`).

## RULED OUT FOR THIS SYMPTOM - do not rebuild any of it

    coplanar depth tie ....... displaced 3% of distance, VISIBLY separated, still wrong.
                               That run also PROVED our world matrix reaches Remix's geometry.
    draws blinking ........... 60-frame record, 3 identity keys: 648 identities, decal ones
                               20/24 present in ALL 60 frames, 4 single-block, 0 scattered.
                               The only alternating identities in the record are particle
                               billboards. deferFreshStaticDraws fired ZERO times in the window.
    albedo texture churn ..... 0 draws/frame over 245 tracked identities. The 10 ALBEDO CHURN
                               lines that did fire are LOD/streaming swaps at two fixed
                               positions on large meshes (v=28708, v=7121).
    uv bind offset / ring .... provably 0 for static sources (UvKey uses first=0,count=0 so one
                               entry covers the whole buffer; the only non-zero write is in the
                               dynamic-ring branch). 0 conversion failures all session.
    undefined interpolators .. MOOT: the world path does NOT keep the game's pixel shader.
    missing alpha cutout ..... already reproduced as a real alpha test since 2026-08-19.
    a UV SCALE error ......... none exists. All six decal families compute
                               `uv = vertexTexcoord x Decal_Map_TilingU/V x 1/1024`, no offset,
                               no matrix, and `TilingForAlbedo` resolves all six CORRECTLY.
                               "no pair for it 260/frame" is mostly correct behaviour.
    Decal_Map_Offset* ........ not a translation: a Time-driven flipbook CELL INDEX, and only in
                               ir_sr3megatv_*, ir_sr3television_*, ir_sr3vr_pixel_*.
    rtx.decalTextures ........ 91 hashes applied, no change. Remix only sets isDecal when the
                               draw genuinely blends, and it counts ONE/ZERO as opaque.
    opacity micromaps ........ turned OFF as a test (see rtx.conf below); result NOT yet reported.

## FACTS WORTH KEEPING

**`ShaderInfo::firstSampler` is ALPHABETICAL, not the lowest register.** D3DX writes the CTAB in
name order, so "Decal_MapSampler" sorts before "Diffuse_MapSampler". 400 pixel shaders report a
decal map first; only 126 have one at their lowest register. Use `lowestSampler` /
`lowestSamplerReg` (added 2026-09-14, switch `decalByLowestSampler=1`, both tests counted).

**The world path renders with FIXED-FUNCTION PIXEL processing too.** `BeginFFP` nulls the pixel
shader (`g_origSetPixelShader(dev, nullptr)`) and `SetupTextureStages` (~6700-6790) builds the
stage state: ONE texture chosen by `AlbedoRank` NAME scoring, stage 0, `COLOROP=SELECTARG1`,
`TEXCOORDINDEX=0`, stages 1-7 DISABLED, a texture matrix of `1/1024 x tiling`. "FF vertex + the
game's own pixel shader" is true ONLY of the HUD user-pointer path (`uiKeepPixelShader`) and the
atlas composite.

**Remix's texture hash is CRACKED and validated:** `XXH3_64bits(packed mip 0, native byte order)`,
seed 0, no header, no other mips. NOT XXH64 (the DLL calls that "obsolete"). Validated three
ways: 1,661 of 1,669 hash-named files in `Saints Row 3/rtx-remix/captures/textures/` reproduce
their own filename; 1,238 capture hashes matched to packfile textures by content; 589 of 718
shim-dumped DDS files hash to capture names. RGBA8 captures need R and B swapped first (the
capture writer swizzled). rtx.conf prints them as signed hex. Scratchpad artefacts:
`texture_hashes.csv` (76,650 rows), `capture_names.txt`, `rtx_decalTextures_candidate.txt`,
`build_hash_table.py`. **This makes every rtx.conf texture list generatable instead of
hand-tagged.**

**The UI floating in the world was a missing brace** (2026-09-11): after a HUD draw was rebuilt
as an XYZRHW quad, the game's raw draw still went out, turning float4 PIXEL positions into world
coordinates a thousand units up. `uiRawAfterHud=0`. Confirmed fixed by the user.

**The occlusion culler is working** (2026-09-10): 1 verdict flip in 7,590 camera-still jobs,
0 dropped jobs, ~54% of boxes occluded, ~140 objects removed a frame. `forceOcclusionVisible`
must stay 1 with capture off.

## Deployed / to deploy

    Saints Row 3/rtx.conf   81249e23c4a929ea89d0b27f39b9d4e9
        rtx.useVertexCapture = False
        rtx.decalTextures = 91 entries (2026-09-14; backup rtx.conf.before-decaltextures)
        rtx.opacityMicromap.enable = False   <- TEST (backup rtx.conf.before-omm-off)
        rtx.geometryAssetHashRuleString = indices,texcoords,geometrydescriptor  (non-default;
            positions REMOVED. Used only for SKY DETECTION per the DLL's own description, and
            excluding positions is what keeps SKINNED meshes stable - do not "fix" casually.)

    ffp=1  hiddenPassMode=2  screenSpaceMode=0  forceOcclusionVisible=1  (NEVER 0 with capture off)
    skipOcclusionProxies=1  skipDeclinedPassThrough=1  dietVsConstants=1  dietVsBinds=1
    skinFast=1  skinThreads=1  scanCommandBlocks=0
    occlusionCull=1  occlusionDryRun=0  occlusionMeshBudget=16  occlusionWidth=320
    occlusionMinDistance=5  occlusionKeyByObject=1  occlusionSelfToleranceCm=50
    occlusionExplainFlips=40  indexedInvalidation=1  occlusionRangeInvalidation=1
    occlusionStickyOccluders=1  (occlusionMaxOccluders=2000  occlusionMaxTris=250000)
    probeIndexWindow=1  deferFreshStaticDraws=1  deferFreshFrames=1  ringFrames=60
    snoopThreadScopedInternal=1  crashTestDumpAtStart=0   <- the crash fix and its self-test
    tightenConvertedWindow=1   <- THE DECAL FIX UNDER TEST. Code default is deliberately 0
        (safe) while the ini runs the experiment at 1; do not "sync" that one away.
    frameStepPauseKey=118 (F7)  frameStepKey=119 (F8)  frameStepDumpEachStep=1  captureKey F9
    decalOffsetPermille=0  layerOffsetPermille=0  (ALL geometry pulls OFF - the tie is disproven)
    layerOffset=1  biasOffset=1  decalPrimaryOffset=1  blendedOffset=1  decalByLowestSampler=1
    albedoChurnProbe=1  decalUvFromDeclaredSet=1  decalAlphaFromTexture=1   <- the two new fixes
    remixApi=1  remixApiCharacter=0  compositeFfp=1  clothUniformFromDiffuse=1
    hairStrandsFromDob=1  clothDominantPattern=1  clothPadUnused=1  clothCutout=1
    clothMeshDecal=1  clothDecalTiles=1  clothColourCurve=1
    uiDemoteUP=1  uiConvertUP=1  uiKeepPixelShader=1  uiRawAfterHud=0  generateCloth=1

## Read first, in this order

1. This file's top four sections.
2. `docs/worklog.md` - the 2026-09-14 entries, newest last, especially (6) and (7).
3. `docs/YOUR-INSTRUCTIONS.md` - **THE CLOTH COLOUR FORMULA** (confirmed on every garment; do not
   change colour output without keeping them all right), then **STATE, 2026-09-07**.
4. `docs/cloth-uv-map.md` - which UV set feeds which sampler, per shader. Its headline -
   *"Albedo is TEXCOORD0, pattern is TEXCOORD1 is NOT a rule of this engine"* - is the same
   lesson FIX A is built on.

## Where the project stands otherwise

The game's OWN character renders with correct skin, correct customisation colours on EVERY
garment of the 2026-09-09 outfit (silhouettes, decals, the star on the left cup only) and hair
strand detail, with `rtx.useVertexCapture = False`. The Remix API character is OFF. The technique
that unlocked it: Remix refuses a draw for its VERTEX shader, not its pixel shader.

Open behind the decals: the in-game HUD's visibility now that it is drawn once; the sky; other
outfits' cloth; frame rate 23-42 fps in the city with the shim at 9-19 ms, where skinning is the
biggest single cost (11-25 ms at 110k-355k skinned verts/frame, SIMD is the lever) and ~12k state
calls a frame still cross the bridge (a lazy layer needs a BRIDGE-side shadow).

## How to work here

- **Look at the thing - and check the tool that shows it.** The DDS writer fabricated alpha 255
  for a day. Convert to PNG, view it, and know which channels the picture actually carries.
- **`ShaderInfo::firstSampler` is ALPHABETICAL** - see FACTS above. A whole day of "decal"
  classification was really tagging walls.
- **A guard that refuses silently is a guard that hides a class of bug.** `InstallFloatUV`
  returned false with nothing counted and the report read "failures: 0" for weeks.
- **Measure the RESULT, not the process.** Print numbers, not verdicts.
- **Read the refusal.** Every guard that refuses a fix says why, and the reason has been the next
  fix every time.
- Build: `powershell -NoProfile -ExecutionPolicy Bypass -File "src/sr3-rtx/build.ps1"`. Deploy by
  copying `build/sr3-rtx.asi` and `configs/sr3-rtx.ini` into `Saints Row 3/`, hash-verify both.
  **The ASI is LOCKED while the game runs** - check for the process before building expectations.
- Frame stepping: F7 freezes with the frame on screen, F8 steps one frame (a full dump per step),
  F9 writes the last 60 frames to `sr3-rtx-ring-N.log` plus a frame dump and a shape probe.
- Per-draw dump fields: index, disposition, `v=` vertices, `p=` primitives,
  `zw/zt/zf/bias/blend(src/dst)/cw`, `layer=`, `pull=`, vs flags, `ps=` (ALPHABETICAL first
  sampler), `rank=`, `low='<name>'@<reg>` (lowest register), `tex0=`, `alb=<ptr>@<stage>` (what
  was ACTUALLY rendered), `uv=<N>` (the PS INTERPOLATOR index, **not** the vertex set - it is
  1-5 on most correctly-rendering draws, so it is not a bug indicator), `inst=`, `at(objTM)`,
  `cam(...)`, `proj(...)`, and the reason text after the last `|`.
- Asset tools: `tools/vpp.py`, `tools/peg.py`, `tools/cloth_uv_table.py`, `tools/fxo_disasm.py`
  (`py`, not `python`). Shader disassembly archive: `re/shaders/DX9_disasm/*.asm`, verified
  instruction-for-instruction against live tool output for the six decal containers.
- Diagnostics land in the game dir as `sr3-remix-hard-N-*.dds`, `sr3-remix-gen-N-*.dds`,
  `sr3-remix-decal-N-conflicts.dds`. Move them to `dumps/<date>/` when done; `dumps/` is
  gitignored, as are `game-textures/`, `player-textures/`, `*.dmp`, `tools/vibe-re/`,
  `docs/evidence/cloth/`.

Backups: `D:\SR3RTXREMIXCOMP-backup-2026-09-10` and earlier. `engine-control/` holds the inert
74-entry engine census plugin, MOVED OUT of the game folder on 2026-09-10 to
`engine-control/removed-from-game-dir-2026-09-10/`.

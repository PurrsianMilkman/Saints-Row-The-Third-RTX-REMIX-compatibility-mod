# Worklog

## 2026-08-12 — Project start
- Working copy of GOG SR:TT confirmed at `Saints Row 3\`; DX9 exe is 32-bit → bridge runtime path.
- Pre-existing mods found: ASI loader (`dinput8.dll`), SRTT.MixFix (QOL: FPS uncap, particle
  crash fix, black bars off), ZMenu disabled (`.asi.bak`), `zmods_twitch.dll`.
- Project scaffolded: README, compatibility assessment, starter `rtx.conf`,
  Remix triage `display.ini` profile (old settings had MSAA 8x — hard Remix blocker),
  install/deploy/pull/launch scripts.
- Open question: check RTX Remix Showcase Discord compatibility table for prior SR3 attempts.

## 2026-08-12 (later) — Runtime installed, FIRST BOOT RENDERS
- Installed runtime **remix-1.5.2** via `tools\install-runtime.ps1`; deployed configs.
- Crash #1: game died in seconds. Event log: access violation in `SRTT.MixFix.x86.asi`
  → **MixFix is incompatible with the Remix d3d9 wrapper.** Disabled the ASI loader
  (`dinput8.dll` → `dinput8.dll.disabled`); revisit selectively later if FPS uncap is needed.
- Relaunch: stable. `NvRemixBridge` picked the **RTX 3090** (AMD iGPU correctly skipped).
  All four starter rtx.conf options accepted by 1.5.2 (no unknown-option warnings).
- **Menu background scene renders through the path tracer** — Steelport geometry, signage
  ("Loren Square") visible → vertex capture works on this engine. Scene is very dark, noisy,
  purple-tinted: expected (no game lights reach Remix; only the fallback light).
- Log notes (remix-dxvk.log): expected deferred-game fallbacks ("non-primary render target →
  rasterization"), occlusion queries ignored, one unknown texture format (1396921934),
  `rtx-interleaver` skipping a color0 buffer format (37). Nothing fatal in ~5 min uptime.
- DLSS Frame Generation unavailable on Ampere (fine); defaults set to Medium.
- Next (needs eyes on screen): Alt+X triage — fallback light brightness, texture categorization
  (UI/sky/ignore), check in-world rendering past the menu, then `tools\pull-conf.ps1`.

## 2026-08-12 (later still) — In-world triage, driven remotely via synthetic input
- **Outdoor gameplay renders**: street, vehicles, peds, emissive traffic lights, clean HUD.
- User's in-game texture clicking silently tagged 100+ textures into terrain/lightmap/
  raytracedRenderTarget/playerModelBody categories (invisible effects → felt like "nothing
  happened"). Saved via dev menu (Settings Management → rtx.conf → Save), then **cleaned**:
  kept the 8 intentional `rtx.uiTextures` hashes, dropped the accidental categories.
  Note: the runtime rewrites rtx.conf wholesale on save — comments live in configs\ master only.
- Runtime 1.5 config layers: user menu "Save Settings" writes `user.conf` (brightness/DLSS
  prefs); texture categories save through the **dev menu** Settings Management section.
- DLSS + Ray Reconstruction (CNN) active by default; render res 928x522 → 1600x900.
- **Interiors are pitch black** (crib). Log: `Trying to raytrace but not detecting a valid
  camera` → camera-constant extraction fails indoors; nothing enters the RT scene. Proved it's
  not lighting: sphere fallback light (mode 2, radiance 50, radius 3 — options accepted by
  runtime) changed nothing. Reverted expectation: this needs camera/matrix tuning or a hook.
- Researched strategy for full compatibility → `docs/research.md`. Short version: follow the
  xoxor4d gta4-rtx blueprint (ASI hook converting submission to fixed-function + light
  injection via Remix C API), after exhausting runtime camera options
  (`rtx.fusedWorldViewMode`, Game Setup Step 2) and checking Discord prior art.
- Current deployed rtx.conf: vertex capture + captured normals, fallbackLight sphere/50/r3
  (mode 2), uiTextures list. Master synced in configs\.

## 2026-08-12 (session 4) — Debug view diagnosis: RT scene is (nearly) EMPTY
- Restored the 27 accidental `rtx.raytracedRenderTargetTextures` hashes as an experiment →
  no change at the crib. Theory that they enabled session-1 rendering: busted.
- Learned to drive the dev menu remotely: game mouselook recenters the cursor every frame, so
  synthetic clicks only land while the game is PAUSED (Esc) or in a game menu. Pause first,
  then click. Wheel scroll and hover work regardless; keyboard menu nav works for game UI.
- **Enable Debug View (DEBUG section) with Primitive Index / Geometry Hash: the world shows
  NOTHING at/around the crib** — the ray-traced scene is empty there; only stray fragments
  (HUD-ish/world-space items) appear. Log correlates: `Trying to raytrace but not detecting a
  valid camera`.
- Working model: the "rendered" outdoor look the user saw is the game's own raster output
  passing through Remix's rasterization fallback (G-buffer/composite passes to non-primary
  render targets), NOT path tracing. Where even that fallback chain isn't composited (crib
  area), the screen is black except UI/emissive stragglers.
- All three saves (AUTOSAVE, HELI ASSAULT, HO TRAFFIC) spawn at the crib doorway. The earlier
  "street" frame after loading HO TRAFFIC was the loading-screen video, not gameplay.
- Next: with a viewpoint where the world visibly renders, debug view instantly answers
  raster-vs-RT (hash colors = RT geometry; normal game look = raster). Then attack camera
  extraction (`rtx.fusedWorldViewMode`, Game Setup Step 2) so draws enter the RT scene at all.

## 2026-08-12 (session 5) — VERDICT: path tracing WORKS outdoors; the problem is lighting
- User walked to the open street with Primitive Index debug view on: **the entire world is in
  the RT scene** — buildings, ground, player, peds all present. Camera detection + vertex
  capture work outdoors. The earlier "raster fallback" theory was wrong for the street.
- Debug view off, baseline captured: the street IS path traced but nearly unlit — only neon
  emissives + the small camera sphere light; heavy red firefly noise. "Doesn't look path
  traced" = "has no lights", exactly as predicted for a deferred game (no D3D9 lights exist).
- The crib block remains a real dead zone (RT scene empty, "no valid camera" in log) — that's
  the camera-extraction bug to attack with Game Setup Step 2 / `rtx.fusedWorldViewMode`.
- rtx.conf cleaned & synced to configs\: vertex capture, sphere fallback (mode2/50/r3),
  22 UI texture hashes. Removed the useless raytracedRenderTargetTextures experiment.
- Remote-driving lesson bank: pause (Esc) before dev-menu clicks; wheel/hover work unpaused;
  game menus keyboard-navigable (arrows+Enter); loading screens are bink videos (not gameplay);
  window position must be re-screenshotted before every click burst.
- NEXT SESSION (lighting, the actual fix):
  1. Rendering tab → LIGHTING: raise fallback light radiance (200+) live; try Distant type
     (moon/sun) — instant visual payoff, no restart.
  2. Game Setup Step 1: tag night-sky textures as Sky → enables sky/environment light.
  3. Real answer: scene capture → Remix Toolkit relight (sun/moon + street lamps as lights).
  4. Separately: crib camera fix experiments (fusedWorldViewMode 0/1, Step 2 camera params).

## 2026-08-13 (session 6) — TWO RETRACTIONS, then shader RE succeeds
**Retractions (both my errors, both from uncontrolled observation):**
- "Path tracing works outdoors" — WITHDRAWN. Rested on a Primitive Index debug view that almost
  certainly never applied (frame still showed brick *textures*) and an A/B toggle never actually
  compared. User's report stands: outdoors looks rasterized.
- "Distant light lit the penthouse" — WITHDRAWN. The penthouse was always lit; the user had
  physically moved locations between the two screenshots. The fallback light's effect on this game
  remains **unverified**. The black interior is one specific place: **Shaundi's loft**.
- New rule: no progress claim without same-save/same-location/one-variable screenshot pairs.

**Research (2 deep web investigations, full detail in `docs/research.md`):**
- SR3 uses Volition **Inferred Lighting**, not classic deferred (SIGGRAPH 2009; GDC 2012).
- Remix takes its camera from `SetTransform(D3DTS_VIEW/PROJECTION)` — *never* from shader
  constants. So a hook can supply the camera with plain D3D9 calls.
- `SetLight`/`LightEnable` → `RtxContext::addLights()` = real ray-traced lights on the **stock**
  runtime, bridge-compatible. Beats the Remix API (whose `SetupCamera` is disabled over the bridge).
- Zero Saints Row Remix prior art exists. `Clippy95/SR.MixFix` (MIT, this exact game) is the ASI
  template; `softsoundd/dxvk-remix-mirrorsedge` the fork template.

**Shader RE — DONE, and it answers both blockers** (`docs/shader-map.md`):
- Installed Python 3.12. Wrote `tools/vpp_extract.py` (VPP_PC v6 reader — header layout derived by
  hand; key gotcha: blocks are zlib with the **adler32 trailer stripped**, so decode as raw
  deflate) → **1693/1693 files extracted, 0 failures**.
- Wrote `tools/fxo_scan.py` (CTAB parser) → 844 DX9 shader files, **7,276 shaders**, 52,990 named
  constants → `re/shader_constants.csv`.
- **Camera matrices at fixed invariant registers**: `projTM` **c28** (4 regs), `objTM` **c32**
  (3), `IR_World2View` **c48** (3), `eyePos` c41, `Bone_weights` c52 (192). Separate matrices,
  **not** a fused WVP → `fusedWorldViewMode` stays None.
- **Every light type is a named shader** with parameters in known ps registers:
  `ir_light_point/spot/directional/tube/local_ambient`, e.g. spot = `IR_Light_Pos` c0,
  `IR_Light_Dir` c1, `IR_Light_Color` c13, `IR_Spot_Info` c15. `ir_light_directional` = the sun.
- G-buffer/LBuffer samplers confirm the inferred-rendering flow and corroborate (not prove) the
  "world renders into offscreen targets → Remix rasterizes it" hypothesis.
- Next: Phase 0 controlled A/B (raytracing on/off, same save) at both locations; then decode
  `IR_Light_Info`/`IR_Spot_Info` layout; then the ASI (camera module first).

## 2026-08-13 (session 7) — **PATH TRACING ACHIEVED** via a custom ASI
**Root cause found and fixed.** Disassembly showed the game computes clip space as
`projTM · worldPos` — i.e. `projTM` (c28) is a **fused view-projection**, and `IR_World2View`
(c48) is the separate pure view matrix. The engine transforms everything itself and, as the ASI
then proved at runtime, **never calls `SetTransform` even once** (`game's own SetTransform
calls=0` after 1800 frames). Remix reads its camera *only* from that fixed-function state, so it
had **no camera anywhere, ever** → it rasterized the whole game. That single fact explains every
symptom: "looks rasterized", the black loft, and the log line.

**`sr3-rtx.asi` (new, `src/sr3-rtx/`)** — built with MSVC x86 Build Tools, loaded by the existing
Ultimate ASI Loader (`dinput8.dll` re-enabled; `SRTT.MixFix.x86.asi` stays disabled):
- Hooks `IDirect3D9(Ex)::CreateDevice(Ex)` by patching the shared vtable of a probe object (no
  inline hooks, no MinHook). **The game uses `CreateDeviceEx`** — the plain hook alone caught
  nothing, which cost one iteration.
- Hooks `SetVertexShaderConstantF`, watches registers c28/c32/c48, transposes them into D3D
  row-vector form, recovers a true projection as `inverse(view) · projTM`, and publishes
  world/view/projection via `SetTransform`.
- Validation that the math is right: decomposed projection gives `_22 = 1.7321` → **60° vertical
  FOV**, and `_11/_22` → **aspect 1.778 = 16:9**, exactly matching the 1600x900 window. Near
  plane 0.1 with an infinite far plane.
- Logging goes to `sr3-rtx.log` (shared-access, tailable while running).

**Controlled A/B at the main menu** (identical scene every launch; only variable = ASI present),
archived in `docs/evidence/`:
- WITHOUT ASI → the familiar flat purple/magenta neon menu = the game's own raster output.
- WITH ASI → a completely different image: desaturated, depth-shaded, lit by our white distant
  fallback light. **The path tracer is now doing the shading.**
- Remix's per-frame `Falling back to rasterization` message is gone from the session; only a
  single startup-time `not detecting a valid camera` remains (logged once, before the first
  publish).

It looks grey/washed out because the only light in the scene is our white fallback sun — the
game's own lights are volume draws Remix never sees as lights. That is exactly the next module.

## 2026-08-13 (session 7b) — camera stability pass
User feedback on the first working build: real path-traced GI visible in places, but textures
flicker colours, glitches "like unstable hashes" when rotating/flying, and the game **crashed**
(on Continue → level load). Remix log showed repeated `Camera cut detected`, each one
re-initialising the Neural Radiance Cache, right before an access violation.

Cause: the shim published a camera on *every* write to c28/c48 — ~86/frame — and the engine
reuses those registers for shadow maps, light volumes and other passes. Remix saw the camera
teleporting many times per frame.

Fixes:
1. **Aspect filter** — reject any pass whose recovered projection isn't perspective
   (`_34`≈1, `_44`≈0) or whose aspect doesn't match the back buffer (captured from the present
   parameters: 1600x900 → 1.778). Rejects shadow/reflection/light passes.
2. **One camera per frame** — latch the first surviving candidate each frame. The engine offers
   **~70** perspective back-buffer-aspect candidates per frame, mostly duplicate re-uploads.
3. **Publish only on change** (memcmp) for view/projection and the world matrix.
4. `rtx.neuralRadianceCache.resetSceneBoundsOnCameraCut = False` in rtx.conf.

Result: publishes fell from ~86/frame to ~0.14/frame (rises only while the camera actually
moves — verified against the log while the user played), and **the game survived a full level
load** where it previously crashed. Camera tracking confirmed correct: the penthouse renders
path traced from the right viewpoint.

Still open:
- **Shaundi's loft is still black** (the earlier "loft fixed" reading was the penthouse — user
  correction; the penthouse rendered before too). Needs a controlled visit to that exact location.
- Texture colour flicker / instability while moving. Prime suspect: `D3DTS_WORLD` being carried
  over to draws that never write c32, misplacing geometry. Added **`sr3-rtx.ini`** with
  `publishWorld` / `oneCameraPerFrame` / `aspectFilter` toggles so this can be A/B'd without
  rebuilding.
- Lights still not injected (scene lit only by the fallback sun).

### Location sweep with the stabilised build (same session)
Drove the game myself through menu → load → locations. Results:
- **Penthouse**: renders well path traced — daylight, sky through the windows, marble floor,
  correct third-person viewpoint. Camera tracking verified correct.
- **HELI ASSAULT save (crib exterior, night)**: this is the save that used to load to a
  **pitch-black screen with only HUD**. It now **renders fully** — street, buildings, character,
  city lights. Big improvement from the camera fix.
  Caveat: "ACCESS CRIB" opens the crib *menu*, not a physical interior, so this is not proof
  about Shaundi's loft interior — that still needs a visit on foot.
- Visible defect at that location: heavy **red speckle/firefly noise** on floors and surfaces.
  Consistent with a scene lit by a single very bright distant light (radiance 100) and no sky —
  almost all illumination arrives via high-variance indirect paths. Injecting the game's real
  lights and tagging the sky should reduce it structurally; denoiser/firefly options are the
  cheaper stopgap.
- Camera candidates rose to **90/frame** outdoors (vs 70 indoors); the latch keeps publishes at
  ~0.10/frame, and `game SetTransform=0` still holds everywhere.

Input-driving note: the game's menus need **extended-key** scancodes for the arrow keys
(`KEYEVENTF_EXTENDEDKEY`, flags 0x09/0x0B). Without the extended bit, Down is swallowed and
Enter selects the wrong item — this put me in the SAVE screen by accident once (backed out with
Esc, nothing overwritten).

## 2026-08-13 (session 8) — light injection + two geometry/camera fixes
**Light injection implemented and working.** The shim now recognises the engine's light volumes
at runtime by parsing each pixel shader's **CTAB constant names** as it is created — no hash
database, and it adapts to variants automatically (registers differ between `ir_light_point` and
`ir_light_point_tex`, so hardcoding them would have failed). Detected **24 light shaders**,
correctly classified point / spot / directional.
- Parameters are read from a shadow copy of the PS constants, converted from view space to world
  space with the inverse of the view matrix we already track, and emitted as `D3DLIGHT9` via
  `SetLight`/`LightEnable` on the draw call for each light volume.
- **All lights reuse slot 0**: Remix accumulates game lights per draw call, so the usual
  8-simultaneous-light cap never applies and the light count is effectively unbounded.
- Measured **~13–21 lights/frame** in gameplay; user confirms spotlights are visibly captured.

**Two further fixes, both A/B verified:**
1. **Reject cameras at the world origin.** Some utility pass submits a perspective back-buffer-
   aspect camera at 0,0,0; the once-per-frame latch was grabbing it, leaving the path tracer
   staring at empty space. Rejecting it made the camera report a real position (244.8, 33.8,
   111.7) and, per the user, also fixed the grey main menu.
2. **`publishWorld` must be OFF** (now the default). Skinned meshes transform through
   `Bone_weights` (c52) and never write `objTM` (c32), so they inherited a stale world matrix and
   **exploded into spiked garbage** — clearly visible in one screenshot, and cleanly gone in the
   next with the toggle off. Remix's vertex capture supplies their final positions instead.

**Result**: the "HO TRAFFIC" save — the location that was pitch black — now renders as a lit
interior with a visible light pool on the floor, correct character geometry, and path-traced
falloff. The user also raised Remix's exposure and ignored a few screen-blocking textures.

### Green/yellow tint SOLVED — wrong texture stage used as albedo
Remix treats **texture stage 0 as a surface's albedo**. Querying the extracted shader data
(`re/shader_constants.csv`) showed Saints Row only binds the diffuse map there part of the time:

| sampler at s0 | shaders |
|---|---|
| `Diffuse_MapSampler` | 804 |
| **`Damage_Normal_MapSampler`** | **488** |
| **`Normal_MapSampler`** | **415** |
| `Decal_MapSampler` | 108 |

So ~900 shaders were being shaded with a **normal map as their base colour** — and compressed
normal maps read as colour are exactly that bright yellow-green. The diffuse for those materials
lives at s1 (418 shaders) or s3 (53).

No `rtx.*` option exists to redirect the albedo stage, so the shim now fixes it: the same CTAB
reflection used for lights also records each pixel shader's real `Diffuse*Sampler` register, and
`BeginAlbedoFix`/`EndAlbedoFix` rebind that texture to stage 0 for the duration of the draw and
restore it immediately after, leaving the game's own state untouched.
**Result: ~25 corrections/frame, and the tint is gone** — the same interior that was drenched in
green now renders with neutral grey/white walls. Toggle: `fixAlbedoStage` in `sr3-rtx.ini`.

Also decoded: the `ConvertFormat: Unknown format encountered: 1396921934` warning is FourCC
**"NVCS"**, an NVIDIA driver feature-detection token rather than a real texture format — a red
herring, safely ignorable.

### "Scene colours change constantly when the camera moves" — one real bug fixed, one open
**Bug found and fixed in our own light code.** `EmitLight` transformed each light from view space
to world space using `g_view`, which holds *whatever pass wrote c48 most recently* — shadow maps
and reflections included. So a light volume drawn after a shadow pass was placed with the wrong
matrix, and the error shifted as the camera moved. Remix de-duplicates game lights by world
position (`rtx.lightConversionEqualityDistanceThreshold`), so drifting lights read as a constant
stream of *new* lights → churning colour. Now uses the published main camera's view
(`g_lastView`) instead. Fixed in the deployed build.

### Camera-attached "blocking" geometry — cause identified, fix shipped (needs user verdict)
User: "a lot of stuff still blocking the camera which moves with the camera as I rotate", plus a
flat panel of HUD icons rendered in 3D. **This was a side effect of our own camera fix**:
publishing the main camera's perspective view/projection leaves it set for *every* draw, so
screen-space quads (post-process passes, HUD atlases) — which previously carried no meaningful
transform and were ignored — inherited a perspective camera and got ray-traced as world geometry
hanging in front of the player.

Fix: reflect **vertex** shaders too. A world-space VS always multiplies by `projTM`; one that
doesn't is emitting clip space itself, i.e. a fullscreen/HUD quad. Those draws now get an
identity (orthographic) transform for their duration, which Remix classifies as UI via
`rtx.orthographicIsUI`. Measured: **185 vertex shaders** classified screen-space, **~32 draws per
frame** demoted — a plausible fraction, not an over-broad sweep. Toggle: `screenSpaceAsUI`.
The blocking icon panel is gone from the test frame; whether the rest is gone needs the user
moving around to confirm.

### The camera-blocking pattern: vertex capture fails on shader-built geometry
Three separate offenders blocked the camera, each found and demoted in turn — and together they
reveal one underlying rule.

| Offender | Signature in CTAB | Demoted |
|---|---|---|
| Post-process / HUD quads | no `projTM` (emits clip space itself) | 185 shaders |
| **Characters & pedestrians** | `Bone_weights` (192-register skinning palette) | **432 shaders** |
| **Particles / billboards** | name contains `particle` / `Billboard` | ~25 shaders |

**The rule: Remix's vertex capture only reconstructs geometry whose vertex shader is a simple
transform.** Anything the shader *builds* — skinned meshes, camera-facing billboards, procedural
quads — comes back at the wrong scale/position, usually as a screen-filling plane parked in front
of the camera (a giant blue blob for the player, a giant orange sheet with a flame sliver for
fire particles). Publishing a real camera is what made these visible: before, they had no usable
transform and Remix ignored them.

Current mitigation demotes all three classes to UI (identity transform → `rtx.orthographicIsUI`),
so they rasterise normally instead of being ray-traced into the world: **637 shaders, ~78 draws
per frame**. Toggles: `screenSpaceAsUI`, `skinnedAsUI`, `proceduralAsUI`.

**This is a workaround, not a solution.** Characters and effects lose path-traced shading. The
structural fix is the xoxor4d approach: hook the engine's submission and **re-submit that geometry
already transformed** (software skinning / pre-built billboard quads) so it enters the ray-traced
scene correctly. That is the single biggest remaining piece of work.

**Where shader-class demotion ran out of road.** A fourth attempt added fixed-function draws
(null vertex shader) to the demotion set, on the theory that the HUD icon atlas was drawn that
way. It was not — **the atlas still renders as floating geometry**. Four classification passes in,
the conclusion is that this approach is treating symptoms:
- The atlas is most likely drawn by a *world-space* UI shader that legitimately uses `projTM`, so
  no shader-shape heuristic will separate it from real world geometry.
- The right tool for it already exists and is texture-based, not shader-based: Remix's
  **World Space UI Texture** category (`rtx.worldSpaceUiTextures`), applied by hovering the atlas
  in Alt+X → Game Setup and assigning it. That persists in `rtx.conf` and takes seconds.
- **Recommendation**: stop adding shim heuristics for UI; tag the handful of offending textures
  in the Remix UI instead, and spend shim effort on the pre-transformed-geometry work above,
  which is what actually gets characters and effects path traced.

### 2026-08-13 (session 9) — light injection CONFIRMED by the user; tags reset and rebuilt
- User verdict, unprompted: *"lights are being captured by RTX Remix… I see a street light
  shining down on world and character geometry with path traced shadows."* That is independent
  confirmation the light module works end to end — engine light volume → CTAB reflection →
  `D3DLIGHT9` → Remix ray-traced light → shadows on both world and characters.
- Screenshot also shows the world rendering correctly: brick, ornate stonework, wood flooring,
  awnings, correct neutral materials (the albedo-stage fix holding up).
- Shaundi's loft / "HO TRAFFIC" reported **more stable, less blocking the camera**.
- All texture categorisation was **reset to zero** at the user's request (backup:
  `configs/rtx.conf.before-tag-reset.bak`), then re-tagged by hand from a clean slate. Now
  **19 categories** saved and versioned (`configs/rtx.conf`, snapshot
  `configs/rtx.conf.tagged-2026-08-13.bak`): 27 uiTextures, 38 raytracedRenderTarget,
  16 playerModel, 9 ignore, 8 terrain, 5 particle, plus sky/water/lightmap singles.
- Note: the earlier live-tagging losses were because tags stay in memory until
  **Settings Management → Remix Config → Save**. Worth re-checking after every tagging session.

**Remaining visible issues** (from the latest screenshot): the **minimap panel** still renders as
a large floating world-space quad (untagged), and **character materials are still wrong**
(orange/blue/green body) — a separate problem from the skinned-geometry demotion.

### 2026-08-13 (session 10) — world-correctness pass; character colour cause CONFIRMED
**Character colour: cause found (user's hypothesis, confirmed in the data).** The user suggested
player colour comes from the skin-tone/customisation system rather than a texture. Querying the
221 skinned shader files confirms it — their pixel shaders take **`Tint_color` (448 shaders)**,
**`Specular_Color` (278)**, **`Self_Illumination` (223)** and **`Base_Paint_Color` (138)** as
*constants*. Remix never executes game pixel shaders, so **no texture-stage choice can ever
produce correct character colour.** This is an architectural mismatch, not a bug, and it explains
why the albedo fix did nothing for characters. Options recorded: (A) push the tint through
fixed-function material state / `D3DRS_TEXTUREFACTOR`, (B) software skinning with the tint baked
into vertex colours, (C) fork dxvk-remix to read the tint constants, (D) authored replacement
materials, (E) ship world-only. User's chosen order: **world correctness first, then A → B → C**
(long compile times explicitly acceptable, so C is on the table).

**World pass, three changes:**
1. `rtx.fallbackLightMode` 2 → **1**. Real lights are injected and confirmed working, so the
   always-on white sun at radiance 100 was double-lighting the scene and dragging auto-exposure.
2. Cleared `rtx.playerModelTextures` (had grown to ~74 hashes) and `playerModelBodyTextures` —
   the user's in-game untag had not persisted.
3. Tried `demoteCameraMismatch=1` (the measured 5.1% of draws whose camera differs from the
   published one). **REVERTED** — the main menu went back to the rasterised purple look instead
   of the path-traced grey, i.e. it demotes real world geometry, not just broken draws. The
   5.1% measurement stands but this is not a safe lever as implemented.

### Character materials — one correctness fix made, root cause NOT found
Hypothesis: `BeginAlbedoFix` swapped texture stage 0 on *every* draw. That is safe for a
ray-traced draw (Remix reads only the texture and discards the game's shading) but wrong for a
**demoted** draw, which is rasterised through the game's real pixel shader — feeding it the wrong
map. Fix applied: the albedo swap now skips demoted draws.

**Result: characters are still mis-coloured**, so this was not the cause. The change is kept
because it is correct on its own terms (never corrupt the inputs of a draw whose own shading is
used), but the real cause is still open. Next things to check:
- `rtx.playerModelTextures` (16 hashes) and `playerModelBodyTextures` (2) were tagged by hand
  this session; player-model categories change how Remix treats those surfaces. Try clearing
  just those two categories and compare.
- SR3 character materials layer several maps (`Diffuse_Map_1Sampler`, `Damage_Normal_MapSampler`,
  `Pattern_MapSampler`, team/tint colours). The character shader may resolve its final colour
  from constants rather than a single diffuse texture, in which case no stage-0 choice is right.
  Disassemble a character pixel shader (`tools/fxo_disasm.py`) before changing more code.

**Auto-exposure investigated, not concluded.** The user's tuned config had
`evMinValue = -80`, `evMaxValue = 13.01` — a 93-stop window, which lets exposure hunt hard as
bright emissives enter/leave frame. Tried two settings on the dark "HO TRAFFIC" interior:
- `enabled = False` → scene almost entirely black (auto-exposure had been carrying it).
- `evMin = -2, evMax = 6` → still very dark; the clamp stops it lifting the scene.
Conclusion: **this interior is genuinely under-lit**, and exposure is compensating rather than
causing. The structural fix is more/better injected lights, not exposure tuning. The user's
original config was **restored** (backup kept at `configs/rtx.conf.user-tuned.bak`) since live
tuning is better done by someone watching the screen.

Still open:
- ~~Strong green/yellow colour tint~~ — **fixed**, see above. It **predated light injection**
  (it is present in the user's first screenshot from session 7), so it is a material/texture
  resolution problem, not a lighting one. Prime suspects: the `ConvertFormat: Unknown format
  encountered` warning in the Remix log, and texture hash instability from streaming (the exact
  problem GTA IV's fork solved with constant texture-hash recalculation).
- Exposure is very sensitive — a few blown-out emissives drag auto-exposure down and crush
  everything else.
- Residual glitching while moving; texture tagging is still an uncontrolled variable (the user
  has been ignoring screen-blocking textures by hand).

## 2026-08-14/15 (session 11) — four theories killed, one root cause found, engine mapped

**Method failure to record first.** The handoff note said: change one toggle, restart, confirm
on screen *and* in the log. I stacked `skipCameraMismatch`, `skipCompositePasses`,
`demoteLightVolumes`, `cullBlankScreenQuads` and `cullStaleAlbedoDraws` across restarts without
a clean confirmation between them. When the screen went black there was no way to attribute it,
and a working build was lost. Everything was reset to baseline (all experimental switches 0).
The findings below survive because they are measurements, not impressions.

**A trap worth knowing:** the `sr3-rtx.log` on disk at handoff predated the ini edit that turned
the four toggles off, so it recorded the toggles-ON run. Reading log and ini together suggested
the toggles were being ignored. Always check the log's start time against the ini's mtime.
Same class of problem hit twice more: the user's live edits (`screenSpaceAsUI=0`,
`skipCompositePasses=1`) existed only in the running process, not in the file; and one deploy
half-failed because the `.asi` was locked by the running game while the `.ini` copy succeeded.
Deploys are now hash-verified on both files.

### Disproved, with evidence
- **`demoteViewSpheres` — REMOVED.** A `Viewsphere*` sampler appears in exactly 128 pixel
  shaders across 46 files; **all 128** also carry `Tint_color` and `Base_Paint_Color`, and 19 of
  the files are skinned. It is the car-paint/character-body material family, not a reflection
  shell. The toggle was demoting cars and characters — the "characters broke" regression.
- **`demoteLightVolumes` — broken by construction, then reworked.** `EmitLight` sets a
  world-space light, then the same draw's `D3DTS_VIEW` was set to identity, so Remix resolved
  the light against an identity view. The old log proves it numerically: demoted light-volume
  draws and emitted lights matched to the decimal (16.9/16.9, 17.2/17.2). Now overrides
  **projection only**, which is all Remix's orthographic-is-UI test reads.
- **`fixAlbedoStage` is not the wrong-texture cause.** Tested at 0, returned to the *same wall*:
  identical wrong tiling. Restored to 1 (it does fix the yellow-green tint).
- **Light volumes are not the spheres.** `skipLightVolumes=1` dropped every light-volume mesh
  (108.8/frame, matching lights emitted) and the spheres remained.
- **Characters are not the spheres.** `skipSkinned=1` removed all characters; spheres remained.

### The white plane: three failed theories, and why each failed
1. *Its white texture hash* — added `-0x5DD44DFBED334A45` (1280x720, pure white) to
   `rtx.ignoreTextures`. Survived a config Save, so Remix parsed it; plane unaffected.
2. *An untextured clip-space quad* — matched **zero** draws, proving the plane's vertex shader
   does reference `projTM`, so Remix classifies it as world geometry.
3. *Stale-albedo draws* — see below; it worked as designed and deleted most of the world.

### Root cause found: most world draws declare NO sampler at register 0
`cullStaleAlbedoDraws=1` collapsed **474 draws/frame** and "most stuff" vanished. That is the
finding: SR3's visible world shaders have no sampler at s0 at all — confirmed independently by
the geometry log, where 29 of 30 world draws report `diffuseStage=-1`.
**Remix reads stage 0 as albedo, so for most of the world the albedo is whatever texture the
previous draw left bound.** This explains the shifting/wrong textures scene-wide, why the
spheres wore the player's clothing atlas, why hovering them offers nothing to tag, and why
ignoring any single hash does nothing. It also explains why `fixAlbedoStage` underperforms: it
only acts on samplers *named* `Diffuse*`, which these shaders do not have.
Next fix (not yet written): bind the shader's lowest-numbered sampler to stage 0 instead of
name-matching. Additive, removes no geometry.

### Capture analysis (Remix USD captures, parsed with usd-core)
Tooling: `pip install usd-core pillow`; captures live in `Saints Row 3/rtx-remix/captures/`.
- **Geometry is duplicated 2-3x.** 1454 mesh instances for one room. The player mesh appears
  twice, at x=**+96.2** and x=**-96.2** — mirrored. Secondary passes (shadow/reflection) submit
  under a different projection and Remix rebuilds them with our main-camera matrices.
- **Lights barely arrive.** 3-4 light prims in the scene while the shim emits 82-126/frame.
  One SphereLight sits 12 units from the camera at intensity 10093; another at 24.7.
  Unresolved. Suspect the slot-0 reuse assumption ("Remix accumulates game lights per draw
  call") which was never verified.
- Texture `73514330879EEF92` decoded to an image: the player's green/blue clothing atlas —
  identifying the "wall" texture conclusively.
- Capture camera X is negated vs the shim's log (capture -96.2, shim +96.9); Y and Z match.
  Probably a handedness flip on USD export, but do not read coordinates from a capture without
  accounting for it.

### Vertex format (phase 1 of the re-submission project)
Uniform across all 30 sampled world draws:
`pos@0 float3` | `normal@12 ubyte4n` | `uv@16 or @20 short2` | stride **20** or **24**.
Positions need no dequantization. **UVs are packed short2** — a scale must be applied, and the
source of that scale is not yet identified (`Object_instance_params_2` c36 is the suspect).
Only 3-4 of the 11-12 declared elements are in stream 0; blend weights/indices for skinning are
in other streams, which phase 5 will need.

### New in the shim
`mode=0|1` (interpose vs engine); engine mode force-disables every interpose heuristic.
Bisection skips (`skipSkinned/Procedural/ScreenSpace/CameraMismatch/LightVolumes`) which DROP
draws rather than demote; a degenerate-vertex-shader cull that collapses geometry without
dropping the draw call (`cull: collapse shader ready` — the mechanism works); `logGeometry` and
`logBlankDraws` diagnostics. **All default 0.**

**Do not retry** `skipScreenSpace=1`: it froze the image ("repeats the last 3 frames"), because
`g_currentVSClass` defaults to `VS_SCREEN` for null shaders, so it also dropped every
fixed-function draw. Split fixed-function into its own switch before revisiting.

---

## Session 12 — 2026-08-16 — restart on the SR2 proxy design

**The project was reset.** Everything built before reading
[BRAGme/sr2-rtx-remix-proxy](https://github.com/BRAGme/sr2-rtx-remix-proxy) was deleted and the
shim rewritten around that design. Full rationale and the port table:
[sr2-fork.md](sr2-fork.md). Backup of the whole project (minus the 11G game dir) at
`D:\SR3RTXREMIXCOMP-backup-2026-08-16`; previous source and config at
`docs/evidence/pre-sr2-fork/`.

### Why reset rather than keep patching
Every switch in the old shim was a heuristic compensating for Remix *reconstructing* geometry
from vertex-shader output. Converting draws to fixed function removes the reconstruction, so
those symptoms have no source. Keeping the heuristics alongside the conversion would have meant
two layers both removing draws and no result attributable to either.

### Read the actual source, not a summary
The repo was fetched and read (`ffp_state.hpp/.cpp`, `renderer.hpp`, README) rather than worked
from my earlier reconstruction of it. That was worth doing — it contradicted my implementation
in three places:

1. **`COLOROP = SELECTARG1` from `D3DTA_TEXTURE`**, not `MODULATE` against `D3DTA_DIFFUSE`.
   Ours multiplied in a vertex-colour term, baking the game's own shading into the albedo.
2. **Alpha must come from TFACTOR unless the draw is a real cutout** (alpha test on AND
   `ALPHAREF > 0`). Ours took texture alpha unconditionally — SR2's note is that this makes
   "every wall go X-ray", and we would have shipped it.
3. **Never re-submit through `DrawIndexedPrimitiveUP`** — it null-derefs in the Remix bridge
   server. Matches our own 2026-08-15 crash (`ACCESS_VIOLATION` reading `0x20` in
   `.trex\d3d9.dll`).

### The finding that may explain the whole project
> "SR2 uses an infinite-far projection (Q ≈ 1.0); **Remix's CameraManager rejects that and
> falls back to the wrong camera.**"

Our `PublishCamera` validated `_34 ≈ 1` and `_44 ≈ 0` and **never looked at `_33`**. If SR3
also ships an infinite far plane, Remix has been silently substituting its own camera all
along — which would produce precisely the symptoms chased for eleven sessions. `finiteFar=1`
now rewrites `_33`/`_43` for a far plane of 5000 whenever `_33 ≥ 0.999`, changing Z only.
**The log counts substitutions per frame; a non-zero count confirms it.**

### Built in from the start: the state-call trap
SR2's profiling found its own FFP conversion issuing ~28 state changes per draw — 6,460
`SetRenderState` per frame against ~530 draws — with the GPU at 37% and the frame time spent in
32→64-bit bridge IPC. SR3 submits ~3x SR2's draws. So the rewrite has a shadow cache over
`SetRenderState`/`SetTextureStageState`/`SetSamplerState` and dirty-tracked transforms from
day one, and reports dropped calls per frame.

### Kept from our own work
CTAB sampler-name ranking for albedo (SR2 picks by texture size; we can read the shader's own
names) and property-based pass selection — `skipDepthOnly` keys on
`D3DRS_COLORWRITEENABLE == 0` instead of the render-target *indices* the old build used, which
were never stable.

### State
Builds clean at 158,208 bytes, deployed hash-verified. **Not yet run.** First run should be
judged on, in order: `far substitutions` non-zero, `FFP converted` as a share of draws, the
`vertex layout` lines (short2 UVs need `uvScaleDenom` before texturing can be correct), then
the screen.

### Not yet ported
Skinning (characters excluded), vertex expansion for `short2`/`ubyte4n` fields, the mesh albedo
cache for streaming eviction, and light injection through the Remix API instead of `SetLight`.

### Run 1 and 2 (2026-08-16) — conversion works, two claims retracted, one real bug fixed

**Measured, first run:** 1,985 draws/frame, **75.7% converted** (up from 4%). Skip breakdown
all legitimate: depth-only 375, skinned 86, screen-space 14, ortho 0. Shadow cache dropped
**21,000–30,000** redundant state calls/frame, confirming the SR2 IPC trap was real and avoided.
Transform writes 288/frame against 1,504 converted draws — dirty tracking working.

**RETRACTED — the infinite far plane.** I reported `_33 = 1.00003` as confirming SR3 ships an
infinite far plane, and called it a likely root cause. It is not. `Q = 1.00003` with
`Qn = 0.15` is *exactly* `near=0.15, far=5000` (`5000/(5000-0.15) = 1.0000300`). My threshold
of `_33 >= 0.999` matched an ordinary finite projection and substituted an identical matrix —
1,504 no-ops per frame reported as fixes. Threshold now requires an implied far > 10⁶.
**SR3's camera was never broken by this.**

**REAL BUG, fixed.** The applied-transform cache used identity as its "unknown" sentinel. But
identity is the most common legitimate world matrix — every shader without `objTM` gets one —
so after each Present the first such draw compared equal, skipped its `SetTransform`, and
inherited the previous frame's last world matrix. Replaced with explicit validity flags. Also
fixed: lights gated on a per-frame camera recapture (had fallen ~17 → ~5/frame), and texture
restore issuing 8 `SetTexture` per converted draw (~12,000 bridge calls/frame the shadow never
saw) now restores only stages actually changed.

**Still glitchy after both, with "random polygons and geometry stretching across the whole
world."** That is the signature of vertices whose positions are being read wrongly, so the
next build tests the thing never logged: **the POSITION element's type**. SR3 compresses
normals (`ubyte4n`) and UVs (`short2`), so a compressed POSITION is entirely plausible — and
fixed function transforms POSITION itself, so it must be an uncompressed float3/float4 in
stream 0 or the mesh scatters. Added as a representability precondition (same category as the
skinned exclusion, not a heuristic) with a `vertex-format` skip counter, plus a direct
measurement: sample each converted draw's vertices, transform by the applied world matrix, and
report any draw landing beyond |world| > 20000 along with its layout. Steelport runs to the low
thousands, so that threshold isolates the offenders and names the class rather than inferring
it from shader names.

Confirmed from run 1: **all world geometry uses `uv=short2`** with `uvScaleDenom=0`, so
texturing cannot be correct yet regardless. `short2 UV range` measurement added to derive the
divisor instead of guessing it.

### Run 3 (2026-08-16) — the world-matrix gate, and the UV divisor measured

User: "almost all the world geometry is all where the character is."

**The counter that found it:** `FFP converted 1828/frame` against `transform writes 52/frame`.
Nearly 1,800 converted draws were sharing ~50 world matrices — the whole world drawn at a
handful of places.

**Cause, and it was mine.** `ComputeTransforms` required *both* that the shader declares
`objTM` **and** that we had witnessed the c32 upload since the last shader bind. The second
condition was ported from SR2, whose comment justifies it as "new VS may have different
constant layout". That is true for SR2 and **false for SR3**, whose register layout is fixed
and documented — c32 is always `objTM`. Whenever the engine uploaded the matrix *before*
binding the shader, `Hook_SetVertexShader` cleared the flag and the object silently fell back
to identity.

Fixed: the shader's own constant table is the authority. If a shader declares `objTM`, the
engine must have uploaded a valid one or the game's own rendering would be wrong. Added
`world matrix changes/frame` and `objTM used without a fresh upload/frame` so the next run
shows both the effect and how often the old gate was firing.

**Theories killed by measurement this run** (worth recording, both were mine):
- *Compressed positions.* Wrong. Every layout reports `pos=float3@0(stream 0)`; the
  vertex-format skip is 5/frame. The precondition stays because it is correct, but it explained
  nothing.
- *Outlier geometry.* 0.6/frame, and all 20 reports are one 173-vertex mesh at |world| = 51256
  — almost certainly the skybox, i.e. legitimate.
- *Camera thrash.* `distinct cameras this frame 3`. Not a factor.
- *Far-plane substitution.* Now 0/frame after the threshold fix, confirming the retraction.

**UV divisor measured = 1024.** The `short2 UV range` lines show 4-vertex quads spanning
exactly `u 0..1023, v 0..1023` — one texture repeat — while large meshes read ±14000 and ±29000,
which divide by 1024 into sensible tiling counts (13.7, 28.5). `uvScaleDenom=1024` set. This is
derived from measurement, not guessed.

Also improved this run: lights back to **35.8/frame** (were ~5 when gated on per-frame camera
recapture). Still open: 793 of 1828 converted draws (43%) have no colour texture in their
shader at all and render untextured.

### Run 4 (2026-08-16) — instancing found, mirroring measured, protocol settled

Screen: recognisable city, correctly placed, plus a mirrored copy hanging below. User: "we are
getting there."

**The call-order trace settled the objTM protocol.** Captured sequence:
`OVx OVx OVx OxOxOxOx...` — the c32 upload consistently PRECEDES the shader bind. That confirms
removing the `SetVertexShader` reset was right, and `objTM used without a fresh upload` is now
**0/frame**: the gate is finally correct.

**SR3 uses hardware instancing, heavily — 998 draws/frame.** Found by the `SetStreamSourceFreq`
hook added this run. Fixed function has no per-instance transform, so every converted instanced
draw collapsed all its copies onto one matrix. **That was "the city is bunched up".** Now
refused, which is why conversion fell to 6.5% — deliberate: a refused draw is merely not
path-traced, a wrongly-placed one corrupts the scene. Converting these properly (one draw per
instance, transform read from the instance stream) is now the single biggest remaining task,
and the log reports instance counts per frame to size it.

**The mirrored duplicate, measured from the capture** (`capture_2026-08-16_09-42-55.usd`,
729 mesh instances):

```
mirrored (det<0): 500     upright: 229
mirrored sample : det=-1  translation (-98.85, 9.18, 234.33)  diagonal scale (-1, 1, 1)
log camera      :         position    ( 98.8,  9.2,  234.3)
```

So the mirroring is an **X-axis negation**, not a water reflection — the camera position with X
negated. This is the same artefact recorded in session 10 (player mesh at x=+96.2 and a
duplicate at x=-96.2), now with a measured cause rather than a guess. All 229 upright meshes sit
at translation (0,0,0), i.e. exactly our identity-world conversions.

**The fix is deliberately not "skip draws whose view determinant is negative".** Whether SR3's
`IR_World2View` preserves handedness at all is unverified; if its convention flips handedness,
an absolute test would reject every draw in the game. Instead the frame's first camera sets the
reference and only passes whose handedness DIFFERS are refused — self-calibrating, correct under
either convention. The baseline determinant is logged so the convention becomes known either way.

Note for the next session: after a converted draw the shim restores VS/PS but leaves
D3DTS_WORLD/VIEW/PROJECTION set. Every subsequent shader-driven draw Remix captures therefore
inherits the last converted draw's matrices. That is a plausible second source of misplaced
duplicates and has not been investigated.

### Run 5 (2026-08-16) — duplicate world, and the leftover-transform mechanism

Screen: world duplicated, the copy rotated onto its side.

**Root cause is the note left at the end of run 4, now acted on.** Only ~6% of draws convert;
the other ~2,400 stay shader-driven and Remix reconstructs them through **vertex capture**,
which reads `D3DTS_VIEW`/`D3DTS_PROJECTION`. The shim restores VS/PS after a converted draw but
leaves the transforms set — so with 3-4 cameras per frame, Remix rebuilt the entire remaining
world against whichever camera the last converted draw happened to use. A duplicate world at
the wrong orientation is exactly what that produces.

**Fix: `mainCameraOnly=1`.** The frame's main camera is latched as the first perspective pass
whose aspect matches the back buffer and whose position is not the world origin (Steelport's
coordinates are in the hundreds, so an origin camera is a utility pass). Draws from any other
camera are refused.

This solves two problems with one rule, and the second is the load-bearing one:
- auxiliary passes (shadow, reflection, dual-paraboloid — `Dual_Paraboloid_Transform` appears
  in 244 shaders) no longer put a second copy of the world into the scene;
- **the transforms left set after a converted draw are now always the main camera**, so vertex
  capture reconstructs the unconverted majority against the right matrices by construction
  rather than by accident.

Restoring the main camera after every converted draw was considered and rejected: it would
double the transform traffic across the bridge for no benefit, since restricting conversion to
main-camera draws already makes the leftover state correct.

New counter: `other-camera`. Worth reading next run alongside `distinct cameras this frame`,
which has been 3-4.

### Run 6 (2026-08-16) — shifting wall textures, and the instancing claim corrected

User: shifting wall textures, geometry slightly corrupted. Notably **no duplicate world** —
`mainCameraOnly` worked, and `distinct cameras this frame` is now **1**.

**CORRECTION — the instancing story was wrong.** Run 5 reported ~1,000 instanced draws/frame as
unconvertible because "fixed function has no per-instance transform". The measurement this run:

```
instancing: 861 instanced draws/frame carrying 861 instances/frame (largest single draw 1 instances)
```

**One instance each.** SR3 uses D3D9 instancing with a count of 1, which means the object's
transform lives in the per-instance vertex stream rather than in `objTM` — not that many copies
would collapse onto one matrix. These draws ARE convertible; the transform simply has to be read
from the instance stream. Refusing them is why conversion sits at 4.5%, and it is ~45% of the
world. Added `DumpInstancedDeclaration()` to log the full element layout and all four stream
strides for the first three such draws, so the instance transform can be decoded from
measurement next run instead of an assumed layout.

**Shifting wall textures — the mesh albedo cache.** This is the failure the SR2 proxy documents:
under memory pressure SR3 evicts a wall's unique texture and rebinds a shared fallback. The
game's own renderer hides it; Remix hashes whatever it is handed, so the surface visibly swaps
material. Fixed by remembering each mesh's first-seen albedo and rebinding it. Keyed on the
stream-0 vertex buffer **plus base vertex** — the buffer pointer alone would collapse every mesh
sharing one of SR3's large shared buffers onto a single texture. New counter:
`restored from mesh cache /frame`.

**Do not over-read the captures.** I previously called 500/729 mirrored meshes a mirrored
duplicate of the world. This run's capture shows 496/705 with the same `[-1,1,1]` scale while
the shim reports `mirrored 0/frame` and a handedness baseline of `+1.000` — so that scale is
almost certainly just Remix's left-to-right-handed conversion, present in every capture,
including before any of these changes. The user's on-screen reports have been the more reliable
signal throughout.

**Tooling fix:** `build.ps1` reported "OK" after a FAILED compile, because it only checked that
`sr3-rtx.asi` exists and the previous one was still there. It now deletes the output first. This
came within one step of deploying a stale binary and testing it as though it were new.

### Run 7 (2026-08-16) — the UV formula, read from the shader disassembly

User: textures/UVs not scaled correctly and seams look misoriented; trees weird.

**Resolved by disassembling the shaders instead of tuning a constant.** `ir_bbsimple2_decal_s`
vertex shader [0]:

```asm
mul r0.x, c1.x, v1.x        ; Normal_Map_TilingU * u_raw
mul r0.y, c2.x, v1.y        ; Normal_Map_TilingV * v_raw
mul o1.xy, r0, c3.x         ; c3.x = 0.0009765625 = 1/1024
```

So **uv = raw x tiling / 1024**. The 1/1024 confirms the measured divisor was right; what was
missing is the **per-material tiling pair**. Because U and V tile independently, a wrong pair
*skews* the texture rather than merely mis-scaling it — which is exactly "seams look
misoriented".

Two details that make a hardcoded implementation wrong, both found by checking a second shader:
- **The tiling registers move between shaders.** `ir_bbsimple2_decal_s` has TilingU/V at c1/c2;
  `ir_bbsimple_1uv_decal_s` has them at c2/c3. They must be read from each shader's CTAB.
- **Some shaders have no tiling at all** — `ir_at_sr3pccloth_s` is `mul o1.xy, c0.z, v1`, a bare
  scale — so the factors must default to 1.

Implemented: `ReflectShader` records the tiling registers (preferring `Normal_Map_*`, which the
disassembly shows driving the primary UV output wherever it exists), and the texture matrix is
built per draw as `(tilingU/1024, tilingV/1024)`, cached so it is only re-sent when it changes.
The 1/1024 is applied only to short2 coordinates; the tiling applies regardless of storage
format, because the shader multiplies by it either way.

New counter: `uv matrix writes/frame (last scale ...)`.

Method note: three runs of guessing at UV scale from measured ranges would probably have landed
on 1024 and stopped there, leaving every material with non-unit tiling wrong. Reading the
shader took one command and gave the whole formula including the part measurement could not
reveal.

### Runs 8-9 (2026-08-16) — instance transforms decoded, then a self-inflicted flicker

**The instance transform, read off the declaration dump:**

```
stream=4 offset= 0 type=float4 usage=POSITION index=2   |
stream=4 offset=16 type=float4 usage=POSITION index=3   |  3x4 world matrix
stream=4 offset=32 type=float4 usage=POSITION index=4   |
stream=4 offset=48 type=d3dcolor usage=COLOR index=1/2     (tint)
```

The ~860 draws/frame refused as "instanced" carry their world matrix as three float4 rows in
**stream 4** - the same row-major 3x4 layout as objTM, delivered through a vertex stream instead
of constants. The shim tracked only streams 0-3 and could not see it. Now tracks 8 streams,
parses the instance elements from the declaration, and builds WORLD from them.

**Then I broke it for performance.** Reading the instance record per draw would be ~860 buffer
locks a frame, each a bridge round trip, so the buffer was snapshotted once per frame and
indexed by offset. That is wrong: SR3's instance buffers are DYNAMIC and filled progressively
through the frame (lock, write a batch, draw it, lock, write the next). A snapshot taken at the
first use serves stale transforms to every later batch, and which objects get stale data shifts
with draw order, which shifts with the camera.

User, exactly: "objects flicker if there are instances of them... when I see multiple similar
objects next to one another, they flicker back and forth based on camera position, orientation".

**Fix without giving up the economy:** every `IDirect3DVertexBuffer9` shares one vtable, so
patching slot 11 (`Lock`) from the first buffer seen installs an invalidation hook for all of
them. A write-lock marks that buffer's snapshot stale; the next draw re-snapshots. One snapshot
per *fill* rather than per frame or per draw. New counters: `snapshots/frame` and
`invalidations/frame` - a non-zero invalidation count confirms the buffer really is refilled
mid-frame, i.e. that the original approach was unsound rather than merely unlucky.

**Skyboxes.** User identified the spheres/ovals around the player as skyboxes and made the right
point: they are supposed to be in the render, they just need handling rather than exclusion.
Remix 1.5.2 exposes exactly two mechanisms (confirmed by scanning `.trex/d3d9.dll`):
`rtx.skyBoxTextures` (hash list, already has 2 entries) and **`rtx.skyDrawcallIdThreshold`**
("the first N draw calls of a frame are sky"). The threshold handles a sky DOME rather than a
tagged texture. A one-shot probe now logs the first 24 draws of a frame with their depth state,
flagging those with depth writes or depth test disabled, so the threshold can be read off
instead of guessed. Not yet set - awaiting that measurement.

### Runs 8-10 (2026-08-16) — instancing lands; flicker traced to my own cache

**Instance transform decoded** from the declaration dump:

```
stream=4 offset= 0/16/32  three float4 rows, usage POSITION index 2/3/4  = 3x4 world matrix
stream=4 offset=48/52     d3dcolor COLOR index 1/2                       = tint
```

Same row-major 3x4 layout as objTM, delivered through a vertex stream. The shim tracked only
streams 0-3 and could not see it. Now tracks 8, parses the instance elements, builds WORLD from
them. Result: **conversion 3% -> 53.4%** (1,396 of 2,615 draws), 1,632 instanced draws converted
per frame, 0 refused, and `world matrix changes` 50 -> 1,012/frame - objects finally have
distinct transforms.

**Then a self-inflicted flicker.** To avoid ~860 buffer locks a frame I snapshotted each instance
buffer once per frame. SR3's instance buffers are dynamic and refilled mid-frame, so later
batches read stale transforms, and which objects were stale shifted with draw order - hence with
camera position. User: "objects flicker if there are instances of them... they flicker back and
forth based on camera position, orientation". Fixed by patching `IDirect3DVertexBuffer9::Lock`
(slot 11) - all vertex buffers share one vtable, so one patch covers every buffer - and dropping
a buffer's snapshot when the game write-locks it. One snapshot per *fill*. User confirms the
world is now stable.

Lesson worth keeping: the performance shortcut was reasonable, the *assumption underneath it*
(that the buffer is filled once) was never checked. Same shape of error as the SR2-derived
`objTM` gate.

### Open, with measurements added this run

- **54% of converted draws render untextured** (750/frame of 1,396). Only 67 of 1,983
  diffuse-samplerless pixel shaders carry a `Diffuse_Color` constant, so a constant-colour
  fallback does not explain them. Added a probe that logs the first sampler name of each
  distinct untextured material, so the class is observed rather than inferred.
  Also fixed: `AlbedoRank` matched "Diffuse" only as a PREFIX, so `Decal_diffuse_mapSampler` and
  similar scored 0 and were blanked despite naming a diffuse map. Now matches as a substring.
- **Mesh albedo cache was over-firing**: 921 restores/frame against 1,396 draws, far more than
  streaming eviction could explain. Cause: the key was (vertex buffer + base vertex), so one
  mesh drawn with several materials collapsed onto the first texture seen - the very
  substitution bug the cache exists to prevent. Pixel shader added to the key.
- **Sky is NOT drawn early.** Draws 2-23 all have depth write and depth test enabled, so
  `rtx.skyDrawcallIdThreshold` may be the wrong mechanism. The probe now scans a whole frame for
  depth-disabled draws wherever they fall, reporting vertex counts and whether they use objTM,
  to decide between the threshold and `rtx.skyBoxTextures`.
- **`other-camera` refuses 375 draws/frame** (14%). Not yet investigated; some may be legitimate
  scene geometry rather than auxiliary passes.

## Session 13 — 2026-08-17 — the mechanism found: skipping is not hiding

### The measurement

A state probe keyed on vertex count (`probeVerts` in the ini — vertex count is a stable mesh
property, so it picks one shape out of ~2,600 draws a frame) produced this, live:

```
verts=74 prims=117  target=14EDE580  ps=''                   rank=0    converted=0
verts=74 prims=117  target=14EE44D8  ps='Decal_MapSampler'   rank=100  converted=1
verts=52 prims=26   target=14EDE580  ps=''                   rank=0    converted=0
verts=52 prims=26   target=14EE44D8  ps='Diffuse_MapSampler' rank=100  converted=1
```

**The identical mesh is submitted twice per frame, to two render targets**: once into the
DSF/geometry prepass whose pixel shader samples *nothing*, and once into the material pass
carrying the real diffuse map. That is Volition's inferred lighting, directly observed.

### The bug, which was architectural and mine

**Refusing to CONVERT a draw does nothing to stop Remix seeing it.** Remix's vertex capture
reconstructs unconverted draws from vertex-shader output regardless. So the untextured prepass
copy of the entire world was being drawn on top of the correctly-converted material pass.

**That is what every "shell", "sphere", "oval" and "shape around the character" has been** —
across sessions 9-13. Not light volumes, not decals, not a texture problem. It explains why
`rtx.ignoreTextures` only ever chipped fragments off the circle (user: "changed the shape of the
circle"): the shapes are ordinary world geometry drawn in the wrong pass, so they have as many
materials as the world does.

### The fix: a three-way disposition

```
CONVERT  re-issue as fixed function; this is what Remix path-traces
HIDE     still drawn (the engine needs its output) but given an orthographic projection so
         Remix's orthographicIsUI keeps it out of the ray-traced scene
PASS     untouched; Remix reconstructs it. Only for REAL geometry we cannot convert yet
```

HIDE: prepass (no colour sampler), depth-only, auxiliary camera, mirrored, screen-space, light
volumes. PASS: skinned characters, unconvertible vertex formats, multi-instance draws — hiding
those would delete pedestrians and props from the scene.

Nothing is dropped. Every draw still executes, so the engine's light buffer and post chain are
untouched — the lesson from the sessions where dropping draw classes froze the image and crashed
the runtime.

### Method note

The user's instruction — study the engine's mechanisms rather than tune symptoms — is what found
this. Three sessions of property heuristics (`hideDepthlessUntextured`, texture ignoring, size
and distance tests) were all chasing a shape whose real identity was "the world, drawn in the
prepass". One probe that reported render target alongside sampler names settled it immediately.

Also added at the user's request: `dumpFrame=1` writes one complete frame to
`sr3-rtx-frame.log` — every draw in submission order, grouped by render target, with states,
shader properties and the disposition applied. That is the artefact for understanding the frame
structure rather than counting draws.

### Reverted this session

`rtx.ignoreTextures` additions (two disc materials). The user's standing preference is to avoid
ignoring textures: it removes a texture everywhere it is used, breaks when hashes change, and
here it was treating a geometry-pass problem as a material problem.

## Session 14 — 2026-08-17 — the freeze was never performance

### Measured, and it overturned two of my own fixes

```
TIMING: frame 17.9 ms avg (56 fps), worst 35 ms | shim 0.79 ms avg (4.4% of frame)
```

**56 fps.** The "freeze" was never a stall. I shipped two fixes aimed at lock stalls before
measuring - a `D3DLOCK_READONLY` on a dynamic buffer, and `MeasureWorldExtent` locking stream 0
on every converted draw (~1,400 locks/frame to feed a diagnostic whose *log* was capped but whose
*work* was not). Both were genuine defects and are worth keeping fixed, but neither was this
symptom. One timing build settled in minutes what two builds of reasoning did not.

### The actual cause

```
converted -> 14EBE570  fmt=113 (HDR)              : 186 draws/frame
converted -> 14EB8A60  fmt=34  (G16R16)           :   9 draws/frame
converted -> 14EB8D48  fmt=21  (A8R8G8B8 = BACKBUFFER) : 0 draws/frame
```

Nothing reaches the back buffer. SR3 renders into offscreen HDR targets and the **composite quad
is the only draw that writes the back buffer**, which Remix presents. `skipComposite=1` dropped
it, so the back buffer kept stale content and frames presented at 56 fps showed an unchanging
image. Exactly what session 8 recorded. `skipComposite=0` restored it and the user confirms the
freeze is gone.

**The user's reframing is the key insight:** the rasterised overlay *is* the game's composited
frame, so what looked like "the path-traced world freezing" was a frozen OVERLAY covering a world
that was updating fine underneath. That collapses two symptoms into one problem.

### Also settled by the same data

The z-fighting is **not** double conversion - 186 draws to one target and 9 to another, so the
same geometry is not being converted twice. The doubling is Remix capturing the unconverted
prepass alongside our converted material pass.

### The composite: three measured dead ends

| approach | result |
|---|---|
| pass through | quad welded to the camera |
| demote to UI | game's rasterised frame painted over the path-traced world |
| skip | back buffer never written, image freezes |

None is solvable at the D3D9 boundary, because the problem is not the draw - it is that Remix has
not been told this texture is the game's own composited output. `rtx.ignoreTextures` is the
designed mechanism for exactly that, and this is a legitimate use of it (one composite texture),
distinct from using it as a substitute for understanding geometry.

### Caution on capture analysis - my own tools disagreed

`tools/capture_near.py` reported 15 four-vertex quads at distance 1.0 in
`capture_2026-08-17_00-23-04.usd`; a second script on the SAME file reported nearest = 1.99 with
no such quads, and a third agreed with the second. **The "15 camera-welded quads" figure and the
"48 near-camera meshes -> 0" comparison drawn from it are therefore unreliable** and should not
be built on. Reconcile the scripts before quoting capture geometry again. Recorded because that
figure was used to declare the planes fixed.

### Engine facts established this session (these are solid)

- Frame structure: `1280x720 G16R16` geometry/DSF pass, `1280x720 A16B16G16R16F` material pass,
  three `400x288` HDR light buffers, final composite. Textbook inferred lighting, measured.
- **A draw whose result the engine READS BACK cannot be skipped, only hidden.** Skipping the
  prepass collapsed the engine's own submission from 2,615 to 593 draws/frame - almost certainly
  GPU occlusion culling reading it back.
- `rtx.raytracedRenderTargetTextures` is **empty**; the old note claiming 38 stale hashes is out
  of date.
- SR3 does use instance counts > 1 (10,000 seen); the earlier "always 1" claim was true only of
  the sample then in hand.

## Session 14 (continued) — architecture, cleanup, and the validation run

### The disposition model

Replaced the single "convert or not" decision with an explicit four-way one, because conflating
"do not convert" with "hide" was a real architectural bug:

```
Convert      re-issue as fixed function; this is what Remix path-traces
Hide         still drawn (engine needs it) but given an orthographic projection
Skip         never reaches the device
PassThrough  untouched; Remix reconstructs it from shader output
```

`Hide` turned out NOT to mean invisible - Remix rasterises UI as a 2D overlay - and `Skip` breaks
the engine when it reads the draw back. Both are recorded as dead ends in YOUR-INSTRUCTIONS.md.

### `ffp=0` validation run — the approach is sound

With the master switch off every counter read zero (a true no-op: no conversion, no light
injection, no camera publishing) and the user reported **completely unmodified graphics**. So
**Remix does not path-trace SR3 without this shim** - it falls back to rasterising, which is the
original problem the project exists to solve. Worth having measured rather than assumed.

Same run established that the **missing shadows are Remix's, not ours** - they are absent with
the shim fully inert. Remix does not reproduce the game's rasterised shadow-map path.

### Settings cleanup: 24 -> 16

Removed with evidence (`skipDepthOnly` inert, `finiteFar` inert on SR3, `startupDelayMs` unused,
`skipComposite` harmful) and three settled ones hardcoded (`perspectiveOnly`, `mainCameraOnly`,
`uvScaleDenom` = 1024). The rationale is written into `configs/sr3-rtx.ini` so they are not
re-added from first principles. Verified programmatically that the ini and the code now define
exactly the same 16 keys.

### Bugs found by auditing the code against these notes

- `BeginUIDemote` fell back to a SECOND property test, re-classifying draws `Classify` had
  already decided - turning real geometry into UI overlays.
- `hideDepthlessUntextured` was built on a theory the frame dump disproved. Removed.
- Light volumes were routed to `Hide`, i.e. rasterised as a visible overlay.
- `Hook_DrawPrimitive` never set the mesh-cache key, so it keyed on whatever the previous indexed
  draw left behind and mis-assigned textures.
- `ShadowGetRS` returns 0 on a failed query, and D3D9 pure devices fail every `Get*` - a spurious
  0 on `COLORWRITEENABLE` would have skipped the entire world. Render states we make decisions
  from are now seeded with D3D9 defaults.
- The frame dump's disposition labels were one entry short after `Skip` was inserted into the
  enum, so every skipped draw printed as "PASS". That mislabelling hid 2,400 wrongly-skipped
  draws per frame.

### rtx.conf: Remix owns this file

The user tagged textures in the Remix UI and saved, which rewrote `rtx.conf` in the game
directory - adding `-0x4B17F5ECBA4D9E4D`, `-0xA22BB20412CCB5BB`, `-0xC75ABEE33CC2F1EF` to
`ignoreTextures`. A routine deploy nearly overwrote that work. The game's copy was pulled back to
`configs/` (old one kept at `configs/rtx.conf.pre-remix-save.bak`).

It also settled the sign convention: Remix writes `-0x4FAE8190C113287B` where a hand-written
entry had `0x4FAE8190C113287B`. **Hand-written hashes have been the wrong sign and probably never
matched.** Only hashes Remix writes itself should be trusted.

### State at handoff

Working: ~50% conversion, instancing, UVs, lights ~60/frame, mesh albedo cache, 56 fps at
0.6-4.4% shim cost. Broken: z-fighting from unconverted passes, partial raster overlay, most
surfaces white, characters excluded.

Next step: the **marker-texture** plan in YOUR-INSTRUCTIONS.md - bind our own 4x4 texture to the
sampler-less prepass draws (whose output cannot be affected by it) and ignore that single hash,
giving Remix a handle to skip them without touching any game texture.

## Session 15 — 2026-08-17 — the marker texture, built and deployed

### State check first

Deployed `.asi` matched `build/` by hash, and both `configs/sr3-rtx.ini` and `configs/rtx.conf`
were byte-identical to the game's copies. Nothing had drifted since session 14, so no
reconciliation was needed. The last gameplay run on disk is run 18, the `ffp=0` baseline.

### The prepass identification is now arithmetic, not inference

Run 17's per-frame counters, the last run with `ffp=1`:

```
draws 1827 | converted 314 | untextured(prepass) 877 | skinned 394
no-albedo 100 | vertex-format 19 | other 57 | screen-space 33 | other-camera 26
```

`314 + 394 + 100 + 19 + 57 = 884`, against 877 sampler-less draws. **The prepass carries one copy
of every mesh the material pass carries**, and those 877 draws are currently passed through, so
Remix's vertex capture reconstructs a complete second world on top of the 314 we convert. The
two-pass structure was already measured in session 13 with the state probe; this is the same fact
falling out of the population counts independently, which is worth having.

### Built: `hiddenPassMode=3`, the marker

A 4x4 A8R8G8B8 texture created at device init and bound to stage 0 for the duration of exactly
the draws we need Remix to drop. The draw is otherwise untouched — it still executes, still writes
the prepass target, and the engine's readback is unaffected. Remix reads stage 0 as albedo, so one
hash now names this pass and can go in `rtx.ignoreTextures` or `rtx.hideInstanceTextures`.

Safety is a *test*, not an assumption. `HiddenDisp` is reached from five call sites — the prepass,
light volumes, orthographic passes, auxiliary cameras, mirrored passes — and only the prepass has
a shader that samples nothing. Marking anything whose shader does sample would change what the
game renders, so mode 3 marks only draws with an empty sampler list and counts the rest as
"refused", which pass through as they do today. That count is in the per-frame log: if it is
large, the marker is not covering the population it was aimed at.

Details that mattered:

- **`D3DPOOL_MANAGED` would have failed silently.** SR3 comes up through `CreateDeviceEx`, and
  D3D9Ex rejects the managed pool. The marker is created in `D3DPOOL_DEFAULT` and filled through
  a `D3DPOOL_SYSTEMMEM` staging texture plus `UpdateTexture` — which is also the upload Remix
  hashes.
- **Stages 1-7 are cleared too.** D3D9 texture state is sticky and the frame dump shows all 2,043
  `ps=''` draws arriving with a real game texture still bound; zero of them had a null stage 0.
  Leaving other stages populated would let Remix hash one of those instead of ours, and the
  ignore would silently never fire.
- **Magenta on purpose.** Until the hash is tagged, the marked pass renders bright magenta. That
  is the diagnostic: a magenta duplicate world is direct visual proof that the doubling is the
  prepass, and it makes the texture trivial to find and tag in the Remix UI.
- A `Disp::Count` sentinel plus a `static_assert` now ties the frame dump's label array to the
  enum. That is the exact bug from session 14 that mislabelled 2,400 skipped draws a frame as
  "PASS"; it can no longer recur.

Built clean, deployed, verified by hash both ways.

### What the run will settle

`rtx.ignoreTextures` is documented in the runtime as "completely ignoring such draw calls", and
`rtx.hideInstanceTextures` as "hidden from rendering, but not totally ignored ... allowing the
hidden objects to still appear in captures" (both strings read out of `.trex/d3d9.dll`). Neither
description says whether the underlying rasterisation into the game's own render target survives.
That is the open question, and it is the same one `Disp::Skip` answered badly:

- If the engine's draw count stays near 1,800/frame, the raster survives, the readback is intact,
  and the hiding problem is solved for the prepass.
- If it collapses toward ~600/frame the way `Skip` did, then Remix's ignore removes the raster as
  well, and the marker's value is that it lets us try `hideInstanceTextures` and the other
  categorisation lists against one safe hash instead of guessing with game assets.

Either way the shim's own counter reports it, so one run settles it.

### Queued next, not yet tried: `rtx.useVertexCapture = False`

Vertex capture is what reconstructs unconverted shader draws — it is the mechanism that produces
every duplicate. It has been on since session 1, when it was the *only* thing putting geometry in
the scene. That is no longer true: our converted draws are fixed function, which Remix path-traces
natively and which does not need capture at all.

Turning it off would remove **every** unconverted class from the ray-traced scene at once — the
877 prepass draws, the auxiliary cameras, the mirrored passes — with no shim change and no
texture hashes. The cost is that the honest pass-throughs go too: 394 skinned draws a frame
(characters, pedestrians) and the unconvertible vertex formats. That is a real loss, but it is
also exactly the population the skinning port is meant to convert, and a clean world with no
characters is a far better place to work from than a doubled one.

It is a config toggle, instantly reversible, and it needs no build — so it is the cheapest
remaining experiment. Not bundled with the marker run: one variable at a time.

## Session 15 (continued) — 2026-08-18 — the marker works. The z-fighting is solved.

### Result

The user ran it, saw the magenta duplicate, tagged it in the Remix menu and saved.
**The duplicated world and the sphere are gone and the z-fighting on world geometry is solved.**
Characters still flicker, which is the known pass-through population awaiting the skinning port.

That closes the problem this project has been circling since session 9, under the names "shell",
"sphere", "oval" and "shape around the character". Log archived as
`docs/evidence/sr3-rtx-fork-run19-marker-works.log`.

### The important engine fact this settled

The open question was whether `rtx.ignoreTextures` also removes the underlying rasterisation, in
which case the engine would lose the prepass readback and collapse the way `Disp::Skip` did.
**It does not.** Run 19, live, with the marker ignored:

```
draws 3649/frame   (Skip had collapsed the engine's own submission 2,615 -> 593)
SKIPPED entirely 0/frame (mode 3)
MARKED with the marker texture 1737/frame (refused as unsafe 728/frame)
```

The engine submits normally, so its occlusion culling still gets what it reads. So:

> **Remix's ignore removes a draw from the ray-traced scene while the game's own rasterisation
> and readback survive. That is precisely what the D3D9 boundary could not do, and it is the
> general solution to the hiding problem.** Anything the engine reads back can now be hidden from
> the path tracer by marking it, without skipping it.

### The refused population is exactly the remaining duplicate source

`other-camera 560 + light volumes 168 = 728`, which is the refused count to the draw. Those two
classes have shaders that DO sample, so marking them would change what the game renders and they
still pass through for Remix to reconstruct. They are the next unconverted duplicates, and they
are a plausible contributor to the character flicker - worth checking before assuming skinning
is the whole story there.

### The cost: 56 fps -> 36, and it was mine

```
TIMING: frame 27.9 ms avg (36 fps) | shim 6.85 ms avg (24.5% of frame)
```

Session 14 measured the shim at 0.79 ms (4.4%). Draws roughly doubled between the two scenes,
which explains maybe 1.6 ms of it; the rest is marking. `BeginMark` cleared stages 1-7 as well as
binding stage 0, so at 2(1+N) `SetTexture` calls across ~1,700 marked draws it was adding on the
order of 10,000 bridge round trips a frame.

**This is the exact failure `docs/sr2-fork.md` section 6 records from SR2** - their own FFP
conversion, not the game, generating 4,029 `SetTexture` calls a frame and pinning a 4070 Ti at 37%
GPU. It is written down at the top of the design document, it is the reason the state shadow
exists, and I walked into it anyway by adding a defensive loop I had no evidence for.

Fixed: **stage 0 only**, which the run itself disproved the need for a wider clear - the ignore
fired with the marker on stage 0, so stage 0 is what Remix hashes. That is 2 calls per marked
draw, the same as the converted path which measured at 0.79 ms for 1,400 draws. If Remix did need
the other stages cleared the failure announces itself: the magenta comes back.

Also added a `SetTexture calls/frame` counter for marking, because "how much of that 6.85 ms was
this?" should not have needed estimating.

### Also fixed: a counter that did not count what it said

`g_skipNoAlbedo` incremented for every draw that reached the end of `Classify`, so the log line
"no-albedo materials left to Remix" read 1,403/frame against 843 converted draws - it was really
reporting converted + other-camera. Now conditional on `albedoRank == 0`. This file has already
lost a session to a mislabelled counter (the `kDisp` drift that hid 2,400 wrongly-skipped draws);
a counter whose name does not match its arithmetic is worse than no counter.

### Dead end recorded: do not try to compute Remix's texture hash

Two hashes were added to `rtx.ignoreTextures` by the user's save, `0x978271113F293CE4` (also
mirrored into `rtx.uiTextures` as negative, which is just how Remix serialises the two lists) and
`-0xFBC6C5FFCC8AD259`. A plain XXH64 of the marker's 64 bytes of texel data gives
`0x96038DDB2CB121EB`, matching neither, so Remix folds in something else - dimensions, format,
mip layout or a different seed. **Do not try to hand-compute a hash to save a round trip.** Tag it
in the UI and save; that remains the only reliable way, exactly as the sign-convention lesson in
session 14 already said.

## Session 15 (continued) — 2026-08-18 — a crash on fast movement, and a lifetime bug that explains it

### The report

The game dies roughly one time in three when moving fast, or freefalling from the penthouse roof.
No dump in `.trex/` newer than 2026-08-15 (both predate the fork), and `sr3-rtx.log` simply ends
mid-line - which, since `Log` flushes every write, means abrupt process death rather than a hang.

### The defect found by audit

`g_meshAlbedo` stored raw `IDirect3DBaseTexture9*` pointers **with no AddRef**, and the cache's
entire purpose is to hold a texture across the streamer evicting it. So it deliberately held a
pointer across the one event that destroys the object, and then bound it with `SetTexture`. That
is a use-after-free, and its trigger is heavy streaming - which is precisely fast movement and
freefall, and not standing still. Intermittency around a third fits: whether it faults depends on
whether the freed allocation has been reused or unmapped yet.

Run 19 had 2,921 entries cached and "restored from mesh cache 234/frame", so the path is hot.

**It is also a texture-substitution bug short of the crash.** A freed texture's address gets
recycled by the next one streamed in, and the cache then binds an unrelated texture to the mesh.
"Surfaces flickering through unrelated images" is a symptom this project has chased before under
other explanations - `excludeRTAlbedo` was written for one version of it.

`g_rtTextures` had the same flaw. It is only ever compared against, never bound, so it cannot
crash - but a recycled address would permanently misclassify a newly created texture as generated
data and refuse it as albedo.

### Fixed

Both containers now hold a reference. `g_meshAlbedo` is capped at 4,096 entries, because a
reference held is a texture the streamer cannot evict and pinning an unbounded number of them
would trade a crash for VRAM exhaustion; at the cap it stops growing and says so in the log.
`g_rtTextures` holds ~55 entries, so pinning them costs nothing.

### And made diagnosable, because the above is a hypothesis

A strong hypothesis is still a hypothesis, and this project's own notes are emphatic about the
cost of fixing before measuring - the "freeze" that was 56 fps cost two builds. So the shim now
installs an unhandled-exception filter that records, to `sr3-rtx.log`:

- the exception code, the faulting address, and the module + offset it falls in;
- for an access violation, whether it was a read or a write and **what address was touched**;
- what the shim was doing: frame, draw index, the albedo last bound, and **whether the mesh cache
  supplied it**.

Then it writes `sr3-rtx-crash.dmp` next to the exe (`tools/read_minidump.py` already exists to
read it) and chains to whatever filter was there before, so nobody's handling is lost. dbghelp is
loaded at crash time rather than imported, so a missing dbghelp.dll costs the dump, not the
process.

**If the faulting address equals the last-bound albedo and the cache supplied it, the diagnosis is
finished on sight.** The filter is re-installed every 600 frames, because the game and the Remix
bridge both install their own well after we start and the last caller wins.

### The control, if the fix does not hold

`cacheMeshAlbedo=0`. It needs no build. If crashes continue with the cache off, the cache is not
the cause and the dump is the next move; if they stop with it off but not with the AddRef fix in,
the reference counting is wrong somewhere. Worth remembering that the cache may now be redundant
anyway - `excludeRTAlbedo` addresses one of the two symptoms it was written for, and address
recycling in this very bug could account for the other.

### Deploy note

The rebuild was ready while the game was running, and the copy failed with "Device or resource
busy" rather than silently doing nothing. That is the "verify deploys by hash, every time" lesson
working as intended. **Not yet deployed** - waiting on the game being closed.

### Run 20 — crashed while driving, on the build without the handler

The crash reproduced before the fixed build could be deployed (the game was running, so the copy
refused rather than silently leaving a stale binary). So there is no dump for this one - the
running build was the stage-0 marker fix, `95889d01`, which predates the crash handler. Log kept
as `docs/evidence/sr3-rtx-fork-run20-crash-driving.log`; it ends mid-camera-line as before, with
the camera moving ~10 units per sample, i.e. driving.

**Supporting evidence for the streaming hypothesis**, from the cache size counter:

| | frame 2400 | frame 3000 | frame 3600 |
|---|---|---|---|
| meshes cached (driving, run 20) | 1,332 | 2,569 | 2,879 |

Run 19, on foot, reached 2,921 entries only by frame **24,000**. Driving churns the cache about
seven times faster, which is exactly the eviction pressure a use-after-free in that cache would
need. Circumstantial, but it points the same way as the audit.

Also confirmed in the same log: **1,318 SetTexture calls against 659 marked draws - exactly 2.0
per draw**, so the stage-0-only marker fix is behaving as designed, and no magenta returned.

### Deployed for the next run

`cf95a6e3` - reference counting on both caches, the 4,096 cap, and the crash handler. Verified by
hash. **Leave `cacheMeshAlbedo=1` for this run**: it tests the actual fix, and if it crashes
anyway the dump points somewhere new. Turning the cache off is the fallback that isolates it, not
the first move.

One consequence to watch that is not yet measured: a held reference is a texture the streamer
cannot evict, so a long drive pins up to 4,096 textures. The log announces the cap. If that
produces VRAM pressure or stutter, the right question is whether this cache should exist at all
rather than what the cap should be.

## Session 15 (continued) — 2026-08-18 — run 21: the crash is fixed

The user could not reproduce the crash on the fixed build (`cf95a6e3`). No `sr3-rtx-crash.dmp`, no
`CRASH` line in the log, 21,600 frames of driving and freefalling.
Log at `docs/evidence/sr3-rtx-fork-run21-nocrash.log`.

**The caveat that belongs on any intermittent bug:** the crash was roughly one attempt in three,
so N clean attempts only buys 1-(2/3)^N confidence. Not reproducing it is strong evidence, not
proof. The crash handler stays installed - if it ever fires it will say in one line whether the
mesh cache was involved.

### The cap was reached, and it cost nothing visible

`mesh albedo cache full at 4096 entries` appears early (log line 695), so the run spent almost all
of its 21,600 frames with 4,096 textures pinned against the streamer, and neither the crash nor
any reported stutter followed. The worst-frame outliers (5,274 ms once, then 377/262 ms) look like
load and streaming hitches rather than a trend. So the pinning concern raised when the cap was
added is, on this evidence, not a problem - but it is one run in one part of the city.

### Correcting a number I gave too early

I reported the marker's cost fix as "0.39 ms, 2.1% of frame". That was a cumulative average read
at frame 12,000 in a nearly empty scene, and it was not a fair figure. The dense-city reading is
**5.01 ms, 18.3% of frame at 37 fps**. The fix did work - the mechanism check is exact, 2,690
SetTexture calls against 1,345 marked draws, 2.0 per draw where it used to be ~6 - but the shim
still costs real time.

A rough decomposition from the two runs (different scenes, so an estimate rather than a
measurement):

```
run 19:  843 converted, ~10,400 marking calls -> 6.85 ms
run 21: 1079 converted,   2,690 marking calls -> 5.01 ms
```

Solving the two gives roughly **4 us per converted draw and 0.36 us per bridge SetTexture call**,
i.e. marking now costs about 1 ms and conversion about 4 ms. **That settles where to optimise if
it ever matters: not the marker.** Cutting marking to zero would buy ~1 ms of a 27 ms frame. The
conversion path is the cost, and it is the thing actually producing the path-traced world.

## Session 16 — 2026-08-18 — the post chain, the second prepass, and a broken instrument

Three problems attacked with one build. Two are fixed by evidence found this session; the third
turned out to be blocked on an instrument that was sampling the wrong moment.

### First: the frame dump was lying, and everything else depended on it

The dump triggered on the **first frame that has a camera** - which is a loading screen. The
2026-08-18 dump recorded 94 converted draws where steady-state gameplay has 1,079, and its 1,302
material-pass pass-throughs were mostly "main camera not latched yet" rather than anything
structural. Its render-target populations described a half-built scene.

`dumpFrame` is now a **delay in frames after the camera appears** (default 1800, ~45 s), and every
line carries the REASON its disposition was chosen - "auxiliary camera (view differs from the
frame's main one)", "skinned, awaiting the skinning port", "prepass: stipple, no colour sampler".
Previously the dump said *what* happened and left *why* to be re-derived, which is why the
473-per-frame other-camera population could not be characterised from it.

An instrument that samples the wrong moment is worse than no instrument, because its numbers still
look authoritative. Both figures quoted from that dump in the session 15 notes should be treated
as loading-screen data.

### #1 The rasterised overlay is the whole post chain, not "the composite quad"

The frame dump lists **49 screen-space draws (no projTM), every one of them PASS**. Read in
submission order they are the entire back end of the renderer:

```
1923  depth_sampler            -> G16R16 prepass target
1951-1993  IR_GBuffer_Lighting / IR_GBuffer_Depth resolves   (19 fullscreen quads)
1994-2001  light-volume meshes  -> the 400x288 light buffers
2006-2016  base_sampler blur    -> 512x512
3523-3528  auto-exposure reduction: 320x180 -> 64x64 -> 16x16 -> 4x4 -> 1x1 -> 1x1
3529       colour LUT           -> 256x128
3530, 3533 base_sampler         -> 037B84B0 1280x720 X8R8G8B8 = THE BACK BUFFER
```

Remix reconstructs all 49 and rasterises them over the path-traced world. So "a partial rasterised
image overlays the path-traced world" was never one quad - it is the game's whole deferred resolve
and post chain being drawn on top.

**Fixed by `screenSpaceMode=2`**: mark the ones that sample a render target. The separator is what
the quad SAMPLES - a render-target texture means it is compositing the engine's own output, which
is exactly what we replace, so marking is safe by construction. An authored texture means the HUD,
a light cookie or the LUT, which must still be drawn; those stay pass-through. A null stage 0
groups with the render targets, since there is no game texture to preserve.

That separator has been described in a code comment since the fork. **The code never implemented
it** - it demoted or passed through every screen-space draw alike.

### #3 "Most surfaces white" is a SECOND prepass shape, found by disassembly

The open question in YOUR-INSTRUCTIONS was where sampler-less materials get their colour. The
question was wrong, and so were its numbers - it claimed 1,983 sampler-less pixel shaders when the
corpus has **307**.

The real white population is different: **415 pixel shaders that DO declare samplers, but only
utility ones.** 236 sample `IR_Stipple_Pattern_2D` alone; 179 sample it plus `Normal_Map`. None
names a colour map, so `rankAlbedo` unbinds stage 0 and they convert WHITE.

Disassembly settles what they are. `ir_bb_tod_window_bs.fxo_pc`:

```
shader [6]  Normal_Map (s0) + IR_Stipple_Pattern_2D (s11)
            texld r0, v0, s0 ; mad r0.yzw, r0.xxyw, 2, -1 ; rsq/rcp   <- normal decode
            consts: Specular_Power, Normal_Map_Height, IR_Pixel_Steps, IR_Stipple_Repeat_Info
            -> writes G-buffer normal + specular power. NO COLOUR ANYWHERE.

shader [8]  Diffuse_Map, Decal_Map, Specular_Map, IR_LBuffer, IR_GBuffer_DSF_Data,
            Single_Paraboloid_Map + Tint_color, Fog_color, Self_Illumination
            -> the material pass.
```

Same file, two shader indices, the two halves of inferred lighting. So the prepass signature is
not "samples nothing" - it is **"samples no surface colour, and samples the IR stipple"**. We were
converting the normal prepass and painting it white, coincident with the correctly textured
material pass. Every rank-0 converted draw in the frame dump was one of these.

**The safety argument for marking them is a register fact, not a guess.** Stage 0 holds the NORMAL
MAP; the stipple that drives the dithered discard sits at **s11** and our marker never touches it.
So the depth this pass writes - and therefore the engine's occlusion culling, which reads it back -
is unchanged. Only the G-buffer normal changes, and that feeds the light buffer and the composite,
both of which are now hidden.

### #2 deferred, deliberately

The auxiliary cameras and light volumes still pass through. Marking them is NOT safe today: their
shaders sample real data at stage 0, so rebinding it changes what the game renders. That objection
weakens once #1 lands - if the game's raster never reaches the screen, corrupting it costs
nothing - but that is a claim to make after #1 is confirmed, not alongside it. `HiddenDisp` now
takes an explicit `markSafe` argument supplied by the caller, so each class states its own reason
rather than a second classifier re-deriving one.

The steady-state dump with reasons is what #2 needs, and this build produces it.

### Deployed

`b6a25fda`, verified by hash. ini and code define the same 16 keys, checked programmatically.

## Session 16 (continued) — 2026-08-18 — the magenta overlay explains itself, and #3 gets its real answer

### The magenta overlay was a config collision, not a logic error

User: *"a magenta texture stuck as an overlay, visible in free cam, kinda like how the UI is
supposed to work. If I turn off ray tracing all I see is magenta."*

Both halves are exactly right, and together they name the cause. The marker hash was in **two**
Remix lists:

```
rtx.ignoreTextures = ... 0x978271113F293CE4 ...
rtx.uiTextures     = ... -0x978271113F293CE4 ...     <- same texture, Remix's other sign convention
```

For 3D prepass geometry the ignore fires and it vanishes; for a screen-space quad the **UI
classification wins, and Remix rasterises UI as a 2D overlay**. So the composite quads were being
marked correctly, then drawn on top as UI - "kinda like how the UI is supposed to work" is a
precise description of what Remix was told to do. "Ray tracing off shows only magenta" confirms
the other half: the game's own post chain really is sampling our marker, so its back buffer is
magenta.

Removed `-0x978271113F293CE4` from `rtx.uiTextures` in both `configs/` and the game directory.
This is deleting the exact string Remix wrote, not hand-writing a hash, so the sign-convention
trap does not apply.

**A first attempt to check this missed it.** A script normalising the two lists compared
`0x9782...` against `-0x9782...` as different unsigned values and reported "neither hash is in
uiTextures". Session 14 had already recorded that Remix writes the same texture with opposite
signs in different lists; the script did not encode that. Worth remembering that a normaliser is
itself a hypothesis.

### #3 has two populations needing OPPOSITE treatment

The widened instrument showed the first fix caught only 19 draws a frame while **233 still
converted white**. Splitting them by sampler, and checking whether each duplicates an existing
converted mesh:

| population | draws | shares a shape with a coloured convert | render target | verdict |
|---|---|---|---|---|
| `Normal_MapSampler` only | 83 | **83 of 83** | G16R16 prepass | duplicate - hide it |
| `IR_GBuffer_DSF_DataSampler` | 150 | **0 of 150** | material pass | unique geometry - hiding it would punch holes |

That check is the whole point. Hiding the second group on the strength of "no colour sampler"
would have removed 150 real surfaces a frame, which is precisely the failure recorded as a dead
end when `albedoRank == 0` was used as a prepass test.

**So what colours the second group?** The shader filenames answer it - `ir_bbsimple2_nodiffmap` -
and the disassembly proves it. `ir_bbsimple2_nodiffmap_bs.fxo_pc` shader [8], last instruction:

```
mul_pp oC0, r1, c37        ; r1 = accumulated lighting, c37 = Tint_color
```

The constant **is** the albedo, not a modulation on top of one. 117 of the 121 DSF-sampling
shaders without a colour map carry `Tint_color`; 20 carry `Base_Paint_Color` (vehicle paint), 18
`Diffuse_Color`, 10 `Glass_Color`. Only 4 carry none.

**This answers the question YOUR-INSTRUCTIONS carried for weeks** as "where do those materials get
their colour?" - and the old framing was wrong twice over: it quoted 1,983 sampler-less pixel
shaders (the corpus has 307) and looked for `Diffuse_Color` (the answer is overwhelmingly
`Tint_color`).

Implemented as `ConstantAlbedo`: read the ranked colour constant out of the shadowed pixel-shader
registers, pack it, and select `D3DTA_TFACTOR` instead of `D3DTA_TEXTURE` for stage 0. That is the
fixed-function equivalent of the `mul` above. Alpha is forced opaque, because several of these
carry `Opacity_fade` in the alpha channel and Remix would read sub-1.0 alpha as translucency - the
same trap that once turned every wall to X-ray.

A related bug fixed while doing it: `SetupLighting` also wrote `D3DRS_TEXTUREFACTOR`, and it runs
once a frame AFTER `SetupTextureStages`, so the first converted draw of every frame would have
lost its constant colour. The write is gone; the register now has one owner.

### The prepass test widened, with the check done first

`prepassSamplersOnly` - every sampler the shader declares is a stipple, normal, depth or shadow
map. **Positive identification**: an unrecognised sampler name keeps the shader a material. That
is the safeguard the old `albedoRank == 0` test lacked when it hid 2,400 real draws a frame.

### #2 implemented, switched off

`markAuxCamera=0`. The 415 auxiliary-camera draws a frame are 395 `Diffuse_Map` draws into one
**512x288 A16B16G16R16F** target - 16:9 at 0.4 scale, so a reflection or secondary-view render,
not a shadow map. Marking them rebinds stage 0 and changes what the game renders into that buffer,
which is harmless only once the game's raster can no longer be seen. So it waits one run.

### The stage-0-only marker is not enough for screen-space draws

Removing the marker from `rtx.uiTextures` did NOT stop the magenta sheet. The user relaunched with
that fix in place - `rtx.conf` on disk confirms it, marker gone from `uiTextures`, other 27
entries intact - and still saw it. Clearing the **entire** `uiTextures` list in the Remix menu is
what stopped it.

That rules out the simple explanation and points at the stages we stopped clearing. A post quad
arrives with the game's own textures still bound on stages 1-7, several of which are legitimately
tagged as UI, and **Remix categorises a draw from any bound stage**. One UI-listed texture
anywhere makes the whole draw UI, UI is rasterised as a 2D overlay, and the overlay's colour is
our magenta stage 0.

So `BeginMark` takes a `clearAllStages` flag, set by `Classify` only for the screen-space post
class. This is the wide clear that was removed for cost earlier the same day - reinstated for the
**~23 draws a frame that need it** rather than the ~1,700 that did not. Under 400 bridge calls a
frame, against the ~10,000 that made the first version cost 6.85 ms.

The general lesson, which the ignore-list result had hidden: **marking a draw controls what Remix
uses as its albedo, but not what Remix uses to CATEGORISE it.** Categorisation reads every stage.

### Note for whoever reads this next: the user's untag is not on disk

`rtx.uiTextures` in `configs/rtx.conf` and the game directory still holds its 27 entries. The
"untagged all UI textures" state that cleared the magenta lives only in the running Remix session.
**It should not be saved**: it works by removing the HUD's UI classification along with everything
else, and the code fix above makes it unnecessary.

### Run 23 — the magenta is almost gone, and Remix rewrote the sign convention again

User: *"the magenta is gone. In some rare cases I got it blocking the camera even the free cam,
but it's gone almost all of the time. One thing that fixed almost all of the magenta was untagging
all of the UI textures I tagged by hand."*

That is #1 essentially solved, and the residual is the case the `clearAllStages` fix targets: a
post quad whose leftover stage bindings still let Remix categorise it as UI.

**Remix rewrote `rtx.uiTextures` on the way out**, and the rewrite is worth recording. Same 27
entries, same magnitudes - but **15 of them flipped from positive to negative**:

```
before:  0x09D3..., 0x196F..., 0x2CB8..., 0x37D8..., 0x3AE0...   (18 positive)
after :  0x09D3..., 0x2CB8..., 0xFC7B..., -0x196F..., -0x37D8...  (3 positive)
```

`0x196FBE2CAB23CB16` and `-0x196FBE2CAB23CB16` are different 64-bit values, so these are not
cosmetic. This is the session 14 lesson recurring: **hashes that were not written by Remix itself
have been the wrong sign, and a wrong-signed entry simply never matches.** The user's untag/retag
cycle made Remix re-serialise the list in its own convention, which is a plausible part of why
"untagging the ones I tagged by hand" changed so much.

Game copy pulled back to `configs/rtx.conf`; the previous one is at
`configs/rtx.conf.before-ui-untag.bak`.

### Deployed

`fabf9f0b`, verified by hash, with `sr3-rtx.ini` and `rtx.conf` in sync. Carries:

1. screen-space post chain marked **with all eight stages cleared** - the residual-magenta fix;
2. prepass test widened to normal/stipple/depth-only shaders (83 draws/frame, each verified to
   duplicate a mesh we already convert);
3. **constant-colour materials** - 150 draws/frame that rendered white now take their albedo from
   `Tint_color` / `Base_Paint_Color` / `Glass_Color` via TFACTOR;
4. `markAuxCamera=0`, ready to flip once the overlay is confirmed gone.

## Session 16 (continued) — the residual overlay was a classifier ORDERING bug

The build with `clearAllStages` ran and the magenta is gone almost everywhere. The steady-state
dump then showed exactly what was left: **31 screen-space draws still passed through**, and they
split into two causes, neither of which was the marker mechanism.

### 21 lighting-resolve quads never reached the screen-space test

```
2044-2088  v=6  ps='IR_GBuffer_DepthSampler'    tex0=00000000  -> 1280x720 fmt=113   PASS
2040-2042  v=6  ps='IR_GBuffer_LightingSampler' tex0=...(RT)   -> 1280x720 fmt=113   PASS
```

`tex0 = NULL` should have matched the mark condition outright. It never got the chance: `Classify`
tested **light volumes before screen space**, and these quads carry `IR_Light_*` constants, so the
light-volume branch claimed them and refused to mark them (`markSafe = false` for volumes, which
is correct for a 3D hull).

**A fullscreen quad that RESOLVES lighting is not a light volume.** The volume test is about a 3D
hull whose shader happens to carry a light's parameters; a screen-space draw is part of the
composite chain whatever its constants say. Screen space is now tested first - shape before
contents.

Light injection is unaffected, and that is worth stating rather than assuming: `EmitLight` runs in
the draw hook *before* `Classify`, so it has already harvested those constants either way.

### 1 depth resolve our render-target tracking cannot see

```
2022  v=6  ps='depth_samplerSampler'  tex0=14EC8598  (not flagged RT)  -> G16R16 prepass
```

`g_rtTextures` is populated from `Hook_CreateTexture`, and a depth surface is made with
`CreateDepthStencilSurface` - a different vtable entry we do not hook. So the set genuinely cannot
know about it.

Rather than hook another entry point, the mark condition gained a third clause: **the shader names
no colour sampler at all**. A composite quad reading generated data never does; the HUD, the LUT
and the light cookies (`Diffuse_Map_1` into the 400x288 light buffers) all do, and are left alone.

The full test for "this quad is compositing the engine's own output" is now: stage 0 is a known
render target, OR stage 0 is null, OR the shader names no colour sampler.

### Confirmed working in the same dump

- Both back-buffer writes (`03769FB8 1280x720 fmt=22`) are `MARK`ed.
- The whole auto-exposure reduction chain 320x180 -> 64x64 -> 16x16 -> 4x4 -> 1x1 is `MARK`ed.
- `constant-colour materials 204/frame` - the `Tint_color` path is live.
- The widened prepass test went from 19 to **263 draws/frame**.
- Conversion is up to 1,503 draws/frame (30.2% of 4,975).

### Known risk, stated rather than discovered later

The third clause is the loose one. A HUD element whose shader names no colour sampler would now be
marked and disappear. Nothing in the dump looks like that - the only authored-texture quads left
are the colour LUT and the light cookies - but if a HUD piece goes missing, that is the cause and
`screenSpaceMode=1` or `0` reverses it.

### Preflight before run 24, and two traps Remix's own save had set

Deployed `32e0dd6c` (screen-space tested before light volumes, third mark clause for shaders with
no colour sampler). ini and rtx.conf byte-identical between `configs/` and the game directory, 17
ini keys matching the code exactly, all new strings present in the binary.

Remix rewrote `rtx.conf` again during the last session, and two of its changes would have cost a
run:

- **`rtx.camera.enableFreeCamera = True` with `rtx.camera.lockFreeCamera = True`.** The game would
  have started in a LOCKED free camera at a stale saved position - a frozen view that looks
  exactly like a shim bug. Both set back to False; Alt+X re-enables free cam in game.
- **`rtx.uiTextures` is gone entirely.** The user's "untag all the UI textures I tagged by hand"
  was persisted this time, so the whole list is absent and nothing is classified as UI any more.

Left the uiTextures removal alone deliberately: the magenta is now handled in code, and restoring
27 tags in the same run as a new build would confound the two. The consequence to watch is the
HUD - with no UI classification, HUD quads pass through and Remix's vertex capture may weld them
to the camera, which is the old "camera-blocking plane" symptom. If that appears, the previous
list is at `configs/rtx.conf.before-ui-untag.bak` in Remix's own sign convention.

Also changed by Remix and worth knowing: `rtx.orthographicIsUI` is now False (harmless - the ortho
demote path is unused), and `rtx.fallbackLightMode` went 1 -> 0.

## Session 16 (continued) — camera-blocking layers cleared; "the egg" remains

User: *"basically all of the camera blocking stuff are gone. But there are some shapes around me...
stuff like the egg."*

**#1 is done.** The rasterised composite chain no longer reaches the screen.

### The egg is no longer the light volumes

Worth stating because it was the standing explanation. `not converted: ... other 0` - the
light-volume branch now fires **zero** times a frame, where it was 11-24 before. That is a direct
consequence of testing screen space first: SR3's inferred lighting resolves through screen-space
quads, not 3D hulls, so those draws are now marked as part of the composite chain. There are
effectively no 3D light volumes left to reconstruct, so the ellipsoids-hugging-the-player
explanation recorded in earlier sessions no longer applies to what remains.

### The leading suspect is the auxiliary camera, and the log will settle it

506 draws a frame, the largest population Remix still reconstructs. It renders real world geometry
from a viewpoint that is not the player's, into a 512x288 HDR target. Remix rebuilds those draws
against whatever transforms are current, which produces a warped copy of the world - and "shapes
around me" is a claim about POSITION, which fits a secondary view rebuilt near the player.

That is a hypothesis, so the shim now **logs where the auxiliary camera actually is**: its world
position, the main camera's, the distance between them, and its aspect ratio. One line, printed
once. If the two cameras sit together, a reflection probe is being rebuilt as a shell around the
player; if they are far apart, this is the wrong suspect and the next step goes elsewhere.

`markAuxCamera=1` for this run.

### The frame dump fired on an unrepresentative frame AGAIN

576 draws with 26 converted, against 2,535 and 619 in the counters at the same time. The delay
after the camera appears was not enough - a camera exists during loading and in menus.

The countdown now does not START until a frame has submitted at least 1,500 draws, which is a
property of the frame rather than of elapsed time and cannot be fooled by a slow load. Two dumps
have now been quoted before anyone noticed they described nothing real; that is twice too many.

## Session 16 (continued) — the egg identified, and an instrument built for the rest

User: *"the egg is gone and is attached to the magenta copy of the world. But other stuff that were
with the egg are still there. The shapes are like a dish, a cap and a ring."*

**Corrected by the user, and the correction matters.** The egg is part of the MARKED geometry -
the same magenta prepass copy of the world - and it went away back in run 19, when the marker's
hash was first tagged into `rtx.ignoreTextures`. The user said so at the time: *"the sphere was
also part of it so that too is gone"*. It was not the classifier reorder, and it was certainly not
`markAuxCamera`, which was still **0** in that run because the game held `sr3-rtx.ini` when the
setting was changed and the deploy could not land.

So the egg has been solved since run 19, and the credit belongs to the marker plus the ignore
list. The aux-camera switch remains **untested** rather than falsely confirmed.

**This sharpens what the dish, cap and ring can be.** The marked prepass is invisible, so they are
not it. Whatever they are, they are draws that are NOT being marked - which leaves only the
converted geometry and the pass-through populations: skinned characters (413/frame), the auxiliary
camera (684/frame), unconvertible vertex formats, and multi-instance draws.

### Stop guessing at shapes

A dish, a cap and a ring are descriptions of GEOMETRY, and this project has attributed exactly
such descriptions to light volumes, to decals, to texture problems and to the geometry prepass
across sessions 9-16 - wrongly each time, and the corrections cost whole sessions. There is no
reason to expect a fifth guess to land.

So: `sr3-rtx-shapes.log`, one frame of world-space bounding boxes for every draw near the camera.

```
draw  disp     verts  prims  size x/y/z   centre   dist  flat  ps='...' rank | why
```

`flat` is smallest axis over largest. **A dish, a cap and a ring are all flat** - they read near
0.0, where a building or a vehicle reads near 0.3-1.0. That single column separates the described
population from everything else, and size, distance, shader and disposition should then name each
one individually.

Bounded by construction: one frame; only draws under 400 units across and within 300 of the
camera; and the vertex-buffer **lock is gated by the same conditions as the report**. That last
point is the `MeasureWorldExtent` lesson - it capped its output at 20 lines while still locking a
buffer on every converted draw, ~1,400 times a frame, to feed a diagnostic that had stopped
printing. It also refuses to lock a `D3DUSAGE_DYNAMIC` buffer at all, per the standing rule.

### Deployed

`caeef224`, verified by hash, 18 ini keys matching the code. `markAuxCamera=1` and `shapeProbe=1`
both active for this run.

## Session 16 (continued) — run 25: the probe measured the wrong things, but the counters answered anyway

### The shape probe capped out on invisible geometry

`shape probe written to sr3-rtx-shapes.log (200 shapes)` - it hit its cap. **148 of the 200 were
`MARK`ed**, i.e. draws the ignore list already drops, so it never reached the material pass at
all. The prepass is submitted first and ate the entire budget.

Every flat shape it did find was a marked prepass draw: 56.1/3.8/40.0 and 48.0/1.1/32.0 slabs
around the player, `ps=''` or `Normal_MapSampler`. Those are floors and ceilings of the interior,
already invisible. Useless for the question asked.

Fixed by asking the right question: the probe now **skips `Mark` and `Skip` outright**. A draw
Remix drops cannot be a shape the user is looking at. Cap raised to 400. That is both the correct
filter and a large saving on buffer locks.

### The counters narrowed it anyway

`markAuxCamera=1` took effect - `refused as unsafe 0/frame`, and the auxiliary camera's 323 draws
a frame are now marked. With that, the frame dump's entire pass-through population is:

```
PASS  159  skinned (c52 bone palette), awaiting the skinning port
PASS   63  position not float3/float4 in stream 0
PASS    6  screen-space, authored texture (HUD/LUT)
```

**That is the whole list of what Remix still reconstructs.** So the dish, the cap and the ring are
either among those 228 draws - most likely the 63 unconvertible vertex formats, which are real
geometry we cannot place - or they are something we CONVERT and place wrongly. Either way the
search space went from "everything" to two candidate populations.

### The auxiliary camera is not an auxiliary camera

```
auxiliary camera at (96.3 143.8 36.1), main camera at (96.3 147.7 36.1), 3.9 units apart
aspect 1.778 vs back buffer 1.778
```

Same X, same Z, same 16:9 aspect as the back buffer, **3.9 units directly below the player's
camera**. That is not a shadow map (square) and not a cubemap face. It is exactly what a **planar
reflection about a horizontal plane** looks like: a plane at y = 145.75 mirrors a camera at 147.7
to 143.8.

If that is right, `skipMirrored` should have caught it and reports 0/frame - which would mean
SR3's reflection does not flip handedness, and the handedness test has been inert all along. The
next build logs the aux camera's forward and up vectors alongside the main camera's, plus both
determinants. A mirror negates the vertical component; a genuine second viewpoint does not.
Position alone cannot separate those, which is why the first version of this log line could not
settle it.

### Deployed

`6109f1fe`, verified by hash.

## Session 16 (continued) — the UI is being rebuilt as 3D planes, and that has a known cause

User: *"every UI element is drawn as a plane in 3D in front of the camera. Not a major problem
because they are not blocking the camera, but at some point it needs to be dealt with."*
And: *"markAuxCamera=1 - no, I did not notice a change."*

### The UI planes are a consequence of the untag, and the fix is to put the list back

`rtx.uiTextures` was **absent entirely** - the user's "untag all the UI textures I tagged by hand"
was persisted, so nothing was classified as UI any more. Without that classification a HUD quad is
just a screen-space draw with an authored texture, which `Classify` correctly passes through, and
Remix's vertex capture then reconstructs it as world geometry sitting in front of the camera.
That is exactly what was reported.

Restored, from `configs/rtx.conf.before-run24.bak` - the copy **Remix itself re-serialised**, 27
entries with 24 of them negative. That matters: the older `before-ui-untag.bak` has the same 27
textures with only 9 negative, and session 14 established that a wrong-signed hash simply never
matches. Restoring the wrong backup would have looked like a fix that did nothing.

The marker is not in the restored list, and post quads now clear all eight texture stages, so the
collision that produced the magenta sheet cannot recur from this.

### markAuxCamera made no visible difference, which is itself a result

The 512x288 pass is now marked and the dish, cap and ring are unchanged. **They are not the
auxiliary camera.** Combined with the frame dump's pass-through census, the remaining candidates
are exactly:

```
159  skinned characters
 63  position not float3/float4 in stream 0   <- real geometry we cannot place
  6  screen-space, authored texture (HUD/LUT)
```

...or geometry we CONVERT and place wrongly. The fixed shape probe measures precisely this set,
because it now skips marked and skipped draws.

Worth noting the possibility that the two reports are one thing: SR3's HUD has a circular minimap
and a radial weapon wheel, and "a dish, a cap and a ring" is a fair description of those rebuilt
as 3D planes. If restoring `uiTextures` removes the shapes as well as the floating UI, that was
the answer. If it removes only the UI, the probe names what is left.

## Session 16 (continued) — why tagging UI textures turns the screen magenta, and the hash-free fix

User: *"the reason why I untagged the UI textures is because any UI texture tagged causes the
screen to go magenta. Because the game itself is rendering magenta if I turn off ray tracing."*

That is the mechanism, stated exactly, and it changes the approach.

**A UI-tagged draw is RASTERISED by Remix from the game's own output - and we have deliberately
filled that output with the marker.** Marking the composite chain means the game's back buffer and
post targets now contain magenta by design; that is invisible only for as long as nothing shows
them. `rtx.uiTextures` is precisely a mechanism for showing them. So the two features are in
direct conflict, and no amount of picking the right hashes resolves it: the list itself is the
hazard.

Restoring the 27-entry list, which was the plan an hour ago, would have walked straight back into
this. Recorded because the reasoning looked sound right up until the user supplied the one fact
that invalidated it.

### The fix uses no hashes at all

An authored-texture screen-space quad is the HUD. It now gets `Disp::Hide` - an orthographic
projection - so Remix classifies it as UI by SHAPE rather than by texture hash.

The pleasing part is that this is the same mechanism session 14 recorded as a **failure**:

> "orthographic demote - Remix classifies it UI, and UI is rasterised as a 2D overlay, so the
> shape stays visible with a changed transform"

That was a failure when the goal was to hide world geometry. For the HUD, "rasterised as a 2D
overlay" is exactly the goal. Same measured behaviour, opposite requirement - so a dead end for
one problem is the tool for another, which is worth remembering before deleting a mechanism
outright.

`rtx.uiTextures` removed again (it is a liability while the composite is marked) and
`rtx.orthographicIsUI` set back to True, which the demote depends on and which Remix had flipped
to False during an earlier session.

### The frame dump gate was still wrong

Third unrepresentative dump: 997 draws against 2,661 in the counters. The gate tested
`g_lastFrameDraws >= 1500` when the countdown STARTED, then fired 1,800 frames later without
re-checking. A condition tested once, long before the thing it guards, guards nothing. It is now
re-checked at the moment of firing, and pushes the target 60 frames forward if the frame is too
small.

### Deployed

`38f8cfd5`, verified by hash, 18 ini keys matching the code.

## Session 16 (continued) — the reflection pass confirmed, and Disp::Hide proven inert

### The "auxiliary camera" is a planar water reflection. Settled.

```
aux  forward (0.087  0.008 -0.996)  up (0.001 -1.000 -0.008)  det -1.000
main forward (0.087 -0.008 -0.996)  up (0.001  1.000 -0.008)  det +1.000
```

Forward's Y component negated, up fully inverted, **determinant flipped**, position 3.9 units
directly below the player's camera with the same 16:9 aspect. That is a mirror about a horizontal
plane at y = 145.75, not a shadow map, not a cubemap face, not a secondary viewpoint. 172-684
draws a frame depending on location, and it is now marked.

Note this also explains why `skipMirrored` reported ~0-1/frame despite a genuinely mirrored pass
existing: `kMainCameraOnly` runs FIRST in `BeginFFP` and catches these as "other camera", so the
handedness test almost never sees them. It is not inert, it is shadowed.

### Disp::Hide does not work on shader-driven draws, and never could

The HUD demote landed - `HUD quads demoted to UI 5/frame` - and changed nothing on screen. The
reason is structural:

`BeginUIDemote` sets `D3DTS_PROJECTION`, which is **fixed-function** state. A hidden draw keeps
its vertex shader bound (only `BeginFFP` nulls shaders), and Remix reconstructs a shader draw from
its vertex-shader OUTPUT. The fixed-function projection is never read, so `orthographicIsUI` has
nothing to act on.

**So `Disp::Hide` is inert for every shader-driven draw**, which is all of them except the ones we
convert. That retroactively weakens session 14's entry describing ortho demote as "the shape stays
visible with a changed transform" - a changed transform implies it did something, and that was
most likely observed on converted draws.

`rtx.orthographicIsUI` set back to False: it buys nothing while the demote cannot reach shader
draws, and it was a change made in the same run as a newly reported artefact.

**The path that would work for the HUD** is to CONVERT those quads - null the shaders, bind the
HUD texture, and supply an orthographic projection through `SetTransform` - so Remix sees a
fixed-function draw whose projection it does read. That is real work for a low-priority symptom,
and it is not started.

## Session 16 (continued) — hall of mirrors: nothing writes the back buffer any more

User: *"like the hall of mirror effect when you noclip outside of GoldSrc games... the clear
pixel/frame buffer effect where it just shows the last pixels on it."* At some angles and
locations while flying.

That is the uncleared-framebuffer artefact, and the cause follows directly from what this session
did. Session 14 already recorded the mechanism:

> "the composite quad is the only draw that writes the back buffer, **which Remix presents**.
> `skipComposite=1` dropped it, so the back buffer kept stale content and frames presented at
> 56 fps showed an unchanging image."

We now **mark** that quad, and Remix's ignore list drops it. So nothing writes the back buffer.
Where path-traced geometry covers the screen this is invisible; where it does not - looking out
past the map while flying - the previous contents remain. Session 14 saw the whole image freeze
because the composite was skipped for every pixel; we see it only in uncovered regions because
Remix does fill the rest.

### This corrects a session 15 claim

> "Remix's ignore removes a draw from the ray-traced scene while the game's own rasterisation and
> readback survive."

That was inferred from the engine's draw count not collapsing after the marker was ignored. But
that count depends on the engine reading back the **prepass**, not on the back buffer being
written - two different things, and the inference silently assumed they were one. The hall of
mirrors is direct evidence that **the rasterisation IS dropped**. The prepass readback survived
for its own reason, most likely because depth is written by draws we do not mark.

Worth recording as a method note: "X still works after the change" is only evidence for the part
of X that the change could have affected. The counter that stayed healthy was measuring something
else.

### Fix

`clearBackBuffer=1`. One `Clear` per frame, issued the first time a draw targets the back-buffer
surface - which is before Remix composites anything - to opaque black. The surface is captured
once at device init with `GetBackBuffer` and its reference deliberately kept.

Black rather than a colour, so a region with genuinely nothing in it reads as empty rather than as
an artefact. If the sky turns out to be missing as well, that is a separate question and this at
least stops it looking like corruption.

### Deployed

`60b3f36e`, verified by hash, 19 ini keys matching the code.

## Session 16 (continued) — hall of mirrors fixed; the black is a missing SKY, not a regression

`back-buffer clears 1/frame` - the clear works, and the artefact changed from stale smearing to
black. The user then localised it exactly: *"looking at the sky and being too high, or on the
outskirts of the map flying and looking away from the city, it turns black."*

That is not the clear failing. That is **the sky, and there is no sky in the path-traced scene.**

### There is no sky geometry in SR3's frame

Searched a proper steady-state dump (5,214 draws, 1,356 converted) for anything sky-shaped:

- **No sky dome.** The largest depth-disabled draw in the whole frame spans 695 units at a
  distance of 702 - ordinary world geometry. A sky dome would be thousands of units across and
  centred on the camera. Session 8's sky probe reached the same conclusion from the other
  direction: SR3's sky is not an early draw, so `rtx.skyDrawcallIdThreshold` cannot address it.
- **338 draws have neither depth write nor depth test** - the shape a sky is usually drawn with -
  and every one of them is a post/composite quad: `IR_GBuffer_Depth` x312, `base_sampler` x18,
  the LUT, the colour-grade.

So SR3 almost certainly writes its sky in the **deferred resolve**: a fullscreen quad that fills
sky colour wherever depth is at the far plane. That is part of the post chain, which we mark.

### This is exposure, not regression

The sky was never in the path-traced image. It only *looked* present because the game's rasterised
composite was being painted over everything - the very overlay this session removed. Turning off
the overlay revealed that the world underneath has no sky, and the back-buffer clear then made
that read as clean black instead of smeared garbage.

Both changes are correct. The gap they exposed is real and older than either.

### The frame structure, now fully mapped

```
128x128 / 64x64 / 32x32           3 marked        small utility targets
1280x720 G16R16               2,198 draws       DSF / geometry prepass  (2,021 marked)
1280x720 A16B16G16R16F          647 draws       L-buffer accumulation   (ALL marked)
512x288  A16B16G16R16F          842 draws       planar water reflection (840 marked)
400x288  A16B16G16R16F x3        21 draws       low-res light buffers
512x512  A16B16G16R16F           11 draws       blur chain
1280x720 A16B16G16R16F        1,474 draws       MATERIAL PASS          (1,252 converted)
1280x720 + 320x180 + 64..1x1     post chain, auto-exposure reduction, colour LUT
1280x720 X8R8G8B8                 2 draws       BACK BUFFER
```

### Options for a sky, none free

Remix 1.5.2 offers exactly two sky mechanisms, both confirmed by reading the runtime's own
strings: `rtx.skyBoxTextures` (a hash list) and `rtx.skyDrawcallIdThreshold` (first N draws are
sky). Both need a DRAW carrying the sky, and the resolve quad that carries SR3's sky is one we
mark - so its texture, as far as Remix is concerned, is our marker.

The cheap experiment that would settle where the sky comes from is one ini toggle and no rebuild:
`screenSpaceMode=0` for a single run. If the sky returns (along with the rasterised overlay), the
post chain is confirmed as its source and the next step is to except that one quad from marking
and tag it as a skybox instead. If it stays black, the sky is not in the D3D9 stream at all and
the answer has to be a Remix-side environment.

## Session 16 (continued) — SR3 DOES have sky geometry, and it is the rfg-skybox family

The `screenSpaceMode=0` experiment answered cleanly: *"I had the sky come back but not in the path
tracer. In the path tracer, I got at least 1 plane blocking the camera again."*

So the sky is carried **only** by the rasterised composite, and restoring the composite restores
the overlay plane as expected. Set back to 2.

### But the sky is not screen-space after all

Searching the shader corpus for sky names found a whole family, inherited from Red Faction
Guerrilla - same studio, same engine lineage as `docs/sr2-fork.md` describes:

```
rfg-skybox_s        rfg-skybox-clouds_s     rfg-skybox-clouds-2_s   rfg-skybox-matte_s
rfg-skybox-overhead_s   rfg-skybox-simple_s     rfg-skybox-stars    rfg-skybox-meteors
```

And they are ordinary textured geometry. Running our own `AlbedoRank` over their samplers:

| shader | samplers | our verdict |
|---|---|---|
| `-clouds_s`, `-overhead_s` | `Diffuse_Map_1` + `Diffuse_Map_2` | CONVERT, rank 100 |
| `-matte_s` | `Diffuse_Map` + `Normal_map` | CONVERT, rank 100 |
| `-simple_s` | `Decal_Map_1` + `Decal_Map_2` | CONVERT, rank 90 |
| `-clouds-2_s` | `Layer01_map` + `Layer23_map` | rank **0** - would render blank |

So the sky SHOULD convert and path-trace like anything else, and the earlier conclusion that "SR3
writes its sky in the deferred resolve, there is no sky geometry" was wrong. It was drawn from a
frame dump captured in one spot - almost certainly indoors, since the camera sat at y=147.7 - and
absence in one frame is not absence in the game. Recorded because it was stated confidently.

### Two fixes, one of them certain

`Layer01_map` / `Layer23_map` now rank as colour (45). Without it `rfg-skybox-clouds-2_s` scores 0
and its cloud layers render blank - a definite defect regardless of what else is wrong.

The rest is instrumented rather than guessed. `ProbeSkyDraw` reports the first 12 sky draws with
their disposition, world-space size, centre and distance from the camera. Sky shaders are
recognised by constants unique to that family (`Cloud_Fade_Height`, `Layer_strengths`,
`TOD_Light_Dir`, `Star_strength`, `Layer01_map`, `Layer23_map`).

**The leading hypothesis it will test: the far plane.** SR3's projection is near 0.15, far 5000.
A skybox drawn at a radius beyond 5000 is clipped away entirely by fixed function, while the
game's own shader path need not respect the same clip - which would produce exactly "sky in the
raster, no sky in the path tracer". The probe prints the geometry's extent, so that is a
measurement rather than an argument.

## Session 16 (continued) — the black sky was MY rule, and the probe named it in one run

```
SKY draw: HIDE verts=343 ps='Diffuse_Map_1Sampler' rank=100 |
          size 36x9x36 centre (96 149 36) 1 from camera | why: screen-space HUD, demoted to UI
```

SR3's skybox is a **343-vertex dome, 36x9x36 units, one unit from the camera**, and its vertex
shader never references `projTM`. So `Classify` saw "no projTM" and called it screen space; the
HUD rule added earlier this session then demoted it to UI, and the sky disappeared from the path
tracer while the game's own raster still drew it.

The far-plane hypothesis was wrong - the dome is 36 units across and sits on top of the camera,
nowhere near the 5000-unit far plane. Worth noting because it was the confident guess, and the
probe cost one run to replace it with a fact.

Also visible in the same output, and separately useful:

```
SKY draw: MARK verts=343 ... | why: auxiliary camera (view differs from the frame's main one)
SKY draw: PASS verts=173 ps='Blend_MapSampler' | size 102541x0x102511 | why: no usable transform
```

The water reflection draws the sky too (correctly marked), and there is a **102,541-unit flat
plane** passed through for want of a transform - the horizon/ground plane, and a candidate for one
of the "shapes" still being reported.

### Fix: the sky is world content, not an overlay

Sky shaders are now checked BEFORE the screen-space test and passed through. Three reasons, in
order of importance:

1. Pass-through is what they did before the HUD rule existed, so this restores a known-good state
   rather than inventing a new one.
2. Remix's vertex capture then reconstructs them **with the game's own Diffuse_Map bound**, which
   is the precondition for `rtx.skyBoxTextures` to work at all - the mechanism needs to see the
   real texture. Marking would hand Remix our marker and tagging could never succeed.
3. Converting is not available: with no `projTM` there is no projection to rebuild.

### The proper endpoint for the sky is a tag, not a code change

A 36-unit dome one unit from the camera is geometry Remix will place as a **shell around the
player** - which is very likely one of the shapes reported as "a dish, a cap and a ring". Passing
it through fixes the black but may expose it as a shell.

`rtx.skyBoxTextures` is the mechanism for exactly this: it tells Remix the draw is environment at
infinity rather than nearby geometry. The conf already carries two entries from an earlier
session, so the workflow is known to work - the sky texture needs tagging in the Remix menu now
that Remix can see it again.

### Method note

Three explanations for the black sky were offered before any measurement: the deferred resolve
writes it (wrong - the rfg-skybox family exists), there is no sky geometry (wrong - 343 verts,
right there), the far plane clips it (wrong - 36 units). One probe settled it. The pattern is old
and this file has recorded it before; the difference this time is that the probe was built before
the third guess could be shipped.

## Session 16 (continued) — the dish, the cap and the ring ARE the skybox

User, tagging in the Remix menu: *"I selected the topper cap and it colored in the sky. What else
should I select? There is a ring around and a dish under."*

**The three shapes reported as unexplained artefacts across this session are the three pieces of
SR3's skybox.** Not light volumes, not decals, not the prepass, not the auxiliary camera - all of
which were proposed at some point. A cap on top, a ring around the horizon, a dish underneath,
each a 343-vertex piece sitting one unit from the camera, reconstructed by Remix as nearby
geometry instead of environment at infinity.

The evidence lines up on both sides:

- The probe caught **three distinct sky draws per camera**, two with `Diffuse_Map_1Sampler` and
  one with `Diffuse_MapSampler`, all 343 vertices, all ~1 unit from the camera.
- The shader family has exactly these members: `rfg-skybox-overhead_s` (the cap),
  `rfg-skybox_s` / `-simple_s` (the ring), `rfg-skybox-matte_s` (the dish), plus `-clouds`,
  `-clouds-2`, `-stars`, `-meteors`.

Tagging all three as `rtx.skyBoxTextures` is the fix, and tagging the cap alone already coloured
in the sky - so the mechanism is confirmed working before the other two are done.

### Why this took so long to see

The shapes were only ever describable, never measurable, until `ProbeSkyDraw` reported world-space
size and distance from the camera. Every earlier attempt reasoned from what the shapes looked
like. The moment a draw printed "36x9x36, one unit from the camera, Diffuse_Map, rfg-skybox", the
question answered itself.

The general lesson, which this file keeps rediscovering: **a visual description is a symptom, and
symptoms in this engine have consistently had causes that look nothing like them.** Build the
instrument first.

## Session 16 (continued) — a sky piece had been deleted since run 19

The user tagged all three skybox pieces and saved. Pulling `rtx.conf` back revealed a conflict
that explains more than this session:

```
0xFBC6C5FFCC8AD259  in rtx.skyBoxTextures   (just tagged)
-0xFBC6C5FFCC8AD259 in rtx.ignoreTextures   (tagged in RUN 19)
```

Run 19 added **two** hashes to `ignoreTextures` when the marker was first tagged, and at the time
there was no way to tell which was the marker. The marker is `0x978271113F293CE4`. **The other one
was a skybox piece, ignored by accident, and has been deleted from the scene ever since.** Ignore
beats a skybox tag, so tagging it this session could not have brought it back on its own.

Removed from `ignoreTextures`; the skybox tag stays. Both copies, verified in sync, and the edit
survived the game's exit without Remix overwriting it.

That is the second time a hash tagged by hand has done something nobody intended, and it is the
same root cause as the sign-convention lesson from session 14: **a hash is opaque, so tagging is
an action whose effect cannot be read back from the file.** The only defence is to record what was
tagged and why at the moment of tagging - which is now done here.

### Also cleaned up for the next run

`rtx.uiTextures = -0x196FBE2CAB23CB16` reappeared. By the user's own measurement a UI-tagged draw
is rasterised from the game's output, which the marker has filled with magenta, so any entry there
risks a fullscreen magenta sheet. The HUD demote is inert on shader draws, so the entry bought
nothing either. Removed - it is one line to restore if we ever want to test that interaction
deliberately against the current build, which is now different in three ways from the build the
magenta was observed on.

### Left alone, deliberately

`0x645CF1DD53FF6357` is in `ignoreTextures` AND `skyBoxTextures`, and also in `ignoreLights`,
`lightmapTextures`, `hideInstanceTextures` and `worldSpaceUiTextures`. It has been in that state
for many sessions and looks like a junk hash tagged everywhere at once. Changing something
long-standing in the same run as a real fix would confuse both, so it stays for now and is
recorded here instead.

## Session 16 (continued) — starting on the UI: measure the vertex space before converting

The HUD renders as flat planes floating in the world because Remix's vertex capture rebuilds it as
world geometry, and `Disp::Hide` provably cannot fix that: it sets `D3DTS_PROJECTION`, which is
fixed-function state Remix never reads for a shader-driven draw. Five demoted quads a frame
changed nothing on screen, which settled it.

The mechanism that CAN work is **converting** the HUD quads - null the shaders and supply
transforms through `SetTransform`, so Remix reads a projection it actually sees and
`orthographicIsUI` fires. That is the same conversion the world geometry already goes through.

The unknown is what space the HUD's vertex positions are in, because nulling the vertex shader
makes fixed function transform them itself. Three possibilities, three different answers:

| the positions are | what fixed function needs |
|---|---|
| already clip space | identity world / view / projection |
| screen pixels | an orthographic projection sized to the back buffer |
| `D3DDECLUSAGE_POSITIONT` | no transformation at all - FFP bypasses it |

Guessing between those is three builds and three of the user's runs. `ProbeHudDraw` reports the
declaration - `posType`, `posOffset`, `posStream`, whether `POSITIONT` is present, stride, element
count, buffer usage - **and the raw values of the first four vertices**, so the answer is readable
at a glance. Six reports, capped, and it refuses to lock a dynamic buffer.

Deployed `1cdcfdf9`, verified by hash.

## Session 16 (continued) — the sky is correct, and the HUD is not screen-space at all

**Sky confirmed correct** by the user with all three pieces tagged and the run-19 ignore lifted.
`sky draws 32/frame`. That closes the black sky, the dish, the cap and the ring together - they
were one problem wearing four descriptions.

### The HUD probe reported nothing, which was the finding

`HUD quads demoted to UI 0/frame` - the screen-space HUD branch fires **zero** times a frame. The
HUD is not screen-space by our test, so every assumption built on that was wrong, including the
ortho-demote attempt and the probe written to measure it.

The frame dump shows what it actually is. The last non-post draws:

```
4111 CONVERT v=96  zw=0 zt=1 blend=1 proj=1 obj=1 ps='Diffuse_Map_1Sampler' -> 1280x720 fmt=113
4114 CONVERT v=140 zw=0 zt=1 blend=1 proj=1 obj=1 ps='Depth_bufferSampler'  -> 1280x720 fmt=113
4121 CONVERT v=28  zw=0 zt=1 blend=1 proj=1 obj=1 ps='Diffuse_Map_1Sampler' -> 1280x720 fmt=21
4122 CONVERT v=24  zw=0 zt=1 blend=1 proj=1 obj=1 ps='Diffuse_Map_1Sampler' -> 1280x720 fmt=21
```

They reference **both projTM and objTM**, and they are **converted like world geometry**. Converting
UI with the world's view and projection is exactly how it ends up standing in the world, which is
what the user has been describing.

Note the target: `1280x720 fmt=21` is A8R8G8B8 at back-buffer size, a **different surface** from
the HDR material pass (`fmt=113`). The engine composites its UI into an 8-bit full-size target
while the scene stays in A16B16G16R16F.

### The probe re-aimed at the pass, not the shader

`ProbeHudDraw` now triggers on the RENDER TARGET - a back-buffer-sized 8-bit surface - rather than
on a shader property. That is a property of the pass rather than an index into a list of targets,
which matters: selecting passes by target *index* is a recorded dead end because the indices are
not stable, but the format and size of a target are stable descriptions of what it is for.

It now reports the declaration, the raw vertex positions, **and the transforms we would hand fixed
function**, including whether the resulting projection is orthographic. If it is, Remix's own
`orthographicIsUI` would classify these correctly the moment they are converted - and the fix is
small. If it is perspective, the HUD is genuinely being placed in the world and needs its own
projection substituted.

Built `198144 bytes`, awaiting deploy.

## Session 16 (continued) — the UI: a stale view matrix, and the fix

The re-aimed probe answered in one run:

```
HUD draw: CONVERT verts=28 target 1280x720 fmt=21 | stride=52 posType=2 positionT=0 usage=0x208
    proj row0 (1.6085 0 0 0) row3 (0 0 -0.1500 0) perspective=1
    world translation (0.00 0.00 -1024.00), camera (91.62 147.63 31.33)
```

The projection being handed to fixed function is **the world's perspective camera** - `_11` 1.6085
and `_43` -0.15, the scene's exact near plane - and the HUD's world matrix sits at a fixed
`(0, 0, -1024)`. So UI geometry was being placed in the world and viewed with the player's camera.
`orthographicIsUI` could never fire, because the projection we gave Remix was perspective.

### Why: the UI pass never uploads a view

These draws reference `projTM` and `objTM` but **never refresh `IR_World2View`**, so `c48` still
holds whatever the last world draw left there. `ComputeTransforms` decomposed the UI's `projTM`
against that stale view, and unsurprisingly recovered the world's projection.

The engine's own shader computes `clip = projTM * objTM * position` and needs no view at all.

### Fix: view = identity, projection = projTM

For draws on the UI target, `ComputeTransforms` now sets `view = identity` and
`projection = projTM` directly, skipping the decomposition. That is **more faithful to what the
shader does** than the decomposition was, not merely a workaround - and it is the only form that
lets Remix see the UI's real projection.

`perspectiveOnly` and the main-camera latch are both bypassed for this pass, since its projection
is expected to be orthographic and its view is deliberately identity - exactly the two things
those tests exist to reject.

The UI pass is identified by its render target: a back-buffer-sized **8-bit** surface, where the
scene stays in `A16B16G16R16F`. Format and size describe what a pass is FOR, which is what makes
this different from selecting passes by render-target *index* - a recorded dead end, because
indices are not stable while formats are.

The target check runs once per `SetRenderTarget`, not per draw. `GetDesc` on every draw would be
thousands of bridge round trips a frame, which is the cost mistake this file has already made
twice.

`rtx.orthographicIsUI` back to True - it was disabled earlier today only because the demote path
it served was inert on shader draws, and a converted draw is a different case.

### Deployed

`364654f1`, verified by hash. The log will now state outright whether the UI's raw `projTM` is
orthographic (`raw projTM ... perspective=0/1`), which decides whether this is finished or whether
Remix needs telling some other way.

### The UI fix was wrong, and the log says why

*"The UI now disappears at certain camera angles. The UI is still geometry in the world."*

```
raw projTM row0 (-0.5863 0.4811 0.9159 0.9159) row3 (100.7555 -456.2618 -46.0588 -45.9083)
perspective=0
```

That is a rotated basis with a large translation - **neither perspective nor orthographic**.
`perspective=0` in that line means "failed the perspective test", not "is orthographic", and
reading it as the latter is exactly what made the fix look plausible. With no orthographic
projection to find, `rtx.orthographicIsUI` had nothing to latch onto; substituting
`view = identity, projection = projTM` merely handed Remix an unusual transform, which is what
produced the new disappearing-at-angles symptom.

The diagnosis was half right: the UI pass genuinely does not refresh `IR_World2View`, so the
decomposition is against a stale `c48`. The conclusion drawn from it was wrong.

**Reverted**, along with the `perspectiveOnly` and main-camera bypasses that went with it, and
`rtx.orthographicIsUI` back to False since nothing depends on it now.

What this rules out, which is worth having: the 8-bit full-size target is **not simply "the HUD"**.
It carries a full world transform, so "one UI pass with one UI projection" is the wrong model and
any further attempt needs to start from what that pass actually contains - most likely by reading
its vertex data, which needs a non-dynamic path since the buffer is `D3DUSAGE_DYNAMIC` (0x208) and
the probe correctly refused to lock it.

The UI remains as it was: geometry in the world, visible, not blocking the camera. The user rated
it not a major problem, and it has now cost two builds without progress - so it is parked here
rather than pursued further, with the measurements recorded for whoever picks it up.

---

## Session 17 - 2026-08-18 - the choppiness: the timing measurement was too narrow

The user reports the character moving choppily through the world, worst at certain positions and
camera angles, and while flying and looking towards the sky - *"like seeing choppy animations or
animations that stop/freeze"*.

### The evidence was already in the log, and had been misread

Run 29's own TIMING lines carry it:

```
TIMING: frame 17.6 ms avg (57 fps), worst 115 ms | shim 3.03 ms avg (17.2% of frame), worst 4.6 ms
TIMING: frame 17.5 ms avg (57 fps), worst 110 ms | shim 3.01 ms avg (17.1% of frame), worst 3.9 ms
TIMING: frame 17.3 ms avg (58 fps), worst 116 ms | shim 2.97 ms avg (17.2% of frame), worst 4.0 ms
```

The average frame is healthy. The **worst** frame in nearly every 600-frame window is 100-116 ms -
six dropped frames in a row at 58 fps, which is precisely what "animations stop/freeze" looks
like. The spikes cluster tightly at 100-116 ms rather than scattering, and appear in most windows;
600 frames is ~10 seconds, so this is a regular drumbeat, not a rare event.

### Why "shim worst 4.6 ms" did not exonerate the shim

It looked like proof the hitch was outside our code. It was not. `g_shimMsThisFrame` is
accumulated at exactly three sites, all inside `Hook_DrawIndexedPrimitive`. So the number
established "the hitch is not in the draw path" - a much weaker claim than "the hitch is not
ours". Outside the measurement entirely:

- `Hook_CreateVertexShader` / `Hook_CreatePixelShader`, which run `ReflectShader` (a byte-wise
  `CTAB` scan plus up to 256 constants x ~15 substring searches) on **every** shader the game
  creates;
- `Hook_CreateTexture`;
- `Hook_SetRenderTarget`, which does a `GetDesc` - a bridge round trip;
- `Hook_Present` itself, including the periodic report block. Note the ordering: `g_lastPresent`
  is stamped *before* that block and `g_shimMsThisFrame` is zeroed there too, so whatever the
  report costs was invisible to the shim timer and landed in the next frame.

Creation work is exactly the kind that varies with position - flying into a district streams in
new materials - which fits "some places and angles" far better than anything in the draw path.
That does not make it the cause; it makes it a candidate that had never been measured.

### What was ruled out first

- **Our own logging.** 982 lines total across 32,400 frames: ~12 `fflush` calls every 600 frames.
  Too little to reach 100 ms, despite the tempting coincidence that the report cadence and the
  hitch cadence are both 600 frames.
- **The bridge logs.** `bridge64.log` is 899 KB but every `err:` line in it is one shutdown leak
  dump (15,381 objects at module eviction). Nothing per-frame. `remix-dxvk.log` has no repeated
  runtime warning.

### The instrument

Rather than guess a fourth time, the whole Present-to-Present interval is now partitioned:

| term | what it is |
|---|---|
| `present` | inside the real `Present` - Remix's own end-of-frame work |
| `our-present` | our end-of-frame block: the periodic report and bookkeeping |
| `draws` | the existing draw-path accumulator |
| `other-hooks` | shader/texture creation, the render-target hook |
| `game/bridge` | the remainder: the game's CPU work plus bridge submission |

`present` and `our-present` are carried in "Last" variables and attributed to the **next**
interval, which is the correct attribution: `g_lastPresent` is stamped at the top of the hook, so
everything the previous Present did after its own stamp falls inside the interval being measured
now.

Any frame at or above `hitchMs` (default 40, ~2.3 frames at 58 fps) writes one line, capped at 60
per run so the recorder cannot become the hitch. The periodic report also gains a tail line
counting how *many* frames ran long, since one worst-case number cannot distinguish a rare event
from a drumbeat.

The remainder term is as diagnostic as the parts. A hitch that is nearly all `present` is Remix's
end-of-frame work - BVH build, pipeline compile - and nothing at the D3D9 boundary will touch it.
One that is nearly all `game/bridge` is submission stalling on the bridge. One that is
`other-hooks` is ours to fix.

Built `c564e37496e17a0412fe2eec07bf5c33`, deployed, `sr3-rtx.ini` and `rtx.conf` verified in sync.
Run 29's log archived as `sr3-rtx-fork-run29-ui-substitution-failed.log`.

**Aside, not acted on:** `rtx-remix/mods/sr3rtx/deps` is a symlink to its own grandparent
`rtx-remix`, so that tree recurses without end. It is Remix-toolkit convention and cannot cause a
per-frame hitch, but it makes any recursive search under `rtx-remix` pathological - worth knowing
before running one.

### Run 30: the partition answered it, and the answer was "not the frame rate"

14 HITCH lines. The shape of the run matters for reading them: frames 60-4200 report **0 draws**,
so that was all menu and loading; actual gameplay was frames ~4500-6600, about 36 seconds.

```
HITCH frame 4567: 506.2 ms = present 0.1 + our-present 0.0 + draws 11.2 + other-hooks 0.0 + game/bridge 494.7 | draws 4079
HITCH frame 4574: 102.7 ms = present 76.4 + our-present 0.0 + draws  7.3 + other-hooks 0.0 + game/bridge  15.7 | draws 4070
```

Every hitch is at world load (the 4563-4574 cluster, where draws jump from ~2,030 to 4,082) or at
startup. Within gameplay the tail counts read **0, 0, 0 and 1** frames >=33 ms per 600-frame
window. So during play the frame rate is steady at ~57 fps and there is no drumbeat at all.

Two things settled:

- **`other-hooks` is 0.00 ms in every window.** The shader/texture creation hypothesis that
  motivated the instrument is dead. `ReflectShader` costs nothing measurable. Recorded so it is
  not proposed again.
- **The choppiness is not a frame-time hitch.** The user reports choppy, freezing character
  movement in a run whose frames were smooth. Those cannot both be about frame pacing.

### What it actually is: instance matching on skinned draws

The user's own guess - *"it looks to be with the render that renders characters"* - lines up with
a mechanism that was already written down in this source, at the skinned classification:

> *"Skinned meshes transform through the c52 bone palette, so objTM means nothing for them."*

Skinned characters pass through unconverted (~157/frame during gameplay) and Remix reconstructs
them by vertex capture. To keep an object temporally stable Remix must match each draw to the
same object in the previous frame. At the draw level every character carries a meaningless world
transform, so they are not distinguishable that way - and the option that would give Remix real
per-draw bounds to match on is off:

```
rtx.enableAlwaysCalculateAABB = False
```

The Remix binary labels it exactly: **"Always Calculate AABB (For Instance Matching)"**. Verified
by string extraction from `.trex\d3d9.dll` rather than from memory of the docs, because the
obvious reading ("it's a culling box") is the wrong one and would have sent this somewhere else.

That fits every part of the report: characters specifically, animation that stutters and freezes
rather than runs slow, and variation with position and camera angle as characters move relative
to one another and the matching becomes more or less ambiguous. It is very likely the same defect
as the long-standing "characters are still flickering", set aside back in session 15.

**Changed `rtx.enableAlwaysCalculateAABB = True`** (backup: `rtx.conf.before-aabb-instance-matching.bak`).
No build needed - this is Remix-side. It is a hypothesis with a mechanism, not a proven fix, and
the run that tests it is the one that decides.

Note this does not remove the need for the skinning port. It would make Remix's reconstruction of
characters temporally stable; converting them to fixed function would mean there is nothing to
reconstruct. If the AABB change works, the port becomes a quality and performance improvement
rather than a correctness fix.

### Run 31: the skinned probe, and the CPU skinning port

The AABB change did not help - the user confirmed the stutter was unchanged - and the probe says
why. Every skinned draw in the probe frame reports the SAME objTM, `(96.87 145.75 29.67)`, which
is the player's own position. They are the pieces of one character (body, head, hair, clothing),
each a separate draw. The probe never caught a second character, so instance matching between
characters was never the thing being tested and the AABB option had nothing to work with.
**Reverted to False.**

The real cause is plain in the same lines:

```
SKINNED draw #5: verts=1080 disp=3 stride=36 | VB usage=0x8 static | live bone regs 8
    vert 0: pos(0.456 1.099 -0.043) weightBytes[129 126 0 0] indexBytes[0 3 255 255]
```

**The skinned vertex buffer is STATIC and holds the bind pose.** Usage 0x8 is WRITEONLY with no
DYNAMIC bit. Every frame of animation lives in the c52 bone palette - vertex shader constants,
which Remix does not track. So the geometry Remix reconstructs genuinely never changes, and
characters freeze while the converted world moves. That is exactly the split the user reported:
*"the world does not freeze but the character animations and characters do"*. Two populations,
two code paths, and only the reconstructed one is broken.

### The measured declaration

```
offset  0  float3   POSITION
offset 12  ubyte4n  NORMAL
offset 16  ubyte4n  TANGENT
offset 20  ubyte4   BLENDWEIGHT
offset 24  ubyte4   BLENDINDICES
offset 28  short2   TEXCOORD0
```

Two details that a reasonable assumption would have got wrong, both silent failures rather than
obvious ones:

- **BLENDWEIGHT is UBYTE4, not UBYTE4N.** Every sampled vertex sums to 254-255, not 1.0. This is
  why the shader computes `rcp(w.x+w.y+w.z+w.w)`. Reading them as normalised would collapse
  every mesh toward the origin.
- **Index 255 is a sentinel for "no influence"**, always paired with weight 0. `255*3 = 765`
  addresses far past the 192-register palette, so influences must be rejected on WEIGHT and the
  index never trusted on its own.

### The port

`pos' = (SUM_i w_i * Bone[idx_i]) * float4(pos,1) / SUM_i w_i`, with the palette at c52 as
row-major float3x4, 3 registers per bone, 64 bones. objTM is applied AFTER the blend, so skinning
runs in OBJECT space and the existing WORLD/VIEW/PROJECTION path places the result unchanged -
no transform logic is duplicated. Normals rotate only (the shader uses `dp3`, dropping the
translation column) and are renormalised.

Design decisions worth recording:

- **The bind pose is decoded once and cached**, keyed on (buffer, offset, stride, minIndex,
  count). Read-locking the source buffer is a bridge round trip; doing it for ~157 skinned draws
  every frame is precisely the state-call catastrophe of sr2-fork.md section 6. The cached buffer
  is **AddRef'd** - the key contains the pointer, and a released buffer's address can be reissued
  to a new one, which is the same use-after-free that crashed run 20.
- **The game's index buffer is reused verbatim.** Our output goes into a ring buffer at a write
  position forced to be at least `minIndex * stride`, so the stream offset
  `writePos - minIndex*stride` stays non-negative and index i still lands on the right vertex.
  That avoids copying or rebasing indices at all.
- **UVs are emitted as raw short values cast to float**, not pre-scaled, because the conversion
  already installs a texture matrix for short UVs. One owner for that scale, rather than two that
  can disagree.
- **A persistent dynamic VB, never `DrawIndexedPrimitiveUP`** - sr2-fork.md records that path as
  a null-pointer crash inside the Remix bridge server.
- **Failure falls back to PASS-THROUGH, not to converting.** Fixed function cannot read a bone
  palette, so a skinned draw converted without skinning would render as a rigid T-posed statue -
  visibly worse than the stutter. The report counts refusals for exactly this reason.

Built `bc801214badf287fcec9faf3947f2d0e`, `convertSkinned=1`, deployed and verified in sync.

The open risk is cost: ~157 skinned draws a frame at up to four bone blends per vertex is real
CPU work, and the draw-path timer now covers it. If `shim ms` jumps, the answer is to skin only
the draws that convert (already the case) and to cache more aggressively - not to abandon it.

### Run 32: skinning works, and what "partially textured" turned out to be

The user confirms characters animate. Measured cost:

```
SKINNING: 53 draws skinned/frame, 9 refused/frame | 52 meshes decoded, 52 cached
TIMING: frame 18.9 ms avg (53 fps) | shim 6.85 ms avg (36.2% of frame), worst 11.1 ms
```

57 -> 53 fps, shim 3.0 -> 6.85 ms. The bind-pose cache is holding (52 decodes, 52 cached - no
thrashing), so that is the arithmetic itself, not the locks. Acceptable for now; the population
to trim is the 9 refusals and any draw skinned more than once per frame.

### "Partially textured" is a fallback-albedo problem, not a missing texture

The frame dump separates the player's draws cleanly (`vs[proj][obj][skin]`), and the split is:

| chosen albedo | draws | what it is |
|---|---|---|
| rank 100 `Diffuse_Map` | 18 | correct, and these are the parts that look right |
| rank 80 `Pattern_Map` | 12 | clothing customisation |
| rank 70 `Blend_Map` | 1 | clothing customisation |

The rank-80/70 materials have **no diffuse map by design**. They are SR3's clothing shaders, and
their constant list gives it away: `Diffuse_Color_a`, `Diffuse_Color_b`, `Diffuse_Color_c`,
`Tint_color`, `Pattern_Map`. Colour comes from customisation constants masked through a pattern.
From `ir_sr3npcclothfull_c.fxo_pc` shader [8]:

```
texld_pp r3, v0, s0             ; Pattern_Map
mad_pp   r3.xyz, r3.x, c3, ...  ; combined with Diffuse_Color_c
mul_pp   oC0, r1, c37           ; and the whole result * Tint_color
```

We were binding the pattern map raw and dropping that final multiply, so those parts read as
untextured next to the diffuse-mapped ones. Note this is the same `mul oC0, r1, c37` that session
16 already found in `ir_bbsimple2_nodiffmap_bs` - the constant-albedo fix. The rule was just too
narrow: it fired only when `albedoRank == 0`, so a material that HAD a fallback map got the map
and lost the tint.

**Change:** when a fallback map is chosen (`0 < albedoRank < 100`) and the shader carries a
colour constant, stage 0 becomes `MODULATE(TEXTURE, TFACTOR)` instead of `SELECTARG1(TEXTURE)`.
Constant-only and real-diffuse materials are untouched.

This is an approximation and is recorded as one: the real shader masks three separate colours
through separate channels, which fixed function cannot express. A tinted pattern is much closer
than an untinted one, and it is safe by construction - where `Tint_color` is white the modulate
is a no-op, so no material can get worse than it is today.

New report line splits the three cases so the next run says how many draws each rule caught:

```
ALBEDO: constant-only N/frame, tinted fallback N/frame, still blank N/frame
```

Built `0151aa9bafd5452c8a3337b33e9d7789`, deployed and verified. Run 32 log and the skinned frame
dump archived as evidence.

### The world's untextured population is smaller than the log implied

Chasing "texture the world" started with the wrong number. The report line read
*"no-albedo materials left to Remix 41/frame"*, but `g_skipNoAlbedo` increments for **every**
rank-0 draw, including the ones the constant-colour rule successfully rescues. So it counts
`constant-only + genuinely-blank` under a label that says only the second. The new ALBEDO line I
had just added repeated the error by printing that same counter as "still blank" - the precise
mistake the note beside `g_skipNoAlbedo` was written to prevent.

Added `g_blankAlbedo`, incremented only where a draw takes neither the constant nor the tint
path. The report now separates all four cases and says which counter is which:

```
ALBEDO: constant-only N/frame, tinted fallback N/frame, genuinely blank N/frame (of N rank-0 draws/frame)
```

Two independent checks say the blank population is small:

- **Every one of the 117 no-albedo MATERIAL passes** in `re/shader_constants.csv` - pixel shaders
  that sample `IR_LBufferSampler` but declare no rankable colour map - carries a colour constant
  the shim already recognises (`Diffuse_Color`, `Base_Paint_Color`, `Glass_Color`, `Base_Color`,
  `Draw_Color`, `Tint_color`). Zero missed by name.
- The run's own deduplicated naming found only **two** distinct families:
  `IR_GBuffer_DSF_DataSampler` and `Detail_Normal_MapSampler`.

So the constant rule already covers the named world population, and the tinted-fallback change
extends it further - car paint, for one, has `grime_map` as its only map and `Base_Paint_Color`
as its colour, which previously bound grime raw and now modulates it by the paint colour.

Also removed a duplicate reporter: `AlbedoForDraw` already names untextured materials once per
distinct first-sampler, so the second one added here was deleted rather than left to disagree.

Built `69f7fb27fe1e7a25258cb13ca9707ddc`, deployed and verified.

### White surfaces: two paths to white that no counter was watching

The user reports the world's problem as **white / untextured surfaces**. The first attempt to
size that population from the shader database said the problem should not exist:

- 117 no-albedo material passes, and **all 117** carry a colour constant `ConstantAlbedo` knows.
- Re-checked with EXACT name matching, because the first query used a substring regex while the
  runtime uses `_stricmp` - `Diffuse_Color_a` would have counted as `Diffuse_Color`. Same answer:
  117 of 117, every register below the 96-register shadow limit. The classification is sound.

So the white surfaces are not the rank-0 population at all. Reading `EffectiveAlbedo` end to end,
there are **three** ways it returns null and only one of them is counted:

| path | counted? | result |
|---|---|---|
| `albedoRank == 0` - no colour map named | yes, `g_albedoBlanked`, then rescued by the constant rule | coloured |
| `albedoRank > 0` but **nothing bound at the named stage** | **no** | **white** |
| render-target exclusion finds no non-RT texture | **no** | **white** |

The second and third render white with `COLORARG1 = TEXTURE` and no texture bound, and neither
appears in any log line. Every "untextured" number in the log to date describes materials that
name no colour map - while a material that names one and does not receive it has been completely
invisible. That is why the counters and the screen disagreed.

Both are now counted, and the named-but-not-bound case is also named once per distinct sampler,
with the full 8-stage bound mask so the next run says whether the texture is absent entirely or
merely sitting on a different stage than the CTAB claims:

```
albedo named but NOT BOUND #N: sampler 'X' rank=N expected stage N (stages bound: 01001000)
ALBEDO WHITE (previously uncounted): named-but-not-bound N/frame, render-target exclusion left nothing N/frame
```

No fix yet - deliberately. Which of the two dominates decides the fix, and they need opposite
ones: a texture on the wrong stage means the stage index is wrong, whereas no texture bound at
all means the material is drawn before its texture arrives and the mesh albedo cache should be
covering it.

Built `54ed32331d14f55b37372c116b5737a0`, deployed and verified.

### Run 33: both instrumented paths were zero, and the real rule was a property, not a list

```
ALBEDO: constant-only 69/frame, tinted fallback 22/frame, genuinely blank 20/frame (of 90 rank-0/frame)
ALBEDO WHITE (previously uncounted): named-but-not-bound 0/frame, render-target exclusion left nothing 0/frame
```

Both hypotheses from the previous session are dead: **0/frame** for each. Worth stating plainly -
the instrument was built to catch them and it caught nothing, which is the instrument working.

The white population is the third number, `genuinely blank`, and it GREW through the run
(8 -> 13 -> 20 per frame) while two new material families appeared in the naming report:
`Damage_Normal_MapSampler` and `Dual_Paraboloid_Map_Back...` - both vehicle shaders. The user had
started driving.

### Why the earlier "117 of 117 are covered" was true and still misleading

It was true of MATERIAL passes. The white draws are not material passes. Two concrete examples:

```
ir_sr3pchair_mc [5]           samplers: Dob_MapSampler                     consts: (none)
cust_normal_map_blend_mc [2]  samplers: baseSampler body_age muscle ...    consts: (none)
```

Neither sampler is on the `prepassSamplersOnly` utility list (`Stipple`, `Normal_Map`,
`Depth_map`, `shadow_map`), so neither is recognised as a prepass; and neither has a colour map
or a colour constant, so both convert and render white. The existing rule needs every sampler to
be a name this shim already knows - which means every unlisted sampler in the game is a potential
white surface, and the list will never be complete.

### The rule that replaces the list

SR3 uses inferred lighting, so a pass that produces visible colour **must read the L-buffer** to
shade itself. A pass with no colour map, no colour constant and no L-buffer read produces no
colour at all - it is filling the G-buffer. That is a property of what the shader does, not a
guess about what it is called.

Checked against all 7,276 shaders BEFORE writing it, because hiding a real surface makes it
invisible:

```
no albedo map, HAS constant, reads L-buffer :  117   material passes - untouched by this rule
no albedo map, NO constant,  reads L-buffer :    0   <- nothing can be wrongly hidden
no albedo map, NO constant,  no L-buffer    :  962   prepasses - what this rule hides
```

The middle row being zero is the whole safety argument: there is no shader in the game that this
rule could hide and that could also have produced colour. That is the check the "hid ~2,400 real
draws a frame including 17,601-vertex terrain" dead end did not do.

Implemented as `samplesLBuffer` on ShaderInfo, set from the CTAB during reflection, and a second
prepass test in Classify beside the existing one. Reported as `colourless passes now hidden
N/frame` so the population size is visible.

Built `d76507b372f973372fd9120b7038f224`, deployed and verified. Run 33 archived.

### The character drawn twice: two candidates, both plausible, so neither assumed

Reported after run 33: *"it looks like there are two renders of my character as if it being drawn
twice."* This is a NEW symptom - it appeared with the skinning port, so the port is the place to
look. Two mechanisms can produce a second copy, and they need opposite fixes.

**Candidate 1 - refusals.** `SKINNING: 53 skinned/frame, 9 refused/frame`. A refused draw falls
back to PASS-THROUGH by design, which is the safe choice against rendering a rigid T-pose - but
pass-through means Remix reconstructs it, so a refused piece appears BESIDE the skinned copy.
Nine a frame is the right order of magnitude for "part of the character, twice". The refusal
reason was never recorded, so the fix could not be chosen.

**Candidate 2 - repeat draws.** The skinned frame dump has converted skinned draws sharing an
exact vertex AND primitive count and the same texture:

```
4 x  v=958  p=93
4 x  v=1080 p=499
2 x  v=7977 p=309
2 x  v=1426 p=3400
```

The game draws these parts several times - layered blend passes, which a rasteriser resolves by
blending and a path tracer receives as coincident duplicate surfaces.

Both were instrumented rather than guessed:

- every refusal path now names itself (`skinning REFUSED #N: <reason> | verts= stride= decl=...`),
  covering all nine early-outs in `GetBaseMesh` and `SkinAndBind` separately - declaration
  mismatches, dynamic source buffer, failed lock, cache full, ring exhausted;
- a per-frame key set counts how many skinned conversions are the SAME mesh already skinned this
  frame, reported as `REPEATS/frame`.

Deliberately no fix in this build. The two need opposite treatments - a refusal wants its cause
removed so the draw converts, whereas a repeat wants the extra copies suppressed - and choosing
between them from the symptom alone is the mistake that cost two builds on the UI.

Built `0c5c31220a51deab35f86678f97c7414`, deployed and verified.

### Run 34: the colourless rule worked, my repeat counter did not, and the tint was a regression

```
ALBEDO: constant-only 55/frame, tinted fallback 43/frame, genuinely blank 0/frame
colourless passes now hidden 43/frame
SKINNING: 88 skinned/frame, 23 refused/frame, 69 REPEATS/frame | 174 cached
```

**The L-buffer rule worked.** `genuinely blank` went 20 -> **0**, 43 colourless passes a frame are
now hidden, and the user confirms nothing vanished - the outcome the 7,276-shader pre-check
predicted. But white surfaces are still reported, so they are a DIFFERENT population from the one
this fixed. Progress, not the answer.

**The repeat counter was wrong and its number must not be used.** It keyed on the vertex range
only, so SR3 drawing one character mesh as many material sub-ranges over the same vertices - the
frame dump has v=7977 with p=3408, p=427, p=677 - counted every legitimate sub-range as a repeat
of the first. 69 of 88 was an artefact of the key, not a finding. Re-keyed to include the INDEX
range and triangle count, so a hit now means the identical triangles really were submitted twice.
With the corrected key, exact duplicates are HIDDEN (not skipped - the engine still reads them),
behind `dedupSkinned`.

**The tint modulate was a regression and is reverted.** The user reports character and NPC
clothing dark, and the tinted population had doubled to 43 draws/frame as more NPCs appeared. The
reasoning was sound; the result was not. The shader computes

```
(pattern.r * Diffuse_Color_c + pattern.gba) * Tint_color
```

where the pattern's channels are **masks** selecting between three customisation colours.
Modulating the whole texture by `Tint_color` multiplies a mask by a colour - two things the
shader never multiplies - and the product is darker than either. The claim that it was "safe by
construction because a white tint is a no-op" was true and irrelevant: these tints are not white.
Kept as `tintFallbackAlbedo=0` rather than deleted, because the underlying finding still holds -
these materials do take their colour from constants, and binding the pattern raw is also wrong.
Getting it right needs the three-colour mask evaluated per texel, which fixed function cannot
express.

**Refusals are real but small**, and now named:

```
skinning REFUSED #1: blend weights are not ubyte4 | verts=460 stride=28 ... weights=?
skinning REFUSED #2: no stream 0, or stride < 28  | verts=865 stride=24 ... weights=?
```

Both are a second skinned vertex format this shim does not decode - `weights=?` means a
declaration type outside `DeclTypeName`'s table. 23 draws a frame fall back to pass-through
because of it, each a second copy of whatever it is.

**"Shoes have normals but the wrong colour"** has a candidate path, now counted: `EffectiveAlbedo`
ends in a bare `else { albedo = g_curTexture[0]; }` that runs when there is no usable pixel shader
reflection, binding whatever sits on stage 0 - a tangent-space normal map, on a material whose
stage 0 is one. Reported as `ALBEDO stage 0 taken raw (no shader reflection) N/frame`.

Built `f28be0a2a2b89ca383b70f9fa2dcd946`, deployed and verified. Run 34 archived.

### Run 35: the dedup was a regression, and the user named the clothing system

Three results, one good and two corrections.

**The tint revert is confirmed.** *"clothing color is back to how it was before."*

**The dedup made NPCs flicker and did not fix the doubling.** *"npc now have componets from them
disappearing and coming back. it might be an instancing thing im guessing."* That guess is right,
and the mechanism is exact: the key is (buffer, vertex range, index range, triangle count), which
is **identical for two different NPCs wearing the same garment**. They differ only in their bone
palette and objTM, neither of which is in the key - so the second character's parts were hidden.
Reverted to `dedupSkinned=0`. Making it correct would need the pose in the key, i.e. hashing the
bone palette per draw, and since it did not reduce the doubling there is nothing to weigh against
that cost.

Two dedup attempts, two different errors, both from keying on too little: first the index range
was missing (every material sub-range counted as a duplicate), then the pose was missing (every
NPC sharing a mesh counted as a duplicate). Recorded together because the pattern is the lesson -
"identical geometry" is not "the same object".

**The refusals were self-inflicted.** `GetBaseMesh` demanded `stride >= 28`, which is simply the
size of the one layout that had been measured, and it rejected a decodable 24-byte layout. 23
draws a frame went back to pass-through because of a magic number. Replaced with per-field bounds
arithmetic (`offset + sizeof(type) <= stride`) and a decoder that accepts the formats the game
actually uses:

- weights `UBYTE4`, `UBYTE4N`, `D3DCOLOR`, `FLOAT4` - and `UBYTE4`/`UBYTE4N` decode identically
  here, because the skinning divides by the sum of the weights so their scale cancels;
- indices `UBYTE4`, `D3DCOLOR`;
- normals `UBYTE4N`, `FLOAT3`; texcoords `SHORT2`, `FLOAT2`.

`D3DCOLOR` is stored BGRA, so its bytes are reordered rather than copied - a straight `memcpy`
would have silently swapped bone influences. Weights are now carried as floats in the cache so
one skinning loop serves every format.

The refusal log also prints the numeric `D3DDECLTYPE` now. The previous run printed `weights=?`,
which says only that the type is outside `DeclTypeName`'s table, not which type it is.

**The user's own read of the clothing system is right:** *"it looks like clothes in this game use
a segmentation map?"* That is exactly what `Pattern_Map` is - its channels are masks segmenting
the garment into regions, each taking one of `Diffuse_Color_a/b/c`. It is why modulating the whole
texture by a single tint could never be right.

Built `dd75a9b0e13d62d7a5f4887ed77db4f0`, deployed and verified. Run 35 archived.

### Code audit, 2026-08-19

Requested as a general correctness pass. Compiled at `/W4` (the build normally uses `/W3`): one
warning, an unreferenced parameter. The real findings came from reading, not from the compiler.

**1. Three pointer-keyed caches held no reference - the run-20 bug, twice more.**

`g_meshAlbedo` and `g_rtTextures` were fixed on 2026-08-18 by taking a reference, because a
released object's address can be handed straight back to a newly created one. Three caches were
never given the same treatment, and none of them erases entries, so the stale-key window is the
whole process lifetime:

| cache | keyed on | what a recycled address returns |
|---|---|---|
| `g_shaders` | shader pointer | the PREVIOUS shader's reflection - wrong sampler names, wrong albedo rank, wrong `skinned` flag |
| `g_layouts` | declaration pointer | another mesh's field offsets, which skinning reads blend weights and indices through |
| `g_instCache` | vertex buffer pointer | one object's instance transforms served to another |

`g_shaders` is the worst of the three because it does not crash - it silently mis-classifies
draws, which is much harder to find than a crash and matches the shape of "surfaces are the wrong
colour". All three now `AddRef` on insert.

Note the two changes depend on each other: inserts became `emplace` (which does NOT overwrite an
existing key) and that is only safe BECAUSE the reference makes address reuse impossible. Either
one alone would be wrong - `emplace` without the reference would preserve stale data on a
recycled address, which is worse than the `operator[]` it replaced.

**2. A full bind-pose cache was a permanent cliff.** `GetBaseMesh` refused every new mesh once the
1,024-entry cap was reached, and a refusal falls back to pass-through - so past that point every
newly seen character would silently stop animating and gain a second copy, forever, with only a
counter to show for it. Now the cache is flushed (releasing its pinned buffers) and rebuilt.

**3. A failed skinning-buffer creation retried on every draw.** `CreateSkinBuffer` returned early
only on success, so a failure meant thousands of `CreateVertexBuffer` calls a frame, each a round
trip across the 32->64 bit bridge, on the one path that must stay cheap. Now tried once.

**4. `D3DLOCK_DISCARD` was paired with a sub-range lock.** DISCARD's contract is "I am about to
overwrite the ENTIRE buffer"; combining it with an offset and size is not something D3D9 promises
anything about. It happened to work under DXVK. A discard now locks `(0, 0)` and indexes into the
returned pointer; only NOOVERWRITE takes the sub-range form.

**Checked and found correct**, recorded so they are not re-audited:

- bone palette indexing - `bone < 64` gives a maximum register of 244 against `kMaxVsConst` 256;
- lock/unlock balance - nine lock sites, nine unlock sites, and every `return` between a lock and
  its unlock is the lock's own failure path, where there is nothing to unlock;
- the ring buffer's offset arithmetic, including the wrap case and the non-negative stream offset;
- `mesh.owner` is assigned and referenced before the `std::move` into the map, not after.

One comment had drifted onto the wrong block during the dedup edit and was moved back.

Built `5f5c1aadbcb2928421b194a6ea79940b`, deployed and verified.

### Run 36: two of the three reported issues had a measured cause in the log

Reported: (1) the world is not textured, (2) particle/decal textures - blood, bullet holes, tyre
tracks - appear fullscreen as a plane blocking the camera, (3) the character is still drawn twice.

**(1) The world losing its texture: the mesh albedo cache hit its cap.**

```
albedo: ... restored from mesh cache 195/frame (4096 meshes cached)
```

4,096 is exactly `kMaxMeshAlbedo`. The insert was guarded by `g_meshAlbedo.size() < kMaxMeshAlbedo`
and the comment beside it said "at the cap the cache simply stops growing" as though that were
the safe outcome. It is not. This cache exists to hold a mesh's original texture across the
streamer swapping it, so once full, **no newly streamed mesh can ever be protected again** - and
the world progressively loses its textures the longer the session runs. That matches the report
exactly, and it is the same permanent cliff the bind-pose cache had, found in the same audit and
fixed in the same way: flush (releasing the pinned textures) and rebuild.

Two caches, the same mistake, written months apart - the pattern is that a bounded cache needs an
eviction policy, and "stop accepting" is not one.

**(3) The double-draw: 55 refusals a frame, one cause, and it was a decoder gap.**

```
skinning REFUSED #1: blend weights are a type this decoder does not read
  | verts=2892 stride=32 decl: pos=float3(2)@0 normal=ubyte4n(8)@12 weights=?(-1)@-1
    indices=ubyte4(5)@20 uv=short2(6)@24
```

`weights = ?(-1)@-1` is not an unknown TYPE - it is **no BLENDWEIGHT element at all**, beside a
perfectly ordinary `indices=ubyte4@20`. That is rigid single-bone attachment: each vertex follows
one bone with an implicit weight of 1. The decoder demanded a weight element the format never
had, and every one of those draws fell back to pass-through - which is precisely a second,
Remix-reconstructed copy beside the skinned one. Refusals had risen 23 -> 55/frame as more NPCs
appeared, which fits "double draw on my character and NPCs".

Handled by treating an absent BLENDWEIGHT as weight 1.0 on the first index; the other three slots
stay zero and the skinning loop already skips influences on weight, so their indices are never
dereferenced.

**(2) Particles fullscreen: instrumented, not guessed.** The obvious suspects are already ruled
out by the counters - UI demotion is 1/frame and post/composite marking is 58/frame, neither
large enough to be "blood, bullet holes and tyre tracks". A small quad that covers the screen has
been given a transform that is not its own, so `ProbeDecal` records the TRANSFORM of converted
draws of <= 12 vertices - the three basis-vector lengths of the world matrix, its translation, and
the camera position - named once per distinct sampler. No buffer lock, which keeps it off the list
of probes that quietly cost a lock per draw forever.

Also noted, not yet chased: `back-buffer clears` reads 0/frame where it read 1/frame in run 33.
The surface is captured at startup (logged), so `SetRenderTarget` is simply never naming it. If
nothing clears the back buffer, whatever the game last rasterised there persists - which is a
candidate for (2) and is recorded here so it is not rediscovered from scratch.

Built `ed3b04e3afcea9a93572b35c6e4d2f57`, deployed and verified. Run 36 archived.

### The decal plane: four theories ruled out from data before writing any fix

Clarified by the user: not particles - **decals**. *"like if i shoot the ground. a plane
fullscreens my camera."*

That reading suggested deferred decal volumes: a bullet hole drawn as a BOX that projects onto the
G-buffer, harmless in rasterisation because it only writes where geometry already is, but a real
solid box once converted to fixed function - and shooting the ground puts the camera inside it.
It is a good theory and the data does not support it:

- **Depth-buffer sampling.** A projected decal must read depth to reconstruct world position. 75
  shaders in the game sample a depth buffer and NONE of the decal-named ones are among them - they
  are particles (`rl_particle_*`), light volumes (`ir_light_*`) and projectors (`clb_projector`).
  The `ir_at_sr3decalonly_*` / `ir_bbsimple2_decal_*` family are ordinary alpha-tested surface
  materials.
- **Stale world matrix**, the obvious alternative for a quad in the wrong place at the wrong size:
  `objTM used without a fresh upload 0/frame`. Not that either.
- **UI demotion**, which would rasterise a draw as a 2D overlay: 1/frame.
- **Post/composite marking**: 58/frame, and stable - not the population that appears when the
  player shoots.

So the transform is not stale, the shader is not a projector, and neither screen-space path is
catching them. Rather than invent a fifth theory, `ProbeDecal` measures the thing that must be
true for the symptom to occur: a small converted draw whose world matrix is not small. It records
the three basis-vector lengths of the world matrix, its translation and the camera position, once
per distinct sampler, with no buffer lock.

Its vertex cap was raised 12 -> 64 after writing it: a bullet hole is four vertices, but the
engine may batch many impacts into a single draw, and a cap tight enough to mean "one quad" would
have missed precisely that case.

Built `16308d6688fbdf48046785ea7a67a2d9`, deployed and verified.

### Run 37: the decal probe named it, and the safe rule was narrower than the obvious one

The probe fired on twelve distinct materials. Two lines pointed straight at the answer:

```
#6  ps='Depth_bufferSampler'      verts=4  translation (95.8 144.5 5.2)  camera (96.2 147.7 36.1)
#11 ps='Decal_diffuse_mapSampler' verts=16 translation (69.8 144.5 3.3)  camera (95.2 151.6 14.8)
```

Small quads, near the player, being CONVERTED. Tracing `Decal_diffuse_mapSampler` back through the
shader database identifies them exactly: **`ir_decal_screenspace`** and
**`ir_blood_pool_screenspace`** - bullet holes and blood splatters, by name.

A screen-space decal is drawn as a quad or box that projects its texture onto whatever the
G-buffer already holds. It is clipped to real geometry by the projection, so its own extent can be
arbitrary. Converted to fixed function that proxy becomes an actual surface in the world, and
standing inside one fills the view: *"if i shoot the ground, a plane fullscreens my camera."*

**The obvious rule was unsafe and was rejected.** "A shader that samples a depth buffer is doing
screen-space reconstruction" is true, and it would have hidden 73 shaders including all five
water shaders - `ir_sr3standingwater*` and `ir_sr3dynamic_water1*` sample the depth buffer for
soft edges and are entirely real surfaces. Hiding the world's water to fix a bullet hole would
have been a straight trade down, and the check that caught it took one query.

**The rule that shipped** tests for `IR_GBuffer_Normals` instead. Reading the ORIENTATION of
geometry already on screen is something only a screen-space pass needs; a real surface carries its
own normals. Verified across every pixel shader in the game first:

```
30 shaders sample IR_GBuffer_Normals:
   22  ir_light_*                 light volumes - already hidden separately
    4  rl_ssao_* / rl_rao_*       ambient occlusion
    1  ir_decal_screenspace       bullet holes
    1  ir_blood_pool_screenspace  blood splatters
    0  real surfaces
```

Water is absent from that list, which is precisely why this test was chosen over the depth one.
Same safety shape as the L-buffer rule: the category that could be wrongly hidden is empty.

**Stated plainly: these decals are LOST from the path-traced scene, not corrected.** Fixed
function cannot express a projection onto the depth buffer, so the choice is between not seeing a
bullet hole and having it fill the screen. If they are wanted back later it needs Remix-side
decal support, not a conversion.

Built `ad05669a2c760e929439ba647fd36ecd`, deployed and verified.

### Run 38: five reports, one of my rules dead on arrival, one regression

**My screen-space rule never fired: `screen-space passes hidden 0/frame`.** It was written against
`ir_decal_screenspace` / `ir_blood_pool_screenspace`, identified from the sampler name
`Decal_diffuse_map`. That identification was wrong. Every shader in the game carrying
`Decal_diffuse_mapSampler` is `ir_decal_c` / `_mc` / `_ms` - ordinary decal materials with
`Normal_Map`, `Specular_Map`, `IR_LBuffer` and NO G-buffer normals and NO depth sampling. The
probe even showed their transform is sane: scale (1,1,1), translation 37 units from the camera.
They are real world-space decals and they are not what blocks the view.

Lesson: a sampler name identifies a FAMILY, not a file. `Decal_diffuse_map` appears in eleven
shaders and only two of them are the screen-space ones.

**What is actually blocking the camera: particle billboards.** Probe lines #6 and #7 - four
vertices each, `Depth_bufferSampler` and `Diffuse_Map_1Sampler`, one carrying an objTM translation
of (0, 0, -1024). Those are `rl_particle_*`, whose VERTEX SHADER builds the quad from camera basis
vectors and per-particle data; the vertex buffer holds corner offsets, not positions. Fixed
function cannot reproduce that, so converting one submits corner data as geometry and it lands
anywhere at any size.

The discriminator is the sampler SPELLING, and it is exact:

```
Depth_bufferSampler      29 shaders - every one rl_particle_*, none reading the L-buffer
IR_GBuffer_DepthSampler  40 shaders - this is what WATER uses
Depth_mapSampler          5 shaders - projectors
```

This is the same rule the "samples any depth buffer" version would have been, minus the five water
shaders it would have deleted. Two attempts at this rule, and the difference between them is one
string.

**Regression: rigid single-bone skinning made clothing vanish.** Enabling it took refusals from
55/frame to 0 and, in the same run, "some character clothes items have vanished". Nothing else in
that build touches clothing. Gated OFF as `skinRigidSingleBone=0`. The reasoning still looks sound
- BLENDINDICES with no BLENDWEIGHT really is the shape of rigid attachment - but so did the tint
modulate and so did the dedup. The likely truth is that these meshes are not bone-driven at all,
so skinning them moves them off camera, which reads as vanishing rather than as a wrong pose.
Off is the better failure: a garment drawn twice is visible and wrong, a garment moved out of the
world is simply gone.

It also settles one thing: **refusals were NOT the double-draw.** Refusals hit 0 and the character
is still drawn twice.

**Still unexplained, carried forward:**
- the world showing only flat material colour rather than textures, with `constant-only 84/frame`
  and `blanked 84/frame` against 999 converted draws - so the counters say most draws DO receive a
  texture, and the screen disagrees;
- the double-draw on characters, now with refusals ruled out;
- windshield glass and door windows appearing to move with the camera.

Built `c0cb15ff7c778cd5cdd9c572e77f538a`, deployed and verified. Run 38 archived.

### Run 39: particle rule works; dedup returns with the pose in the key

```
particle billboards hidden 5/frame | screen-space passes hidden 0/frame | colourless 29/frame
SKINNING: 94 skinned/frame, 6 refused/frame | 115 meshes cached
```

The particle rule fires. `skinRigidSingleBone=0` brought most clothing back, confirming that gate
was the right call and that the population is real (6 refusals/frame remain, all the
BLENDINDICES-without-BLENDWEIGHT layout at stride 28).

**Hair is NOT one of my rules.** `ir_sr3pchair_c/_mc` shaders [8] and [9] carry `Diffuse_Map` and
`IR_LBuffer`, so they rank 100 and convert normally; only their prepasses [5]-[7] are hidden, which
is correct. Checked before assuming, because "I hid something" was the obvious guess and it is
wrong. Hair remains unexplained.

**The dedup returns, with the pose folded into the key.** Face z-fighting on NPCs is the
double-draw seen close up - two coincident copies of one surface. The two previous attempts both
failed by keying on too little:

1. no index range - every material sub-range of a 7,977-vertex mesh counted as a duplicate, and
   the "69 of 88 repeats" figure was an artefact of the key;
2. no pose - two NPCs in the same garment hashed identically, so the second lost its clothing.

The key now hashes eight bone matrices from the c52 palette alongside the geometry. Two characters
differ in their palette; one mesh submitted twice in one frame in one pose does not. 96 floats per
draw against ~94 skinned draws a frame.

Geometry alone never identified an object - that is the same mistake in both earlier attempts, and
the pose is the missing half.

Built `704077f6f3feb802cc45b63f2ec18ef7`, deployed and verified. Run 39 archived.

### Run 40: the dedup key was too small a THIRD time, and a probe for the flat-colour world

**NPC parts still missing, and the dedup is why.** `9-10 exact duplicates dropped/frame` against
"some npcs are missing parts". The bone palette is in OBJECT space - objTM is applied after the
blend, which this project established from the disassembly on 2026-08-18 - so two idle NPCs
holding the same pose have **byte-identical palettes** and differ only in where objTM puts them.
The pose key merges them and the second loses parts.

That is the same error three times:

| attempt | key | what it merged |
|---|---|---|
| 1 | buffer + vertex range | every material sub-range of one mesh |
| 2 | + index range + triangle count | every NPC sharing a garment |
| 3 | + bone palette | every NPC sharing a garment AND a pose |
| 4 | + objTM | - |

Each fix was correct as far as it went and each left out one more thing that distinguishes an
object. Writing it down as a table because the pattern is more useful than any of the individual
fixes: an object is geometry AND pose AND position, and dropping any one of the three merges
things that are not the same.

**The flat-colour world: the counters and the screen disagree, so measure the texture itself.**
Ruled out first: the UV texture matrix is correct at 0.00098 (= 1/1024, the scale settled by
disassembly), so short2 UVs are being scaled properly and the "UVs collapse to one texel" theory
is dead. Every other counter says a real texture is bound - 0 named-but-not-bound, 0 left empty by
the render-target exclusion, 84 blanked against ~999 converted.

But those counters only record that a POINTER was non-null. They never record what it points at.
`ProbeBoundAlbedo` now logs the stage-0 texture's dimensions, format, mip count, pool and usage
for converted draws, once per distinct sampler. A 4x4 or 1x1 surface renders as exactly one flat
colour - the reported symptom - whereas a 1024x1024 DXT means the texture is fine and the problem
is on Remix's side of the bridge. Those need completely different work.

**This also matters for the stated end goal.** The user intends to replace assets with the Saints
Row: The Third Remastered set through Remix. Remix keys a replacement off the ORIGINAL texture's
hash, so whatever this probe reports is what the replacements will be authored against. A wrong or
unstable texture reaching Remix means every replacement is mapped to the wrong hash - which makes
"what is actually bound" a prerequisite for that plan, not a side quest.

Built `25e11071f2c20016f76254fb22c6bde6`, deployed and verified. Run 40 archived.

### Every injected light was sharing D3D9 slot 0

Reported: flashing polygons on random frames; light hashes churning, worse further from the
camera; and a shotgun flashlight leaving a **trail of lights hanging in mid air** that fades after
a second or two, visible in Remix's light debug view.

The trail is the diagnostic one, and it led straight to `EmitLight`:

```cpp
if (SUCCEEDED(g_origSetLight(dev, 0, &light))) {
    g_origLightEnable(dev, 0, TRUE);
```

**Index 0, for every light in the frame** - about 46 of them, written through one slot one after
another. A D3D9 light index is the only handle Remix has for correlating a light with its
previous-frame self. Rewriting slot 0 forty times a frame tells Remix that a single light
teleported forty times, and that next frame it did so again with entirely different values.
Nothing can be matched across frames, so:

- **hashes churn** - a light's hash comes from properties that, at slot 0, belong to a different
  light on every call, and distant lights churn worst because they are the ones whose ordering
  shifts as they stream in and out;
- **the flashlight trails** - each frame's flashlight is a NEW light to Remix, so the previous
  one is kept alive for a few frames instead of being recognised as the same light having moved.
  The trail is Remix's light-keeping doing exactly what it is supposed to do, fed a lie;
- **polygons flash** - unstable lights destabilise everything they illuminate.

One line, three symptoms. It had been there since lights were first injected and every counter
looked healthy the whole time: `lights 46.8/frame` is a perfectly good number for a completely
broken arrangement, which is worth remembering the next time a counter is used as evidence that
something works.

**Fixed:** each light now takes its own slot, numbered by emission order, capped at 64. Slots the
current frame did not use are explicitly disabled at Present - without that, a light that goes
away stays lit at its last position forever, which is the same trail by another route.

Emission order as identity is imperfect: a light appearing mid-list shifts every later light by
one slot. But the engine submits its light volumes in a consistent order within a frame, so it is
stable in the common case, and it is enormously better than one shared slot. If ordering proves
unstable the next step is a position-derived slot, not a return to slot 0.

Remix's own `rtx.suppressLightKeeping` would also hide the trail, and was deliberately NOT set:
it would mask the symptom while leaving every light unmatched across frames, and the hash churn
and flashing would remain.

Built `d2c013ed2c027b31e9b5c465f934b4b0`, deployed and verified.

### Run 41: the world's textures are PROVEN correct, and the light fix needed a second half

**The flat-colour world is not our binding.** `ProbeBoundAlbedo` settles a question that six
counters could not:

```
ALBEDO BOUND #1: 'Diffuse_MapSampler'  -> 512x512   DXT5     mips=8
ALBEDO BOUND #2: 'Diffuse_mapSampler'  -> 1024x512  DXT5     mips=8
ALBEDO BOUND #4: 'Blend_MapSampler'    -> 2048x1024 X8R8G8B8 mips=9
ALBEDO BOUND #5: 'Decal_MapSampler'    -> 512x512            mips=10
```

Full-resolution authored textures with complete mip chains, in the default pool, bound to stage 0.
The shim is handing Remix exactly what it should. So "I only see basic material colour" is
downstream of us, and no further work on albedo selection can fix it. That is worth as much as a
fix: it closes off the entire area the last several sessions kept circling.

The counters could never have shown this. Every one of them recorded that a POINTER was non-null;
none recorded what it pointed at. A 1x1 texture and a 2048x1024 texture are the same "1" to a
counter.

**The light-slot fix was half a fix, and the missing half broke the flashlight.** Lights per frame
fell 46.8 -> 28.0 and the shotgun flashlight stopped working. D3D9 limits how many lights may be
simultaneously ACTIVE (`D3DCAPS9::MaxActiveLights`); past that limit `SetLight` still succeeds and
`LightEnable` quietly fails. Only `SetLight` was checked, so lights beyond the limit were stored
and never lit - and the flashlight, emitted late in the frame's list, was one of them.

Now: the enable is checked as well, the slot count comes from `MaxActiveLights` (asked at device
creation and logged, with 0 meaning "no limit" per the D3D9 spec) instead of an assumed 64, and a
light that cannot get its own slot falls back to slot 0 rather than being dropped. The overflow
therefore behaves exactly as everything did before, while the first N lights keep stable
identities.

Two counters in a row - `lights 46.8/frame` before, `28.0/frame` after - looked healthy while
describing broken behaviour. Same lesson as the textures.

**Dedup is off, and this time by policy.** Three attempts, three regressions - merged sub-ranges,
merged NPCs, merged NPCs in matching poses - and not one run where it demonstrably reduced the
double-draw it was written for. The key is complete now and the analysis is sound, but it has
never paid for itself, and "some npcs are missing parts" is too high a price for an unproven
benefit. `dedupSkinned=0`.

Built `e8773ca2e2ed11972115b4f2332f4b38`, deployed and verified. Run 41 archived.

### The flat-colour world: sampler state was never set at all

The user's aside is what cracked it: *"quick thing. trees have albedo texture."* Trees work, the
rest of the world does not - so whatever is wrong distinguishes foliage from ordinary surfaces.

`grep D3DSAMP_` over the whole shim returns **nothing**. The conversion sets render states,
texture stage states, transforms and textures - and never once sets a SAMPLER state. So every
converted draw sampled with whatever the engine had last configured for one of its own passes,
and the engine has no reason to leave a mode that suits fixed function: its pixel shaders compute
their own coordinates.

Why that splits trees from everything else:

- foliage UVs sit inside a single atlas cell, so they land in 0..1 and CLAMP does nothing to them;
- a tiled world surface has UVs past 1 BY DESIGN - the per-material tiling registers this shim
  already reads exist precisely because these surfaces repeat - and under CLAMP every pixel of
  such a surface samples the same edge texel.

One inherited state, and exactly the observed split between what works and what shows a single
flat colour.

This also explains why `ProbeBoundAlbedo` found nothing wrong: it did not. The right texture, at
full resolution with a complete mip chain, was bound the whole time - and then sampled at one
texel. The probe answered its question correctly and the question was not the problem. Worth
recording, because "the texture is correct" was taken as "the texture path is fine".

**Set now for stage 0:** ADDRESSU/V to WRAP (what the game's own world materials use), and
MAG/MIN/MIPFILTER to LINEAR - the textures arrive with 8 to 10 mip levels, which are wasted if
the inherited MIPFILTER happens to be NONE.

**And instrumented, not assumed:** the inherited values are logged six times before being
overwritten. If they read CLAMP (3) the diagnosis holds; if they already read WRAP (1) then
sampler state was never the problem and this change is inert - which needs to be known before any
more of the flat-colour work is built on top of it.

Built `c8aca04ae8fadfb13702f4e2ca0733ba`, deployed and verified.

### Tree leaves: the cutout is done inside the pixel shader, so our cutout rule never saw it

The user guessed the leaf texture was "scaled wrong or use parallax". Both were checked and both
are wrong, which is worth recording because the real answer was in neither place.

**Scaling is correct.** `tree_s.fxo_pc` shader [0] emits its texcoord as

```
mul o1.xy, c4.w, v1
def c4, 1000, 0.159154937, 0.5, 0.0009765625
```

`c4.w` is 0.0009765625 - exactly 1/1024, the same scale this shim already applies to short2 UVs.
No hidden tiling register, no parallax. The tree vertex shader does have leaf and frond WIND
animation, which fixed function cannot reproduce, but that displaces geometry rather than
texture.

**The cutout is the problem.** The tree pixel shader:

```
float Alpha_Threshold;   // c41
texkill r0
```

It kills the fragment itself. `D3DRS_ALPHATESTENABLE` is never involved, so the existing rule -
"a real cutout is alpha test ON and ref > 0", inherited from sr2-fork.md section 3 - cannot see
it and hands the draw opaque alpha. Every leaf card therefore rendered as a solid rectangle with
the leaf shape discarded, which is exactly "tree leaves texture is not working correctly".

403 pixel shaders do this. The families name themselves: **`ir_at_*` - at for alpha test** -
plus foliage, decals, windows and cloth. So this was never only about trees; it is every
alpha-cutout material in the game.

The sr2 rule was not wrong, it was incomplete: it correctly stops opaque walls going X-ray from
sub-1.0 texture alpha, and it has no way to recognise a cutout the shader performs privately.
Both tests are needed.

**Fixed:** a pixel shader declaring `Alpha_Threshold` counts as a cutout, takes texture alpha,
and gets a real alpha test built from its own threshold constant - which is also the signal Remix
needs to treat the surface as a cutout rather than as glass.

The alpha states are SAVED and restored in EndFFP. That function restores textures and shaders
and nothing else, so a render state left set there follows the engine into its own draws, and
alpha test decides which of its pixels survive. Texture stage states needed no such care - the
engine binds pixel shaders, which ignore them entirely.

Built `65d00b3d15d6fc7fd237ae7b36e069d8`, deployed and verified.

### Run 42: the sampler theory was wrong, and the alternating NPC faces are our cache

**The sampler fix is inert, and the probe is what proved it.**

```
INHERITED sampler0 before conversion #1: addressU=1 addressV=1 mipfilter=2 magfilter=2
```

WRAP and LINEAR already. The state we inherited was correct all along, so setting it changes
nothing and the CLAMP explanation for the flat-colour world is dead. Recorded rather than quietly
dropped, because the reasoning was good - trees working while tiled surfaces did not is exactly
what CLAMP looks like - and it was still wrong. The instrument was added in the same build as the
fix precisely so this could be settled in one run instead of being believed.

Where that leaves the flat-colour world: texture binding is proven correct (full-resolution DXT
with complete mip chains), UV scale is proven correct (1/1024, confirmed twice - from the shim's
own applied value and from `c4.w` in the tree vertex shader), and sampler state is proven correct.
Three of the four things on our side of the bridge are eliminated.

**NPC faces alternating between wrong faces: the mesh albedo cache.** Its key is
(vertex buffer, base vertex, pixel shader) - and every NPC face in the game shares all three. Same
head mesh, same buffer, same character shader; only the bound face TEXTURE differs. So the cache
treats them as one mesh and rebinds whichever face it saw first onto all of them, alternating as
entries are populated and flushed.

The comment directly above that key already describes this exact failure - "the same car body in
different paint... collapses onto whichever texture was seen first" - and the fix at the time was
to add the pixel shader to the key. That cannot help when the shader is shared too. **Skinned
draws are now excluded from the cache entirely**: there is no property available at that point
that separates two characters (pose and objTM would, and hashing those per draw is the cost the
dedup already showed is not worth paying), and the cache is not needed for them anyway - it exists
to survive the STREAMER evicting a mesh's unique texture, and a character's face is rebound every
frame regardless.

### Vibe-RE toolkit integrated

Cloned to `tools/vibe-re/`, dependencies installed, `verify_install.py` reports all required
checks passing. Written up in `docs/vibe-re-tools.md`.

The find that matters is not a tool: the repository carries a `dx9-ffp-port` SKILL describing this
exact task, and two of its documented pitfalls name bugs we have open - "bones mixed up between
NPCs: stale slots from a previous object" and "everything is white/black: albedo on stage 1+". It
also names the #1 Remix porting mistake as a pre-multiplied WorldViewProj, which SR3 does not do
and this project had already established independently.

One hard constraint recorded so it is not discovered the expensive way: **the DX9 tracer ships its
own `d3d9.dll` and Remix IS a `d3d9.dll`.** They cannot coexist in the game directory, so a tracer
capture shows what the GAME submits with Remix absent. That is exactly right for engine questions
and useless for Remix ones. Our ASI hooks the device vtable and is unaffected either way.

It also provides a concrete procedure for the class of bug that has now cost four dedup attempts -
telling one object from another at the D3D9 boundary - by finding the engine's per-object function
through tracer hotpaths and confirming the call count matches the NPC count under Frida.

Built `cc6bbdcec9f8f93678ddc143e079a394`, deployed and verified. Run 42 archived.

---

## Consolidation, 2026-08-19

At the user's request, everything learned so far is now written down in a form that survives this
session. `docs/YOUR-INSTRUCTIONS.md` was substantially rewritten, since its status section still
described run 17 while the project is at run 42:

- **Current state** replaced with two tables - SOLVED, each row carrying the measurement that
  settled it, and OPEN, each carrying what has already been ELIMINATED. The second matters more:
  the flat-colour world has three separate disproofs on our side of the bridge, and recording them
  is what stops the next session re-testing binding, UV scale and sampler state.
- **Established engine facts** gained the skinning algorithm (c52, 3 regs/bone, 64 bones, objTM
  applied after the blend, UBYTE4 weights, 255 sentinel), the skinned vertex declarations
  including the rigid single-bone variant, and the fact that alpha cutouts are performed inside
  the pixel shader by `texkill` against `Alpha_Threshold` in 403 shaders.
- **Property rules** written up as a table with the safety check for each: the count of shaders it
  hides and, crucially, the count of REAL SURFACES at risk - zero in every case. Also the note
  that `Depth_bufferSampler` (particles), `IR_GBuffer_DepthSampler` (water) and `Depth_mapSampler`
  (projectors) are distinct spellings, and that a rule written against "any depth buffer" would
  have deleted the water.
- **A new section on changes measured worse and reverted**, with the symptom that killed each.
  Five entries so far, every one of which looked correct when written.
- **The dedup lesson** as its own table: four attempts, each adding one more component to the key,
  each still merging things that were not the same object. An object is geometry AND pose AND
  position.
- **"Counters are not evidence that something works"**, with the three cases where a healthy
  number described broken behaviour - lights at 46.8/frame while every light shared slot 0, a
  no-albedo counter that counted rescued draws too, and albedo counters that recorded a non-null
  pointer while the surface sampled one texel.

The "Untextured materials" section was marked superseded rather than deleted, with a pointer to
the rule that replaced it and a note on why the sampler-name list could never have been complete.

`docs/vibe-re-tools.md` added and linked from the file map.

## Session 18, 2026-08-19 - putting the toolkit to work: two dead ends and one stale-config find

The instruction was to use the Vibe-RE toolkit to fix the outstanding issues. Two of its three
routes turned out not to apply to this game, and establishing that cheaply is most of the value
of this entry - both would have cost a broken game directory or an afternoon to discover the
hard way.

### The DX9 tracer CANNOT be deployed on SR3 (verified, not assumed)

`find_d3d_calls.py` on `SaintsRowTheThird.exe`:

    DLL: d3d9.dll
      IAT: 0x0101C510  D3DPERF_GetStatus
      IAT: 0x0101C514  D3DPERF_EndEvent
      IAT: 0x0101C518  D3DPERF_BeginEvent
      IAT: 0x0101C51C  D3DPERF_SetOptions
      IAT: 0x0101C520  Direct3DCreate9
    [.rdata] 0x012A0110: d3d9.dll
    [.rdata] 0x012A011C: Direct3DCreate9Ex

The game **statically imports five symbols** from `d3d9.dll` and additionally resolves
`Direct3DCreate9Ex` by name at runtime. The tracer's `d3d9.def` exports exactly one:

    LIBRARY d3d9
    EXPORTS
        Direct3DCreate9 @1

So dropping the tracer into the game directory fails at load time on IAT resolution - the game
would not start at all, never mind trace. And even past that, SR3 prefers the Ex entry point
(our own marker code already records that SR3 comes up through `CreateDeviceEx`, since D3D9Ex
rejects `D3DPOOL_MANAGED`), which the tracer does not wrap.

Using the tracer here means adding the four `D3DPERF_*` forwards plus `Direct3DCreate9Ex` and
full `IDirect3D9Ex` / `IDirect3DDevice9Ex` wrappers. That is a real change to shared tooling,
not a deployment step. **Not attempted; the game directory was not touched.**

Recorded because the `dx9-ffp-port` skill's skinning-stability procedure opens with "capture 2+
frames with the D3D9 tracer" and every later step depends on that capture.

### Static PE analysis has near-zero yield on SR3

`find_skinning.py` on the game exe:

    No skinned vertex declarations found.
    No FVF skinning patterns found.
    No bone palette patterns detected.
    D3DRS_VERTEXBLEND: 1 site(s), values: DISABLE
    -> [Skinning] Enabled=0  ; No skinned meshes detected in this binary.

Which is flatly wrong - SR3 has 221 skinned shader files and this shim skins 69 draws a frame.
The scanners look for literal register numbers and declaration blobs in the binary, and SR3 is
**data-driven**: vertex declarations and constant register assignments come out of the `.fxo_pc`
shaders and packfile format tables, not out of immediates in the exe.

The consequence is that the skill's fallback route - find the bone-upload call site statically,
then walk up to the per-object boundary - has nothing to start from either.

**`re/shader_constants.csv` is this project's substitute for those scanners, and it is
strictly better**: 52,991 rows of real constant and sampler bindings parsed from the shaders
themselves, including the register numbers the static scanners failed to find. Reach for it
first. The retools *string* search on binaries remains useful (see below); it is the
pattern-matching scanners that do not apply.

### The rank-0 "flat colour" draws are correct behaviour, not lost textures

The frame dump shows 203 converted draws a frame naming `IR_GBuffer_DSF_DataSampler` at rank 0 -
large meshes, 5,000-7,000 vertices, exactly the shape of "buildings rendering flat". Checked
against the shader database rather than guessed at:

| population | count |
|---|---|
| pixel-shader entries sampling `IR_GBuffer_DSF_DataSampler` | 1329 |
| of those, max `AlbedoRank` == 0 | **121** |
| of those 121, sampling ONLY the DSF buffer and the L-buffer | 69 |

and the files are `ir_bbsimple2_nodiffmap_*`, `editor_filled_*` and friends. The name says it:
**`nodiffmap`**. These materials have no colour map by design and take their base colour from a
constant, which is what the constant-albedo path already does for them (114/frame, with
`genuinely blank 0/frame`).

So this population is *not* a texturing failure and no amount of sampler-ranking work will
change it. Cross off "the big rank-0 draws are losing their diffuse map" as a cause of the
flat-colour world.

### The actual find: rtx.conf is still carrying texture tags from before the FFP pipeline

Searching **Remix's own binary** for its option documentation (`retools.search strings` on
`.trex/d3d9.dll`, 190 MB) turned up the sentence that matters:

    Requires "rtx.terrainBaker.material.replacementSupportInPS_fixedFunction = True"
    to apply for draw calls with fixed function graphics pipeline.

Every world draw this shim produces is a **fixed-function** draw. That flag has never appeared
in `rtx.conf`. So the eight textures in `rtx.terrainTextures` have been going into the
experimental Terrain Baker, whose material replacement does not apply to our draws.

Config history confirms the tags are stale rather than considered:

| config snapshot | `rtx.terrainTextures` |
|---|---|
| `before-tag-reset.bak` | absent |
| `tagged-2026-08-13.bak` | present (session 9 hand-tagging) |
| every snapshot since, including current | carried forward unchanged |

They were chosen on 2026-08-13, when the world still reached Remix through **vertex capture from
shader output** - a programmable-shader draw, where terrain baking does apply. The premise died
when the world became fixed-function and the tags were never revisited.

Two more categories in the same state, and a third pattern that is plainly accidental:

- `rtx.lightmapTextures` - tells Remix the texture is baked lighting rather than albedo, which
  suppresses base colour on whatever surface uses it.
- `rtx.hideInstanceTextures` - hides the instance outright.
- One hash, `-0x3869FE5976C427DA`, appears in **seven** unrelated categories
  (ignoreBakedLighting, smoothNormals, antiCulling, ignoreAlphaOn, animatedWater,
  opacityMicromapIgnore, postfx.motionBlurMaskOut) and another, `-0x645CF1DD53FF6357`, in six.
  A texture cannot coherently be all of those at once. The 2026-08-12 entry above records this
  exact failure mode once already - "user's in-game texture clicking silently tagged 100+
  textures into terrain/lightmap/... categories (invisible effects, felt like nothing
  happened)". It has crept back.

**Change made:** those ten lines removed from `configs/rtx.conf` (backup
`configs/rtx.conf.before-stale-tag-cleanup.bak`). Deliberately left alone:

- `rtx.ignoreTextures` - carries the marker hash `0x978271113F293CE4`, identified by diffing
  `before-marker-ignore.bak` against the current file. It is load-bearing for ~1,863 marked
  draws a frame; removing it puts the magenta prepass shells back on screen.
- `rtx.skyBoxTextures` - the shim passes the sky through specifically so it can be tagged.
- `uiTextures` / `worldSpaceUi*` / `particleTextures` / `raytracedRenderTarget` / `playerModel`.
- `rtx.ignoreLights` - part of the `-0x645C...` smear, but lights currently work and that is one
  hypothesis too many for a single run.

### Probe added: the weightless BLENDINDICES layout

All 38 refused skinned draws a frame are one layout, and the shim has only ever printed one
refusal reason:

    skinning REFUSED #1: blend weights are a type this decoder does not read | verts=3044
    stride=36 decl: pos=float3(2)@0 normal=ubyte4n(8)@12 weights=?(-1)@-1
    indices=ubyte4(5)@20 uv=short2(6)@24

Each refusal passes through and Remix reconstructs it beside the skinned copy, so these are a
measured contributor to the double draw. Treating them as rigid single-bone attachment - weight
1.0 on index component 0 - was tried on 2026-08-19 and made character clothing vanish.

The reason that experiment could not be diagnosed: `ProbeSkinned` returns early whenever
`blendWeightOffset` is negative, which is precisely this layout. **Nothing has ever read these
bytes.** Component 0 was an assumption.

`ProbeRigidSkinned` (`rigidSkinProbe=1`) transforms one bind-pose vertex by the palette slot
named by *each* of the four index components and prints all four results plus the length of each
matrix's first row. A slot that owns the vertex lands it near the rest of the character with a
row length near 1; a slot that does not lands it at the origin or at a wild coordinate. That
separates "wrong component" from "this layout is not bone-driven at all" - the question the
vanishing clothes left open. Read-only, one frame, six reports, no behaviour change.

### Deployed

    sr3-rtx.asi   6d06c7578e849ea0b1421d2756ed4bc8
    sr3-rtx.ini   bd3460b6feb3ba4fa78179d52ca1021e
    rtx.conf      0d1601063993e546603b242f3c53b0df

All three hash-verified against their masters. Build clean, no warnings.

### Run 43: the rigid-skin probe answered its question, and the flat world points at our own cache

Reported: (1) some props are not in the world, (2) the world is not textured - "maybe textures are
loaded but the path tracer shows the material color", (3) NPCs may have the right face but the head
z-fights and there is no hair, (4) **"didn't we make it so that the path tracer shows the material
color?"**

**(4) first, because it is the sharpest question asked in this project so far.** Yes - `useConstant`
in `SetupTextureStages` deliberately renders a flat constant colour, via
`COLOROP=SELECTARG1, COLORARG1=TFACTOR`, for materials whose shader has no colour map and takes its
base colour from a constant. It is exactly "the path tracer shows the material colour", and it was
built on purpose.

It is not the explanation for this bug, and the number says so: **78 of 756 converted draws a
frame**, 10%, and `genuinely blank 0/frame`. The other 678 have a real texture bound. But the
instinct behind the question was right - the symptom being described *is* a material-colour
symptom, so the thing to look for is a mechanism that removes a texture, not one that fails to find
one.

**(2) The mesh albedo cache is the only mechanism in this shim that substitutes textures, and it is
firing on a third of the world.**

    albedo: moved off stage 0 199/frame, blanked 78/frame,
            restored from mesh cache 238/frame (2217 meshes cached, 3 flushes)

238 of 756. Four things put it under suspicion:

1. It is the only code path that replaces the texture the game chose with a different one.
2. **Its key holds a raw vertex-buffer pointer with no reference on the buffer.** This is the same
   defect already found and fixed in `g_shaders`, `g_layouts` and `g_instCache`: the streamer frees
   a buffer, the allocator hands the address to a different one, and the key now names another
   mesh. The `AddRef` added during the crash fix protects the *texture*; nothing protects the
   *buffer the key is built from*. `g_baseMeshes` pins its `owner` for exactly this reason -
   `g_meshAlbedo` never did.
3. Even with the buffer alive, a static buffer can be re-filled. 2,217 entries with **3 full
   flushes** in one session is roughly 14,500 distinct keys - the regions churn hard, so a repeated
   key is not evidence of a repeated mesh.
4. Timing. "The world is not textured" first appears at run 36, inside the window in which the
   world moved to fixed-function conversion and this cache was added. Session 9's screenshots, before
   that window, show brick, ornate stonework and wood flooring rendering correctly.

Point 2 matters most: this cache was written to survive the streamer swapping a texture, and the
streamer swapping things is precisely when its key stops meaning what it claims. The evidence for
the problem it solves has only ever been the restore count *it produces itself* - no wall has been
watched changing material with the cache off.

`cacheMeshAlbedo=0` for this run. No build needed; the INI already carried the note that the real
question was whether the cache should exist at all.

**(1) and (3): the refused draws.** 25 skinned draws a frame are still refused, all one layout, and
each passes through to be reconstructed by Remix rather than skinned. A prop that Remix then fails
to reconstruct is a prop that is not in the world; a head drawn once by us and once by Remix
z-fights. Both reports have the same shape as that population.

**The rigid-skin probe answered its question, and the answer was not the one being looked for.**

    RIGID-SKIN probe #1: verts=3044 stride=36 indices=ubyte4@20 | objTM t=(-159.88 19.44 -194.94)
        vert 0: bind pos(-1.152 0.025 -0.315) indexBytes[5 5 5 5]
            via component 0 (bone   5): pos(-6.254 0.787 -0.981) row0 length 1.000
            via component 1 (bone   5): pos(-6.254 0.787 -0.981) row0 length 1.000
            via component 2 (bone   5): pos(-6.254 0.787 -0.981) row0 length 1.000
            via component 3 (bone   5): pos(-6.254 0.787 -0.981) row0 length 1.000

**All four index bytes are identical.** Every component names the same bone, so the choice of
component was never capable of being the bug - component 0 was right, and the vanishing clothes had
another cause. `row0 length 1.000` says the palette slot is a clean rotation, so the layout **is**
bone-driven; it is not the "not bone-driven at all" case the earlier note guessed at.

Probes #4 and #5 (`indexBytes[0 0 0 0]`) transform to **exactly** their bind position - bone 0 is
identity for them. Skinning those would be a no-op. Probe #1's bone 5 moves the vertex about 5
units in X, which for a character part is far enough to read as vanished.

That leaves one candidate: **the palette these draws are read against may not belong to these
draws.** A rigid draw does not upload bones, so `c52` still holds whatever the last skinned object
wrote. Skinning by it poses the object with another character's limbs.

Nothing measured this, so `ProbeRigidSkinned` now reports how recently the palette was written.
Deliberately a **distance in draws**, not a per-draw boolean: one upload serves every material
range of a character, so a boolean cleared at each draw would call all but the first of them stale.
Zero means this draw's own setup wrote it; a small number means the same character; a large number
or a previous frame means the palette belongs to something else.

### Deployed

    sr3-rtx.asi   33ebf9ee035d84c348031ba115653ba4
    sr3-rtx.ini   a21f5416b20fcf5e5d4e305649a410a5   (cacheMeshAlbedo=0)
    rtx.conf      0d1601063993e546603b242f3c53b0df   (unchanged from run 42)

One behaviour change this run, so the world's textures are a clean test. Build clean, no warnings.

### Run 44: THE WORLD'S TEXTURE COORDINATES ARE A FORMAT REMIX CANNOT READ

`cacheMeshAlbedo=0` and the world was still flat: `restored from mesh cache 0/frame, 0 meshes
cached`. **The mesh albedo cache is exonerated** - it substitutes textures on a third of the world
and has a real pointer-reuse defect, but it is not this bug.

The answer was in a log this project had never opened. Remix writes
`rtx-remix/logs/remix-dxvk.log`, and in it:

    warn: [rtx-interleaver] Unsupported texcoord buffer format (80), skipping texcoord
    warn: [rtx-interleaver] Unsupported color0 buffer format (109), skipping color0

**"Skipping texcoord" is literal.** The geometry reaches the path tracer with no UVs at all, so
every surface samples one texel of its texture and renders as flat material colour. That is the
whole of "the world is not textured", and it was never on our side of the bridge: we bind the
right texture, at the right stage, with the right sampler state, and Remix then discards the
coordinates needed to read it. Three shim-side disproofs were all correct and all beside the
point.

**Format 80 decoded from Remix's own binary rather than from memory.** `.trex/d3d9.dll` carries
the full `VK_FORMAT_*` name table in enum order; indexing it gives entry 81 (1-based) =
`VK_FORMAT_R16G16_SSCALED`, so VkFormat 80 is `R16G16_SSCALED` - exactly what DXVK maps
`D3DDECLTYPE_SHORT2` to. Entry 110 gives 109 = `R32G32B32A32_SFLOAT`, a float4 COLOR, also
skipped and far less important.

Every world vertex format in SR3 stores texture coordinates as SHORT2 - layouts 3, 4, 5, 6, 7,
11, 21 all read `uv=short2` in the log.

**What confirms it rather than merely fitting it: the three textured populations are the three
that are not SHORT2.**

| population | texcoord format | textured? |
|---|---|---|
| characters | float2 - **this shim rebuilds their vertices** into an FVF for skinning | yes |
| trees, foliage | float2 - a 10,000-instance system whose stream 0 is one `float2 TEXCOORD`, stride 8 | yes - the user's "trees have albedo texture" |
| everything else | short2, straight from the game's declaration | **flat** |

The one population this shim rebuilds itself is the one population that works, and it works
because rebuilding it happens to produce float coordinates. That was an accident of the skinning
port, not a decision.

**The fix reinterprets the same bytes as SHORT2N**, which DXVK maps to `VK_FORMAT_R16G16_SNORM`.
No vertex data is copied and no buffer allocated - only the declaration is cloned, once per
distinct game declaration, and only its TEXCOORD0 element changes. SNORM divides by 32767 where
SSCALED does not, and the uv texture matrix absorbs that: `kSnormUVScale = 32767/1024` instead of
`kShortUVScale = 1/1024`. Exact, because SNORM values are multiples of 1/32767.

Signed rather than USHORT2N/`R16G16_UNORM`: the source is a signed short and negative coordinates
are legal, so unsigned would wrap them to the opposite edge of the texture.

Skinned draws are excluded - `SkinAndBind` replaces the declaration with its own float2 FVF, so
their coordinates already arrive in a readable format.

The declaration cache references both key and value. A raw declaration pointer as a key is the
defect already found in three other caches here; a recycled address would hand a later mesh this
one's substitution.

**If SNORM is not supported either, the log says so in the same words with a different number**
(78 instead of 80), and the answer is then to convert the coordinates to float2 in a buffer of
our own - the treatment the skinned path already gives them.

### The rigid-skin probe: palette freshness ruled out

    RIGID-SKIN probe #1..#6: objTM t=(-95.71 19.01 -176.89)
        | bone palette last written earlier this frame  (3, 4, 5, 6 draws ago)

Same objTM across all six, palette written a few draws earlier in the same frame. **The palette
belongs to these draws.** Combined with run 43's finding that all four index bytes are identical
and every palette row is a unit-length rotation, both easy explanations for the vanishing clothes
are now dead: it is not the wrong index component, and it is not a stale pose.

### rtx.conf: the accidental multi-tag happened again, in this session, and was caught in the act

Remix rewrote `rtx.conf` during the run. Diffing it key-by-key against our master shows a single
hash, `-0x8E4C8047F62D947A`, added to **24 categories** - 18 of them where it is the only entry:

    terrainTextures, ignoreBakedLightingTextures, postfx.motionBlurMaskOutTextures,
    lightConverter, playerModelTextures, smoothNormalsTextures,
    ignoreTransparencyLayerTextures, antiCulling.antiCullingTextures, ignoreAlphaOnTextures,
    beamTextures, animatedWaterTextures, hideInstanceTextures, lightmapTextures,
    decalTextures, opacityMicromapIgnoreTextures, playerModelBodyTextures,
    particleEmitterTextures, uiTextures

plus worldSpaceUi, worldSpaceUiBackground, ignoreTextures, particleTextures, skyBoxTextures and
ignoreLights. One texture cannot coherently be terrain *and* UI *and* a lightmap *and* a player
model *and* hidden. `remix-dxvk.log` shows the same hash being toggled through
terraintextures / ignorelights / ignoretransparencytextures at 23:51-23:52, so this is the
in-menu tagging behaviour the 2026-08-12 entry recorded, recurring.

It matters beyond tidiness: `hideInstanceTextures`, `uiTextures` and `lightmapTextures` each
suppress a surface outright or strip its albedo.

Removed from all 24; the 18 single-entry categories are gone and the other 6 keep their remaining
hashes. Backup `configs/rtx.conf.before-8E4C-smear-cleanup.bak`. The marker hash
`0x978271113F293CE4` and `rtx.skyBoxTextures` are intact, both load-bearing.

The master was re-synced FROM the game's file rather than overwriting it, so the run's other
in-menu changes are kept.

### Deployed

    sr3-rtx.asi   ca745af1180d96af75ec127ba15c1454   (remixShortUV=1)
    sr3-rtx.ini   79984f2ce978225e96d874a2fb91ac73   (cacheMeshAlbedo still 0)
    rtx.conf      7dd794b69d3795043289d55631043a35

`cacheMeshAlbedo` stays at 0 so the only difference from run 43 is the texcoord format. Build
clean, no warnings.

### Run 45: SNORM refused too, so the coordinates are converted rather than re-labelled

The SHORT2N experiment fired exactly as intended and Remix rejected it in the same words with a
different number:

    sr3-rtx.log:      SHORT2 texcoords re-declared as SHORT2N: 448 draws/frame
                      (8 declarations built, 0 could not be)
    remix-dxvk.log:   warn: [rtx-interleaver] Unsupported texcoord buffer format (78),
                      skipping texcoord

78 is `VK_FORMAT_R16G16_SNORM`. **The number tracked our change**, 80 to 78, which confirms the
mechanism a second time: the interleaver is deciding on the declared vertex format, and it wants
real floats. No reinterpretation of those four bytes can produce them.

Rather than guess a third format, the fix uses the one that is already **known** to work on this
path. Two populations prove it, and they are the only textured ones in the game today:

- **characters** - this shim already rebuilds their vertices into a float2 FVF for skinning;
- **foliage** - drawn from a game instance stream whose stream 0 is a single `float2 TEXCOORD`.

`VK_FORMAT_R32G32_SFLOAT` is therefore not a hypothesis.

**The design: convert whole buffers, not draws.**

A float2 buffer is built per source vertex buffer and bound as a second stream, with a clone of
the game's declaration whose TEXCOORD0 points at it. Position, normal and every other element
still come from the game's own buffers, so the geometry itself is untouched and only the
coordinates change - which keeps the blast radius to the thing that is broken.

Whole-buffer rather than per-draw for two reasons. ~450 draws a frame need this and most come out
of a handful of large shared buffers, so per-draw conversion would rewrite the same city block's
coordinates hundreds of times a frame. And whole-buffer conversion makes the stream offset
trivial: entry i is source vertex i, so any draw binds it unchanged at any minIndex. A per-draw
ring would instead have to satisfy `streamOffset + minIndex*stride` with a **UINT** offset, which
forces it to waste `minIndex*8` bytes on every allocation - the compromise the skin ring makes
and can afford only because it resets every frame.

Three things this had to get right, each of them a lesson already paid for here:

| concern | how it is handled | why |
|---|---|---|
| key is a buffer address | the source buffer is referenced | a recycled address would hand a later mesh these coordinates - the defect found in three other caches |
| the game refills a static buffer | the vertex-buffer `Lock` hook drops the conversion on any non-readonly lock | a reference keeps the buffer alive but cannot stop its contents changing; this is the only place that sees it happen |
| bounded memory | 48 MB cap with a **flush**, not a stop | two caches here have had the "stop growing" cliff, where the world progressively loses whatever the cache protects |

Values are stored **raw** - the short widened to float, unscaled - because the uv texture matrix
already carries the 1/1024 and the per-material tiling, and that is the convention the skinned
path uses. One owner for one scale.

The stream index is clamped to `D3DCAPS9::MaxStreams - 1` rather than assumed, and under
instancing the uv stream is given `D3DSTREAMSOURCE_INDEXEDDATA | 1` to match stream 0 - without
it D3D would read one coordinate per instance instead of one per vertex.

**What the next run should show.** `SHORT2 texcoords converted to a float2 stream: N draws/frame`
with N near the converted-draw count, a small number of buffers converted holding a few MB, and
**no further interleaver texcoord warning in remix-dxvk.log**. If a warning does appear, the
number in it names what Remix rejected, which is how both previous rounds were settled.

### Deployed

    sr3-rtx.asi   f93dcaed419e208254ebb5ddd7c17e71
    sr3-rtx.ini   17bea7594769e36065dc8df4300c254d
    rtx.conf      7dd794b69d3795043289d55631043a35   (unchanged)

Build clean, no warnings.

### Run 46: THE WORLD IS TEXTURED. Tiling was being taken from the wrong map

User: *"finally most of the world is textured."* The float2 texcoord conversion is confirmed - 620
draws a frame converted out of 830 converted total, 12,283 buffers built, 9.7 MB held, one arena
flush, and **zero declaration failures**.

Three follow-ups, in descending order of how fixable they are.

**(3) "some surfaces like roads tiled too densely" - fixed, and the cause was a mismatch this
shim created itself.**

The uv scale was taken from whichever tiling pair ranked highest, and `Normal_Map_*` was
deliberately preferred:

    // the disassembly shows the PRIMARY UV output driven by Normal_Map_Tiling wherever it
    // exists, so that pair is preferred and anything else is a fallback.

That observation was correct and the conclusion drawn from it was not. `Normal_Map_Tiling` drives
the vertex shader's primary UV output because that output feeds the **normal** map - and a detail
normal is tiled far more densely than the diffuse it detail-maps. The texture this shim binds as
albedo is the **diffuse** map, which is scaled by its own pair or by nothing at all.

The numbers say how wide the error was: **443 shaders carry a `Normal_Map` tiling pair and only
44 carry a diffuse one.** So for most of the world we were multiplying the diffuse coordinates by
a factor that never applied to them.

Tiling is now resolved **by name against the map the albedo actually came from**. Every pair is
kept with its map (`Normal_Map_TilingU` gives base `Normal_Map`), the winning albedo sampler's
name is recorded during reflection (`Diffuse_MapSampler` gives `Diffuse_Map`), and the two are
matched at draw time - exact first, then prefix, so `Diffuse_TilingU` still matches
`Diffuse_MapSampler`.

**When no pair names the albedo map, the answer is NO TILING**, not the nearest available pair.
Scaling a texture by a factor belonging to a different map is the whole of this bug.

The resolution has to happen per draw rather than in reflection, because the tiling constants are
in the **vertex** shader and the albedo sampler that selects among them is in the **pixel**
shader. Neither `ReflectShader` call can see both.

**Some of the world is still SHORT2.** `remix-dxvk.log` still reports format 80 once, and the
conversion counted ~13 failures a frame against 620 successes. A single counter cannot say which
draws those are, so the failure is now broken out by reason - layout, DYNAMIC source, desc,
create, lock. The DYNAMIC bucket is the one to watch: a dynamic buffer cannot be cached per
buffer because its contents change, and those draws would need a per-draw ring instead.

**(2) Animated billboards and parallax.** Two different things.

The animated billboards select an atlas frame with a uv **offset**, and this shim's texture matrix
only ever sets `_11`/`_22` - scale. There is no translation, so every frame shows the same cell.
The constants exist and are findable the same way tiling was: `UV_anim_tiling` in 52 shaders,
`Decal_Map_OffsetU` in 26, `Render_offset` in 79. Tractable, not yet done.

The parallax materials - store interiors behind windows - are **not tractable in fixed function**.
Parallax is a per-pixel raymarch against a depth map inside the pixel shader, and FFP has no
per-pixel programmability at all. This one needs a Remix replacement material and belongs with the
asset-replacement plan, not with the shim.

**(1) "I only see the color texture."** Correct and expected. A fixed-function draw hands Remix one
texture, and Remix builds its legacy material from it; normal, roughness and specular maps have
nowhere to travel. The game's own normal and specular maps are bound on higher stages and are
deliberately disabled during conversion, because under FFP they would be *blended into the colour*
rather than interpreted as surface detail.

This is the architectural boundary the whole project has been heading toward, and the answer is
already the stated plan: replacement materials authored from the SR3 Remastered assets, which is
what `rtx.enableReplacementMaterials` exists for. Correct albedo and stable texture hashes are the
prerequisite for that, and they are what this run just achieved.

### Deployed

    sr3-rtx.asi   bf7a6efc4a4db008f95adceafb1364b3
    sr3-rtx.ini   17bea7594769e36065dc8df4300c254d   (unchanged)
    rtx.conf      7dd794b69d3795043289d55631043a35   (unchanged)

Build clean, no warnings.

### Run 47: road tiling confirmed fixed; geometry hash churn separated into inherent and fixable

User: *"road tiling is fixed"* - the albedo-matched tiling rule holds. The counters show why it was
so wide-reaching: **tiling now applies to 11.3 draws a frame and is correctly withheld from
163.6**, because those shaders tile a map that is not the one being bound as albedo. Under the old
rule all 175 were scaled by the normal map's factor.

**The remaining unconverted texcoords are all one thing.** The failure breakdown answered it in a
single run:

    conversion failures by reason: 0.0/frame layout, 11.1/frame DYNAMIC source,
                                   0.0/frame desc, 0 create, 0 lock

Every one is a DYNAMIC source buffer, and that is by design: a dynamic buffer's contents are
rewritten constantly, so a conversion cached on the buffer would be stale the moment it is made.
Those ~11 draws a frame keep their SHORT2 coordinates, which is why `remix-dxvk.log` still reports
format 80 once. Fixing them needs a per-draw ring conversion instead of a per-buffer cache - the
same structure the skin ring uses, and affordable at 11 draws a frame.

### Geometry hash churn: two different causes, one of them ours to configure

Reported from the debug view: NPCs, the player character and some building windows change geometry
hash constantly. Three populations, two explanations.

**Characters and NPCs: inherent to CPU skinning, and not a defect.** This shim computes skinned
positions on the CPU and hands Remix the result, so an animating character's vertex positions
genuinely differ every frame. Remix's default hash rules, read out of its own binary:

| option | default |
|---|---|
| `rtx.geometryGenerationHashRuleString` | `positions,indices,texcoords,geometrydescriptor,vertexlayout` |
| `rtx.geometryAssetHashRuleString` | `positions,indices,geometrydescriptor` |

Both include `positions`, so both change every frame for anything animated. A stationary NPC is
stable - the palette is constant and the arithmetic is deterministic - which is consistent with the
report being about characters that are moving.

**The asset hash is the one that matters, and it does not have to include positions.** It is what
replacement matching keys on, so with `positions` in the rule an animated mesh has a different
asset identity every frame and **no replacement can ever attach to it**. That is a direct
obstacle to the stated end goal of replacing assets with the SR3 Remastered ones.

Set: `rtx.geometryAssetHashRuleString = indices,geometrydescriptor` - the default minus
`positions`. Index data and the geometry descriptor are unchanged by animation, so a character
keeps one identity across every pose.

Deliberately minimal: `texcoords` would discriminate further and is also animation-stable, but
~11 draws a frame still have no readable texcoords, so including them would make those hashes
depend on data Remix sometimes does not have.

**Doing this BEFORE authoring replacements is the point.** The rule decides what hashes a capture
produces; changing it afterwards invalidates every hash already authored against the old rule.
Backup `configs/rtx.conf.before-asset-hash-rule.bak`.

One consequence to know about, from Remix's own text: *"The geometry hash being used for sky
detection is based off of the asset hash rule."* Sky here is tagged by TEXTURE
(`rtx.skyBoxTextures`), not by geometry hash, so nothing in the current config depends on the old
rule - but any future geometry-hash-based tagging must be done after this change, not before.

**Building windows: not skinned, and not ours.** These are the dynamic-buffer draws above. The
game regenerates that geometry every frame - which is also why it cannot be texture-converted per
buffer - so its positions, and therefore its hash, change by construction. No shim change makes
geometry the game rebuilds every frame hash stably; the asset hash rule above is what gives them a
stable identity too.

### Deployed

    sr3-rtx.asi   bf7a6efc4a4db008f95adceafb1364b3   (unchanged)
    sr3-rtx.ini   17bea7594769e36065dc8df4300c254d   (unchanged)
    rtx.conf      23157b899318ab3a63e1e019477a7f09   (asset hash rule)

Config-only change, so this run tests exactly one thing.

### The Remix API: available from our process, and recreate-only

The question was whether a Remix API mesh can be updated in place or must be destroyed and
recreated, because that decides whether CPU-skinned characters can be submitted through the API
instead of through fixed-function capture. It is answerable statically, and the answer is
recreate-only.

**The API is reachable from the 32-bit process.** `Saints Row 3/d3d9.dll` - the bridge client our
ASI already sits beside - exports `remixapi_InitializeLibrary` (RVA 0x599F0) and
`remixapi_RegisterCallbacks` (0x59BC0). No fork and no 64-bit work is required to reach it.

**The complete command set, from the bridge's own dispatch strings:**

    RemixApi_CreateMaterial      RemixApi_DestroyMaterial
    RemixApi_CreateMesh          RemixApi_DestroyMesh
    RemixApi_DrawInstance
    RemixApi_CreateLight         RemixApi_DestroyLight
    RemixApi_SetConfigVariable
    RemixApi_CreateD3D9          RemixApi_RegisterDevice

Ten commands, and **every resource is a Create/Destroy pair with no update anywhere**. There is no
`UpdateMesh`, no vertex-buffer write path, nothing that mutates an existing handle.

`remixapi_InitializeLibrary` confirms it from the other direction. It `memset`s the output
interface to zero across `0x58` bytes - 22 function-pointer slots on 32-bit - and then installs
**twelve** of them, leaving the remainder null. A partial interface, and none of the installed
entries is an update.

**Two of the ten commands are stubs**, and they say so themselves:

    [remixapi_dxvk_CreateD3D9] Not yet supported. Device used by Remix API defaults to
    most recently created by client application.
    [remixapi_dxvk_RegisterD3D9Device] Not yet supported. ...

Harmless for us - there is exactly one device - but it is a fair signal of how finished this
surface is.

**Calling convention, read out of the prologue** (useful whenever we do use it):

| condition | result |
|---|---|
| `exposeRemixApi` not set in `bridge.conf` | returns **11**, logs "Remix API is not enabled" |
| `info` null, `info->sType != 1`, or `out` null | returns **3** |
| otherwise | fills the interface, sets an initialised flag |

The feature is gated behind `exposeRemixApi = True` in a `bridge.conf` that does not exist in the
game directory yet.

### What this means for characters

**The API is the wrong tool for them.** Skinned vertices change every frame, so with no update
path the shape of the code would be destroy-and-create per mesh per frame - roughly 90 destroys
and 90 creates a frame at the current skinned draw count, each one an IPC round trip across the
32-to-64-bit bridge. That is the exact cost model `docs/sr2-fork.md` section 6 records as the
mistake to avoid, and this shim has already been bitten by it once, in the wide texture-stage
clear that cost ~10,000 bridge calls a frame.

The hoped-for prize was stable instance identity - our own handles instead of Remix hash-matching.
Recreating the handle every frame gives that up, so the trade is cost for nothing.

**Where the API is genuinely good:** static geometry created once and drawn many times, which is
what `CreateMesh` + `DrawInstance` is shaped for, plus lights, plus `SetConfigVariable` for
driving Remix settings from the shim at runtime. The POM window work fits that shape exactly.

**So characters go back to runtime texture generation over the existing fixed-function path**,
which was the independently-motivated answer anyway: the clothing mask is
`(pattern.r * Diffuse_Color_c + pattern.gba) * Tint_color`, a per-texel recipe that no material
*parameter* can express - with or without the API, and with or without a fork. Generating the
texture also gives each colour combination a stable texture hash, which is what asset replacement
wants.

**And it settles the fork question.** A fork was only ever worth considering for per-texel
material maths inside Remix's shading. Runtime texture generation reaches the same result from
outside, in our own code, with no drift from upstream. Nothing found here argues for forking.

### Run 48: the clothing recipe, read from the shader rather than described

Direction set: runtime texture generation for the character materials, POM for the windows with
modelled interiors deferred.

**The recipe, from `ir_sr3npcclothfull_c.fxo_pc` shader [8].** The worklog has carried a rough
version of this since session 10; the disassembly is more specific and the differences matter.

    texld_pp r3, v0, s0                   ; Pattern_Map
    sum  = p.r + p.g + p.b
    dev  = |p.r-sum/3| + |p.g-sum/3| + |p.b-sum/3|
    test = sum - (dev*165.016495 + 256)/255

    test <  0 -> albedo = p.r^2.2 * Diffuse_Color_a
                        + p.g^2.2 * Diffuse_Color_b
                        + p.b^2.2 * Diffuse_Color_c
    test >= 0 -> albedo = saturate((p - 0.372549) * 1.59375) ^ 2.2

    mul_pp oC0, r1, c37                   ; the whole result * Tint_color

Two things a description of it as "a three-colour mask" gets wrong:

- the channels are **gamma-2.2 weights**, not linear masks - `log/mul 2.2/exp` is applied to each
  channel before it multiplies its colour;
- there is a **selector**. `test` measures how chromatic the texel is; achromatic texels take a
  completely different branch, a desaturated `(p - 0.372549) * 1.59375` raised to 2.2. That is how
  trim, buckles and skin escape being tinted by the three customisation colours.

This also explains precisely why `tintFallbackAlbedo` failed and darkened everything: it modulated
the whole pattern by `Tint_color`, multiplying a weight texture by a colour the shader never
multiplies it by, and ignoring both the gamma and the selector.

**Population:** 34 shader entries across 19 files - the entire `ir_*sr3pccloth*` /
`ir_*sr3npccloth*` family, player and NPC, including the `ir_at_` alpha-test variants.

**Probe first, generator second.** Two facts decide whether the generator can be written at all,
and neither is in the shaders:

1. **Can the Pattern_Map be read?** SR3's textures are `D3DPOOL_DEFAULT` (`pool=0` in the ALBEDO
   BOUND probe) and D3D9 does not promise a lock on those. The bind-pose decode reads a DEFAULT
   *vertex buffer* successfully under DXVK, but a texture is not a vertex buffer. If the lock
   fails the source has to be snooped from `UpdateTexture` at upload time, which is a different
   and larger piece of work.
2. **How many distinct (pattern, a, b, c, tint) combinations occur?** The generator caches one
   texture per combination. A handful per character is affordable; hundreds is not, and the answer
   changes the design rather than merely sizing it.

`ProbeClothMaterial` (`clothProbe=1`) reports the four constants per distinct combination, the
pattern texture's format, dimensions, pool and usage, and whether the read-lock succeeded.
Read-only, capped at 8 reports and 64 tracked combinations, no behaviour change.

Writing a BC1/BC3 decoder before knowing whether the compressed data can be reached would be
building on a guess - the mistake this project has made often enough to name.

### Deployed

    sr3-rtx.asi   e5aa4e8816b5e5f37aa35cbcddbf4b35
    sr3-rtx.ini   51a1c8797ae5247d19876d93356035ca
    rtx.conf      23157b899318ab3a63e1e019477a7f09   (unchanged)

Build clean, no warnings.

### Run 49: the clothing probe answered both questions, and both answers made the work cheap

User confirms the asset hash rule landed: *"npc seem to have a more stable geometry hash. my
character hash has not changed this session. the windows that had unstable hash now are as stable
as npcs."* That is `geometryAssetHashRuleString = indices,geometrydescriptor` doing exactly what it
was set for, on all three populations - including the dynamic-buffer windows, whose geometry the
game rebuilds every frame and which therefore could never have been stabilised any other way.

**Probe result 1: the Pattern_Map read-lock WORKS.** Eight of eight, none failed, on
`D3DPOOL_DEFAULT` textures. D3D9 does not promise this and DXVK allows it, the same latitude the
bind-pose decode already relies on. The `UpdateTexture` snooping fallback is not needed.

**Probe result 2: every pattern is 32x32 DXT1.** 1024 texels. That collapses the performance
design: an outfit generates in microseconds and costs 4 KB, so the one-generation-per-frame
amortisation and the worker thread that were being planned are both unnecessary. The 256-entry
gamma tables are kept anyway - they cost nothing and they are exact, since every input to
`pow(x, 2.2)` is an 8-bit channel.

**A finding that would have been a bug: Tint_color is (5.0, 5.0, 5.0).** Uniform, and far above 1.
It is an exposure multiplier for the inferred-lighting pipeline, applied after lighting - not a
material colour. Baking it into an 8-bit albedo would clip every channel to white. It is excluded.
`tintFallbackAlbedo` made the same class of mistake in a different form and darkened every
garment; this time the measurement came first.

**The two families are different recipes, and only one is implemented.**

The NPC family - 20 shader entries where the pattern IS the albedo - uses the weighted sum with a
chromaticity selector, read from `ir_sr3npcclothfull_c.fxo_pc` shader [8]. That is implemented:
DXT1 decode, recipe per texel, staged through SYSTEMMEM and copied up with `UpdateTexture`, which
is also the upload Remix hashes - so each outfit gets a stable, replaceable hash of its own.

The **player** family - 14 entries, `ir_at_sr3pccloth_*` - is not the same thing at all. From
`ir_at_sr3pccloth_c.fxo_pc` shader [8]:

    layer  = lerp(lerp(lerp(1, Diffuse_Color_c^2.2, p.b), Diffuse_Color_b^2.2, p.g),
                                                          Diffuse_Color_a^2.2, p.r)
    albedo = Diffuse_Map * Diffuse_Color * layer

A layered mask rather than a weighted sum, multiplying a **full-resolution Diffuse_Map**, with the
pattern sampled from a **second texture coordinate set** (`v1`, with `ClampU1`/`ClampV1`). The two
are not in the same UV space, so they cannot be folded into one texture unless TEXCOORD1 is a fixed
transform of TEXCOORD0.

That is a question about the vertex data, not the shaders, so `clothProbe` now reports the texcoord
sets for those draws. Applying the NPC recipe to the player would have been wrong, and the
temptation to treat "clothing" as one problem is exactly what the two disassemblies rule out.

### Deployed

    sr3-rtx.asi   7563ecc0006a792344c2f6067db0bfc4
    sr3-rtx.ini   89d7f131ca745912014623f31a9e44e8
    rtx.conf      23157b899318ab3a63e1e019477a7f09   (unchanged)

All three hash-verified against their masters. Build clean, no warnings.


### Run 50: the wrong heads were ours - baseVertex was never passed to the skinner

Reported: some NPCs now have correct clothing but not all; every character has the wrong head,
recognisably the old "random head texture"; and the head z-fights because **it is a separate mesh
drawn twice, once with the wrong texture**.

That last observation is the one that resolved it.

**The skin shaders were not the culprit.** `ir_sr3npcskinfull_*`, `ir_sr3pcskinfull_*` and
`ir_sr3pchair_*` all carry `Diffuse_MapSampler` and NO `Pattern_Map`, so the new clothing generator
never touches a head. It also cannot be the mesh albedo cache, which has been off since run 43.

**`SkinAndBind` was never given `baseVertex`.**

    skinned = SkinAndBind(dev, minIndex, numVertices);      // baseVertex not passed
    GetBaseMesh: Lock(g_stream0Offset + minIndex * stride, ...)
    SkinMeshKey(vb, offset, stride, minIndex, count)
    const UINT base = minIndex * stride;

D3D9 fetches the vertex for index i from `streamOffset + (BaseVertexIndex + i) * stride`, so the
vertices a draw uses begin at `baseVertex + MinVertexIndex`. All four places above used `minIndex`
alone - the decode, the cache key, the stream offset and the ring allocation.

**Consistently wrong is not harmless.** The decode reads a different window of the shared character
buffer, the bind places it so the indices line up against that window, and the result is one
character's mesh rendered from another character's vertices - with another character's UVs. That is
"each character had the wrong head" exactly, and the key omitting `baseVertex` is the collision
that made it look random: two heads at the same `minIndex` and vertex count but different
`baseVertex` hash identically.

It survived turning the mesh albedo cache off because it was never a texture problem. The texture
bound was right all along; the geometry it was painted onto was somebody else's.

**Heads are also the refusal population.** A head is a rigid single-bone attachment - one bone, the
neck - which is precisely the BLENDINDICES-with-no-BLENDWEIGHT layout this shim refuses, and a
refused draw passes through for Remix to reconstruct beside our copy. That is the z-fighting and
the second, wrongly-textured copy, and it is the same 22-39 draws a frame that have been counted as
refusals since the CPU skinning port.

It also supplies the missing explanation for `skinRigidSingleBone` making clothing vanish. A bind
pose decoded from the wrong window is arbitrary geometry; posing arbitrary geometry by a real bone
matrix puts it somewhere the camera never looks. The setting stays off for this run so the
baseVertex fix is measured on its own, and `skinned draws with a non-zero baseVertex` is now
reported - if that reads zero, this fix is inert and the wrong heads have another cause.

**The clothing decoder was fitted to eight samples.** 234 outfits of 287 could not be generated
against 53 that could, because the first version accepted DXT1 only - on the strength of a probe
that saw eight patterns, all 32x32 DXT1. The generated outfits then measured 256x256, so the sample
described what the probe caught first rather than the population. DXT3/DXT5 and uncompressed
sources are now decoded, the size cap is 1024, and any remaining format is **named** in the log
rather than counted, because a format number can be looked up and a failure count cannot.

Caught while writing it: the widened guard read `d.Format` to classify the format *before*
`GetLevelDesc` filled the descriptor, which would have failed every texture. Fixed before building.

### Deployed

    sr3-rtx.asi   650c01956a0580ca7b46d81e073d703a
    sr3-rtx.ini   89d7f131ca745912014623f31a9e44e8   (unchanged)
    rtx.conf      23157b899318ab3a63e1e019477a7f09   (unchanged)

Build clean, no warnings.

### Run 51: heads fixed; the darkness is a colour-space round trip, and hair/skin are a third family

User: *"npcs look like they have the correct head now"* - the `baseVertex` fix landed. Remaining:
wrong tint on heads, hair white, clothes textured but wrong colour and **really dark**.

**"Really dark" is a round-trip error, not a mistake in the recipe.**

The recipe's `pow(x, 2.2)` is an sRGB-to-linear conversion, so everything it produces is linear
light. The generator wrote those linear values straight into an 8-bit texture - and Remix reads an
8-bit albedo as **sRGB** and linearises it again. The value reaching the path tracer was therefore
`albedo^2.2`: 0.5 became 0.22, 0.8 became 0.61. Uniformly, visibly dark, with the texture detail
entirely intact, which is exactly what was reported.

Fixed by re-encoding to sRGB on the way out, so Remix's own decode returns the linear albedo the
shader actually computes. Worth stating plainly because it will recur: **anything this shim
generates for Remix has to be stored in the space Remix expects to read it in, not the space it was
computed in.**

**Hair and skin are a third family, and the shader constants name the problem.**

| shader | colour constants | ends |
|---|---|---|
| `ir_sr3pchair_c` | `Tint_color` and nothing else that could colour it | `mul_pp oC0, r6, c37` |
| `ir_sr3npcskinfull_c` | `Diffuse_Color` (c0), plus `Tint_color` | - |

Both multiply their diffuse map by a constant. This shim binds the raw map and discards the
constant, so a greyscale hair mask stays **greyscale** - the reported white hair - and skin keeps
whatever tone the map happens to hold, which is the wrong tint on every head.

Fixed function can express that: MODULATE the texture against TFACTOR. What it **cannot** express
is a multiplier above one, and `Tint_color` measured **(5.0, 5.0, 5.0)** on the clothing draws - an
exposure factor for the inferred-lighting pipeline, not a colour. Whether hair and skin see the
same 5.0 or a genuine colour decides whether MODULATE is the fix or a trap, and it is a runtime
value that reading shaders cannot answer.

`tintFallbackAlbedo` darkened every garment in this project by guessing at precisely this. So
`ProbeCharacterConstants` reports, once per distinct skinned material, which constant won the
colour ranking, its register and its actual value alongside `Tint_color`. One run decides it.

### Deployed

    sr3-rtx.asi   7ed2fc641c33e10674826bd01cc548dd
    sr3-rtx.ini   89d7f131ca745912014623f31a9e44e8   (unchanged)
    rtx.conf      23157b899318ab3a63e1e019477a7f09   (unchanged)

Build clean, no warnings.

### Run 52: the clothing generator is byte-exact, verified against its own output

*"some of them are the correct color. some parts seem to have the wrong color."*

Rather than reason further about which half was wrong, the shim dumped the first three outfits and
their decoded source patterns as raw RGB (`clothDump`), and the recipe was then checked
**numerically** against those pixels: for each distinct texel class in a 512x512 pattern, compute
what the shader would produce and compare it to what the generator actually wrote.

    pattern   branch      expected        actual   match
    (0,218,0)  colour  (24, 24, 24)  (24, 24, 24)   ok
    (0,0,222)  colour (146, 0, 110) (146, 0, 110)   ok
    (194,0,0)  colour  (33, 33, 33)  (32, 32, 32)   ok
    (131,97,98) desat    (57, 3, 5)    (57, 3, 5)   ok
    ...
    0 mismatches out of 21 sampled texel classes

**The DXT decode, the channel-to-colour mapping and the sRGB round trip are all correct.** The
dumped pattern reads as a clean garment sheet - green body, blue trim, red logo, grey unused space -
so neither of the two candidates that argument had narrowed it to was the fault.

Two things visible in that table explain the appearance without any bug being involved:

- `(0,218,0)` pure green becomes `(24,24,24)`, a near-black charcoal, because this outfit's
  `Diffuse_Color_b` is **0.008**. The colours themselves are dark.
- `(131,97,98)`, a near-neutral grey, becomes `(57,3,5)` - strongly saturated - because the
  desaturated branch remaps `[95,255]` to `[0,1]` (`(p*255-95)/160`) and then applies gamma 2.2, so
  a 34-count channel difference becomes an 18:1 ratio. That steep toe is the shader's own.

**What remains is the term deliberately left out.** The game computes
`final = albedo * lighting * Tint_color` with `Tint_color` at (5,5,5) on every clothing draw. It
cannot go into an 8-bit albedo without clipping, and leaving it out is defensible as physics - but
it makes these materials about five times darker in linear terms than the game shows, which shifts
apparent colour and not only brightness.

Whether Remix's lighting wants physical reflectance or something nearer the game's scaled value is
a question about reconciling two lighting models, not one more thing to derive. `clothAlbedoPercent`
exposes it: 100 is unchanged, 500 applies the game's factor in full, values above 100 clip the
bright end. A knob to test with, deliberately not a default.

`clothDump` is back off, and the raw dumps are converted and kept under
`docs/evidence/cloth/` - the pattern and result images are worth having beside this entry.

### Deployed

    sr3-rtx.asi   536ac1423230d0dd9be99d7b98ffbdc4
    sr3-rtx.ini   clothDump=0, clothAlbedoPercent=100
    rtx.conf      unchanged

### Run 53: the character colour constants ruled themselves out, and one of them was painting 72 draws white

`ProbeCharacterConstants` across four distinct skinned materials:

    CHAR CONST #1: first='Diffuse_MapSampler'         rank=100 | (c14) = (1.000 1.000 1.000 1.000) | Tint_color(c37) = (5.000 5.000 5.000 1.000)
    CHAR CONST #2: first='IR_GBuffer_DSF_DataSampler' rank=80  | (c37) = (5.000 5.000 5.000 1.000) | Tint_color(c37) = (5.000 5.000 5.000 1.000)
    CHAR CONST #3: first='Blend_MapSampler'           rank=100 | (c0)  = (1.000 1.000 1.000 1.000) | Tint_color(c37) = (5.000 5.000 5.000 1.000)
    CHAR CONST #4: first='Decal_MapSampler'           rank=100 | (c37) = (5.000 5.000 5.000 1.000) | Tint_color(c37) = (5.000 5.000 5.000 1.000)

**Every character colour constant is (1,1,1) - identity - and Tint_color is (5,5,5) everywhere.**

That kills the hypothesis from run 51 outright. Hair is not white because we drop a colour
constant: there is no colour constant to drop. If `Tint_color` were the hair colour it would vary
between hairstyles and it does not; it is a uniform exposure factor for the inferred-lighting
pipeline, on every character material in the game.

Worth stating because it was a good hypothesis with the shader constants apparently backing it -
`ir_sr3pchair_c` really does carry `Tint_color` and nothing else that could colour it, and really
does end `mul_pp oC0, r6, c37`. The constant list said "the colour is here". The runtime value said
otherwise, and only the runtime value is evidence.

**A real bug fell out of the same measurement.** `ConstantAlbedo` clamps each channel with

    if (v >= 1.0f) return 255;

so a material whose best-ranked colour constant is `Tint_color` at 5.0 renders **pure white** - and
is counted as a rescued "constant-colour material" while doing it. That is 72 draws a frame
reported as fixed and rendering white. Such a constant is now rejected rather than clamped, so
these fall to the honest blank count; world materials whose `Tint_color` is a genuine colour are
untouched, since they only reach that test with a channel below 1.

Also fixed: `colourConstName` was written on any ranked constant while `colourConstReg` only
updated on a higher rank, so the two could describe different constants - which is why the first
report printed the impossible `Tint_color(c14)`. The name now tracks the register.

**So the colour is in a texture, and the question is which one.** `ir_sr3pchair_c` samples Dob_Map
(s0) and Diffuse_Map (s1); `ir_sr3npcskinfull_c` samples Blend_Map, Diffuse_Map, Normal_Map and two
Sphere_Maps. This shim picks Diffuse_Map by rank and gets something greyscale. `ProbeCharacterTextures`
now reports every stage of a skinned draw with its CTAB sampler name, dimensions, format and mip
count, and marks which one was bound as albedo - which is a runtime fact about what the game binds
and not something the sampler names settle.

`clothAlbedoPercent` set to 200 by the user in the game copy; the master is synced to match.

### Deployed

    sr3-rtx.asi   163aa90869a0dc8097700b115bb7034e
    sr3-rtx.ini   230b2121443981521f1e63dec86faeb3   (clothAlbedoPercent=200)
    rtx.conf      23157b899318ab3a63e1e019477a7f09   (unchanged)

Build clean, no warnings.

### Run 54: what the character textures actually are, and whether the player's two UV sets can be reconciled

`ProbeCharacterTextures` across eight distinct skinned materials. Two results matter.

**Skin is already choosing the right texture.**

    CHAR TEX #8: first='Blend_MapSampler' albedoStage=0 rank=100
        s0 Diffuse_MapSampler   2048x1024 fmt=22 levels=9   <- bound as albedo
        s1 Normal_MapSampler    1024x512  fmt=21 levels=8
        s2 Sphere_Map_2Sampler   128x128  DXT1 levels=1
        s3 Sphere_Map_1Sampler   128x128  DXT1 levels=1     (same pointer as s2)
        s4 Blend_MapSampler       32x32   DXT1 levels=4

A 2048x1024 **uncompressed** sheet is the composited character body and face that the customisation
system builds at runtime, and binding it as albedo is correct. So the wrong skin tint is not a
texture this shim is failing to choose. The two Sphere_Maps sharing one pointer, blended per texel
by a 32x32 Blend_Map, is the likelier shape - and that is a shader operation, not a binding choice.

**Hair is genuinely ambiguous and the names cannot settle it.**

    CHAR TEX #6: first='Diffuse_MapSampler' albedoStage=1 rank=100
        s0 Dob_MapSampler       512x512 DXT1 levels=8
        s1 Diffuse_MapSampler   256x256 DXT1 levels=7   <- bound as albedo

We take the 256x256 Diffuse by rank while a **larger** 512x512 Dob_Map sits beside it. Either is
plausible from its name, and the constants have already been shown to carry no colour at all. So
`charTexDump` writes every named stage out as raw RGB and the question gets answered by looking -
which settled the clothing question in one run after argument had failed twice.

The DXT decoder is now factored out as `DecodeTextureRGB` and shared with the clothing generator,
so the dumper and the generator cannot drift apart on format handling.

**The player's clothing: can the two coordinate sets be reconciled?**

    PLAYER cloth texcoord set 0: stream=0 offset=28 type=short2
    PLAYER cloth texcoord set 1: stream=0 offset=32 type=short2

The player recipe is `albedo = Diffuse_Map * Diffuse_Color * layer(pattern)`, with the diffuse on
TEXCOORD0 and the pattern on TEXCOORD1. Folding them into one generated texture needs the pattern's
coordinates expressed in the diffuse's space. If UV1 is a fixed affine transform of UV0 - the same
unwrap at another scale and offset - that is a per-mesh constant and the fold works. If they are
independent unwraps, it does not, and the player's colours have to reach Remix another way.

That is measurable. The probe now fits a scale and offset from the first and last sampled vertex
and **checks it against the six between**, so a coincidence in two points cannot pass for a
relationship, and prints four sample pairs either way.

This also needed the current draw's vertex window inside `SetupTextureStages`, where no probe
previously had it; sampling the whole shared buffer instead would have mixed several meshes
together and reported "independent" for that reason alone.

`clothAlbedoPercent` stays at 200 - reported as looking about right, with the user tuning it
against more outfit colours before it is fixed.

### Deployed

    sr3-rtx.asi   040854fbdad00f60f276bc7554932350
    sr3-rtx.ini   charTexDump=1, clothAlbedoPercent=200
    rtx.conf      unchanged

Build clean, no warnings.

### Run 55: the character textures, looked at - and the rigid-skin setting goes back on

`charTexDump` wrote every named stage of the probed skinned materials. Two of the three open
character problems answered themselves.

**Skin is already correct.** The 2048x1024 uncompressed sheet bound as albedo is the fully
composited character texture - face with makeup, eye, tattoos, arms, legs, all in proper skin tone.
The customisation system builds it at runtime and this shim binds it. So the reported wrong tint is
NOT a texture this shim failed to choose, and the Sphere_Maps are not carrying tone either: both
128x128 maps are a neutral warm-grey lit sphere, which is an environment term. That report predates
the baseVertex fix and may already be gone.

**Hair: neither of its textures holds the colour.**

    s0 Dob_Map      512x512  - the actual hair: visible strands, WHITE, on a green field
    s1 Diffuse_Map  256x256  - smooth magenta/green directional data, no strands at all

So `Diffuse_MapSampler` on this family does not name a diffuse texture, and ranking it 100 binds
the wrong one - but swapping to Dob_Map would only give white hair from white strands. The colour
is in neither, which leaves the constants, and `ir_sr3pchair_c` multiplies its result by
`Hair_Spec_Color2` at `mul_pp r0.xyz, r0, c4`. Despite the name, those are the likeliest carriers
of the chosen hair colour, so both are now reported by `ProbeCharacterConstants`.

**`skinRigidSingleBone` back ON, because the reason it failed has been found and fixed.**

It was disabled after clothing vanished. That was never this setting's fault: `SkinAndBind` was
never passed `baseVertex`, so the bind pose came from the wrong window of the shared character
buffer, and posing arbitrary geometry by a real bone matrix puts it where the camera never looks.
Run 50 fixed that, and heads became correct in the same run - which is the same fix confirming
itself on a different symptom.

Two further measurements say the layout is what it appears to be: all four BLENDINDICES bytes are
identical on these draws, so component 0 was never a guess, and the palette is written 3-6 draws
earlier under the same objTM, so it belongs to the draw.

This is also the most likely explanation for the hair. A head is a rigid single-bone attachment and
so, almost certainly, is hair - which means both are refused, pass through, and get reconstructed by
Remix beside our copy. That is the head z-fighting, its second wrongly-textured copy, and plausibly
white hair as well, none of which are texture problems at all.

If clothing vanishes again, the setting is wrong for a reason not yet found and goes back to 0.

`charTexDump` off again; the dumps are converted and kept under `docs/evidence/cloth/`.

### Deployed

    sr3-rtx.asi   4b30178e57f212c958e05a76181e203f
    sr3-rtx.ini   b86a951cecb642a32ef9a4b1f6db70d2   (skinRigidSingleBone=1, charTexDump=0)
    rtx.conf      23157b899318ab3a63e1e019477a7f09   (unchanged)

Build clean, no warnings.

### Audit before run 56: two real defects found

Requested code check ahead of running with `skinRigidSingleBone` back on. Everything added since
run 45 was reviewed - the uv stream conversion, the clothing generator, the DXT decode, the
baseVertex threading, and five new probes.

**1. Unchecked vertex range, newly dangerous.** `GetBaseMesh` called `GetDesc` and then locked

    g_stream0Offset + firstVertex * g_stream0Stride

without ever comparing the range to `vbd.Size`, in UINT arithmetic. That was survivable while the
offset was `minIndex` alone. Since `baseVertex` joined it in run 50 the product is far larger, and
an **overflow would wrap to a small offset that D3D accepts** - reading entirely the wrong vertices,
silently, and looking exactly like the wrong-window bug that omitting baseVertex caused in the first
place. Relying on `Lock` to reject it is not enough: the caller's arithmetic is what is unsound, and
by the time Lock sees the number it is already wrong.

Added `VertexRangeFits`, in 64-bit, and applied it at all three read-lock sites: `GetBaseMesh`,
`ProbeRigidSkinned` and the player UV probe.

**2. `g_internal` clobbered by a nested helper.** `ClothAlbedo` creates textures, so it sets
`g_internal` and cleared it unconditionally on the way out - but it is called from `BeginFFP` at a
point where that flag is ALREADY set and still owned by the caller. It now saves and restores.
A scan of every other function that clears the flag found no second instance: the rest are all
top-level, entered from a draw hook with the flag already clear.

**3. A guessed vertex span, removed.** `Hook_DrawPrimitive` set `g_curDrawVertexCount = count * 3`,
which is only right for a triangle LIST and over-estimates a strip or fan by about three times -
enough to send a probe reading past the end of a mesh. Set to zero instead; the probes using that
window all require a minimum size, so they decline rather than read something arbitrary.

Also checked and found sound: lock/unlock balance in all fifteen locking functions (the two that
looked unbalanced are a ternary pair and an error path); no early exit between any lock and its
unlock except the `if (FAILED(Lock))` branches, where nothing is held; AddRef/Release pairing in the
uv, cloth and base-mesh caches; division guards on the probe sampling step; and the decode size caps.
A duplicated comment left behind by the baseVertex edit was removed. Build is warning-clean at /W3.

### Deployed

    sr3-rtx.asi   19a9468a37fc2f9c07c32d028a4ba2b8
    sr3-rtx.ini   b86a951cecb642a32ef9a4b1f6db70d2   (skinRigidSingleBone=1)
    rtx.conf      23157b899318ab3a63e1e019477a7f09

### Run 56: the baseVertex fix was INERT, the refusals were not the double-draw, and cars are black for the same reason clothes were dark

Three results, and the first two say earlier conclusions were wrong.

**1. `skinned draws with a non-zero baseVertex: 0`.**

The counter added in run 50 specifically to falsify that fix did falsify it. baseVertex is always
zero on these draws, so threading it through changed nothing, and **the wrong heads becoming
correct in run 50 had another cause** - most likely the clothing decoder widening in the same
build, which changed which garments got generated textures. The explanation written into the
worklog and the ini for that run is not supported and is corrected here.

The bounds check the audit added on top of it stands on its own merits; the arithmetic was unsound
regardless of the values it happens to see.

**2. Refusals reached ZERO and the heads still z-fight.**

    SKINNING: 107 skinned/frame, 0 refused/frame

`skinRigidSingleBone` engaged cleanly - every skinned draw now converts, and **nothing vanished**,
which does confirm the run-50 work removed whatever made it fail before. But the head double-draw
survived a refusal count going from 22-39 a frame to 0, so **the refusal population was never the
double-draw.** That hypothesis is dead, and it was the leading one for several runs.

The frame dump shows what the double-draw actually is:

    3190 CONVERT v=1080 p=499 ... ps='Diffuse_MapSampler' tex0=16365DF8
    3191 CONVERT v=1080 p=500 ... ps='Diffuse_MapSampler' tex0=16366178
    3192 CONVERT v=1080 p=501 ... ps='Diffuse_MapSampler' tex0=16365298
    3193 CONVERT v=1080 p=499 ... ps='Diffuse_MapSampler' tex0=16365298
    3194 CONVERT v=1080 p=499 ... ps='Diffuse_MapSampler' tex0=16366178
    3195 CONVERT v=1080 p=499 ... ps='Diffuse_MapSampler' tex0=16365DF8

Six draws of one 1,080-vertex mesh, three distinct textures, **each appearing exactly twice**, all
converted, all alpha-blended. Either three heads are each drawn twice, or six heads share three
textures - and nothing in the dump line separates those two readings. So the dump now carries the
objTM translation: two draws of one mesh at ONE position are a duplicate; at two positions they are
two objects sharing a texture. That single number decides the next move, and guessing between them
is what four dedup attempts already cost.

**3. Cars are black for the same reason clothes were dark.** `ConstantAlbedo` writes its constant
into `D3DRS_TEXTUREFACTOR`, an 8-bit colour Remix reads as sRGB - and the constants are linear:

    Base_Paint_Color = (0.041, 0.008, 0.006)   raw -> byte (10, 2, 2)   read as sRGB -> ~0.0012 linear

About thirty times too dark, which is black. Encoded properly the same constant becomes (58, 29, 26),
a dark red car. This is the identical colour-space round trip found in the clothing generator in
run 51, in the one other place this shim hands Remix a colour it computed itself - and the note
written then said it would recur.

**A correction to run 53 while here.** "Tint_color is (5,5,5) on every character material" was true
of the four character materials sampled and **false in general**: vehicle materials measure
Tint_color at 0.009 to 0.022. The rejection rule added in run 53 keys on "at or above 1.0 in every
channel", which is still right for both populations - but the reasoning behind it was stated more
broadly than the evidence supported.

### Deployed

    sr3-rtx.asi   0054c4ff61219e9854a14aeeaf41c8eb
    sr3-rtx.ini   b86a951cecb642a32ef9a4b1f6db70d2   (unchanged)
    rtx.conf      23157b899318ab3a63e1e019477a7f09   (unchanged)

Build clean, no warnings.

### Run 57: the double-draw measured, and the dedup key was missing a FOURTH thing

The frame dump now carries each draw's object position, which turns the head question from an
argument into arithmetic. Of 75 converted skinned draws in one frame:

    distinct (mesh, prims, texture, POSITION) combinations   70
      ...appearing more than once                             5
    distinct object positions                                19
      56 draws at (96.9 145.7 29.7)   <- one character, many parts
      2 draws at (125.7 14.7 -380.7)
      1 draw at each of 17 others     <- crowd NPCs, one draw apiece

**There is no systematic double-draw.** Only 5 of 75 draws are true duplicates, and all but one sit
at a single position - which matches the ORIGINAL report from many runs ago, "two renders of my
character", rather than the later reading that every NPC head was doubled. The 56 draws sharing one
objTM are the parts of one character, which is correct: skinned geometry is posed in object space
and objTM places the whole character.

**The dedup key was still too small, and measuring it first is what caught that.**

The existing key covers buffer, offset, stride, minIndex, vertex count, startIndex, primitive
count, eight bones of pose and the full objTM - everything four previous attempts had learned to
add. Before enabling it, the same frame was analysed against what that key would actually merge:

    groups the key would merge                       68
      ...sharing one texture (safe)                  67
      ...carrying DIFFERENT textures (WRONG)          1

        v=1080 p=499 at(96.9 145.7 29.7) -> 16484C98, 16484E58, 16484F38

Three draws of one mesh, one pose and one position carrying **three different textures** - separate
material layers on the same head, two of which would have been dropped. That is exactly how the
previous four attempts failed, and no amount of reading the key would have revealed it; only
running it against a real frame did.

So the albedo joins the key - the fourth thing it has been missing, after the index range, the pose
and the position - and it is keyed on what was actually BOUND rather than on stage 0, because that
is what Remix receives. With it, exactly 5 draws a frame collapse.

`dedupSkinned=1` for the first time in the project, on evidence rather than hope.

**What this is not.** Refusals reached zero in run 56 and the head z-fighting survived, so the
pass-through population was never the cause. This is a genuine double submission by the game.

**Hair remains unresolved.** A `Dob_Map` binding was seen but no hair material reached
`ProbeCharacterConstants` in either run - its twelve slots fill with other materials first - so
`Hair_Spec_Color1/2` still have no measured values.

### Deployed

    sr3-rtx.asi   8ae6f116482ffd3b0dd296c83ab0bec3
    sr3-rtx.ini   6c94c90114d82ca4e7da8a410bb2fcaf   (dedupSkinned=1)
    rtx.conf      23157b899318ab3a63e1e019477a7f09   (unchanged)

Build clean, no warnings.

### Run 58: the magenta character IS the head z-fighting, and the code predicted this failure

User, after taking a Remix capture: *"my character was magenta. just like how i was describing my
character seaming to have two textures. i think my character having megenta textures along with the
actual model is linked to the zfighting of the heads."*

**Magenta is the marker texture** - the 4x4 A8R8G8B8 magenta this shim binds to prepass draws so
Remix drops them via `rtx.ignoreTextures`. A character rendered in it means those marked draws are
reaching Remix rather than being dropped, and the frame dump shows they are the same meshes:

    531 MARK    v=1080 p=499 ps='Normal_MapSampler'      <- the character's stipple prepass
    3190 CONVERT v=1080 p=499 ps='Diffuse_MapSampler'    <- the same mesh, converted

Every character part is submitted twice by the engine - once into the G-buffer prepass, once into
the material pass - and the whole marker mechanism exists so Remix sees only the second. **If the
marker fails, every character part is drawn twice.** That is the head z-fighting, and the user
connected the two symptoms before the log did.

**The marker is configured correctly**, which is what makes the cause specific rather than vague:
`0x978271113F293CE4` is in `rtx.ignoreTextures` and `remix-dxvk.log` shows Remix loading it. Stage
0 is being ignored. What is not ignored is the DRAW - because Remix categorises a draw from **any**
stage it finds a texture on, and a prepass arrives carrying whatever the previous material draw
left on stages 1-7.

`BeginMark` had already written down how this would present:

    // If Remix did need the other stages cleared, the failure announces itself: the magenta
    // comes back.

It came back. The wide clear was removed on 2026-08-18 for cost - ~1,700 marked draws a frame at
two bridge calls per stage - and restored in run 42 for the ~23 composite quads that genuinely
needed it. The skinned prepass is the third population that needs it, and at around a hundred draws
a frame it is affordable, so the clear is now applied to marked prepass draws **when the layout is
skinned**. The cost objection stands for the other 1,900.

**Corrections to the previous entry.** `dedupSkinned` was reverted on a misreading - the reported
lost parts were pre-existing, not caused by it - and is back ON, where the frame analysis says it
belongs. It removes the 5 genuinely duplicated draws at the player's position; it was never going
to fix the NPC head z-fighting, which this entry explains instead.

**Capture on demand.** Both dumps fired once, automatically, on the first frame busy enough to
count as gameplay - wherever the camera happened to point. Every character diagnosis in this
project has come from whatever that arbitrary frame caught, and it has now twice missed the thing
being looked at. `captureKey` (F9 by default) re-arms both from Present, between frames, and each
press writes its own numbered pair so several can be compared.

### Deployed

    sr3-rtx.asi   df080cf2d9901637f8e9101f66a3f451
    sr3-rtx.ini   2dbf21cbfb85f7905dbbead1d57a38f6   (dedupSkinned=1, captureKey=0x78)
    rtx.conf      23157b899318ab3a63e1e019477a7f09   (unchanged)

Build clean, no warnings.

### Run 59: the wide clear was on the wrong rule, and the frame dump said so immediately

Magenta persisted after run 58, and the user separated the two symptoms usefully: *"my character is
not zfighting but i can see two textures. one is the megenta, same color as the prepass, and one as
the semi correct colors."* Two surfaces occupying the same space, one of them the marker - which is
the prepass reaching Remix, not a geometry duplicate.

**The patch had missed almost everything it was aimed at.** Grouping the frame dump's skinned MARK
draws by disposition reason:

    85 draws | prepass: shader samples nothing
     1 draw  | prepass: only normal/stipple/depth samplers    <- the rule run 58 patched

There are two prepass rules, and run 58 put the wide stage clear on the stipple one. The character
prepass takes the other: its pixel shader declares **no sampler at all**. So
`g_markClearAllStages` was never set for 85 of the 86 draws the change existed for, and the run
tested nothing.

Cheap to find and worth naming: the counters said the clear was running - marked draws 2,330 a
frame, SetTexture calls up from ~5,800 to 8,552 - and all of that extra work was happening on the
composite quads and one stipple draw. **A counter going up is not the same as the intended
population being covered**, which is the third time that lesson has appeared in this project.

Also checked while there, because it was the other candidate: **no converted draw carries the
marker as its texture.** Zero draws in the entire dump have the marker pointer at stage 0, so this
shim is not painting characters magenta itself - the marked prepass is genuinely being rendered by
Remix.

The clear now sits on the sampler-less rule as well, still restricted to skinned layouts. That rule
fires on ~1,400 draws a frame across the whole world, and the wide clear on all of them is the cost
that had it removed on 2026-08-18; the skinned share is ~85.

**Dedup is working.** The dump shows `skinned duplicate: identical triangles already converted this
frame` firing, so the 5 duplicates a frame are being caught as intended.

### Deployed

    sr3-rtx.asi   91612d4c693ad0c22d988786f112367d
    sr3-rtx.ini   2dbf21cbfb85f7905dbbead1d57a38f6   (unchanged)
    rtx.conf      23157b899318ab3a63e1e019477a7f09   (unchanged)

Build clean, no warnings.

### Run 60: `rtx.ignoreTextures` never hid anything - it VISUALISES

The magenta survived the wide clear, so the mechanism was wrong rather than the placement. Reading
Remix's own strings instead of guessing a fourth time:

    rtx.ignoreTextures
      "These textures will be ignored when attempting to determine the desired textures from a
       draw to use for ray tracing."

    (from the material description)
      "Runtime will not render any objects using an ignored material.
       RTX Remix will render with a PINK AND BLACK CHECKERBOARD."

    rtx.hideInstanceTextures
      "Textures on draw calls that should be hidden from rendering, but not totally ignored.
       This is similar to rtx.ignoreTextures but instead of completely ignoring such draw calls
       they are only hidden from rendering, ALLOWING FOR THE HIDDEN OBJECTS TO STILL APPEAR IN
       CAPTURES."

**The ignore was working the whole time.** It is not a hide - Remix draws an ignored material as a
pink and black checkerboard, which at any distance reads as magenta. Every marked prepass draw in
this game has been rendering as that checkerboard, and the entire `hiddenPassMode=3` design has
rested on a misreading of what the option does since it was introduced.

The user's answers separated the two symptoms and both are now explained:

| symptom | where | explanation |
|---|---|---|
| magenta on the character | live AND in captures | ignored materials are drawn as a checkerboard, in both paths |
| NPC head z-fighting | live only | the second copy IS that checkerboard surface, coincident with the material pass |

So they are one bug, not two, and the z-fighting is the prepass copy Remix was never hiding.

**`rtx.hideInstanceTextures` is the option that hides.** Its description says so in as many words,
and it also predicts the capture behaviour: hidden objects still appear in captures, which is why
the magenta shows there and why that part is expected rather than broken.

The marker hash `0x978271113F293CE4` is now listed in `rtx.hideInstanceTextures`. It is left in
`rtx.ignoreTextures` as well for this run - removing it at the same time would confound the test,
and if hiding is genuinely stronger the magenta goes regardless. Backup
`configs/rtx.conf.before-marker-hideinstance.bak`.

Note the earlier casualty: `rtx.hideInstanceTextures` was one of the eighteen single-entry
categories deleted in run 44 as part of the accidental multi-tag cleanup. That cleanup was correct -
the hash in it then was a smeared game texture, not the marker - but the key has now come back for
a real reason.

**A correction to run 59.** The wide stage clear was moved onto the sampler-less rule on the theory
that leftover textures on stages 1-7 were letting Remix categorise the draw. That theory is not
needed: the draw was never being hidden at all. The clear is harmless and stays, but it was not the
fix and should not be described as one.

### Deployed

    sr3-rtx.asi   91612d4c693ad0c22d988786f112367d   (unchanged)
    sr3-rtx.ini   2dbf21cbfb85f7905dbbead1d57a38f6   (unchanged)
    rtx.conf      978b0ca79e42dcc6379db4f03ff6d1f0   (marker in hideInstanceTextures)

Config-only change, so this run tests exactly one thing.

### Run 61: vertex capture is a leftover from before the shim existed, and it is what resurrects every hidden draw

User's suggestion, and it was the right one: *"could it be one of our previous toggles that we might
not need?"*

`rtx.useVertexCapture` has been True since session 3. Remix describes it as:

    "When enabled, injects code into the original vertex shader to capture final shaded vertex
     positions. Is useful for games using simple vertex shaders, that still also set the fixed
     function transform matrices."

SR3 does **not** set fixed-function transforms - that is the entire reason this shim exists. Vertex
capture was the ONLY mechanism available in sessions 2-3, before any FFP conversion had been
written, and it has been carried forward ever since without being re-examined.

What settles it is the message on the other side of the switch:

    [RTX-Compatibility-Info] Skipping draw call with shader usage as vertex capture is not enabled.

**With it off, Remix skips every draw that still uses shaders.** That is precisely the population
this shim spends its effort trying to suppress: the marked prepass, the composite chain, the
auxiliary-camera passes. No marker, no checkerboard, no second copy - they never enter the scene.

Which reframes the last several runs. `hiddenPassMode=3`, the marker texture, the wide stage clear,
`ignoreTextures`, `hideInstanceTextures` - all of it is machinery for suppressing draws that a
single setting was resurrecting. The marker was never suppressing anything; it was choosing which
colour Remix drew the resurrected copy in.

**The known cost: the sky.** The shim deliberately passes the skybox through with the reason "left
for Remix to capture and tag" - 39 draws a frame in the latest log - and `rtx.skyBoxTextures` holds
its hashes. With vertex capture off those draws are skipped, so the black sky, fixed once already
in session 14, is expected back. That is a bounded follow-up: convert the sky to fixed function
like the rest of the world instead of relying on capture.

Also affected, and wanted: the auxiliary-camera passes (138 a frame) and the screen-space chain
(31 a frame) are skipped rather than suppressed by hand.

`rtx.useVertexCapture = False`. Backup `configs/rtx.conf.before-vertexcapture-off.bak`.

### Deployed

    sr3-rtx.asi   91612d4c693ad0c22d988786f112367d   (unchanged)
    sr3-rtx.ini   2dbf21cbfb85f7905dbbead1d57a38f6   (unchanged)
    rtx.conf      38dee951e7ac20406cb10e85f473cdeb   (useVertexCapture = False)

Config-only, one variable.

### Run 62: turning off vertex capture made the WORLD disappear, which does not fit the model

`rtx.useVertexCapture = False` was loaded - the log confirms it, and
`[RTX-Compatibility-Info] Skipping draw call with shader usage as vertex capture is not enabled`
fired - and the user reports **the world disappeared**, so they turned it back on. The magenta was
unaffected either way.

That does not fit. Converted draws have their shaders NULLed, so they are not "draws with shader
usage" and Remix should not skip them. The shim was still converting 378 draws a frame in that run,
into a 2560x1440 fmt=113 target. **Either those draws did not survive, or what we have been looking
at was never the converted copy.**

Which is a question about the pipeline itself, not about the marker - and six explanations for the
magenta have now been proposed and discarded in a row:

| run | proposed cause | how it died |
|---|---|---|
| 55 | refused rigid draws pass through and get reconstructed | refusals reached 0, symptom stayed |
| 57 | duplicate submissions the dedup key was too small to catch | dedup works, symptom stayed |
| 58 | Remix categorises from leftover stages, clear all 8 | magenta stayed |
| 59 | the clear was on the wrong prepass rule | fixed the rule, magenta stayed |
| 60 | `ignoreTextures` visualises rather than hides; use `hideInstanceTextures` | magenta stayed |
| 61 | vertex capture resurrects hidden draws | the world disappeared instead |

Continuing to guess would be a seventh. The USD captures were checked first for objective evidence
and the naive string extraction hits USDC's LZ4-compressed token table, so they report 2 meshes
where a scene has thousands; writing a USDC parser is not worth it for this question.

**So this run measures the pipeline instead.** `ffp=0` - the shim's own master switch, described in
the ini since it was written as "the A/B for the question is our converted geometry reaching the
path tracer at all" - with vertex capture back ON.

    world looks much as it does now   -> the visible world is Remix's vertex capture of the game's
                                         shader draws, and the FFP conversion has been adding a
                                         SECOND copy rather than the one we see. That would reframe
                                         all of the texturing work and explain why suppressing the
                                         prepass never removed anything visible.

    world untextured or broken        -> the conversion is what renders, and what vertex capture
                                         was contributing is something else - most likely the sky
                                         and the passed-through draws that carry the scene's light.

Either answer is worth more than another attempt at the marker.

### Deployed

    sr3-rtx.asi   91612d4c693ad0c22d988786f112367d   (unchanged)
    sr3-rtx.ini   d1b0f9b465175f62053af4a89b476daa   (ffp=0, DIAGNOSTIC - set back to 1 after)
    rtx.conf      978b0ca79e42dcc6379db4f03ff6d1f0   (useVertexCapture back to True)

### Run 63: the magenta IS vertex capture, and the pass-through population is ONLY the sky

The user's fuller description of the vertex-capture-off run settles run 61 and corrects run 62. With
`ffp=1` and `rtx.useVertexCapture = False`:

- **the magenta was GONE** - so run 61's mechanism was right, and the marker, the wide clear,
  `ignoreTextures` and `hideInstanceTextures` were all managing a symptom of it;
- the world was **culled, with things popping in and out "as if they get rendered and then
  deleted"** and only edges showing;
- the player's texture was **black**.

Run 62 read "the world disappears" as the conversion not rendering at all, and set `ffp=0` to test
that. The fuller description says otherwise - the world DID render, incompletely and unstably - so
the diagnostic was answered before it ran and `ffp` goes back to 1 without a run being spent.

**What actually passes through, measured from the frame dump rather than assumed:**

    dispositions:  979 CONVERT   2765 MARK   50 PASS

    every PASS:    50  sky (rfg-skybox family) - left for Remix to capture and tag

    the MARK population:
      2021  prepass: shader samples nothing
       468  prepass: only normal/stipple/depth samplers
       242  post/composite quad
        22  auxiliary camera
         7  particle billboard
         3  skinned duplicate
         2  prepass: no colour map, no colour constant, never reads the L-buffer

**Nothing but the sky passes through, and marking is not deleting world geometry** - of 2,765
marked draws only ~54 name a colour sampler at all, and those are the composite chain and particle
billboards. So with vertex capture off the scene is the 979 converted draws: the world, minus the
sky.

**The popping has its own cause, and Remix names it.**

    rtx.antiCulling.object
      "Extends lifetime of objects that go outside the camera frustum (anti-culling frustum)."

Not present in `rtx.conf`, so off. SR3 culls aggressively, and with vertex capture on the captured
copies were keeping culled objects alive; turning it off exposed that Remix was dropping them the
moment the game stopped submitting them. That is "rendered and then deleted" exactly.

Enabled together with vertex capture off, because the second is only exposed by the first and
testing them apart would spend a run learning nothing. `numberOfFramesToKeepObjects = 60`.

**Two known gaps remain, both now bounded and measured:**

1. **The sky** - 50 draws a frame, the only pass-through population, skipped when vertex capture is
   off. It is passed through deliberately so `rtx.skyBoxTextures` can tag it, and converting it
   instead needs care: it is a 343-vertex dome one unit from the camera and treating it as ordinary
   lit geometry is what produced the black sky in session 14.
2. **The player's texture is black** with only the converted copy on screen. Previously the
   vertex-captured copy was covering this, so it is not a regression - it is a defect that has been
   hidden all along, and the first one worth looking at once the scene is stable.

### Deployed

    sr3-rtx.asi   91612d4c693ad0c22d988786f112367d   (unchanged)
    sr3-rtx.ini   924e36e67c994a73f074f4c53e00f79e   (ffp back to 1)
    rtx.conf      371c093523bb6ec149e86da891949ed5   (useVertexCapture=False, antiCulling.object on)

Backups: `rtx.conf.before-anticulling.bak`, `rtx.conf.before-vertexcapture-off.bak`.

### Run 64: anti-culling loaded and did nothing; the converted world is submitted as INSTANCED draws

`rtx.antiCulling.object.enable` was verified as parsed rather than assumed - Remix logs
`rtx.antiCulling.object.enable = True` and `numberOfFramesToKeepObjects = 60` - and the draws still
"enter the view frustum and get deleted right after". A clean negative: object anti-culling is not
the cause, and `rtx.enableCulling` is only front/back-face culling for opaque objects, so it is not
either.

Checking that is what surfaced the thing worth looking at:

    instancing: 377 converted from the instance stream/frame  (of 378 converted)
                largest single draw 1 instances

**Almost every converted world draw is a D3D9 instanced draw, and every one carries exactly one
instance.** The classifier already refuses anything with more than one - one world matrix cannot
place several copies - so by the time a draw converts, its per-instance transform has been read out
of the stream and applied as the world matrix and a single copy is drawn. The instancing conveys
nothing at that point.

What it does convey is instancing state that Remix has to handle on a fixed-function draw. While
`rtx.useVertexCapture` was on, the shader copies of the same geometry filled the scene and hid
whatever Remix made of ours; with vertex capture off - the setting that finally removed the magenta
- our converted draws ARE the scene, and their instability became the visible problem.

`deinstanceConverted` resets the frequency on streams 0 and 1 for single-instance converted draws
and restores the exact prior setting afterwards, since the engine's own draws depend on it. The
prior value is now recorded per stream in `Hook_SetStreamSourceFreq` rather than reconstructed from
an assumption about what it probably was.

Stated in the ini so it does not become another change that stays because it is already there: **if
the popping persists with this on, instancing is not the cause and it goes back to 0.**

### Deployed

    sr3-rtx.asi   028f3f116b6780d997a9dd00b2ab5a8b
    sr3-rtx.ini   aa61c333ea83d7d5a72d3666448320b6   (deinstanceConverted=1)
    rtx.conf      371c093523bb6ec149e86da891949ed5   (unchanged)

Build clean, no warnings.

### Run 65: seven mechanisms have now failed, so stop proposing an eighth and measure

De-instancing did not stop the popping, so `deinstanceConverted` goes back to 0 rather than staying
in as a change that did nothing.

Also reported, and the same shape as the sky: **with vertex capture off the UI disappears too.**
SR3 draws its HUD with shaders, the shim does not convert those, and Remix skips shader draws when
vertex capture is off. So the real cost of that setting is *everything the shim does not convert* -
sky, UI, and anything else passed through - which is a more useful way to hold it than discovering
one missing thing per run.

**The record for the magenta and the instability:**

| run | proposed cause | outcome |
|---|---|---|
| 55 | refused rigid draws reconstructed by Remix | refusals hit 0, symptom stayed |
| 57 | duplicate submissions, dedup key too small | key fixed, symptom stayed |
| 58 | Remix categorises from leftover texture stages | magenta stayed |
| 59 | the wide clear was on the wrong prepass rule | rule fixed, magenta stayed |
| 60 | `ignoreTextures` visualises rather than hides | magenta stayed |
| 61 | vertex capture resurrects hidden draws | magenta GONE - but the world became unstable |
| 63 | Remix drops culled objects; enable anti-culling | verified loaded, popping stayed |
| 64 | instanced draws destabilise the FFP scene | de-instanced, popping stayed |

Run 61 is the only one that hit. Everything after it has been an attempt to make its configuration
usable, and none of them worked, which suggests the model of where converted geometry GOES is
wrong rather than any of the individual fixes.

**The unexamined fact.** Converted draws are recorded going to `14EC93D0`, a 2560x1440 fmt=113
target - the game's HDR scene buffer, not the back buffer - and Remix logs
`Found a draw call to a non-primary, non-raytraced render target. Falling back to rasterization`.
If our converted geometry is being RASTERISED into that target rather than entering the ray-traced
scene, then the path-traced world seen all along has been the vertex-captured shader copies, and
turning capture off removes the only world there was. That would explain runs 61 through 64
together, where each individual explanation failed separately.

**So this run measures it instead of arguing it.** `ffp=0` with vertex capture back ON - the shim's
own master switch, documented since it was written as "the A/B for the question is our converted
geometry reaching the path tracer at all". It has been set up twice and confounded twice by the
vertex-capture setting changing underneath it; this time the working configuration is restored
first so only one thing differs.

    world looks essentially unchanged  -> the visible world is the vertex-captured shader draws,
                                          and the FFP conversion has been a second copy all along
    world untextured, flat or missing  -> the conversion is what renders, and the instability with
                                          capture off is a different problem to solve on its own

The playable configuration is restored either way: vertex capture on, de-instancing off.

### Deployed

    sr3-rtx.asi   028f3f116b6780d997a9dd00b2ab5a8b   (unchanged)
    sr3-rtx.ini   27ab6fdb89fd8fffa3a0793aadf6f53c   (ffp=0 DIAGNOSTIC, deinstanceConverted=0)
    rtx.conf      b9434d9408e39759f05dd2e4892aeb95   (useVertexCapture back to True)

### Run 66: ANSWERED - the fixed-function conversion IS the path-traced world

With `ffp=0` and vertex capture on: *"the world is rasterized. no path tracing i believe because the
capture button does not do anything."*

No ray-traced scene exists without the shim's conversion. **The FFP conversion is the path-traced
world, entirely.** That disposes of the run-65 theory that converted draws were being rasterised
into the game's HDR target and never reaching the path tracer - they reach it, and they are all of
it.

**What vertex capture actually does here.** It was never providing the world. It adds a SECOND,
shader-derived copy of everything the shim passes through or marks - and the frame dump says that
is 2,765 marked draws a frame against 979 converted. The magenta on characters and the head
z-fighting are that copy. Turning capture off removed it, which is exactly why run 61 worked.

**Why capture cannot simply stay off.** With it off, everything the shim does NOT convert is gone:
the sky (50 draws a frame, deliberately passed through so `rtx.skyBoxTextures` can tag it), the
entire HUD (shader-drawn, never converted), and the world became unstable in a way that anti-culling
and de-instancing both failed to fix.

So the two configurations are:

| | ray-traced world | magenta / z-fighting | sky | UI | stability |
|---|---|---|---|---|---|
| capture ON | yes | **yes** | yes | yes | stable |
| capture OFF | yes | no | no | no | objects drop in and out |

**The target is capture ON with the marked draws not captured.** That is what
`rtx.hideInstanceTextures` is for - Remix's own text calls it "hidden from rendering" - and the
marker hash is listed in it, and the magenta persisted. The simplest untested explanation is that
`0x978271113F293CE4` is not the hash Remix computes for the marker texture. It was inferred by
diffing config backups in run 44, never confirmed against Remix's own texture list, and everything
built on it since has assumed it correct.

That is checkable in a minute from the Remix menu, by the one person who can see which texture is
magenta on screen, and it should be checked before any more code is written against it.

Playable configuration restored: `ffp=1`, vertex capture on, de-instancing off.

### Deployed

    sr3-rtx.asi   028f3f116b6780d997a9dd00b2ab5a8b   (unchanged)
    sr3-rtx.ini   50e4d4cee8c13543dc1e352c0f90a1a1   (ffp back to 1)
    rtx.conf      b9434d9408e39759f05dd2e4892aeb95   (unchanged)

### Run 67: instrumenting the instance transform, because it is the one input nothing has ever checked

Direction agreed: get off `rtx.useVertexCapture` entirely, since no texture-tag mechanism suppresses
the captured duplicates and the marker hash is confirmed correct - the user checked it in the Remix
menu and it was already the selected texture. Ignore, hide-instance and the wide stage clear were
all correctly implemented against the right hash and none of them work on a captured draw. There is
no configuration with capture on and no duplicates.

Three gaps block turning it off - stability, UI, sky - and stability gates the other two.

**The suspect, and why it is the right one to check first.** Converted instanced draws - 377 of 378
- do not get their world matrix from a shader constant. They get it from a **snooped copy** of the
game's instance vertex buffer, refreshed only when the game locks that buffer, which measures at

    0.7 buffer writes snooped/frame

against those ~377 draws. So almost every converted instanced draw reads bytes written on some
earlier frame. That is correct if the buffer holds static world transforms and fatal if it does
not, and **nothing in this shim has ever distinguished the two.**

`D3DLOCK_DISCARD` is what makes it dangerous rather than merely old: it means the previous contents
are gone, the game gets a fresh allocation and typically fills only the part it needs. Everything
outside that span is stale while still holding plausible numbers. An object whose transform is read
from a stale region lands wherever the previous occupant of those bytes was - and if that is behind
the camera, it looks precisely like "it entered the frustum and got deleted".

**The probe.** `InstanceCache` gains a `fresh` array parallel to `data`: cleared wholesale when a
discard is seen on that buffer, set over the exact span the game writes on each unlock. `InstanceWorld`
then checks the 48 bytes it is about to read and counts fresh against stale, naming the first six
with their translation so a stale one can be recognised on screen.

    instance transforms: N/frame from freshly written bytes, M/frame from STALE bytes
                         | K whole-buffer discards seen

A large stale count confirms it and points straight at the fix - snapshot on discard rather than on
write. A zero stale count kills the suspect outright and the instability is somewhere else, which
is worth just as much after eight failed mechanisms.

Read-only; no behaviour change. Vertex capture stays ON so the game remains playable while this is
measured - the counters do not need the broken configuration to be informative.

### Deployed

    sr3-rtx.asi   79725eda32a1adf03f7774dde708ed5b
    sr3-rtx.ini   50e4d4cee8c13543dc1e352c0f90a1a1   (unchanged, ffp=1)
    rtx.conf      b9434d9408e39759f05dd2e4892aeb95   (unchanged, capture on)

Build clean, no warnings.

### Shader corpus: what the game is actually doing, and whether capture-off can cover it

Asked before proceeding: if vertex capture is not the way, can everything be made to work - and read
the shaders rather than guess.

**The corpus.** 843 `.fxo_pc` files, 7,088 shader entries: 3,363 pixel and 3,725 vertex.

**Multi-pass inferred lighting, confirmed across the whole corpus rather than from one file.** Each
material file holds several pixel shaders for different passes. Classifying them by what they
SAMPLE gives:

    1325  material pass (reads IR_LBuffer)
     562  stipple / normal prepass
     307  no sampler at all (depth / DSF prepass)
      45  reads the G-buffer (post, lighting)
      26  post / composite chain

**The pass is NOT determined by the shader index**, which is worth stating because it would be the
obvious shortcut. Across all 843 files the distribution overlaps heavily - index 5 is 182 "other"
against 164 prepass, index 6 is 195/184/166 three ways, index 7 is 346 material against 225 prepass
- so any rule of the form "index 6 is the prepass" would misclassify hundreds of shaders. The
shim's property-based classification is the only reliable one, and this is the evidence for it.

**Can capture-off cover everything?** From a real frame's dispositions rather than from the corpus:

    979 CONVERT      the path-traced world - proven in run 66 to be the entire ray-traced scene
    2765 MARK        2489 prepass + 242 composite + 22 auxiliary camera + 7 billboards + 3 dupes
      50 PASS        every one of them the sky

Everything in MARK is content that SHOULD be absent from the ray-traced scene; capture-off gives
that for free and correctly, which is what removed the magenta. So the content genuinely lost is:

1. **the sky** - 50 draws a frame, the only pass-through population;
2. **the UI** - shader-drawn, never converted;
3. and stability must be fixed, cause not yet known.

That is a short and specific list, not an open-ended one. The answer to "can everything work" is
yes on the evidence available - two populations to convert plus one bug - with the honest caveat
that the stability cause is unmeasured, which is exactly what the run-67 probe exists to settle.

## Consolidation, 2026-08-21

Documentation brought up to date before compacting the session. Runs 43-67 are recorded above; this
entry records what changed in the durable docs and why.

**`docs/YOUR-INSTRUCTIONS.md`, 395 -> 607 lines.**

- **"The hiding problem - SOLVED 2026-08-18" was WRONG and is now marked so.** It claimed the
  marker texture plus `rtx.ignoreTextures` removed unconverted draws from the ray-traced scene.
  Remix's own strings say `ignoreTextures` is not a hide - it renders ignored materials as a pink
  and black checkerboard. Every marked prepass draw has been rendering as that checkerboard since
  session 18. This single wrong sentence cost runs 55-60.
- Current state rebuilt: SOLVED gained the texcoord format bug, the tiling-to-albedo fix, the asset
  hash rule, the clothing generator and the black cars. OPEN restructured around the one decision
  everything now hangs off - `rtx.useVertexCapture` must go off - with its three blockers.
- New sections: **Remix's own binary is the documentation** (with the option-semantics table that
  has been earned one painful run at a time), **the Remix API is recreate-only**, **colour space**,
  **SR3's material recipes from disassembly**, and **skinning facts settled**.
- New method sections, which are the part most likely to save the next session: the nine-mechanism
  table for the magenta; counters going up is not the population being covered; when argument fails
  twice, dump the data and look; verify a setting was parsed; write the falsifier into the change;
  analyse a risky change against real data before enabling it.
- Dead ends gained six entries, including every texture-tag route to suppressing an unconverted
  draw, the DX9 tracer, the static PE scanners, and naive USDC string extraction.

**`docs/asset-replacement-plan.md`, 133 -> 180 lines.** Two prerequisites now met (world texturing,
stable asset hashes), one new blocker (capture-off must land before authoring, or replacements are
authored against a scene containing duplicate geometry), and the Remix API question settled in the
negative for characters.

**What the next session should do first:** read the run-67 probe's output. It is deployed,
read-only, and vertex capture is left ON so the game is playable. The line to find is

    instance transforms: N/frame from freshly written bytes, M/frame from STALE bytes

A large stale count is the stability blocker and points at snapshotting on discard rather than on
write. A zero count kills the leading suspect, which after nine failed mechanisms is worth as much.

**Deployed and hash-verified at the time of writing:**

    sr3-rtx.asi   79725eda32a1adf03f7774dde708ed5b
    sr3-rtx.ini   50e4d4cee8c13543dc1e352c0f90a1a1
    rtx.conf      b9434d9408e39759f05dd2e4892aeb95

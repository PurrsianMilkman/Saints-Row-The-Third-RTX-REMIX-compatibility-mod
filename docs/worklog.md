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

---

## Session 2026-08-23 to 2026-08-26: capture-off achieved, and the car-part drift

### Run 68: the occlusion-query hook - THE BREAKTHROUGH

`rtx.useVertexCapture = False` had always collapsed the game. Runs 62-67 blamed Remix. The cause
was in SR3, and the codebase already knew the rule - it is written above `hiddenPassMode`:

> **2 skip** - MEASURED TO BREAK THE ENGINE: its own submission collapsed from 2,615 to 593 draws
> a frame and the image froze, **because it reads the prepass back - GPU occlusion culling**. The
> general rule: **a draw whose RESULT the engine reads can never be skipped, only hidden.**

`useVertexCapture = False` IS a skip - Remix refuses every shader draw, the depth prepass among
them. Two frame dumps in the same area (camera y~147) measured the collapse directly:

    capture ON   5330 draws/frame   (MARK 3930, CONVERT 1260, PASS 140)
    capture OFF  1426 draws/frame   (MARK 1182, CONVERT  220, PASS  24)

**73% of the GAME's own submission gone**, against the 77% recorded for `hiddenPassMode=2`. The
same failure. Objects clipped by a frustum plane survived because an occlusion test on a clipped
bounding box is unreliable and engines skip the query for those - which is why only the periphery
rendered, the detail that identified the mechanism.

**The fix needed nothing from Remix.** Occlusion queries are D3D9 objects and we are the D3D9
layer. `Hook_CreateQuery` (device slot 118) patches the shared `IDirect3DQuery9` vtable;
`Hook_QueryGetData` (slot 7) answers `D3DQUERYTYPE_OCCLUSION` with 2^20 visible pixels and
`D3D_OK`. Only occlusion queries - `D3DQUERYTYPE_EVENT` is a frame-pacing fence. The real query is
never drained: calling through can carry `D3DGETDATA_FLUSH`, which stalls across the bridge.

Measured: **243.9 occlusion queries read back per frame, all 243.9 answered VISIBLE**, 4097 query
objects. World, cars, NPCs and props stopped being culled. `forceOcclusionVisible=1`.

### Run 69: hiddenPassMode=2 became viable, and the marker subsystem is gone

With the readback neutralised, mode 2 no longer breaks the engine - 3173 draws/frame, no collapse.
That deleted the entire marker mechanism: **3886 marked draws and 14,319 SetTexture calls a frame,
and the magenta permanently**. `MARKED 0/frame`.

### Lights: the cap was ours to raise

`device reports MaxActiveLights = 8` against `lights 26.8/frame`. D3D9 silently ignores
`LightEnable` past the cap and which lights lose changes per frame - "unstable, culled at random
positions". `d3d9.maxEnabledLights` is a DXVK option that fills that cap; it defaults to 8. The
shim already supported 64:

    g_maxLightSlots = (caps.MaxActiveLights == 0) ? 64u : min(64u, caps.MaxActiveLights);

New `dxvk.conf` with `d3d9.maxEnabledLights = 64`. Confirmed: **`device reports MaxActiveLights =
64, using 64 light slots`**. No code change - the shim had been ready all along and was being told
there were 8. Also added `rtx.antiCulling.light.enable = True`.

**`dxvk.conf` is a separate option layer the Remix in-game menu does not rewrite** - a safer home
for hand-authored settings. Parse order, from the Remix log: `dxvk.conf`, then `rtx.conf`, then
**`user.conf` last, so user.conf wins.**

### Fixed-function vertex blending: DEAD, and cheaply

Asked rather than assumed, one log line at device creation:

    MaxVertexBlendMatrices = 4, MaxVertexBlendMatrixIndex = 8, VertexProcessingCaps = 0x17B

Four influences per vertex is right; **only nine matrices are addressable and SR3 needs 64.** The
route is closed. CPU skinning stays.

### CORRECTION: Remix DOES support skinned replacement geometry

`asset-replacement-plan.md` said "Remix cannot skin a replacement mesh". **That is wrong.** Remix
1.5.2 has `gpu_skinning`, `performSkinning`, `RtxGeometryUtils::dispatchSkinning`, full
`UsdSkelBindingAPI` read paths, a `ReadBoneTransform` graph node, a `skeletons/` capture directory,
and an option whose text is explicit:

> "Limit the number of bone influences per vertex **for replacement geometry**. D3D9 games were
> limited to 4, which is the default." - `rtx.limitedBonesPerVertex`

That removed the strongest argument for forking dxvk-remix. It does require the draw to carry
bones (`dispatchSkinning: draw call has bones but no blend weight buffer`), which our CPU skinning
does not - and FF vertex blending, the way to supply them, is capped at 9 matrices.

### The shader disassembly: vehicles use ONE bone, characters four

Read rather than guessed, with `tools/fxo_disasm.py`.

`ir_sr3cardiffusespec_g_v` shader[0..4], and `ir_sr3carglass_g_v` - identical in form:

    dcl_position v0 / dcl_normal v1 / dcl_blendindices v2      <- NO dcl_blendweight
    mul  r2.x, c0.z, v2.x        (c0.z = 3)
    mova a0.x, r2.x
    dp4  r0.x, c52[a0.x], r1     <- ONE bone, unweighted
    dp4  r1.x, c32, r0           <- then objTM

`ir_sr3npcclothfull_c` shader[0]:

    dcl_blendweight v4 / dcl_blendindices v5
    mul r2, v4.y, c52[a0.x] ... four weighted bones

The shim decided this from the VERTEX DECLARATION, which still carries BLENDWEIGHT bytes on a
vehicle draw that the shader ignores - so we blended four bones where the game blends one, pulling
in three indices from bytes the shader never reads. Now decided from the shader's own `dcl`
instructions.

**A parser bug caught before shipping:** a comment block (CTAB is one, and it sits immediately
after the version token) carries its length in bits 16-30, NOT the 24-27 field instructions use.
Reading the wrong field walks into the constant table and never reaches the dcls. Verified offline
against real bytecode before the run: vehicles SINGLE, characters 4-bone.

Result: `SHADER BLEND FORM: 109.3 four-bone/frame, 82.3 SINGLE/frame`, refusals 0. **It did not
fix the drift.**

### `skinRigidSingleBone` must stay ON

It is the gate that lets a declaration with no BLENDWEIGHT element (`weights=?(-1)@-1`) through
`GetBaseMesh`. At 0, every vehicle body/glass/panel draw is refused, falls back to pass-through,
and under `hiddenPassMode=2` with capture off a pass-through draw is **skipped** - the user's
"cars do not render at all, i only see the wheels and the people in them". Wheels and characters
carry real BLENDWEIGHT and pass either way.

### USD capture tooling now exists

`pxr` (USD python bindings) is **already installed** on this machine
(`Python312/Lib/site-packages/pxr`). Captures are binary `PXR-USDC` and read directly:

```python
from pxr import Usd, UsdGeom, Gf
st = Usd.Stage.Open("capture_....usd")
# /RootNode/meshes/mesh_<hash>/mesh  - points, indices
# /RootNode/instances/inst_<hash>_N  - xformOp:transform
# /RootNode/lights, /RootNode/Looks, /RootNode/cameras/Camera
```

This unblocks the hash-to-asset bridge for asset replacement as well.

---

## The car-part drift: OPEN. Five failed fixes, and what they eliminated

**Symptom, as finally stated by the user:** a knocked-off bumper or car glass is rendered in the
wrong place and moves, in rhythm with player or NPC animation, "depending on what is on screen".
The mesh is correct and undeformed. **The game's own logical/physics position does not move** - the
part can be rolled and behaves correctly. Only the path-traced transform moves.

### The one decisive experiment (the user's, not mine)

**The drift does NOT happen in the vanilla game, and does NOT happen with our .asi disabled while
Remix is running.** It requires our shim.

That also dissolves the paradox that dogged five runs: with vertex capture, Remix reads the game's
**already-transformed** vertex output, so it is identical to the game by construction. We recompute
that transform. Everything verified about our arithmetic can be correct while the result differs,
because the difference is not in the arithmetic.

Note: our .asi with Remix REMOVED renders black - the shim nulls shaders and issues fixed function
with nothing behind it to path-trace. A vanilla test needs both disabled: move Remix's `d3d9.dll`
aside AND rename `sr3-rtx.asi` (the loader is `dinput8.dll` and only picks up `.asi`).

### Verified correct, and therefore eliminated

| link | how it was verified |
|---|---|
| transform form | matches all five vehicle vertex shaders in disassembly |
| bone index | `v.x * 3`, component 0, matches `mova a0.x` |
| palette register | c52 - **all 882 `Bone_weights` declarations in the game**, from `re/shader_constants.csv` |
| objTM register | c32 - all 2357 declarations |
| **constant VALUES at draw time** | **`GetVertexShaderConstantF` read back and compared: 649,925 comparisons, 0 mismatches** |
| geometry + transforms Remix receives | two captures, static camera, **bit-identical**; cars intact, characters coherent |

### Five fixes attempted, all wrong

| # | hypothesis | falsifier that killed it |
|---|---|---|
| 1 | `skinRigidSingleBone` misfiring | not firing - refusals 0, those layouts carry BLENDWEIGHT |
| 2 | bones from a different upload epoch (`rejectStaleBones`) | stranded **2829 verts/frame** in bind pose; epochs were one object's multi-call upload |
| 3 | stale bind-pose cache (`InvalidateBaseMeshes`) | **0 invalidations** - the game never refills those buffers. Kept anyway: correct by construction |
| 4 | bones outside this object's upload (`clampBonesToUpload`) | fired on 15.6% of verts, stopped the rare *stretching*, did not stop the drift |
| 5 | palette not scoped to its draw setup (`paletteSetupScope`) | refused **3491 influences/frame**, worst case got WORSE (1.485 -> 4.083) |

Also eliminated by config, each verified parsed by Remix: `useBuffersDirectly = False`,
`enableAlwaysCalculateAABB = True`, `enableInstanceDebuggingTools = True` (disables temporal
instance correlation), `upscalerType = 0` + `useDenoiser = False` (all temporal reprojection off),
and three geometry hash rules.

### The hash rule, settled

- `positions,indices,geometrydescriptor` - unique per part but **churns**, because CPU skinning
  changes positions every frame
- `indices,geometrydescriptor` - stable but **collides** between parts sharing an index buffer
- **`indices,texcoords,geometrydescriptor` - stable AND discriminating.** Our skinning copies UVs
  from the bind pose untouched, so they never churn, and different parts have different UVs.
  User-confirmed stable. **This is the rule to keep.**

Remix's own text: the ASSET hash rule is for *"sampling from replacements and doing USD capture"*,
NOT for placing geometry - so it was never going to be the drift's cause.

### THREE metrics of mine that measured legitimate behaviour as a defect

This is the methodological failure of the session and the thing most worth not repeating:

1. **"a rigid part should sit at its own origin"** - wrong. `bone[13]` is a 22-degree rotation plus
   `(0, 1.121, -1.440)`; the mesh is authored at the origin and the bone legitimately **places it
   on the car**. An offset result is normal.
2. **"bones written at different draw indices are foreign"** - wrong. One object's palette arrives
   as several `SetVertexShaderConstantF` calls.
3. **"geometry moving while objTM is static is drift"** - wrong. Captured a destroyed car's
   suspension settling: `bone[0]` translation decaying `0.117 -> 0.109 -> 0.097 -> 0.087 -> 0.072
   -> 0.000` across frames. That is animation, correctly read.

**Every one of these was a definition of "correct" that I invented and then measured against.** The
measurements that held up were the ones anchored to something external: the disassembly, the
device's own constants, the game's behaviour with the shim disabled.

### Where it stands

Deployed for the next run: **`skinRingMB` 8 -> 64**. The CPU-skinning ring is the only shared
mutable memory in the skinned path; its write position is forced to `firstVertex*stride` so the
game's index buffer can be reused verbatim, which consumes it far faster than the vertex count
implies, and a wrap issues `D3DLOCK_DISCARD` over the whole buffer. How often that happens scales
with how much skinned geometry is on screen - the one property of the symptom nothing else
explains. New falsifier:

    SKIN RING: 64 MB, high water H MB, W wraps/frame, D discards/frame

If wraps/frame was already 0 at 8 MB, the ring never recycled and this is dead regardless of what
the screen shows.

**If the ring is not it, the honest next step is not a sixth hypothesis.** It is to bisect our own
conversion with the switches: `convertSkinned=0` (those draws vanish rather than drift - proves the
skinned path), then narrow inside it.

---

## 2026-08-26 - the ring answered, and then broke the game

### The ring test came back positive, and it is the first of the three outcomes

Run of 3029 frames (~76 s), `skinRingMB=64`, everything else unchanged:

    SKIN RING: 64 MB, high water 15.24 MB, 0.00 wraps/frame, 0.54 discards/frame

High water **15.24 MB**. The old ring was **8 MB**. So at 8 MB the ring was being recycled
mid-frame - the hypothesis was live, not idle, and the run that would have said so was never
taken at 8 MB during gameplay (the three 8 MB samples in this log are menu frames, `high water
0.00 MB`). At 64 MB wraps go to **0.00/frame**. That is the outcome I labelled "wraps were
happening and now stop".

What it does NOT yet say is whether the drift stopped, because the run ended before that could
be judged: the game became unplayable.

### Why 64 MB broke it

The ring resets at every Present (`g_skinRingPos = 0; g_skinRingFresh = true`), so the first
skinned lock of each frame is `D3DLOCK_DISCARD`. **A discard renames the WHOLE buffer.** Its cost
is set by `skinRingMB`, not by the 15 MB we actually write, and certainly not by the vertex count.
At ~0.6 discards/frame that is ~38 MB/frame of fresh allocation asked of the driver, across the
32->64-bit Remix bridge.

The shape in the log matches a driver waiting for a slice to come free, not work that scales with
the scene:

| frames | draws/frame | shim ms |
|---|---|---|
| 1661-2958 | 4200-4500 | 12-18 |
| 2970-3018 | 4100-5700 | 20-65 |

Draw count rose ~20%. Shim time rose 2-4x, as a hard sawtooth - `20.4, 64.6, 22.4, 28.0, 64.8,
23.8, 22.8, 24.4, 55.7, 25.9, 64.5` - a ~40 ms spike on roughly a 1-in-2 cadence, which is the
same cadence as the discards. CPU-skinning volume varies smoothly with what is on screen; it does
not alternate.

### That is still an inference, so it is now instrumented

The sawtooth is suggestive and the mechanism is sound, but a shape in a graph is exactly the kind
of evidence that has cost this project runs before. The competing explanation is real: the shim
CPU-skins **211,312 vertices/frame** across 54 draws and writes 6.8 MB into write-combined GPU
memory, and the collapse window is also where the street got busy. Both stories fit.

So the lock is now timed directly, split by kind:

    SKIN RING LOCK: X ms/frame discarding, Y ms/frame appending, worst single lock Z ms

The `appending` (NOOVERWRITE) column is the control - same call, same path, no rename. If
`discarding` is a few ms/frame and `worst` is tens of ms, the ring's size is the cost and the fix
is to stop renaming 24 MB a frame. If `discarding` is ~0, the ring is exonerated and the cost is
CPU skinning volume, which is a different fix entirely (and one we already know how to approach).

### Deployed: `skinRingMB=24`

24 MB clears the measured 15.24 MB high water by 57%, so **wraps should stay at 0** and the drift
test stays valid - while asking the driver for a third of the memory per frame. If the ring is the
cost, the spike amplitude should fall by roughly the same factor it shrank by; that is a
quantitative prediction the next run either meets or does not.

### The real fix, if the lock timing confirms it

The ring only needs to be ~16 MB because of this, in `SkinAndBind`:

    const UINT base = firstVertex * stride;
    if (g_skinRingPos < base) g_skinRingPos = base;

The write position is dragged up to the game's own vertex index so the game's index buffer can be
reused verbatim. High water 15.24 MB / 32-byte vertices = ~480,000 vertices, i.e. the ring is
sized by the game's largest `firstVertex`, **not by how much geometry we skin**. We already rebase
through the stream offset (`SetStreamSource(..., g_skinRingPos - base, ...)`); the only reason the
position is forced up at all is that this offset cannot go negative.

`DrawIndexedPrimitive` takes a `BaseVertexIndex`, and the hook currently forwards the game's
unchanged. Adjusting it for converted skinned draws removes the constraint entirely and the ring
drops to ~2 MB - at which point the per-frame discard stops mattering at any cadence. That is a
change on the hottest path and it is not being made on the same run as a measurement.

### New lead, recorded and NOT acted on: FOREIGN BONE

Frames 3007-3008, twenty reports, while a knocked-off part was falling:

    FOREIGN BONE #1: ps='Grime_MapSampler' verts=1976 bone 0 was written under objTM generation
    309836 but this draw is generation 309837 (1 objects later) | objTM t=(101.8 9.4 195.7) |
    moved 0.73 | draw 4733 frame 3007

Consecutive draws 4733-4745, same 1976-vertex mesh, **eight different objTM translations**
(101.8/9.4/195.7, 101.8/9.3/196.2, 100.3/9.0/197.1, 101.0/9.3/195.2, ...) - all reading bone 0
from the *same* upload, generation 309836.

Sharing a palette across instances is not by itself wrong: under the real vertex shader the bone
is object-local and `objTM` separates the instances. What is worth noting is that the shared bone
value **changes between frames** - `moved 0.73` at frame 3007, `moved 0.89` at 3008 - so every
instance reading it shifts together. "Parts move together, with a pattern like the character's"
is what that would look like.

Set against it: the frame-3000 report says `RIGID BONE OWNERSHIP: 6.9/frame use a bone written
under their OWN objTM, 0.0/frame use one written under a DIFFERENT object's`, and `DRIFT (objTM
unchanged between frames): 1.0 draws/frame STILL, 0.0 MOVED (worst 0.000 units)`. The foreign-bone
reports appear only in the two frames around the knock-off.

**This is exactly the shape of the three findings that already wasted runs on this project** - a
probe firing, a plausible story, and an invented notion of what the engine ought to be doing. It
is written down and it is next in line *after* the game is playable and the ring question is
closed. It is not being turned into a fix on a hunch.

### Also seen, unexplained, low priority

`dumpFrame=1800` did not fire: the run passed frame 1800 and `sr3-rtx-frame.log` still carries the
11:37 timestamp from the previous run. Worth a look before the next time that dump is relied on.

---

## 2026-08-26b - the drift, found by disassembly

### The ring is dead, and it was measured dead rather than argued dead

    SKIN RING: 24 MB, high water 24.00 MB, 0.02 wraps/frame, 0.73 discards/frame
    SKIN RING LOCK: 0.01 ms/frame discarding, 0.61 ms/frame appending, worst single lock 0.4 ms

**0.01 ms/frame discarding.** The whole-buffer rename costs nothing, so the sawtooth I read as a
driver stall was not one, and the 64 MB freeze was not the lock. Adding the timer instead of
acting on the shape of the curve was worth the rebuild - the inference was wrong.

And 0.02 wraps/frame cannot produce a drift that affects every part on every frame. The ring is
eliminated as the cause. It did need raising (the 8 MB ring genuinely wrapped), but that was a
real bug next to the one being hunted, not the one being hunted.

### The cause: we decided "skinned" from the vertex declaration, the game decides it from the shader

The user's description was precise enough to aim at: *the rendered part moves as if a character's
skeleton animation were driving it; the physics body is correctly placed; render-side only.*

Disassembly of the car body and car glass shaders. Both ship in two variants over the **same mesh**
and the **same vertex declaration**:

    ir_sr3cardiffusespec_g_v      dcl_position v0 / dcl_normal v1 / dcl_blendindices v2
                                  mul  r2.x, c0.z, v2.x          (c0.z = 3)
                                  mova a0.x, r2.x
                                  dp4  r0.x, c52[a0.x], r1       <- ONE bone
                                  dp4  r1.x, c32, r0             <- then objTM

    ir_sr3cardiffusespec_g_s      dcl_position v0 / dcl_normal v1     <- NO blendindices
                                  dp4  r1.x, c32, r0             <- objTM alone, no palette

`_v` is the vehicle variant, `_s` the static one. Identical for `ir_sr3carglass_g_v` / `_s`.

The shim took `skinned` from `g_curLayout.skinned` - the vertex **declaration** - which is shared
between the variants and still carries BLENDINDICES. It then asked the shader where the palette
lives, got `boneReg = -1` (the static variant declares none), and hit this:

    const UINT boneBase = (g_curVS.boneReg >= 0)
        ? static_cast<UINT>(g_curVS.boneReg) : kRegBonePalette;   // c52

**It fell back to c52 and posed the part from whatever the last character draw left there.**

That is every symptom, exactly:

| reported | explained by |
|---|---|
| rendered part moves, physics body does not | nothing is wrong with the object; only our copy of its geometry is bent |
| movement follows player/NPC animation | c52 holds a character's palette when the part is drawn |
| parts move *together* | they share the one stale palette |
| mesh translates without deforming | one bone, one index, every vertex |
| needs our .asi; vanilla and shim-disabled are clean | the fallback is ours |
| car windows move until broken | intact glass draws through the same pair of variants |

### Why every probe called this clean

    if (boneBase != kRegBonePalette) ++g_skinForeignBoneReg; else ++g_skinC52BoneReg;

`g_skinC52BoneReg` counts "declared at c52" and "declared nowhere, defaulted to c52" as the same
thing. So `bone palette register: 54.1 draws/frame at c52, 0.0 ELSEWHERE` read as a clean bill of
health for the precise draws that were broken. The counter written to catch a wrong register was
blind to no register at all.

The code comment above `boneReg` had even predicted the near-miss - *"A vehicle shader is free to
declare Bone_weights anywhere, and if it does, every car part has been skinned by whatever happened
to sit at c52"* - and then the check that followed only covered the case where it declares it
**somewhere else**, not the case where it declares it **nowhere**.

### The fix, and why it is safe

New `ShaderInfo::usesBlendIndices`, read from the dcl stream (usage 2 on an input register) beside
the existing `usesBlendWeights` scan. A shader that declares no BLENDINDICES input cannot index a
palette, whatever CTAB parsing does or fails to do. Those draws now take 0 influences and fall
through the existing bind-pose path, so objTM places them - which is precisely what the game's own
static variant does.

**Validated before shipping, twice, on the lesson from the last dcl parser bug:**

1. The walk was re-implemented in Python and run against the real bytecode: `_v` -> blendindices
   True, `_s` -> False, for both body and glass.
2. Swept all 1,693 shader files and cross-checked against the CTAB dump:

    | CTAB has Bone_weights | declares blendindices | declares blendweight | files |
    |---|---|---|---|
    | no | no | no | 623 |
    | yes | yes | no | 110 |
    | yes | yes | yes | 111 |
    | **yes** | **no** | - | **0** |

Zero shaders declare `Bone_weights` without `dcl_blendindices`. The implication matters more than
the fix: **`dcl_blendindices` present is exactly equivalent to "this shader skins"** across the
whole shader set, so the gate cannot stop skinning something that genuinely skins.

### What the next run has to show

    NO BONE DECL: N draws/frame whose vertex shader reads no BLENDINDICES

If N is 0 this fixed nothing and the disassembly, while correct, is not what is on screen.
`skinRequireBoneDecl=0` restores the old fallback with no rebuild - the A/B is one ini line.

### Method note

Five hypotheses tested by running the game found nothing in several sessions. Two disassembly
sessions found the vehicle/character bone difference and then this. Both times the instruction to
reverse-engineer came from the user. **Read the shader before measuring what the shader does.**

### CONFIRMED FIXED, same day, by the run and by the probes

User: *"the issue is fixed. no parts move. car glass and parts are where it should be."*

The log agrees, and it agrees in the way that matters - the probes that were firing went silent
without being touched:

| probe | before | after |
|---|---|---|
| `FOREIGN BONE` | 20 reports | **0** |
| `DISPLACED SKIN` | 16 reports | **0** |
| `SKIN DISPLACEMENT` | `worst 2.1 units` moved | `101.7 draws/frame at rest, 0.0 moved >3 units` |
| `DRIFT` | - | `2.6 STILL, 0.0 MOVED (worst 0.000 units)` |

And the gate is doing real work rather than nothing:

    NO BONE DECL: 15.3 draws/frame whose vertex shader reads no BLENDINDICES - gate ON
    NO BONE DECL #1: ps='Grime_MapSampler' verts=1380 - this vertex shader declares no
    BLENDINDICES input and places the mesh by objTM alone. boneReg=-1. Left in its bind pose.

`Grime_MapSampler`, `boneReg=-1` - the car body shader, the static variant, exactly what the
disassembly predicted before the game was run at all. The rate climbs 0.0 -> 5.0 -> 15.3
draws/frame across the run as parts came off.

That closes the bug that consumed the most sessions on this project.

### Two things this run leaves open

**The ring saturates at 24 MB.** `high water 24.00 MB` is pinned at the buffer size and wraps rose
to `0.19/frame`. Deliberately not raised: wraps are now demonstrably harmless (the drift is gone
*with* wraps happening, which is one more nail in that hypothesis), the lock costs `0.01 ms/frame`,
and the unexplained 64 MB freeze argues against churning this while the game works. Known, benign,
documented.

**Shim time grows across a session:** 24.0% -> 35.5% -> 43.8% of frame, worst 107 -> 300 -> 215 ms,
while skinned vertices go 211k -> 354k/frame and decoded meshes 78 -> 181. It tracks skinned
geometry volume, so it may be nothing but a busier district - but `worst 300 ms` is a visible
stall and this is now the largest open problem. Next up, with the HUD and the sky.

---

## 2026-08-27 - NPC heads: four hypotheses killed before a single run

User reports three faults at once: heads **misplaced** ("as if not attached and/or skinned - when
the head moves, the neck is separate"), **mis-textured**, and sometimes **doubled, two heads on one
character, z-fighting**.

Three symptoms, and one wrong bind pose would cause all three - a bind pose carries the geometry,
the UVs and the identity of the mesh together. That is the hypothesis worth aiming at. Everything
below is what was checked first, at no cost in runs, because each would have been a cheaper answer.

### Eliminated without running the game

| hypothesis | how it died |
|---|---|
| The new `skinRequireBoneDecl` gate is leaving heads unskinned | `NO BONE DECL` names only `Grime_MapSampler` and one 7-vertex `Decal_MapSampler`. No `Diffuse_MapSampler`. Heads never reach the gate. |
| Weight/index components are crossed in the four-bone blend | Disassembled `ir_sr3npcskinfull_c`: `mova a0, r1.yxzw` looks crossed, but the `mul`/`mad` order pairs `v4.y` with `a0.x`(=3*v5.y) and `v4.x` with `a0.y`(=3*v5.x). weight[k] does pair with index[k]. The swizzle is scheduling, not semantics. |
| Head bones exceed our palette bound | `kBonesMax = 64`, palette is `float3x4[64]`, highest bone referenced is 58. No clamping. |
| Head vertices are falling through to bind pose | `vertices left in bind pose/frame` is **0.0** in every report before the gate turns on, and afterwards tracks `NO BONE DECL` exactly. No character vertex loses its influences. |
| The cache keys on a raw VB pointer, so a freed buffer's address collides | Already closed: `BaseMesh::owner` AddRefs the buffer, with the 2026-08-18 use-after-free recorded in the comment. |

Also confirmed from the shaders: character skin is `ir_sr3npcskinfull_c`/`_mc`, **four-bone
weighted**, palette at c52, position `blend -> objTM -> projTM` with no scale or bias on `v0`. The
worklog's older claim that "a head is a rigid single-bone attachment" is **wrong** for these
shaders and should not be reasoned from again.

### What the frame dump already shows

One NPC at `at(138.0 13.5 76.4)`, 19 converted skinned draws, two vertex windows (v=704 and
v=799), and a run of ten tiny draws over the same 704-vertex window:

    7348 CONVERT v=799 p=1071  Diffuse_MapSampler tex0=1647FA40
    7349 CONVERT v=704 p=7     Diffuse_MapSampler tex0=1647FA40
    7351 CONVERT v=704 p=1     ...
    7360 CONVERT v=799 p=67    Diffuse_MapSampler tex0=1647FA40

Ten draws of 1-7 triangles each, every one of which CPU-skins all 704 vertices because
`dedupSkinned=0`. That is not a correctness bug but it is a large share of the performance
problem, and it is worth returning to.

### The instrument, so the next run is decisive

The frame dump now carries skinning detail per draw, computed **only on the dump frame** (a
bounding box over every skinned vertex is exactly the per-vertex cost this path cannot afford
every frame):

    | skin first=N vb=P cached|DECODED bind=WxHxD at(x y z) moved M bones=N[lo..hi]

`bind` is the mesh's size **in its own space**, which is what identifies it: a head is a
head-sized box in every pose, so a head draw whose bind box measures a whole body is reading
another mesh's bind pose. `moved` is bind centroid -> posed centroid. Between them:

| reading | meaning |
|---|---|
| head-sized `bind`, small `moved`, neck bones | the mesh is right and the bones are right - look elsewhere |
| head-sized `bind`, large or wrong `moved` | right mesh, wrong bones |
| **body-sized or wrong-sized `bind`** | **wrong mesh - the cache hypothesis, and it explains all three symptoms** |
| two draws, same `first`+`vb`, one `at()` | the doubled head, and whether the two copies share a bind pose |

`dumpFrame` lowered 1800 -> 60, because that value also sets the delay after **F9** re-arms the
dump. The bug has to be caught while a specific bad head is on screen, so the aimed dump needs to
land about a second after the key, not 45 seconds.

### The aimed dump, and a reading of mine that was wrong

F9 dump at frame 3786, 367 skinned draws carrying the new detail. Heads are identifiable at last:
**`ps='Blend_MapSampler'`, own vertex buffer, ~1426 vertices, 3-5 bones, bind box ~0.19x0.30x0.23
centred at y~1.6-1.68** - a head-sized box at head height. Bodies are `Diffuse_MapSampler` /
`IR_GBuffer_DSF_DataSampler`, ~1.23x1.79x0.34, 47-58 bones.

**RETRACTED.** I first read the dump as "37 of 63 head draws have a body-sized bind pose, so they
are reading another mesh's geometry" - the wrong-mesh hypothesis, apparently confirmed. It is an
artifact of my own instrument. `bind` measures the draw's whole vertex WINDOW, not the triangles
it draws, and **45 of 72 windows serve more than one draw**: one window of 7977 vertices serves 36
draws of a single character (body, head, decals, all sub-ranges picked by StartIndex). For those
the box describes the character, not the draw, and says nothing about either.

Restricted to windows that serve exactly one draw shape - where the window IS the mesh - 51 draws
survive, 34 of them head-sized. Only two characters have both a clean head window and a clean body
window, and in both the head moved much further from its bind pose than the body did (+1.58 and
+0.51). Suggestive, n=2, not a finding.

**This is the third time on this project that a probe firing has been mistaken for a defect. The
discipline that catches it is asking what the number is actually measuring before believing it.**

### Eliminated this session

| hypothesis | how it died |
|---|---|
| `skinRequireBoneDecl` leaves heads unskinned | `NO BONE DECL` names only `Grime_MapSampler` and one 7-vertex decal |
| weight/index components crossed | `mova a0, r1.yxzw` is compensated by the `mul`/`mad` order; weight[k] pairs with index[k] |
| head bones exceed the palette | `kBonesMax=64`, highest referenced 62 |
| head vertices fall through to bind pose | `vertices left in bind pose` is 0.0 before the gate exists and tracks it exactly after |
| VB pointer reuse collides in the cache | `BaseMesh::owner` AddRefs; closed 2026-08-18 |
| **stale bind pose from a refilled buffer** | `Hook_VBLock` demonstrably fires (it counts 2042 instance discards) and `bind-pose cache invalidated by a game write: 0`. The game never rewrites those buffers, so the cache is valid |
| weights decoded at the wrong scale | declared `ubyte4` (raw 0..255, e.g. `[46 180 28 0]`); the shader divides by the weight sum and so do we, so scale cannot matter |

Skinned declaration, now recorded properly (two variants, identical in the fields that matter):

    pos float3@0 | normal ubyte4n@12 | tangent ubyte4n@16
    BLENDWEIGHT ubyte4@20 | BLENDINDICES ubyte4@24 | texcoord short2@28 [+ short2@32]
    stride 32 (one texcoord) or 36 (two)

Unused influences are `index 255, weight 0`; the shader multiplies `c52[3*255]` by zero and we skip
it. Same answer.

### The last unverified link, now instrumented

Everything in the chain has been checked against ground truth except one thing: **whether a head
draw reads the head's palette.** The constants match the device (189,458 draws, 0 mismatches), the
bind pose is valid, the arithmetic matches the disassembly - so if the GPU gets it right from the
same inputs, the remaining difference has to be *which* palette is live.

A head is 3-5 bones. The body beside it is 47-58. **Both index the same c52, and every upload in
this game starts at bone 0.** So a head drawn without an upload of its own reads the body's bones
0..4 - the pelvis and spine. A head posed by the pelvis follows the body's core animation and parts
company with the neck, which is the reported symptom stated exactly.

The dump line now carries, for the bones each mesh actually uses:

    pal=<bones in newest upload>@d<draw> .. <bones in oldest>@d<draw> gens=<distinct objects>

| reading | meaning |
|---|---|
| head with `pal=5@dN..5@dN gens=1` | its own upload - hypothesis dead |
| **head with `pal=47@d...` or `gens>1`** | **reading the body's palette - confirmed, and the fix follows** |

Supporting signal already in the log: `MIXED-PALETTE` reports a draw reading *8 bones at draw 4388
(newest) and 55 bones up to 5 draws older*, `1.2 MIXED palettes/frame`, `worst spread 94 draws`,
and `SKIN DISPLACEMENT ... worst 8.1 units`.

### The doubled head, found: a prepass into a target that cannot hold colour

Aimed F9 dump, NPC standing ~5 units in front of the player at `(79.1 145.8 61.7)`.

**The palette hypothesis is dead, and cleanly.** Every head-sized mesh reads its OWN upload:
a 5-bone mesh reads `pal=5@d315 .. 5@d315 gens=1`, a 4-bone mesh reads `4@d1461`, and in every
case the upload is the immediately preceding draw, one object generation. Heads are not being
posed by anybody else's skeleton.

**What the dump showed instead.** The frame has two 2560x1440 render targets carrying character
geometry:

    === RENDER TARGET 14F28B68  2560x1440 fmt=34  ===   D3DFMT_G16R16      463 SKIP, 4 CONVERT
    === RENDER TARGET 14F2E3F8  2560x1440 fmt=113 ===   A16B16G16R16F      the material pass

The four survivors in the G16R16 target:

     290 CONVERT v=7977 ps='Diffuse_MapSampler' rank=100 at(79.1 145.8 61.7) vb=4A4EDBF0
     316 CONVERT v=1426 ps='Blend_MapSampler'   rank=70  at(78.2 145.7 62.9) vb=3B574948

and the same meshes again, later, in the HDR material pass:

    1461 CONVERT v=7977 ps='Blend_MapSampler'   rank=100 at(79.1 145.8 61.7) vb=4A4EDBF0
    1486 CONVERT v=1426 ps='Blend_MapSampler'   rank=100 at(78.2 145.7 62.9) vb=3B574948

**Same vertex buffer, same objTM, both converted.** Remix path-traces every character twice.

### Why every existing rule let these through

The prepass tests are all about SAMPLERS: `shader samples nothing`, `stipple with no colour`,
`only normal/stipple/depth samplers`. These four draws carry `Diffuse_MapSampler` at rank 100 -
a real colour texture, top of the albedo ranking - so every sampler-based test passes them.

**D3DFMT_G16R16 has two channels. No blue, no alpha. Nothing can render an albedo into it.**
The pass is a prepass by the shape of its destination, and no amount of looking at the pixel
shader could say so. 463 of its 467 draws were already being skipped for a different reason,
which is why this never showed up as a target-level anomaly - only 4 leaked, and 4 duplicated
characters is exactly what "sometimes two heads on one character" looks like.

### The fix

`FormatColourChannels()` plus `skipNonColourTargets=1`: a draw into a target with fewer than
three colour channels takes the same `HiddenDisp` path as the other prepasses. Channels are
measured once per **target change** in `Hook_SetRenderTarget`, next to the existing UI test -
never per draw, because `GetDesc` is a bridge round trip.

Unknown formats answer 4. This rule may only ever REMOVE a draw, so anything it does not
recognise has to fall on the side of leaving it alone.

Skipping rather than hiding is safe on this game's own terms: 463 siblings already are, and the
depth readback the engine genuinely reads is answered by the occlusion-query hook.

    non-colour render target (fewer than 3 channels...): N draws/frame hidden

If N is 0 nothing took this path and it fixed nothing. The run log reported `converted -> target
14F28B68 fmt=34 : 12 draws/frame` before the fix, so N should land near 12.

**What this predicts:** the doubling and the z-fighting go. Whether it also fixes the misplaced
and mis-textured heads depends on whether those were ever separate faults - two coincident copies
of a head, posed from two different palettes taken ~1200 draws apart in the frame, would read as
one head sitting wrong and wearing the wrong texture. That is a prediction, not a claim.

### The head fix worked. The rule that delivered it was wrong anyway.

User: *"heads did not swap and there was no head zfighting"* - the doubling is gone.

And: *"the game now seems to stop submitting everything once looking at the horizon or down.
only renders when looking up."* That was mine.

    non-colour render target (fewer than 3 channels...): 707.8 draws/frame hidden
    FFP converted 214/frame (11.8%)        <- was 653/frame (30.1%)

I predicted ~12 draws/frame. It hid **707**. The frame dump says where they went:

    14EB50D0 2560x1440 fmt=34   2181 draws  - 2180 of them hidden by this rule
    14EBDB98 2560x1440 fmt=113  1463 draws

**That G16R16 surface is not a prepass target. It receives 2,181 draws - the world.** SR3 is a
deferred renderer and binds MULTIPLE render targets: the 2-channel normal/depth buffer goes to
slot 0 and a colour target alongside it. So the question I asked - *can slot 0 hold colour?* - is
not the question I claimed to be asking, which was *does this draw write colour?* Those differ
exactly on the G-buffer pass, i.e. on all the world geometry. Looking up at the sky kept working
because there is almost nothing there to lose.

### What was actually wrong with the reasoning

The principle stands: *nothing can render an albedo into a two-channel surface.* What does not
follow is that the DRAW writes no colour, because the draw is not writing to one surface. I took a
true statement about a surface and applied it to a draw, and never checked whether this game binds
more than one target - in a renderer whose G-buffer is the reason that surface exists at all.

The evidence was there before I shipped: the dump groups draws by slot 0 only, and the fmt=34
group held 467 draws in one frame and 2,181 in another. **467 draws is not a prepass either.** I
read '463 SKIP, 4 CONVERT' as "the rules already handle this target" instead of asking why a
depth prepass would contain the whole visible world.

### Corrected

`g_rtChannels[4]` / `g_rtBound[4]`, filled for every slot in `Hook_SetRenderTarget`, and
`AnyColourTarget()`. The rule now fires only when **no bound slot** can hold colour. Both
questions are answered from ONE `GetDesc` per target change - the previous edit had added a
second, on the hook this file itself names as a hitch candidate.

New counter, so the next run distinguishes the two populations instead of leaving it to me:

    non-colour render target ...: N draws/frame hidden
    of which: M draws/frame had a non-colour target at slot 0 but a COLOUR target elsewhere

N should be ~12 (the duplicate character prepass). M is the MRT G-buffer pass, which must never be
hidden, and was ~700/frame when the rule was wrong.

### The lesson, which is the same one as last time

The car-part fix was verified across all 1,693 shaders before shipping, and it was right first
time. This one was shipped on a single frame's dump and a principle that sounded clean. **The
sweep is what makes the difference, not the quality of the argument.** The equivalent check here
was one line - how many draws go to this target, and does that number look like a prepass - and
the answer, 467, was already on screen.

### The corrected rule restored the world - and gave the heads back

    non-colour render target ...: 0.4 draws/frame hidden
    of which: 653.4 draws/frame had a non-colour target at slot 0 but a COLOUR target elsewhere
    converted -> target 14DE5260 fmt=34 : 11 draws/frame

World renders again (653 draws/frame correctly spared), and the rule now hides almost nothing -
**so the previous build fixed the heads only by destroying the G-buffer pass wholesale.** The 11
character draws still converting into the fmt=34 pass have a colour target bound alongside, so
the render target cannot separate them from world geometry. Render target was the wrong axis.

### The right axis: the same draw, submitted twice

Two aimed dumps, strict identity - same vertex count, triangle count, shader, bound texture AND
position:

| dump | converted | identical groups | redundant copies |
|---|---|---|---|
| frame-1 | 993 | 53 | **94** |
| frame-2 | 938 | 48 | **83** |

Byte for byte, from the dump:

       d7  CONVERT v=17601 p=494 zw=1 zt=1 blend=1 cw=0xf ps='Diffuse_MapSampler' tex0=48097178
     d3296 CONVERT v=17601 p=494 zw=1 zt=1 blend=1 cw=0xf ps='Diffuse_MapSampler' tex0=48097178

Every field the dump records is equal. The rasteriser resolves those into one surface; a path
tracer gets two, in the same place.

**Only 4 of the 94 were skinned.** `dedupSkinned` existed, its key was already correct and
complete - buffer, vertex range, INDEX range, triangle count, pose, objTM, bound albedo, built
over four previous failures - and it was gated to skinned draws and switched OFF. The mechanism
to fix this has been in the codebase the whole time, aimed at 4% of the problem and disabled.

That the key separates real sub-ranges is checkable in the same data:

     d916 v=7977 p=309 tex=162B8288  at(75.7 145.7 51.2) vb=44D83928
    d4271 v=7977 p=739 tex=3B982D60  at(75.7 145.7 51.2) vb=44D83928

Same mesh, same character, same position - different triangle count and different texture, so not
merged. That is the distinction four earlier versions of this key got wrong.

### What changed

`dedupAll=1` extends the identical test to rigid draws, and `dedupSkinned` is back ON. Two
additions the generalisation required:

**The instance transform is folded in.** An instanced draw's objTM is identical across the whole
batch - every one reports `at(0.0 0.0 -1024.0)` - so without it the key cannot tell two instances
of a street prop apart and would delete one. `InstanceWorld` is a cache lookup and never locks.

**If an instanced draw's placement cannot be read, the dedup declines to judge it.** A missed
duplicate is a second surface; a wrong merge is a missing object. Those are not symmetric, so the
unreadable case refuses rather than guesses.

`kMaxSkinFrameKeys` 512 -> 2048: it was sized for ~94 skinned draws a frame and now has to hold
~1,000 converted ones. At 512 the table would have filled part way through every frame and every
later duplicate would have gone unnoticed **while the counter still reported successes**. That
failure is now counted:

    DEDUP: N draws/frame hidden ... (K keys held, M/frame did not fit - non-zero means MISSED)
    DEDUP declined on P draws/frame: instanced, and their placement could not be read

### On the 94

The dump does not record `startIndex`, so the offline key is WEAKER than the shipped one - two
draws differing only in index range look identical to my analysis and will be kept apart by the
real key. **94 is therefore an upper bound on what will be hidden, not a prediction.** The error
is in the safe direction, which is the point.

### The heads: SR3 morphs them, and we were reading only the base mesh

Dedup landed - `DEDUP: 12.6 draws/frame hidden`, table never overflowed, so most of my offline 94
differed in index range and were correctly kept apart. It did not fix the heads, because the
character's two passes carry different materials and are not exact duplicates.

The clue that cracked it was the user's: **"heads are also slightly placed lower than they should
be."** A small, consistent positional offset is not a palette fault or a cache fault. It is a
missing term.

Sweeping every vertex shader for inputs the shim ignores turned up `dcl_position1` - a SECOND
position stream - in the `_ms` and `_mc` variants. **`m` is for morph.** Disassembling
`ir_sr3npcskinfull_mc`:

    def c0, 0.000122070313, 2, -1, 3          c0.x = 1/8192
    dcl_position v0 / dcl_position1 v4 / dcl_normal v2 / dcl_normal1 v5
    mov r1.xyz, v4
    mad r1.xyz, r1, c0.x, v0        <- position = v0 + v4/8192, BEFORE the bone blend
    dp4 r2.x, r2, r1                <- then the bones, then objTM

**SR3's character customisation is a base mesh plus a per-character delta in stream 2**, declared
`POSITION1` as `short4` with `NORMAL1` as `ubyte4n` beside it - both present in the vertex
declaration we dumped days ago and never read. SHORT4 is not normalised, so the components arrive
as raw integers and 1/8192 puts the delta in world units.

Reading only `v0` renders every NPC with the SAME base head. That is, in one cause:

| symptom | why |
|---|---|
| wrong heads | the head is the base head, not that character's |
| heads change | each NPC's own delta is what is missing, so each is wrong differently |
| slightly too low | the delta is a position offset and it is not being added |
| not textured properly | the UVs were authored for the morphed surface, not the base one |

### The fix

`ShaderInfo::usesMorph` from the dcl stream (POSITION, usage index 1, input register), and the
delta applied in `GetBaseMesh` before the bone blend, where the shader applies it. `NORMAL1`
replaces `NORMAL0` on those draws - lighting the morphed surface by the shape it no longer has
would be its own bug.

**The morph stream is part of the mesh's IDENTITY, so it is folded into the bind-pose cache key.**
Two NPCs share one base head buffer and differ only in their deltas; a key built from stream 0
alone would serve the first NPC's decoded face to every one after it - which is "heads change"
all over again, introduced by the fix for it.

### Swept before shipping

| shaders declaring dcl_position1 | reads blendindices | defines 1/8192 | count |
|---|---|---|---|
| morph, skinned | yes | **yes** | **384** |
| morph, rigid | no | yes | 330 |
| `rl_particle_*` | no | no | 52 |

**Every skinned shader that reads the morph stream uses the 1/8192 scale - 384 of 384, zero
exceptions.** The 52 without it are all particle shaders, which are not skinned and never reach
`GetBaseMesh`. The morph is additionally skipped unless the declaration says SHORT4, so an
unexpected format falls back to the base mesh rather than being misdecoded, and a morph stream
that cannot be read is counted rather than refused - the fallback is exactly today's behaviour.

    MORPH: N draws/frame use a shader that reads the morph stream | K meshes decoded WITH their
           morph delta, U could not be read

N non-zero with K zero means every one is still rendering the base mesh. `applyMorph=0` reverts.

### Note

This is the second bug today whose cause was an input the shim never read, found by asking what
the shader declares that we do not. The first was `dcl_blendindices` (car parts). Both were
invisible to every runtime probe because the shim cannot measure what it does not know exists.

### The morph never fired

    MORPH: 33.9 draws/frame use a shader that reads the morph stream
         | 0 meshes decoded WITH their morph delta, 12 could not be read

The shader detection works - 33.9 of ~70 skinned draws a frame use a `_mc`/`_ms` variant. **Every**
attempt to read the morph stream failed, so nothing on screen changed and the heads are still the
base mesh. The falsifier written into the ini caught this exactly as intended: `N` non-zero with
`K` zero means the fix never ran.

`g_morphUnread` was one counter for five different causes - stride, GetDesc, DYNAMIC, range, lock -
which is the same mistake as counting "draws hidden" without recording which ones. Split now, and
the first eight failures log their numbers:

    MORPH UNREAD #N: <cause> | ps='...' verts=N | stream 2 vb=P stride=S offset=O size=B
                     | first=F posType=T@X nrmType=T@X
    MORPH UNREAD by cause: A stride, B desc, C DYNAMIC, D out of range, E lock

The sampler is in there because one thing is still unproven: that HEAD draws use the morph variant
at all. 33.9 draws a frame do; whether the heads are among them is an assumption until this line
names them.

**The disassembly is not in doubt** - `mad r1.xyz, v4, c0.x, v0` is what the `_mc` shaders
compute, and 384 of 384 skinned morph shaders define the 1/8192 scale. What is in doubt is whether
the shim can reach that data, and whether the meshes that need it are the ones that look wrong.

### Why it never fired, and the two things that answered at once

    MORPH UNREAD by cause: 0 stride, 0 desc, 10 DYNAMIC, 0 out of range, 0 lock
    MORPH UNREAD #2: buffer is DYNAMIC | ps='Blend_MapSampler' verts=1426 | stream 2
                     vb=037C13D8 stride=12 offset=95724 size=5242880 | posType=7@0 nrmType=8@8

**The morph buffer is DYNAMIC**, and the decoder refuses to read-lock DYNAMIC buffers - correctly,
since that is write-combined memory behind the 32->64-bit bridge. Ten of ten failures, one cause,
no ambiguity.

And the same line settled the assumption I had flagged as unproven: **`ps='Blend_MapSampler'
verts=1426` is the head.** Heads do use the morph variant.

The buffer's shape is now known exactly: ONE 5 MB dynamic buffer, stride 12, one contiguous block
per character, and the STREAM OFFSET selects the character - 0, 17112, 34224, 95724, 112836,
129948, where 17112 = 1426 verts x 12. POSITION1 is SHORT4 at 0, NORMAL1 is UBYTE4N at 8.

### Rebuilt on the machinery that already existed for this

SR3's instance streams are dynamic too, and this shim has snooped them since 2026-08-16:
registering a buffer with `InstanceBufferData` makes `Hook_VBLock`/`Hook_VBUnlock` copy the game's
own writes, and a `fresh` map records which bytes have actually been written since the last
discard. The morph now uses the same path. No lock, no bridge traffic, and the freshness test
comes free - left-over bytes from a previous occupant of an offset would morph a face by a
stranger's data, which is the failure this whole session has been chasing in other forms.

**The morph is applied per DRAW now, not baked into the bind pose.** The first version put it in
`GetBaseMesh` and folded the morph buffer into the cache key. That was wrong in a way worth
recording: the bind pose is cached because the base mesh is static, while the morph lives in a
buffer the game rewrites - so baking it in would have required invalidating the cache on every
write to a per-frame buffer, and served one NPC's face to the next the moment it went stale. The
base mesh stays the base mesh; the delta is added in the skinning loop where the shader adds it.

    MORPH APPLIED: N draws/frame skinned WITH the delta | U draws found no snooped buffer,
                   S stride refusals, V vertices/frame fell back (not written yet)

N is the number that matters. U should fall to near zero after the first sight of each character -
a snooped buffer holds nothing until the game next fills it.

## 2026-08-28 - the morph landed, and took two bugs with it

    MORPH APPLIED: 30.6 draws/frame skinned WITH the delta | 37 draws found no snooped buffer,
                   0 stride refusals, 0 vertices/frame fell back

User: *"the heads are in the right place"*. The position delta is correct and the snoop reaches
every draw that needs it. Two consequences followed.

### 1. The normal is a DELTA too, and I substituted it

    mad r0.yzw, v5.xxyz, c0.y, c0.z     n_morph = v5*2 - 1
    mad r1,     v2,      c0.y, c0.z     n_base  = v2*2 - 1
    mad r0.yzw, r0,      c0.y, r1.xxyz  n = n_base + 2*n_morph
    nrm r1.xyz, r0.yzww

`NORMAL1` does not replace `NORMAL0`. It is added, doubled, exactly as the position delta is
added - and I read the first two instructions, saw `v5` expanded, and assumed replacement without
reading the third. The result lights every morphed surface by a vector the game never computes:
"something looks weird with the normals", on heads and on player skin.

Now `n_base + 2*n_morph`, normalised before the blend as the shader does - with four bones pulling
in different directions, normalising before and after are not the same vector.

### 2. The crash was mine, and the project could not read its own crash dumps

The shim writes `sr3-rtx-crash.dmp` and has since long before this session. Nothing could turn an
address in it into a line of source, because **the build produced no map and no symbols**. That is
now fixed in `build.ps1` (`/MAP`), the map is deployed beside the .asi, and the first thing it did
was resolve this:

    exception 0xc0000005 at 0x74b598ee  params=['0x0', '0x0']
    eip=0x74b598ee  eax=0x1050 ecx=0x1050 esi=0x00000000 edi=0x44252040
    -> sr3-rtx.asi + 0x198ee = CopyUpLargeMov+0xa   (the CRT's memcpy)
    -> memcpy(dst=0x44252040, src=NULL, count=4176)

**4176 = 348 vertices x the 12-byte morph stride.** The faulting call is the snoop:

    if (!g_internal && g_pendingLock.vb == self && g_pendingLock.ptr) {
        ...
        if (c.data.size() < end) c.data.resize(end);          // opaque to the optimiser
        memcpy(c.data.data() + ..., g_pendingLock.ptr, ...);  // RE-READS the global

The pointer is tested, then `resize` forces the compiler to re-read it from the global before the
memcpy. `Hook_VBUnlock` is not called from one thread. Another thread running `g_pendingLock = {}`
inside that window - which sets `ptr` to null - is a null source of exactly this shape.

**The window had always been there and had never mattered.** Only instance streams were snooped,
and the game fills those on the render thread. Registering the morph buffer put a stream the game
writes elsewhere through the same single global slot. The bug was latent; the change made it
reachable.

Fixed by snapshotting the pending lock into locals and clearing the global before any other call,
so nothing is read from it after the null test.

### Method note

Three of today's findings came from reading the shader properly and one from failing to: I stopped
at `mad r0.yzw, v5.xxyz, c0.y, c0.z`, which looked like a complete expansion, and did not read the
line that consumed it. **The instruction that uses a value says what it means; the one that
produces it does not.**

And a crash handler that writes dumps nobody can read is not a crash handler. `/MAP` costs nothing
and turned "the game crashes at random" into a register dump and a line, in one pass.

### Crash pass: the game locks vertex buffers from more than one thread

User asked for the car-part mechanisms to be audited for crash risk. The timeline says those are
not where the crashes started:

| dumps | vs. the car-part fix (08-26 14:10) |
|---|---|
| 08-25 18:31, 18:34, 23:30 x2 | **before it** |
| 08-27 12:57 x2, 22:47 | after |
| 08-28 00:57, 00:59 | after |

Four crashes predate that patch. What the 08-28 dump proves, though, is the thing that explains
all of them.

**The game locks vertex buffers from more than one thread.** That is not an inference from
timing: `memcpy(dst, NULL, 4176)` in `Hook_VBUnlock` requires the source pointer to become null
between the null test and the `memcpy` two lines later, and single-threaded it cannot. Everything
below follows from that, and had been unguarded since the snoop was written.

### Three races, all on state the render thread reads

**1. Deferred invalidation.** `Hook_VBLock` called `InvalidateBaseMeshes`, which erases from
`g_baseMeshes` and calls `Release()` on the buffers those entries pin - while the render thread
holds a `const BaseMesh*` into that same map for the whole length of a skinning loop. An erase
from the other thread frees the vertices out from under it. Lock requests are now queued and
drained on the render thread at the top of the next draw, before anything takes such a pointer.

**2. Snoop buffer pointer stability.** `InstanceBufferData` hands the render thread
`c.data.data()`, and the morph read walks it vertex by vertex for the length of a draw. The
unlock path could `resize()` that vector from another thread and leave the pointer dangling. The
vector is now sized ONCE to the buffer's full declared size and never grown; the unlock path
clamps instead of resizing. A use-after-free becomes at worst a torn read of bytes the `fresh`
map already guards.

**3. Map access under a lock.** `g_instCache` is inserted into by the render thread and read and
written by the locking thread. Both sides now take one critical section, entered on buffer
lock/unlock and once per instanced or morphed draw - not per draw in general.

### Two out-of-bounds reads found by inspection in the same pass

**The bone palette had no upper bound.** `boneReg` comes from the shader's constant table as a
raw 16-bit register number, and `palette + bone * 12` indexes from it into a 1024-float array.
The palette is 64 bones x 3 registers, so any register above c64 reads past the end. Every shader
in this game declares it at c52 and the report has always said `0.0 draws/frame ELSEWHERE` -
which is precisely why nothing caught it. Now bounded by what actually fits, with a named report
if it ever binds.

**The morph offset arithmetic was 32-bit.** `base + i*stride` against a UINT size: a wrapped
value turns an out-of-range offset into one that passes the bounds test. Now 64-bit throughout,
and a base beyond the buffer refuses the morph rather than reading.

Also capped the comment-block skip in the dcl walk against `end` - a corrupt length would have
walked the pointer far past the buffer before the loop condition noticed.

### Note on what is proven and what is not

Race 1 and the palette bound have never been observed firing (`invalidations: 0`, `ELSEWHERE:
0.0`). They are hardening, not fixes for a diagnosed fault. The only crash actually diagnosed is
the null-source memcpy, and that one is fixed. The right way to tell whether the rest mattered is
a run that tries to crash - which is what the user offered.

    VB LOCK HOOK: N invalidations deferred to the render thread

Non-zero means the queue is doing work that used to happen inline on the wrong thread.

### The freeze and the Runtime Error were the crash fix, not the crash

The build that shipped the thread-safety work also pre-sized every snooped buffer to its full
DECLARED size - `data` and the parallel `fresh` map, so twice over - to make `data()` stable for
a reader holding it across a draw. Measured cost of that idea: **2,390 instanced draws a frame**
across many buffers, each now reserving its whole declared size instead of growing to the few
hundred KB the game actually writes. In a 32-bit address space shared with the game, Remix's
client and a 24 MB skinning ring.

The machine paged, then:

    Microsoft Visual C++ Runtime Library
    This application has requested the Runtime to terminate it in an unusual way.

That message is `std::terminate`, not an access violation - an **unhandled `std::bad_alloc`**
out of `vector::assign`. Which is its own finding: **a shim must degrade, not abort the process
it is a guest in.** Refusing a mesh costs one pass-through draw; throwing costs the session.

### What replaced it

Pointer stability was the wrong solution to the right problem. Readers now **copy out** what they
need under the lock (`SnoopCopy`) instead of borrowing a pointer into a vector another thread can
resize. A draw's morph slice is `numVertices x 12` - 17 KB for a head, 96 KB for a body - into
one reused buffer, so the allocation happens once and never again. Instance transforms copy 16
bytes per row. Memory is back to grow-to-what-was-written, plus:

- a **96 MB ceiling** on everything the snoop holds, with refusals counted;
- `try`/`catch` on every growth path, so an allocation failure refuses a draw instead of killing
  the game;
- the same guard on `GetBaseMesh`'s bind-pose allocation, the largest in the skinned path.

### A silent break, caught before shipping

Removing the accessor that handed out pointers also removed the thing that REGISTERED instance
buffers for snooping - registration was a side effect of the getter. Nothing would have failed
loudly; ~1,400 instanced draws a frame would simply have stopped converting. Now an explicit
`RegisterSnoop` call, named for what it does.

    VB LOCK HOOK: N invalidations deferred | snoop holds X MB of 96 MB, R writes refused on
                  budget, F allocations failed

R or F non-zero means the ceiling is doing work and the numbers say how much headroom is left.

### Method note

Two builds in a row shipped a fix whose cost I had not measured: hiding draws by render-target
slot without asking how many draws that target receives, and reserving buffer memory without
asking how many buffers there are. **The answer was one grep away both times** - `2,390 instanced
draws/frame` was already in the report.

### Crash hunt: 26,400 frames, no crash

User flew, drove fast, blew up cars, played a long session. Nothing crashed. The safety nets say
why they did not have to:

    VB LOCK HOOK: 7,185,052 invalidations deferred | snoop holds 2.7 MB of 96 MB,
                  0 writes refused on budget, 0 allocations failed
    instancing: 1,234 converted from the instance stream/frame, 0 refused/frame
    MORPH APPLIED: 34.7 draws/frame skinned WITH the delta

Memory is 2.7 MB against a 96 MB ceiling - the freeze build had been reserving whole declared
buffer sizes to reach the same end. Instancing is intact, which is the check that mattered after
`RegisterSnoop` replaced the accessor that used to register buffers as a side effect.

**7.2 million deferred invalidations - 272 a frame - is also the proof, at scale, that the game
locks vertex buffers off the render thread.** That was inferred from one null pointer in one
crash dump; it is now a measured rate.

### Three things the run found

**1. Every one of those 7.2 million invalidations was wasted.** `bind-pose cache invalidated by a
game write: 0 times`, and the UV buffers report `0 invalidated by the game` too. The game relocks
a handful of dynamic buffers constantly and we hold nothing for any of them. Now coalesced: the
same pointer queued twice between drains is one entry, and the scan stays short precisely because
of that.

**2. The dedup key table was overflowing.** `13.7/frame did not fit - non-zero means duplicates
were MISSED`. 2048 was sized for ~1,000 converted draws a frame; this scene runs 1,288 converted
out of 3,915. Raised to 8192. **The falsifier written into that line when the table was built is
what caught it** - without it the DEDUP counter would have gone on reporting successes while
silently missing everything past the cap.

**3. A counter that lied.** `MORPH: ... 0 meshes decoded WITH their morph delta` has read a
permanent 0 since the morph moved out of `GetBaseMesh` into the per-draw path. It was measuring a
variable nothing increments any more. Removed - `MORPH APPLIED` is the live one. A counter that
lies is worse than no counter, and this project has now been burned by that twice in two days.

### The largest open problem is no longer correctness

    TIMING: frame 50.9 ms avg (20 fps), worst 701 ms | shim 32.01 ms avg (62.9% of frame)
    draws 3,915/frame, FFP converted 1,288/frame, 170 skinned/frame, 126,372 skinned verts/frame
    SKINNING: 1,434 meshes decoded, 410 cached, 1 cache flushes
    SKIN RING: 24 MB, high water 24.00 MB, 0.33 wraps/frame

**The shim is 63% of frame time at 20 fps.** Some of that is a heavier scene than any measured
before - draws are double and converted draws 2.6x what earlier runs showed - but it is
superlinear, and two numbers point at where to look: the bind-pose cache has decoded 1,434 meshes
to hold 410, having flushed once, so it is re-decoding meshes it evicted; and each decode is a
read-lock across the 32->64-bit bridge. The ring is saturated again at 24 MB.

Correctness work outstanding: the head z-fighting, and whether the normal delta fix landed.

### Heads: geometry is fixed, and the texture is a specular mask

User: *"correct head, properly placed, but not the correct texture"*, plus wrong texturing on
some body parts and no way to tell whether the tint is wrong on the head, the body or both.
That is a material fault, not a transform one - the morph work is done.

### The head is not drawn by the shader I had been reading

Head draws report `ps='Blend_MapSampler'`, which never matched `ir_sr3npcskinfull_c` - that
declares `Diffuse_Map` at rank 100. Ranking per SHADER INDEX rather than per file (a .fxo_pc
holds ten) finds it: **`ir_sr3npcskinfull_mc` shader[6] and [7]**, whose only colour-ranked
sampler is `Blend_Map`.

    shader[6]  texld_pp r0, v4.zwzw, s1        Normal_Map
               mad_pp  oC0.xy, r0, c1.x, c1.x  <- writes the NORMAL
               texld_pp r1, v4, s4             Blend_Map
               mad_pp  oC2.z, r1.x, r0.x, c12.x  <- ONE scalar, into a fresnel term
               ... oC1, oC2 data

    shader[8]  mul_pp  oC0, r2, c37            <- Tint_color; the MATERIAL pass
               samples Diffuse_Map, Sphere_Map_1/2, IR_LBuffer

**Shader[6] outputs no colour at all.** It is the deferred G-buffer pass. `Blend_Map` is a
specular mask, and the albedo ranker scores it 70 and hands it to Remix as the head's base
colour. A correctly placed head wearing a specular mask is exactly what was reported.

### The same is true of the world, in a way that is easy to misread

    ir_at_bbsimple1_c[6]   texld_pp r0, v4, s0        <- samples Diffuse_Map
                           texld_pp r0, v4.zwzw, s1   <- OVERWRITES r0 with Normal_Map
                           mad_pp  oC0.xy, r0, ...    <- writes the NORMAL

The diffuse load is DEAD - overwritten before use. So "this shader samples a colour map" does not
mean it produces colour, and any rule built on the sampler list alone will get this wrong. Swept:
**1,343 pixel shaders write three render targets, 1,880 write one**, and 196 of the three-target
ones sample a map the ranker would score.

### Measured, not acted on

A rule that hides the G-buffer pass would be the third attempt at removing SR3's duplicate
geometry. The first keyed on render-target slot 0 and hid the whole city; the second (dedup)
caught only 12 draws a frame because the two passes are not identical. **Both shipped without
asking how large the population was.** So this build only counts it:

    DEFERRED G-BUFFER PASS (pixel shader writes >1 render target, so it outputs no colour):
    N draws/frame, of which M/frame are CONVERTED | single-target draws converted S/frame
    | their albedo samplers: ...

`M` is what a rule would remove and `S` is what would remain. If S is large and M small, hiding
the G-buffer pass is safe and likely fixes the wrong texture AND the surviving z-fighting - two
coincident copies of every object is what a deferred renderer looks like to a path tracer. If M
is most of the converted geometry, the material pass is not being converted and hiding this would
repeat the first failure exactly.

The sampler list in that line says which materials are affected, which is what decides whether
the tint question is about heads, bodies, or the whole G-buffer population.

### The measurement, and the rule it justified

    DEFERRED G-BUFFER PASS (writes >1 render target, so outputs no colour): 399.7 draws/frame,
    of which 8.0/frame are CONVERTED | single-target draws converted 271.4/frame
    | their albedo samplers: Diffuse_MapSampler, Diffuse_mapSampler, Blend_MapSampler

**M = 8, S = 271.** Existing rules already hide 392 of the 400; eight leak through, and they
match `converted -> target ... fmt=34 : 8 draws/frame` exactly - the same draws, found by two
independent routes. `Blend_MapSampler` in that sampler list is the heads.

So every affected object gets a correct material copy AND a specular-mask copy on top of it.
One cause, two symptoms: the wrong texture, and the z-fighting that survived the dedup work.

Compare with the first attempt at this, which keyed on render-target slot 0: it hid **707
draws/frame** and took the city with it. Same intent, same target population in the frame dump,
and a factor of 88 between them - which is the whole argument for counting first.

`skipDeferredGBuffer=1` shipped. Falsifier:

    ...of which H draws/frame HIDDEN by skipDeferredGBuffer (gate ON)

H should land near 8. Near 0 means nothing took the path; the world darkening or objects
vanishing means S was not the material pass and the rule is wrong.

### What this does not address

The tint question is still open in one respect: `mul oC0, r2, c37` is `Tint_color`, applied in
the material pass. Whether the shim reproduces that constant correctly for heads and for bodies
is a separate question from which pass gets converted, and it is the next thing to read if the
colour is still wrong once the duplicate is gone.

### The rule was right. Hiding was the wrong verb, and it was the second time.

Z-fighting gone, heads stopped changing - the diagnosis held. Then the world went black except
looking up, exactly as it had on 2026-08-27.

    ...of which 693.2 draws/frame HIDDEN by skipDeferredGBuffer (gate ON)
    SKIPPED entirely 1418/frame        (was 1205)
    FFP converted 194/frame (11.8%)    (was 285, 22.7%)

**I predicted 8 draws a frame and hid 693.** The probe measured how many G-buffer draws were
CONVERTED (8) - it did not measure how many EXIST (400-693). Putting the test at the top of
Classify applied it to all of them, and `HiddenDisp` with `hiddenPassMode=2` means SKIP.

SR3's material pass reads that buffer back through `IR_GBuffer_DSF_DataSampler` and
`IR_LBufferSampler`. Skipping the pass that fills it leaves the deferred lighting sampling
nothing - a black world, except where nothing deferred is on screen, which is the sky.

**The rule I broke is written in this file, directly above `hiddenPassMode`:** *a draw whose
RESULT the engine reads can never be skipped, only hidden.* It is the same rule that took four
sessions to learn when vertex capture was the blocker, and I had re-read it this week.

### The fix is placement, not the test

The test moved to the END of Classify and returns `Disp::PassThrough`:

- every earlier rule has already had its say, so draws they skip are still skipped - no change
  in submission cost, no change in what the engine gets;
- only the handful that would otherwise have been CONVERTED reach it;
- pass-through submits them to the device exactly as before, so the engine's own buffers are
  untouched, and with vertex capture off Remix never sees them.

That is what "do not convert this" should always have meant here. Hiding and skipping are about
what REMIX sees; the engine's own passes are not ours to remove.

    ...of which H draws/frame passed through instead of converted

H near 8 is right. Hundreds means it is running too early again.

### Method note

Twice now the same shape: a correct diagnosis, a correct test, and a disposition that took the
game's own rendering with it. The counter that would have caught it before shipping is not "how
many draws would this rule affect" but **"how many draws does this rule change the disposition
OF, and from what to what"** - which is a different question and the one I keep not asking.

### The pass-through fix worked exactly as designed, and the world was still black

    ...of which 9.2 draws/frame passed through instead of converted (gate ON)
    SKIPPED entirely 971/frame        (back to normal from 1418)

H = 9.2, precisely the 8 predicted. Skips back to normal. The disposition fix was correct - and
the world was still black, because **the premise underneath it was wrong.**

The frame dump names the nine:

      7 PASS v=17601 p=494  Diffuse_MapSampler inst=1  <- terrain
    149 PASS v=1879  p=19   Diffuse_MapSampler inst=1
    559 PASS v=792   p=1599 Diffuse_MapSampler inst=1
    797 PASS v=1426  p=8    Diffuse_MapSampler inst=1

**SR3's visible world geometry IS the G-buffer pass.** Terrain and every large instanced mesh
reach Remix only through it; the material pass is per-object and does not carry them. Nine draws
a frame - but the nine that matter. Not converting them is not converting the world.

### Why "produces no colour" was true and still useless

Terrain's G-buffer shader is structurally identical to the head's:

    ir_bbterrain1_s[5]   texld r2, v5, s1 / texld r2, v2, s0   <- both dead, r2 never read
                         mad   oC0.xy, r0, c6.x, c6.x          <- the NORMAL

So *this pass outputs no colour* is true of both and separates nothing. What separates them is
not in the shader at all:

- **what the game BINDS.** On a world draw the texture at that stage is the object's real colour
  map, so converting terrain from this pass looks right. On a character draw it is a specular
  mask - a head wearing the wrong texture.
- **whether a material-pass copy exists.** Characters have one; terrain's only converted copy is
  this pass.

Skinned tracks both, and is now the gate. The sweep says the population is `ir_sr3npcskinfull_*`
(8 shaders) with the 8 terrain shaders excluded - the two families that share the
multi-target-plus-Blend_Map signature.

### Method note, third time

I read a shader correctly, drew a correct conclusion about that shader, and applied it to a
population I had not characterised. "No colour is produced by this pass" was never the question.
The question was **"what would Remix lose if this draw stopped converting"**, and the answer
differs per draw for reasons that are not in the shader.

### CONFIRMED: z-fighting and head-swapping gone, world intact

User: *"the zfighting and headswapping seams to be gone."* And the world came back:

    frame 9600 | draws 2965/frame | FFP converted 938/frame (31.6%)
    ...of which 6.1 SKINNED draws/frame passed through instead of converted (gate ON)
    MORPH APPLIED: 32.4 draws/frame skinned WITH the delta
    DEDUP: 39.7 draws/frame hidden, 0.0/frame did not fit
    snoop holds 2.0 MB of 96 MB, 0 writes refused, 0 allocations failed
    TIMING: frame 34.4 ms avg (29 fps) | shim 20.22 ms avg (58.8% of frame)

6.1 draws a frame, character-shaped, against 938 converted. Conversion is back to 31.6% - the
same share as before any of this - so nothing else was taken with it.

## Where the character work ended up

Four separate faults, four separate causes, found over two days:

| symptom | cause | fix |
|---|---|---|
| detached car parts drifting with animation | shim read "skinned" from the vertex DECLARATION; the game reads it from the SHADER, and `_s` variants declare no `dcl_blendindices` but share the declaration | `skinRequireBoneDecl` |
| heads misplaced, slightly low, wrong shape | SR3 morphs characters: `pos = v0 + v4/8192` from a second vertex stream the shim never read | `applyMorph` |
| heads wrongly lit | the morph NORMAL is a delta too - `n_base + 2*n_morph`, not a replacement | in the same path |
| heads wrongly textured, z-fighting, swapping | the character G-buffer pass was converted with `Blend_Map` - a specular mask - as its albedo, on top of the correct material copy | `skipDeferredGBuffer`, skinned only |

Every one was found by disassembly, and every one was an INPUT the shim did not know existed -
a declaration bit, a vertex stream, a shader output signature. None was visible to any runtime
probe, because a probe cannot measure what the code has no concept of.


## 2026-08-28b - the tint question, half of it closed without a run

Picked up the last correctness item on the open list: *the material pass ends `mul oC0, r2, c37`
where c37 is `Tint_color`, and whether the shim reproduces that per-object constant for heads and
bodies was never established.*

Hash-verified the deployed build against `build/` and `configs/` first - all six matched the
2026-08-28 baseline.

### The standing claim was overstated, and the shim's own log disproves it

Written in `ConstantAlbedo` since 2026-08-20:

    // Measured 2026-08-20: Tint_color is (5.0, 5.0, 5.0) on every character material in the game

It is not. The same log that produced that line also contains:

    CHAR CONST #5: first='Damage_Normal_MapSampler'    | Tint_color(c37) = (0.000 0.000 0.000 1.000)
    CHAR CONST #6: first='Dual_Paraboloid_Map_BackSam' | Tint_color(c37) = (0.000 0.000 0.000 1.000)
    CHAR CONST #7: first='Dirt_Rust_MapSampler'        | Tint_color(c37) = (0.000 0.000 0.000 0.000)

Four character draws read 5.0 and three vehicle draws read 0.0. "Every character material in the
game" was **four draws in one frame**, because `ProbeCharacterConstants` reports once per distinct
`firstSampler` name and stops at 12. That is the same shape as the hair finding - *"Hair_Spec_Color
never captured, the probe's slots fill first"* - and the same shape as the two counters this file
already records as having lied.

### Tint_color is a terminal exposure scale. Settled by sweep, not by a run.

Disassembled `ir_sr3npcskinfull_mc` shader[8], the character material pass:

    texld_pp r2, v4, s0        ; Diffuse_Map                     <- the albedo
    mul_pp   r2, r2, c0        ; * Diffuse_Color
    mul_pp   r3.xyz, r2, r5    ; * sphere/blend
    mul_pp   r1.xyz, r1, r3    ; * lighting
    ...
    mad_pp   r0.xyz, r2, c1.x, r0    ; + self-illumination
    lrp_pp   r2.xyz, v3.w, c38, r0   ; fog lerp
    mul_pp   oC0, r2, c37            ; * Tint_color              <- AFTER the fog

`Tint_color` is applied after the fog lerp, to the fully lit and fogged result. An albedo tint
that also tints the fog is not an albedo tint.

Swept all 846 disassembled files rather than generalising from one shader, which is what the last
three regressions on this project were:

| how Tint_color is consumed | count |
|---|---|
| `mul` | 2,025 |
| `rcp` - dividing BY it, to undo it | 945 |
| `mad`/`mov`/`add`, **all on `.w`**, car-glass opacity | 147 |
| last use is the shader's FINAL instruction | 1,681 / 1,944 |
| last use within 3 instructions of the end | 1,927 / 1,944 |

The 945 reciprocals are the strongest single piece of evidence: `ir_at_window_reflectmask_*` does
`rcp r3.xyz, c37.xyz` to remove Tint_color from a value it reads back out of a buffer, then
re-applies it at the end. You take the reciprocal of an exposure scale. You never do that to a
material colour.

**So discarding `Tint_color` is correct, and it needed no run to establish.** The shim's behaviour
was already right; only its recorded reason was wrong, and a wrong reason in a comment is what a
future session reasons from. Corrected in place.

### The per-object albedo tint is a different constant

The albedo multiply in that shader is `mul_pp r2, r2, c0` - **`Diffuse_Color`** - applied to the
diffuse sample immediately after it is loaded. Swept for which constant scales a
diffuse/blend/pattern sample across the corpus:

| constant | shader entries |
|---|---|
| **Diffuse_Color** | **215** |
| Base_Color | 12 |
| Tint_color | **1** (of the 1,944 that declare it) |

`SetupTextureStages` binds the map with `SELECTARG1`/`TEXTURE` whenever one exists, so
`mul r2, r2, c0` never happens on a converted draw. `Diffuse_Color` is already reflected
(`colourConstRank = 100`) and is used only when NO map is bound.

That matters for characters specifically because the head's diffuse map is a shared 2048x1024
atlas (`CHAR TEX #3: s0 Diffuse_MapSampler 3BA466D0 2048x1024`). If many NPCs share one head
texture, per-character skin tone cannot be coming from the texture, and `Diffuse_Color` is the
only constant in that shader that can colour the albedo.

### What is NOT established, and the probe that answers it

`Diffuse_Color` has been observed exactly twice, both `(1.000 1.000 1.000 1.000)`, by the probe
whose sampling limit is the subject of this entry. Neither observation is confidently the head's
material pass - one reports `c14`, the other is the G-buffer pass that `skipDeferredGBuffer` now
passes through.

**If it is white everywhere, applying it changes no pixel and the tint question closes with no
code change.** That is a runtime question, so `diffuseColorProbe=1` measures the DISTRIBUTION -
not a sample, which is the mistake being corrected here:

    DIFFUSE_COLOR (the game multiplies its diffuse map by this; we bind the map raw, so it is
    DROPPED): N draws/frame carry it (S skinned)
      NON-WHITE X draws/frame (Y skinned) | K distinct values seen
        (1.000 1.000 1.000) x N draws   <- white, a no-op

Read-only: it changes no disposition, binds nothing, and cannot turn the world black.

`NON-WHITE 0.00` closes the question. Non-zero SKINNED is the missing per-object tint, and the
same line states how many draws a fix would affect - the number this project has twice shipped a
rule without measuring.

### A bug in the probe, caught before shipping

The first version popped the top-8 buckets by zeroing them. The frame report runs on
`g_frames == 60 || g_frames % 600 == 0` - **every 600 frames, not once** - so the table would have
been emptied by the first report and every later one would have printed a confident, near-empty
distribution. Sorted through an index copy instead.

That is the third time on this project that an instrument would have shipped measuring something
other than its name, and the first time it was caught by checking rather than by a wasted run.

### Deployed

    sr3-rtx.asi  5c68effd54ab7225c6e8fb205c205b95
    sr3-rtx.map  e2ccc84af150a3124f2b7229e8566217
    sr3-rtx.ini  16f4a87b9badb47279103ccd6abcaf90
    rtx.conf     0872ecaa9ff251c4b34a71759cb7ca8d   (unchanged)
    dxvk.conf    1c745b9305a5954d75994fa3408325c5   (unchanged)
    user.conf    8b361562e85c86ab88ab2fe61495b335   (unchanged)

Key verified present in `[sr3-rtx]`. No behavioural switch changed.

### The tint question was the wrong question. The characters are BLACK.

User, asked to characterise the wrongness precisely rather than answer "is it wrong":

> 1. my own player skin is black and has none of the tattoes.
> 2. the hair of my player and npcs are also not colored correctly. Only the top of the player is
>    colored correctly.
> 3. npcs are inconsistent. the head skin color does not match the body skin at times.
> 4. some npc clothes are not textured properly.

Black skin with no tattoos is not a mis-scaled tint. It is an albedo that never arrived, and one
mechanism explains all four symptoms.

### SR3 bakes each character into a render target, and we were skipping the bake

The frame dump names it. The player is 54 draws at `at(96.9 145.7 29.7)`, and **36 of them -
d4140-d4176, v=7977 - are bound to ONE albedo, `tex0=3BA8F080`**, which `ALBEDO BOUND` reports as

    2048x1024 fmt=22 mips=9 pool=0 usage=0x200      <- D3DUSAGE_RENDERTARGET

Skin, tattoos and clothing are one GPU-composited atlas. The quads that FILL it are screen-space
(no `projTM`) and their source is often a rank-0 sampler, so they land in the `post/composite
quad` branch - and `hiddenPassMode=2` turns that branch into **SKIP**. Skipped, the atlas is never
written.

### The evidence is Remix's own captures, not my reasoning

`captures/textures/` holds every texture Remix resolved. Three are 2048x1024:

| texture | mean RGB | near-black |
|---|---|---|
| `8E4C8047F62D947A` | (230.6, 175.4, 142.2) | 0.0% |
| `C2A50BD931666EDF` | (4.2, 3.4, 3.7) | 94.5% |
| `E881A25E37E37B19` | (0.0, 0.0, 0.0) | **100%** |

and they split cleanly by DATE:

    captures 2026-08-13 .. 08-18  (11 of them)   8E4C8047F62D947A   mean 182.7
    capture  2026-08-23                          E881A25E37E37B19   mean   0.0

The atlas was a real character face until 08-18 and pure black on 08-23 - which is when
`hiddenPassMode=2` and capture-off landed.

The linkage was checked rather than assumed, because a black 2048x1024 texture could equally be
an unused slot in a character-atlas pool, and "a probe firing is not a defect" is a mistake this
project has made three times. Opened with `pxr`:

    /RootNode/Looks/mat_E881A25E37E37B19/Shader  diffuse_texture = @../textures/E881A25E37E37B19.dds@
    -> 72 meshes bound to it, bboxes 0.07-0.46 units (hands, head pieces, character sub-parts)

72 character sub-meshes are being path-traced with a 100% black albedo.

### Rule 1, for the fourth time

**A draw whose RESULT the engine reads can never be SKIPPED, only hidden or passed through.** The
engine reads this one as a texture. The rule is written above `hiddenPassMode` in the source and
this is the fourth build to break it.

### The fix, and the population measured BEFORE shipping it

The test is the TARGET, not the source: *a quad drawing into a surface that is not the size of the
screen is not compositing the screen.* Counted off the existing frame dump first - the step this
project keeps skipping:

| composite quads in that frame | count | disposition |
|---|---|---|
| targeting 2560x1440 surfaces (resolve chain, back buffer) | 357 | unchanged |
| targeting smaller off-screen surfaces | **41** | SKIP -> **PASS** |

357 untouched matters: *"skipping the composite quad freezes the image"* is a recorded dead end
and this must not reopen it. And pass-through costs nothing at the path tracer, because with
`rtx.useVertexCapture = False` Remix refuses shader-driven draws outright - the same fact
`skipDeferredGBuffer` already rests on.

`compositeToTexturePass=1`. Falsifier:

    composite into an off-screen texture (engine reads it back as a texture, so it must never be
    SKIPPED): N draws/frame passed through instead - gate ON

N near 41 is right. 0 means the rule did nothing and the black is something else. Hundreds means
it is catching the resolve chain and the rasterised frame will repaint the world - back it out.

### What was learned about the question I had been asking

`Tint_color` and `Diffuse_Color` were both real findings and neither was the fault. The tint
question was answerable only because the user was asked to describe the wrongness instead of
confirm it - "wrong colour" and "black with no tattoos" point at completely different mechanisms,
and I had spent a build on the first reading of it.

### Deployed

    sr3-rtx.asi  9fcb3cc5b322f962896e36840d7e69a8
    sr3-rtx.map  8d5e95b6c262199fedb8b6bd0b97db8a
    sr3-rtx.ini  725c295908402e39b381a79d300c810d
    rtx.conf     0872ecaa9ff251c4b34a71759cb7ca8d   (unchanged)
    dxvk.conf    1c745b9305a5954d75994fa3408325c5   (unchanged)
    user.conf    8b361562e85c86ab88ab2fe61495b335   (unchanged)

### compositeToTexturePass: fired exactly as measured, fixed nothing, REVERTED

    composite into an off-screen texture ...: 35.9 draws/frame passed through instead - gate ON
    frame 10800 | draws 1565/frame | FFP converted 419/frame (26.8%)

User: *"it is basically just like before but now the npc heads look darker as if they have a dark
tint or low exposure."*

The instrument was honest - 35.9 against a prediction of ~41, on exactly the population counted
off the frame dump. The rule did what it said. **It was still the wrong rule**, and the reason is
one I had already written down in the same commit without acting on it:

**The character atlas is composited at SPAWN. It appears in no steady-state frame dump.** Neither
the 08-28 17:40 dump (frame 1335) nor the 18:52 dump (frame 1399) contains a single 2048x1024
render target. So the 35.9 draws/frame this rule passed through were the post chain - the blur,
the 640x360 targets and the auto-exposure reduction - and never the draws it was written for.

Passing the exposure/luminance chain through is not free: the engine's own auto-exposure resumed
running, and NPC heads went darker. Measured worse, fixed nothing, so .

### What this does and does not eliminate

It does NOT clear the atlas hypothesis. The rule never met the atlas composite, so nothing about
it was tested. What it eliminates is the *steady-state* composite population as the cause.

Still standing, and still only circumstantial:

    captures 2026-08-13 .. 08-18   atlas 8E4C8047F62D947A   mean 182.7
    capture  2026-08-23            atlas E881A25E37E37B19   mean   0.0
    + 72 character sub-meshes bound to a material whose diffuse_texture IS the black file

The missing measurement is the obvious one and I should have asked for it before writing a rule:
**what does the atlas look like TODAY?** That is one Remix capture, no build, and it splits the
problem cleanly - a black atlas means the composite really is being lost and the rule needs to
find the spawn-time draws; a correct atlas means the albedo is fine and the black skin is
happening downstream in Remix's material assignment.

`rtx.captureShowMenuOnHotkey = False` and `rtx.captureHotKey = CTRL, SHIFT, P` so it is one key.

### Method note

Third build this week whose population was counted correctly and whose *timing* was not. "How
many draws does this rule change, and from what to what" was answered - 41, correctly. The
question not asked was **"are the draws I care about in the frame I measured?"** They were not,
and the dump said so before the build: no 2048x1024 target in it.

### Deployed

    sr3-rtx.asi  9fcb3cc5b322f962896e36840d7e69a8   (unchanged - the rule is ini-gated)
    sr3-rtx.map  8d5e95b6c262199fedb8b6bd0b97db8a
    sr3-rtx.ini  c888bb46380dc6e999eff04afe881185   compositeToTexturePass=0
    rtx.conf     80ea312f4ede5ccc9db3cfa7ec910aa6   one-key capture
    dxvk.conf    1c745b9305a5954d75994fa3408325c5
    user.conf    8b361562e85c86ab88ab2fe61495b335

### The atlas IS black today, and the reason is that Remix only hashes UPLOADS

User took a Remix capture with a dark-headed NPC beside them. Measured with `pxr` + PIL:

    capture_2026-08-28_19-13-46.usd
      mat_E881A25E37E37B19  diffuse_texture 2048x1024  mean 0.0  100% near-black

So the atlas is black NOW, on the current build, with `compositeToTexturePass` reverted. Same
hash as the 08-23 capture, because an all-zero image always hashes the same - which also explains
why "the atlas went black on 08-23" looked like a clean date split. It is not a date split at all;
it is the same empty texture every time.

The shim is binding the right thing. From this run's own log:

    CHAR TEX #6: s0 Diffuse_MapSampler 3B3F5DD8 2048x1024 fmt=22 levels=9   <- bound as albedo
    ALBEDO BOUND #5: ... 2048x1024 fmt=22 mips=9 pool=0 usage=0x200

`usage=0x200` is `D3DUSAGE_RENDERTARGET`, and the answer was already written in this file, above
the cloth generator:

    // Staged through SYSTEMMEM and copied up with UpdateTexture: ... UpdateTexture is also the
    // upload Remix hashes - which is what gives each outfit a stable, replaceable texture hash.

**Remix hashes the content the game UPLOADS.** A render target is written by the GPU and never
uploaded, so Remix has no pixels for it and builds the material from zeroes. Nothing about the
composite draws was ever wrong; the texture was never readable by Remix in the first place.

### The fix

`rtAlbedoCopy=1`: when the albedo is a `D3DPOOL_DEFAULT` + `D3DUSAGE_RENDERTARGET` texture, read
it back with `GetRenderTargetData` and re-upload it with `UpdateTexture`, then bind the copy.

Cost control, because this project has already killed the game once by allocating per draw:

- ONE readback per distinct texture, cached by pointer - the player binds the same atlas 36 times
  a frame, and `GetLevelDesc` alone is a bridge round trip;
- at most one readback per FRAME across all textures - `GetRenderTargetData` of 8 MB is a full
  GPU sync;
- the source is `AddRef`d, because a cache holding a D3D pointer without one is the exact
  use-after-free fixed on 2026-08-18;
- a 64 MB ceiling with refusals counted, and every allocation in `try`/`catch`;
- a BLANK readback is never cached - a character's atlas is composited at spawn, so the first
  sight of one can legitimately precede its content. `rtAlbedoRetries=240` bounds the retrying.

### The falsifier separates the two causes for good

    RENDER-TARGET ALBEDO ...: N copies made, B blank readbacks, F failed, ...
      last readback mean = M of 255

| reading | meaning |
|---|---|
| M > 0 | the pixels WERE on the GPU and only Remix lacked them - this fix is right |
| M = 0 | the game never wrote the atlas, so the composite draws really are lost and `compositeToTexturePass` was the right idea aimed at the wrong frames |
| N=0, B=0, bound 0/frame | no render-target albedo was seen and this path did nothing |

That is the measurement `compositeToTexturePass` should have been gated on. It was shipped on a
date correlation between two captures, and the correlation was an artefact of content hashing.

### Deployed

    sr3-rtx.asi  fa1985f8994d209f3e7f9a6b2f46070b
    sr3-rtx.map  e39ec495dd28c69d5aeb3ca633586834
    sr3-rtx.ini  5ae85c84fb211e184050641d0e7f531e
    rtx.conf     80ea312f4ede5ccc9db3cfa7ec910aa6
    dxvk.conf    1c745b9305a5954d75994fa3408325c5
    user.conf    8b361562e85c86ab88ab2fe61495b335

`rtAlbedoCopy=1  rtAlbedoRetries=240  compositeToTexturePass=0`

### The copy never ran: I misread the usage flag

    COMPOSITED ALBEDO ...: 0 copies made, 0 blank readbacks, 0 failed | 0.0 draws/frame bound
      no readback has run yet - no render-target albedo has been seen

**`D3DUSAGE_RENDERTARGET` is 0x1. `0x200` is `D3DUSAGE_DYNAMIC`.** I read `usage=0x200` off the
albedo report and called it a render target twice, in the source, the ini and to the user. The
character atlas is a DYNAMIC, CPU-written texture. The rule matched nothing and did nothing, and
the ONLY reason that is known rather than guessed at is the third falsifier branch, which was
written to say exactly this.

### D3DPOOL_DEFAULT + D3DUSAGE_DYNAMIC identifies nothing either

Every albedo descriptor this game binds, from one run:

    1024x1024 DXT1  mips=9  pool=0 usage=0x200
    1024x512  DXT5  mips=8  pool=0 usage=0x200
    512x512   DXT5  mips=8  pool=0 usage=0x200
    256x128   DXT1  mips=6  pool=0 usage=0x200
    128x256   DXT5  mips=6  pool=0 usage=0x200
    2048x1024 fmt=22 (X8R8G8B8) mips=9 pool=0 usage=0x200      <- the character atlas
    512x256   fmt=21 mips=1 pool=0 usage=0x0                   <- our own generated cloth

**Every one is DEFAULT + DYNAMIC, and the world is textured correctly.** So that pair cannot be
the discriminator. What separates the atlas is the FORMAT: all streamed art is DXT1/DXT5, and the
atlas is the only UNCOMPRESSED albedo in the game, because it is composited at runtime rather
than loaded from disk.

Corrected test: DEFAULT pool, uncompressed X8R8G8B8/A8R8G8B8, DYNAMIC or RENDERTARGET, >= 256x256.
A render target is resolved with `GetRenderTargetData`; a dynamic texture is read-locked directly,
which the cloth generator already proves works on DEFAULT pool under DXVK (8 of 8, none failed).

Added `textures matched` - the population counter the first version had no equivalent of. T=0 now
says "the rule never recognised the atlas", which is the failure that just happened.

### Hair: a probe that cannot be crowded out

User reports hair white and the wrong colour, twice. `ir_sr3pchair_c` shader[8]:

    Hair_Parameters c1 | Hair_Spec_Alpha c2 | Hair_Spec_Color1 c3 | Hair_Spec_Color2 c4
    Dob_Map s0 | Diffuse_Map s1

and it builds its colour through a chain of `cmp` selects over those constants before the fog
lerp and `mul oC0, r6, c37`. That is a per-texel recipe of the same shape as the clothing one -
fixed function cannot express it and the honest answer is a generated texture, as it was for
cloth. It cannot be designed without the runtime values, and this project has failed to capture
them since 2026-08-19 with the note *"Hair_Spec_Color1/2 never captured - the probe's slots fill
first"*.

`ProbeHairMaterial` is keyed on the SHADER declaring `Hair_Spec_Color1`, not on a sampler name,
so it cannot lose the race. Read-only, 6 reports, logs c0-c4 and the bound stages.

### Also worth recording: the user says the CAPTURE disagrees with the screen

*"the capture is inconsistent with what i see ingame. the top of my outfit looks okay ingame but
not in capture."* So a Remix capture is not a faithful record of the live frame, and the
capture-derived evidence in the previous two entries is weaker than it was presented as. The
black 2048x1024 material in the capture is still real - Remix built it - but "the capture shows X"
must not again be treated as "the screen shows X".

### Deployed

    sr3-rtx.asi  1b94e58ebd70e6f4a23cdb0348df2f0f
    sr3-rtx.map  75637d4be19fe70a67911c406a3e99de
    sr3-rtx.ini  97b8c5e154eeae4dafc9b74c806faf8b
    rtx.conf     80ea312f4ede5ccc9db3cfa7ec910aa6
    dxvk.conf    1c745b9305a5954d75994fa3408325c5
    user.conf    8b361562e85c86ab88ab2fe61495b335

## 2026-08-29 - the atlas reads back as zeros, and that has two meanings

    COMPOSITED ALBEDO ...: 1 textures matched, 0 copies made, 240 blank readbacks, 0 failed
      last readback mean = 0.0 of 255

**The format discriminator is right.** `textures matched = 1` - DEFAULT pool + uncompressed +
dynamic + >=256x256 picks out exactly the character atlas and nothing else, against a world where
every other albedo is DXT1/DXT5 with the identical pool and usage flags. That was the part most
likely to be wrong and it is not.

**The readback is all zeros, 240 times.** The falsifier as written says that means the game never
wrote the atlas. That is one reading and the counter cannot separate it from the other:

| reading | consequence |
|---|---|
| (a) the atlas genuinely holds nothing | the composite draws really are being lost; resume that hunt |
| (b) the read is blind | `D3DPOOL_DEFAULT` + `D3DUSAGE_DYNAMIC` locked READONLY has UNDEFINED contents in D3D9 - it is write-only memory from the CPU side, and zeros mean "unreadable" |

(b) is not a technicality. This file already records the same fact about vertex buffers - *"the
decoder refuses to read-lock DYNAMIC buffers - correctly, since that is write-combined memory
behind the 32->64-bit bridge"* - and the game samples this very texture to draw characters that
are NOT black in its own rasterised output. So the content almost certainly exists.

**Writing the falsifier with only two branches was the mistake.** "M = 0 means the game never
wrote it" assumed the read was trustworthy, which is the same class of error as reading
`usage=0x200` as RENDERTARGET: a measurement believed without a control.

### The control, and it needs no build

`charTexDump=1` decodes every named stage of the probed character materials through the SAME
`LockRect(0, &lr, nullptr, D3DLOCK_READONLY)` call and writes each to
`chartex-<tag>-<w>x<h>.raw`. The DXT art textures in those same draws are the control:

| dumps | conclusion |
|---|---|
| DXT stages have content, the 2048x1024 atlas is black | the atlas has no CPU-readable content - the copy has to go through the GPU (render a quad sampling it into our own target, then GetRenderTargetData) |
| everything is black | the read path is blind and nothing measured through it can be trusted, including the 240 blank readbacks |
| the atlas has content | the readback loop has a bug, not the texture |

Only `sr3-rtx.ini` changed; the .asi is untouched.

    sr3-rtx.ini  e2338e61d50988a51f8210cfe1ce6cda

### The control answered: the READ was blind, not the texture

`charTexDump=1`, which decodes every named character stage through the same
`LockRect(0, &lr, nullptr, D3DLOCK_READONLY)` the copy used:

| dump | format | mean | |
|---|---|---|---|
| Pattern_Map 32x32 | DXT1 | 85.0 | content |
| Sphere_Map 128x128 | DXT1 | 202.7 | content |
| Dob_Map 512x512 | DXT1 | 132.2 | content |
| Specular_Map 64x64 | DXT1 | 106.3 | content |
| **Diffuse_Map 2048x1024** | **fmt=22** | **0.0** | **BLACK** |
| **Normal_Map 1024x512** | **fmt=21** | **0.0** | **BLACK** |

20 of 24 stages decoded with real content. **Every DXT texture reads back correctly and both
UNCOMPRESSED ones read as zeros.** The decisive one is the NORMAL MAP: a character's normal map
is not empty - the game shades characters with it - so this is a blind READ.

D3D9 leaves the contents of a `D3DPOOL_DEFAULT` + `D3DUSAGE_DYNAMIC` surface UNDEFINED under a
READONLY lock; it is write-only memory from the CPU side. The 240 blank readbacks measured
nothing about the atlas.

**So the falsifier I wrote was wrong in the same way twice over.** "M = 0 means the game never
wrote it" had only two branches, and the missing third - "the read cannot see it" - is the one
that was true. A measurement believed without a control, which is what reading `usage=0x200` as
RENDERTARGET was as well.

### The read now goes through the GPU

`StretchRect` the source into a render target we own, then `GetRenderTargetData` to resolve that.
A render target resolve has DEFINED contents, so if the pixels exist on the GPU this sees them,
and a zero mean now really does mean the atlas is empty.

New failure mode, counted rather than assumed: D3D9 says a StretchRect source should be a render
target or offscreen plain surface, and an ordinary texture level may be refused. `StretchRect
refusals` non-zero means this route is closed and the copy has to be done by drawing a quad
instead.

### Hair, captured for the first time since 2026-08-19

`ProbeHairMaterial` fired - keyed on the shader rather than a sampler name, so it could not be
crowded out:

    HAIR #1: albedoStage=1 rank=100 first='Diffuse_MapSampler'
          Hair_Spec_Color1(c3) = (0.490 0.510 0.522 0.000)
          Hair_Spec_Color2(c4) = (0.153 0.153 0.161 0.000)
          c1 (Hair_Parameters) = (0.000 0.000 0.000 0.030)
          c2 (Hair_Spec_Alpha) = (0.300 0.000 0.000 0.000)
          s0 Dob_MapSampler     512x512 DXT1
          s1 Diffuse_MapSampler 256x256 DXT1   <- bound as albedo

**Both hair colours are achromatic greys.** They are SPECULAR colours, as their names say, and
carry no hair colour at all - so the long-standing theory that hair is white because we discard
`Hair_Spec_Color1/2` is dead.

Looking at the two textures settles where hair colour is not:

- `Dob_Map` is a hair STRAND mask - white strands on a flat green field.
- `Diffuse_Map`, the one ranked 100 and bound as albedo, is a NORMAL/FLOW map - a grey field with
  magenta and green lobes. Binding it as base colour is the documented "shoes have normals but
  they are the wrong colour" trap, and it is what hair is being drawn with.

So hair's albedo is a normal map, and neither of its own textures carries colour. Where the
colour actually comes from is the next question, and it is NOT the constants.

### Deployed

    sr3-rtx.asi  0cbae28588d562dc294335911c80077f
    sr3-rtx.map  79dfef155cbb2eba3dc2f94ca885cccb
    sr3-rtx.ini  231e44e6478c7bbfe51963de37e4a018
    rtx.conf     80ea312f4ede5ccc9db3cfa7ec910aa6
    dxvk.conf    1c745b9305a5954d75994fa3408325c5
    user.conf    8b361562e85c86ab88ab2fe61495b335

### The GPU copy succeeded and still read zero - so the copy path gets a control too

    COMPOSITED ALBEDO ...: 1 textures matched, 0 copies made, 240 blank readbacks,
                           0 StretchRect refusals, 0 failed
      last readback mean = 0.0 of 255

StretchRect returned S_OK. GetRenderTargetData returned S_OK. The mean is still zero. Read
literally, by the falsifier as written, the character atlas is genuinely empty.

**Not accepted, because "it returned S_OK" is not evidence that it moved pixels**, and that is
the same shape as the three readings this project has already got wrong in a row:

| reading believed | what was actually true |
|---|---|
| `usage=0x200` is D3DUSAGE_RENDERTARGET | it is D3DUSAGE_DYNAMIC; the rule matched nothing |
| 240 blank LockRect readbacks mean the game never wrote it | a READONLY lock of a DYNAMIC default surface has UNDEFINED contents |
| a black atlas in the 08-23 capture but not the 08-18 ones is a date correlation | an all-zero image always hashes the same, so it was the same empty texture twice |

Every one of those was missing a CONTROL - a case whose answer is known in advance.

### The control

`SelfTestCopyPath` runs the identical path over a source this shim fills itself, matched to the
atlas in the properties that matter: `D3DPOOL_DEFAULT`, `D3DUSAGE_DYNAMIC`, uncompressed, written
by `LockRect` and never by the GPU. 256x256, once, everything released.

    step 1  ColorFill -> GetRenderTargetData     (tests the RESOLVE alone)
    step 2  StretchRect(dynamic) -> resolve      (tests reading a DYNAMIC default texture)

| step 2 | conclusion |
|---|---|
| near the written mean (~127) | the path works, and the character atlas really IS empty |
| 0.0 with step 1 fine | StretchRect cannot read a DYNAMIC default texture, and every zero this path has reported is meaningless |

Step 1 separates the resolve from the copy, so a failure names which half is at fault instead of
leaving it to be guessed at. The target is cleared to black between the steps so a no-op
StretchRect cannot inherit step 1's colour and look like a success.

### Deployed

    sr3-rtx.asi  124f3bc5d49b1701f4588f096faf385f
    sr3-rtx.map  f6f14c59a1cf6e8647c87642181584ea
    sr3-rtx.ini  231e44e6478c7bbfe51963de37e4a018   (unchanged)
    rtx.conf     80ea312f4ede5ccc9db3cfa7ec910aa6   (unchanged)
    dxvk.conf    1c745b9305a5954d75994fa3408325c5   (unchanged)
    user.conf    8b361562e85c86ab88ab2fe61495b335   (unchanged)

### The control passed, so the atlas really is empty - and that is a paradox

    COPY PATH SELF-TEST (control for the COMPOSITED ALBEDO zero):
        wrote a DEFAULT+DYNAMIC 256x256 source by LockRect, expected mean 126.7
        step 1  ColorFill -> GetRenderTargetData      mean 126.7  hr=0x00000000
        step 2  StretchRect(dynamic) -> resolve       mean 126.7  hr=0x00000000

Both steps returned exactly the value written. **StretchRect reads a D3DPOOL_DEFAULT +
D3DUSAGE_DYNAMIC texture correctly**, so the 0.0 measured for the character atlas is real and not
another blind read. This is the first number in this whole investigation that has survived a
control.

The paradox that leaves: `ir_sr3npcskinfull_mc` shader[8] does `texld_pp r2, v4, s0`, and s0 IS
that atlas. An empty atlas means SR3's OWN rasterised characters are black too. Either something
fills it that we are removing, or character colour never came from this texture.

### The experiment, and it returns a number rather than a judgement

`hiddenPassMode=0` for one run: every hidden pass PASSES THROUGH, so the shim removes no draw
from the engine at all while still converting. Then read `last readback mean`:

| M | conclusion |
|---|---|
| > 0 | a draw the shim was SKIPPING fills the atlas - bisect from here (`screenSpaceMode=0` next), and the fix is a disposition, exactly as rule 1 says |
| 0 | nothing we skip is responsible, and character colour is not coming from this texture at all |

`rtAlbedoRetries` 240 -> 3000, because at 240 the retry loop gives up after ~10 seconds and the
atlas may be composited late.

This is the A/B that this project's own notes say should be proposed after the SECOND failed
hypothesis rather than the fifth. It is well past the second.

    sr3-rtx.ini  6dd2191246598992f9b3b70052526f18   (hiddenPassMode=0, rtAlbedoRetries=3000)
    everything else unchanged

### Nothing the shim skips is responsible. The atlas theory is dead.

    SKIPPED entirely 0/frame
    COMPOSITED ALBEDO ...: 1 textures matched, 0 copies made, 580 blank readbacks,
                           0 StretchRect refusals, 0 failed
      last readback mean = 0.0 of 255

`hiddenPassMode=0` took effect - **the shim removed no draw from the engine at all** - and the
character texture was still empty after 580 GPU readbacks, through a path whose control returned
126.7 of an expected 126.7.

So the chain is closed on that side:

| eliminated | by |
|---|---|
| Remix cannot hash a render target | it is not a render target - `usage=0x200` is DYNAMIC |
| the readback is blind | control: StretchRect of a DEFAULT+DYNAMIC texture read back exactly |
| a draw we SKIP fills it | `SKIPPED entirely 0/frame`, still empty after 580 reads |
| `Tint_color` / `Diffuse_Color` | terminal exposure scale; ~90% white on skinned draws |

And the charTexDump control adds the detail that kills the last version of the theory: the
character's **normal map is empty too**. `Normal_Map 1024x512 fmt=21 mean 0.0`. A black normal map
would wreck character shading, and that is not what is on screen.

### The check that has never been made

Every remaining explanation assumes SR3's own rendering of characters is correct. **That has never
been verified.** The user only ever sees Remix's output. `ffp=0` makes the shim inert - it is the
first line of `Classify` - so the game renders itself with nothing converted:

| characters at ffp=0 | conclusion |
|---|---|
| correctly coloured | the pixels exist and BOTH our read and Remix's copy miss them - a DXVK/Remix texture-residency problem, not a conversion bug, and not something the shim can read around |
| black | the content is lost before anything this shim does, and every theory built on "the game draws them fine" collapses |

This is the same experiment the "costliest failure" section records as the one that should be
proposed after the SECOND failed hypothesis. It is now the seventh, and the reason it kept being
skipped is that each new measurement looked decisive on its own.

    sr3-rtx.ini  restored to hiddenPassMode=2; ffp still 1 and documented for the A/B

## 2026-08-29 - THE CHARACTER FAULT IS NOT IN THE SHIM

    settings: ffp=0 ...
    frame 6000 | draws 2733/frame | FFP converted 0/frame (0.0%)

User, with the shim inert: *"still black / wrong"*.

`ffp=0` returns at the first line of `Classify`, so every draw passes through untouched and
nothing is converted or path-traced. **The player's skin is black anyway.** Every character-
material theory of the last several sessions rested on "the game draws them fine and we lose it",
and that premise is false.

This is the experiment the "costliest failure" section names as the one to run after the SECOND
failed hypothesis. It was run after the seventh, and it took two minutes.

### What the evidence now says, all of it eliminated with controls

| finding | how it was established |
|---|---|
| the character texture is 2048x1024 X8R8G8B8, DEFAULT + **DYNAMIC** (`usage=0x200`) | the albedo report; `0x200` is DYNAMIC, not RENDERTARGET (0x1) |
| it is EMPTY on the GPU | StretchRect into our own RT + GetRenderTargetData reads mean 0.0 |
| that read is sound | control: the same path on a DEFAULT+DYNAMIC texture we filled ourselves returned 126.7 of an expected 126.7 |
| its NORMAL map is empty too | charTexDump: 1024x512 fmt=21 mean 0.0, while every DXT texture in the same draws decodes correctly |
| Remix sees the same thing | its capture holds the texture as an all-zero image, with 72 character sub-meshes bound to it |
| nothing the shim SKIPS causes it | `hiddenPassMode=0`, `SKIPPED entirely 0/frame`, still empty after 580 readbacks |
| **the shim is not involved at all** | `ffp=0`, still black |

### The mechanism that fits, and the one-line test

A DYNAMIC texture is written by locking it with `D3DLOCK_DISCARD`. On most real drivers DISCARD
hands back the same memory, so a game that composites a texture across SEVERAL discard locks -
base skin, then tattoos, then clothing - keeps its earlier writes by luck. DXVK honours DISCARD
properly and genuinely throws the contents away, so every pass but the last is lost.

That fits the symptom nothing else fitted: *"the top of my outfit looks okay ingame but not in
capture"* and *"only the top of the player is colored correctly"*. The LAST region written
survives; everything composited before it does not. Black skin, missing tattoos and a head whose
tone does not match the body are the same fault seen on different pieces.

`d3d9.allowDiscard = False` makes DISCARD preserve contents. Remix's own binary notes it also
disables `rtx.useBuffersDirectly`, which costs nothing here - `rtx.conf` already sets that False.

FALSIFIER: skin and tattoos return. If nothing changes, the next question is whether SR3 renders
characters correctly under plain DXVK with no Remix at all, which would separate DXVK from Remix.

### Method note - seven runs on a false premise

Every measurement in this investigation was believed one step too early:

| believed | actually |
|---|---|
| `usage=0x200` is RENDERTARGET | it is DYNAMIC; the first copy rule matched nothing |
| 240 blank LockRect reads mean "never written" | a READONLY lock of a DYNAMIC default surface is UNDEFINED |
| a black atlas on 08-23 but not 08-18 is a date correlation | an all-zero image always hashes the same - the same empty texture twice |
| the atlas is the character's albedo and we skip its composite | `compositeToTexturePass` fired on exactly the measured population and changed nothing |
| the game draws characters correctly | **it does not** |

The pattern is the same every time: a reading with no control. The three that DID have controls -
the shader sweeps, `SelfTestCopyPath`, and `ffp=0` - were all correct first time.

    dxvk.conf  a671126c3ab60716b86352d43e1f6bc7   d3d9.allowDiscard = False
    everything else unchanged

### allowDiscard: a real negative, and the date split was real after all

`d3d9.allowDiscard = False` changed nothing. Verified PARSED first, per the rule in this file:

    [11:22:02.612] info:    d3d9.allowDiscard = False

So the DISCARD-composite theory is dead on its own terms rather than on a typo.

**And I was wrong to throw away the date correlation.** On 2026-08-28 I dismissed it as an
artefact because an all-zero image always hashes the same. That explains why the BLACK atlases
collapse onto one hash. It does not explain why the POPULATED one stopped appearing - and it did:

    8E4C8047F62D947A   2048x1024   mean 182.7   in captures 08-13, 08-15, 08-16 x4, 08-17 x2, 08-18

Decoded and looked at, it is unmistakably the character atlas: skin, a face with makeup, arms,
legs, an eye, and the TATTOOS including the fleur-de-lis on the chest. Remix had the correct
character texture, with tattoos, for that whole week.

### The variable that changed in the window

    rtx.conf.before-vertexcapture-off.bak   08-20 22:58   rtx.useVertexCapture = True
    rtx.conf                                              rtx.useVertexCapture = False

| captures | useVertexCapture | atlas |
|---|---|---|
| 08-13 .. 08-18 | **True** | populated, 182.7, tattoos present |
| 08-23 .. today | **False** | all zeros |

and Remix's own log says what capture-off does to shader draws:

    [RTX-Compatibility-Info] Skipping draw call with shader usage as vertex capture is not enabled.

If SR3 composites the character texture with shader-driven draws, capture-off means Remix never
runs them and the texture is never built. That fits every measurement taken since: the texture is
empty on the GPU, its normal map is empty too, nothing the SHIM skips matters, and `ffp=0` does
not help - because the shim was never the thing removing those draws. **Remix was.**

### The A/B

`rtx.useVertexCapture = True` for one run, nothing else changed. `d3d9.allowDiscard` reverted to
default so the test has exactly one variable.

| character | conclusion |
|---|---|
| skin and tattoos return | capture-off is the cause. The fix is NOT to leave capture on - that reintroduces everything capture-off solved - but to find why Remix drops those specific draws and keep them |
| still black | capture-off is not it either, and the next split is plain DXVK with no Remix at all, which separates DXVK from Remix |

    rtx.conf   d7e9c7ec79bc7208166f00ba8a56565a   useVertexCapture = True
    dxvk.conf  b74c42d68f2d4209cbd14f034cc683dc   allowDiscard reverted

### Vertex capture is not the cause either

    [11:27:57.705] info:    rtx.useVertexCapture = True
    grep -c "vertex capture is not enabled"  ->  0

Capture was genuinely on and Remix stopped skipping shader draws - the skip line disappears
entirely from its log. The character is still black. So the correlation with the 08-20 capture-off
change was coincidence, and that theory joins the others.

Restored to False, and `d3d9.allowDiscard` left at default. Neither is a fix and neither should
sit in the config pretending to be one.

### Everything reachable from inside the stack is now eliminated

| layer | ruled out by |
|---|---|
| the shim's conversion | `ffp=0`, `FFP converted 0/frame`, still black |
| the shim's skipping | `hiddenPassMode=0`, `SKIPPED entirely 0/frame`, still empty |
| Remix vertex capture | `useVertexCapture=True`, skip line gone, still black |
| DXVK discard semantics | `d3d9.allowDiscard=False`, verified parsed, no change |
| our readback being blind | control returned 126.7 of an expected 126.7 |
| `Tint_color` / `Diffuse_Color` | terminal exposure scale; ~90% white on skinned draws |

And the screenshots from the era when it worked (08-17, 08-18) turn out to show a pause menu and
an empty room - neither contains a character, so they cannot date the regression either.

### The split that is left

**Does SR3 render the character correctly with no Remix at all?** The game loads
`d3d9.dll` (863 KB, the Remix bridge client), which hosts `.trex/d3d9.dll` (190 MB, the renderer)
inside `NvRemixBridge.exe`. Renaming the client makes Windows' own D3D9 load instead, and
`dinput8.dll` (Ultimate ASI Loader) still loads the shim, which `ffp=0` keeps inert.

| character without Remix | conclusion |
|---|---|
| correctly textured | the Remix/DXVK stack loses it, and the 08-13..08-18 captures prove it CAN work - bisect `rtx.conf` from `rtx.conf.before-vertexcapture-off.bak` |
| still black | the game, its mods or its settings are the cause, and none of the Remix-side work applies. Note the install carries loose character mods - `default female diffuse.str2_pc`, four `Female *.str2_pc`, `customization_items.xtbl`, `character_color_pool.xtbl` - plus ZMenu and Ultimate ASI Loader |

This should have been proposed at the same time as `ffp=0`. `ffp=0` bounded the shim; it did not
bound the renderer, and I treated it as though it had.

    sr3-rtx.ini  52170c27cfc0f13c42c47616885635ea   ffp=0
    rtx.conf     80ea312f4ede5ccc9db3cfa7ec910aa6   capture restored to False
    dxvk.conf    b74c42d68f2d4209cbd14f034cc683dc   allowDiscard reverted

## 2026-08-29 - WITHOUT REMIX THE CHARACTER IS CORRECT

User, with `d3d9.dll` renamed away and `ffp=0` - plain SR3 on Windows' own D3D9, no DXVK, no
Remix, shim inert: **"character is textured correctly"**.

That settles the layer, and it clears everything below it:

| cleared | |
|---|---|
| the game, its save and its character | renders correctly |
| the loose character mods | `default female diffuse.str2_pc`, the four `Female *.str2_pc`, `customization_items.xtbl`, `character_color_pool.xtbl` - all present and fine |
| SR3's display settings | unchanged and fine |
| the shim | already cleared by `ffp=0`, twice |

**The Remix/DXVK stack loses the character texture**, and the 08-13..08-18 captures prove it can
carry it - `8E4C8047F62D947A`, mean 182.7, skin, face, arms, legs and the tattoos. So this is a
regression inside the Remix configuration between 08-18 and 08-23.

**And the date correlation I threw away on 08-28 was right.** I dismissed it because an all-zero
image always hashes the same - true, and it explains why the black atlases collapse onto one hash,
but it never explained why the POPULATED one stopped appearing. I used a correct observation to
discard the evidence it did not actually address.

### The bisect

The Remix runtime itself has not changed (`.trex/d3d9.dll` dated 06-04), so the variable is
configuration. `rtx.conf` restored wholesale from `rtx.conf.before-ui-collision-fix.bak`
(08-18 12:59), the closest saved state to the last capture that held a populated atlas
(08-18 12:53). Current config preserved as `rtx.conf.before-0818-restore.bak`.

| character | next |
|---|---|
| textured | it IS an rtx.conf setting - bisect the ~25 differing keys, halving each run |
| still black | not rtx.conf. Then `dxvk.conf` (`d3d9.maxEnabledLights = 64` was added 08-23, inside the window) or `user.conf`, both one-line tests |

Note `rtx.useVertexCapture = True` comes back with this config, so expect doubled geometry and
z-fighting. Irrelevant to the question being asked.

    rtx.conf  bed7387df8d4e294e019ac76a823b352   restored from 08-18

### Not rtx.conf either - so it is not configuration at all

The 08-18 config restored wholesale: still black. `user.conf` is nothing but path-tracing quality
settings, and `dxvk.conf` held one line. So no combination of Remix settings is responsible.

**And the "it worked on 08-18" claim needs qualifying.** A capture is a single frame. The
populated atlas may have been captured during character CUSTOMISATION - where the game certainly
composites and displays that texture - rather than in gameplay. So it may never have been correct
in-game, and the regression window I built the bisect on may not exist. The date evidence was
weaker than I twice presented it, in both directions.

What is solid is mechanical and does not depend on any timeline:

    without Remix  -> the game's writes to that texture land, character correct
    with Remix     -> the writes do not land, texture is all zeros

### The remaining candidate reachable by configuration

`d3d9.apitraceMode = True` backs mapped resources with CACHED system memory instead of
write-combined. That is the memory-type difference which would explain BOTH halves of what was
measured: the game's writes going missing, and our own reads of that texture returning zeros
while every DXT texture in the same draws read back correctly.

FALSIFIER: skin and tattoos return.

If it does not, configuration cannot reach this, and the remaining route is for the SHIM to hook
`IDirect3DTexture9::LockRect`/`UnlockRect`, snoop the game's own writes to that texture, and
re-upload them through `UpdateTexture` - which is the path Remix hashes. That is the same snoop
architecture already proven on the morph and instance streams, applied to a texture instead of a
vertex buffer. It is a real subsystem, not a switch.

    rtx.conf   80ea312f4ede5ccc9db3cfa7ec910aa6   restored to the working current config
    dxvk.conf  949d8f64cd6692a8dceca1ca52950e7b   apitraceMode added

### Configuration is exhausted

    [11:51:21.336] info:    d3d9.apitraceMode = True

Parsed, no effect. Reverted - cached memory costs performance and bought nothing.

**Where the character-texture fault stands, with everything eliminated by a control or an A/B:**

| layer | verdict | how |
|---|---|---|
| the game, save, character, mods, display settings | FINE | renders correctly with `d3d9.dll` renamed away |
| the shim's conversion | not involved | `ffp=0`, twice |
| the shim's skipping | not involved | `hiddenPassMode=0`, `SKIPPED entirely 0/frame` |
| `rtx.conf` | not involved | 08-18 config restored wholesale |
| `user.conf` | not involved | path-tracing quality settings only |
| `dxvk.conf` | not involved | `allowDiscard`, `apitraceMode`, both parsed, both inert |
| Remix vertex capture | not involved | `useVertexCapture=True`, skip line gone |
| our own readback | sound | control returned 126.7 of an expected 126.7 |

**The fault is inside DXVK/Remix and configuration cannot reach it.** SR3's writes to its
2048x1024 X8R8G8B8 `D3DPOOL_DEFAULT` + `D3DUSAGE_DYNAMIC` character texture land on native D3D9
and do not land under the Remix stack. Its 1024x512 normal map is lost the same way, while every
DXT texture bound in the same draws survives.

### The only route left, and it is a subsystem

Hook `IDirect3DTexture9::LockRect`/`UnlockRect`, snoop the game's own writes to that texture at
the CPU boundary - BEFORE DXVK loses them - and re-upload them through `UpdateTexture`, which is
the path Remix hashes. Same architecture as the morph and instance stream snoop, applied to a
texture.

It should be started with a PROBE, not the whole thing: hook the lock pair, log which textures are
locked, at which level, with what size, and whether the character atlas is among them.

| probe result | consequence |
|---|---|
| the game locks that texture | the snoop is viable and the data is reachable at the CPU boundary |
| it never locks it | the game fills it another way and the probe names which call, which decides everything after |

That probe is bounded. The full snoop is not, and the morph stream - the closest precedent - took
parts of two sessions.

**Not started.** Put to the user against the alternative: park the character texture as a Remix
limitation and move to PERFORMANCE, the documented largest problem at ~57% of frame time, which
this session has not touched.

    dxvk.conf  61d3fb899588a0e703ee016ce96acbb8   both negatives recorded, apitraceMode reverted

## 2026-08-29 - starting the snoop, with the probe that decides its shape

User: fix the skin; performance later.

The route is settled by elimination: snoop SR3's own writes to its character texture at the CPU
boundary - before DXVK loses them - and re-upload through `UpdateTexture`, the path Remix hashes.
Same architecture as the morph and instance stream snoop, applied to a texture.

It cannot be written without knowing WHICH call the game uses, so that is all this build does.

### The probe

Hooks the four device entry points that can fill a texture, plus the texture's own lock pair:

    IDirect3DDevice9   30 UpdateSurface   31 UpdateTexture   34 StretchRect   35 ColorFill
    IDirect3DTexture9  19 LockRect        20 UnlockRect

and counts only calls whose destination has the atlas's shape. Report:

    TEXTURE FILL PROBE (how does the game write the character texture?): N atlas texture(s)
    tracked | LockRect N, UpdateTexture N, UpdateSurface N, StretchRect N, ColorFill N
      LockRect levels seen = 0x..., flags OR = 0x...

Whichever is non-zero is the entry point to snoop. All zero means the game fills it by a route
none of these cover - equally worth knowing, and far cheaper to learn now than after building a
subsystem aimed at the wrong call. "N atlas texture(s) tracked = 0" is the third branch: the
classifier never matched, and every count is then meaningless.

### Two things this build had to get right

**The texture vtable is patched ONCE.** Every `IDirect3DTexture9` shares one vtable, so a second
patch would store our own hook as the "original" and the hook would call itself. Guarded by a
static.

**No container is mutated from a foreign thread.** The game locks resources from more than one
thread - proven by a crash dump, then measured at 272 buffer locks a frame off the render thread.
So the candidate pointers live in a FIXED array written only on the render thread and scanned
without a lock. Mutating a map from the locking thread is the bug that already crashed this
project once.

    sr3-rtx.asi  715b85fadb10bbbe1e324f623a6781cf
    sr3-rtx.map  22e26abe1a3fa43393e1bb4e5ffc3612

### The probe killed the planned subsystem before it was written

    TEXTURE FILL PROBE: 1 atlas texture(s) tracked
      | LockRect 0, UpdateTexture 0, UpdateSurface 0, StretchRect 860, ColorFill 14

**`LockRect 0`.** The game never touches that texture from the CPU, so there is no CPU-boundary
write to snoop and the whole planned subsystem - hook LockRect, copy the game's write, re-upload -
would have found nothing to copy. That is exactly what a bounded probe is for, and it cost one
short run instead of two sessions.

SR3 composites the character texture **on the GPU, with StretchRect**.

### But the count was contaminated, and it is fixed before being used

`IsAtlasSurface` matched any uncompressed surface >= 256x256 - which is also the 2560x1440 back
buffer and every HDR target. So "StretchRect 860" counted the whole frame's blits, not writes to
the character texture. Corrected to walk the surface back to its parent with
`GetContainer(__uuidof(IDirect3DTexture9))`, which is exact, and the reference it returns is
released immediately so the probe cannot pin every texture in the game.

Fourth time a counter has measured something other than its name on this project, and the first
time it was caught before anything was built on it.

### What the re-run has to answer

    TEXTURE FILL PROBE ...: LockRect N, UpdateTexture N, UpdateSurface N, StretchRect N, ColorFill N
      of those StretchRects, M FAILED
    TEXTURE FILL #k: StretchRect INTO the character texture | hr=... | src WxH fmt/pool/usage ...

| reading | meaning |
|---|---|
| StretchRect still large, M = 0 | the game composites by GPU blit and DXVK reports success while writing nothing - the shim can shadow those blits into a render target it owns, which is a LEGAL destination, and hand Remix that instead |
| M > 0 | DXVK is refusing the game's own composite outright, and the hr in the detail lines names why |
| StretchRect now 0 | the 860 were all back-buffer blits, none of these calls touch the atlas, and the fill route is still unidentified |

A D3DPOOL_DEFAULT + D3DUSAGE_DYNAMIC texture surface is not a legal StretchRect destination under
the D3D9 spec - destinations are meant to be render targets or offscreen plain surfaces. Native
drivers allow it; DXVK may well not. That would explain every measurement at once: the game's
writes never land, the texture reads back as zeroes through a read path that is proven sound, and
none of it involves the shim.

    sr3-rtx.asi  e65720f921c86d1c2391a4b7e202a73a
    sr3-rtx.map  b2464c9305ebce5054be012bd0523034

### All five zero - and the probe was hooking after the event

    TEXTURE FILL PROBE: 1 atlas texture(s) tracked
      | LockRect 0, UpdateTexture 0, UpdateSurface 0, StretchRect 0, ColorFill 0

With the precise `GetContainer` test, nothing touches the character texture. The earlier
"StretchRect 860" was entirely back-buffer blits, as suspected - so tightening it before building
on it was worth the run.

**But zero across the board does not mean "no route". The probe had a timing hole.** The texture
vtable was patched, and the candidate recorded, at the first character DRAW. SR3 composites a
character's texture when it SPAWNS - at load - long before that character is ever drawn. The hook
was being installed after the event it existed to observe, so every count was taken from the wrong
side of it.

That is the same error as the compositeToTexturePass rule, which measured a population correctly
in a frame that could not contain the draws it was written for. Counting the right thing at the
wrong TIME has now cost two runs.

### Fixed at the earliest point a texture can exist

Both the vtable patch and the candidate list move to `Hook_CreateTexture`, which runs from device
creation. Nothing can be written to a texture before it is created, so nothing can now happen
before the hook is in place. Candidates are classified from the creation parameters directly - no
`GetLevelDesc`, no guessing - and each is named:

    ATLAS CANDIDATE #N created: WxH levels=L fmt=F pool=P usage=0xU

The array is 32 entries with an overflow counter, so "the shape test matches more than the
character atlas" reports itself instead of silently truncating.

    sr3-rtx.asi  7f88ae59d25013bb9391c1aad0d1547b
    sr3-rtx.map  56255685ce8b166a1afe8e61805fd3e7

### The overflow counter caught the shape test tracking the wrong textures

    ATLAS CANDIDATE #3 created: 2048x1024 levels=9 fmt=21 pool=0 usage=0x1
    955 candidates did not fit the 32-entry array
    LockRect 16, StretchRect 1908, ColorFill 14   (pitches 5120/3072/1024 - 1280-wide surfaces)

`usage=0x1` is `D3DUSAGE_RENDERTARGET`. Every candidate the filter caught was a render target,
because it accepted DYNAMIC *or* RENDERTARGET at any size >= 256 - and the game creates hundreds.
The array filled with the first 32 and the character atlas never entered it, so those LockRect and
StretchRect counts describe other textures entirely. The pitches confirm it: 5120 bytes is a
1280-wide surface, not the 2048-wide atlas.

**The falsifier written into that line is what caught it** - "the shape test is matching more than
the character atlas" - exactly as the DEDUP table overflow line did on 2026-08-28. Two for two on
falsifiers that report their own saturation.

### Tightened to the signature the albedo report actually prints

    bound on character draws:  2048x1024 fmt=22 mips=9 pool=0 usage=0x200

fmt 22 is `D3DFMT_X8R8G8B8`, 0x200 is `D3DUSAGE_DYNAMIC`. The flood was fmt=21 with usage=0x1, so
requiring DYNAMIC and explicitly EXCLUDING RENDERTARGET, at >= 1024x512, separates them cleanly.

And a second line that the previous two builds lacked: **every large uncompressed texture is now
logged at creation whatever its usage**, so if the atlas is created with parameters the filter
rejects, it appears here instead of vanishing silently.

    BIG TEXTURE #N created: WxH levels=L fmt=F pool=P usage=0xU

| next run shows | meaning |
|---|---|
| an ATLAS CANDIDATE at 2048x1024 fmt=22 usage=0x200, then non-zero fill counts | the entry point is named and the interception can be built |
| BIG TEXTURE lines but no matching ATLAS CANDIDATE | the atlas is created with different parameters than it later reports - which is itself the anomaly |
| neither | the texture is not created through `IDirect3DDevice9::CreateTexture` at all |

    sr3-rtx.asi  3db1ffcbb19e5e621e43e61fb1666031
    sr3-rtx.map  b8703721c5d2e1604e448cd3519bb3e6

### The atlas is now definitely identified, and none of the five calls touches it

    BIG TEXTURE #14 created: 2048x1024 levels=9 fmt=22 pool=0 usage=0x200
    ATLAS CANDIDATE #4 created: 2048x1024 levels=9 fmt=22 pool=0 usage=0x200
    ALBEDO BOUND #5: ps='Blend_MapSampler' rank=100 -> 2048x1024 fmt=22 mips=9 pool=0 usage=0x200

    TEXTURE FILL PROBE: 7 atlas texture(s) tracked (no overflow)
      | LockRect 1, UpdateTexture 0, UpdateSurface 0, StretchRect 0, ColorFill 0
    TEXTURE FILL #1/#2: LockRect ... pitch=5120

The tightened filter tracks 7 textures with no overflow, and the character atlas is provably among
them - its creation line and its albedo-bound line carry the same signature. The one LockRect
recorded has **pitch 5120**, which is a 1280-wide surface (`BIG TEXTURE #11`), not the 2048-wide
atlas. So the atlas is created, bound as the character's albedo, and written by NONE of
`IDirect3DTexture9::LockRect`, `UpdateTexture`, `UpdateSurface`, `StretchRect` or `ColorFill`.

### The gap that leaves, and it was mine

**`IDirect3DSurface9::LockRect` is a different vtable from `IDirect3DTexture9::LockRect`.** A game
that calls `GetSurfaceLevel(0)` and locks THAT surface never touches the texture method at all,
and the probe was blind to it. That is the commonest way to write a mip level in D3D9 and it
should have been in the first version.

Added: slots 13/14 on `IDirect3DSurface9`, patched from any texture's level-0 surface (the vtable
is shared, like the texture and vertex-buffer ones). The hook uses the same `GetContainer` walk,
so it fires only for a surface belonging to a tracked atlas.

    | texture LockRect N, SURFACE LockRect N, UpdateTexture N, UpdateSurface N, StretchRect N,
      ColorFill N

If SURFACE LockRect is non-zero, that is the write to intercept and the interception can finally
be built. If it is zero as well, the remaining route is `SetRenderTarget` plus draws - which would
be a surprise on a texture created without `D3DUSAGE_RENDERTARGET`, and would say the game is
doing something D3D9 does not sanction, which is itself a fine explanation for DXVK losing it.

    sr3-rtx.asi  15db7188578bdf4e3500c62b1d2000c2
    sr3-rtx.map  4f79a1e8aa7d09cd1a0f0d5ed8d24b6e

## 2026-08-29 - the fill route found, and the snoop built against it

    TEXTURE FILL PROBE: 7 atlas texture(s) tracked
      | texture LockRect 1, SURFACE LockRect 18, UpdateTexture 0, UpdateSurface 0,
        StretchRect 0, ColorFill 0
    TEXTURE FILL #3: SURFACE LockRect | flags=0x0 pitch=8192 whole   <- 2048 wide, mip 0
    #4 pitch=4096  #5 2048  #6 1024  #7 512  #8 256                  <- the rest of the chain

**SR3 fills the character atlas by taking each mip level's SURFACE with `GetSurfaceLevel` and
locking THAT.** `IDirect3DSurface9::LockRect` is a different vtable from
`IDirect3DTexture9::LockRect`, which is exactly why three earlier probes reported zero. Whole
surface, `flags=0` - a plain preserving write, the full mip chain.

That gap was mine, and it is the commonest way to write a mip level in D3D9.

### The snoop

Takes the pixels at the CPU boundary, where they demonstrably exist, and re-uploads them through
SYSTEMMEM + `UpdateTexture` - the path Remix hashes, as the cloth generator's own note says. Bound
in preference to the game's texture in `RemixReadableAlbedo`.

Design decisions worth keeping:

- **Mip 0 only.** Remix samples the top level for a material and 2048x1024x4 is 8 MB per
  character; the rest of the chain would add a third for nothing.
- **A sub-rect lock is refused, not partially captured.** Handing Remix a partial texture as
  though it were whole is a worse bug than the one being fixed.
- **The copy happens in Unlock, before the original unlock call**, because after that the pointer
  is no longer the game's to give.
- **A fixed array under its own critical section, never a map resized from a foreign thread** -
  the game locks resources from more than one thread, proven by a crash dump and measured at 272
  buffer locks a frame, and mutating a container from that side is what crashed this project once.

### The falsifier answers the last open question at the same time

    ATLAS SNOOP ...: N captured, M copies uploaded, F failures | B draws/frame bound
      atlas 1: 2048x1024  mean X of 255   <- what the GAME wrote

`mean` is the number that matters and it is measured at a boundary nothing has measured before.
Every read attempted so far - `LockRect` on the texture, `StretchRect` + `GetRenderTargetData`,
Remix's own capture - reported zero. If the game's own write carries real pixels, `mean` is
non-zero and all of those were looking at an image that never received them.

    sr3-rtx.asi  e8f34e3822c4ecab3c8a338d7a76a63e
    sr3-rtx.map  183f8d1be889259d554902a42cb2f6c2
    sr3-rtx.ini  63040c251f3da101735656e8ab614276   atlasSnoop=1

### The snoop works, and it proves the pixels were never written

    ATLAS SNOOP: 3 captured, 1 copies uploaded, 0 capture failures | 9.7 draws/frame bound
      atlas 1: 1280x768   mean 14.3    <- the capture mechanism reads real data
      atlas 2: 2048x1024  mean  0.0    <- the character atlas
      atlas 3: 1024x512   mean  0.0

The snoop is mechanically correct: it captures, uploads and binds, 9.7 draws a frame take the
copy, and a different texture captured through the identical path reads 14.3. **The character
atlas contains zeroes in the game's OWN mapped memory, at Unlock.**

So nothing downstream was ever losing the pixels. They were never written. That closes, with a
control, the entire family of theories this investigation has been built on since 08-28 - Remix
hashing, DXVK discard semantics, capture-off, the composite draws, the readback being blind. Each
was an explanation for pixels going missing between the game and Remix, and there was no such
journey.

### What that leaves, and it is narrow

The character IS correct without Remix. So SR3's compositor produces zeroes only inside this
stack, and the step before the dynamic texture is the suspect: the game creates a **2048x1024
RENDER TARGET of exactly the same size** (`BIG TEXTURE #3: fmt=21 usage=0x1`). A GPU composite
into that, read back, and written into the dynamic texture would emit zeroes if the READBACK is
what fails - and the game would then be faithfully writing the zeroes it just read.

That also explains why every one of our own reads returned zero: we kept reading the END of the
chain.

### The probe

Every large uncompressed render target is read ONCE, on the render thread in Present, one per
frame, through the path whose control already passed (`SelfTestCopyPath`: 126.7 of an expected
126.7).

    RT CONTENT #N: WxH render target | mean X of 255

| reading | meaning |
|---|---|
| one of them is non-zero and character-shaped | the pixels exist on the GPU in that surface, and the shim can bind THAT as the albedo instead - the snoop machinery to upload it already exists and is proven |
| all zero | the composite never runs at all under this stack, and the fault is upstream of every texture - which would make it a Remix/DXVK defect with no shim-side workaround |

    sr3-rtx.asi  36b0d31a3ecc6b50f252aec02123b5c8
    sr3-rtx.map  56aa778f9fd7a3f13a43b76d6ec8bd75

### All ten render targets read 0.0 - because they were all read during loading

    RT CONTENT #1..#10: ... mean 0.0    (including three 2560x1440 SCENE targets)

The 2560x1440 surfaces are the G-buffer and HDR scene targets. They unquestionably hold the
rendered world. Reading them as empty is not a finding about the character - it is proof the
measurement was taken at the wrong moment.

**One candidate was read per frame starting at frame 1, and each was marked done after a single
attempt.** Frames 1-10 are startup and loading, when those surfaces genuinely are empty.

That is the THIRD time on this bug that the right quantity was measured at the wrong time:

| probe | measured correctly | at the wrong time |
|---|---|---|
| `compositeToTexturePass` | 41 draws/frame, exactly as predicted | in frames that could not contain a character spawn |
| the fill probe | LockRect on the atlas | hooked at the first character DRAW, after the fill |
| the RT content probe | render target contents | during loading, before anything was drawn |

The pattern is always the same and it is not the same as the missing-control pattern: the number
is right, the clock is wrong. A one-shot read of a surface whose contents change is worth nothing;
only the maximum over time is.

### Fixed

Round-robin, one read every four frames, cycling forever, keeping the MAXIMUM per surface and
logging only increases. A surface that fills up later is now caught.

    RT CONTENT #N: WxH | mean rose to X at frame F
    render targets: N candidates, R reads (round-robin, MAX over time)
      RT n: WxH  max mean X

If the 2560x1440 scene targets now read non-zero, the read path is sound and any surface still at
0.0 is genuinely empty. **If they STILL read 0.0 while the world is visibly on screen, then
GetRenderTargetData cannot see this stack's render targets at all** - and every measurement taken
through it this session, including the atlas ones, has to be discarded.

That second outcome is the one to watch for. It would mean the tool has been lying since the
control passed, and the control passed only because it read a surface that had just been written
through the same API by us.

    sr3-rtx.asi  87ad631a6f8c34260a09096c392e05cd

### The round-robin probe FROZE THE GAME - and it was the recorded failure, again

User: *"the game now freezes right after the intro logos."*

The round-robin render-target read allocated a `CreateOffscreenPlainSurface` **per read** - 14.7 MB
for each 2560x1440 candidate - and did a full `GetRenderTargetData` GPU sync **every four frames,
forever**, in a 32-bit address space shared with the game, Remix's client and a 24 MB skinning
ring.

That is verbatim the failure already in YOUR-INSTRUCTIONS.md: pre-sizing the snoop buffers
"exhausted a 32-bit address space and killed the game". The rule written beside it -
**a shim must degrade, not abort its host** - applies with more force to a DIAGNOSTIC, which by
definition is not even earning its keep.

The one-shot version was safe because it was bounded by accident: ten reads, then never again.
Making it correct in TIME removed the only thing bounding it in COST, and I changed one without
looking at the other.

### Recovery, in that order

1. `atlasSnoop=0` - the probe is gated behind it, so the game runs again with **no rebuild**.
   `sr3-rtx.ini 0f6b2183a0f8575108742699d1d2a34a`, .asi untouched.
2. Then the probe made bounded rather than merely slower:
   - a **total budget of 60 reads for the session**, not a rate;
   - one read every 120 frames, about two seconds;
   - surfaces larger than the atlas capped at 3 reads - they are only the control for whether
     `GetRenderTargetData` can see game-written surfaces at all.

    sr3-rtx.asi  f5932bc0e9dab78f3bea61a64f9fc8c8
    sr3-rtx.map  c118be6588bfe4f54e137c5ad2b5b2bb

### Standing lesson, now written twice in this file

Every diagnostic added to this shim needs a CEILING, not just a purpose. Three probes this session
have cost a run through cost or timing rather than through being wrong about the thing they
measured.

### Option A confirmed the recovery; Option B did not run

    frame 3000 | draws 2520/frame | FFP converted 577/frame (22.9%)
    TIMING: frame 26.7 ms avg (37 fps), no crash dump

The bounded build plays normally, so the freeze WAS the unbounded probe and nothing else.

The follow-up run reported `0 reads of a 60 budget` and `gate OFF` - the ini still read
`atlasSnoop=0`. Asking the user to hand-edit a key cost a run; the gate is set here now instead.

### A gap that would have wasted the next run too

`g_rtProbeRead` counted only SUCCESSFUL reads. A `GetRenderTargetData` that refused every surface
would also have printed "0 reads", which is indistinguishable from "the probe never ran" - the
exact ambiguity that just happened for a different reason.

Every step now reports its own HRESULT:

    RT CONTENT FAILED #N: WxH | GetSurfaceLevel=0x... CreateOffscreenPlain=0x...
                               GetRenderTargetData=0x... LockRect=0x...
    ... %u reads FAILED

**A non-zero failure count is itself the answer**: it means the read tool cannot see game-written
surfaces, and every zero measured through it this session - "the atlas is empty", "the game writes
zeroes", the composited-albedo readback - has to be discarded. The self-test that passed only ever
proved the tool could read back a surface WE had just written through the same API.

    sr3-rtx.asi  0d643d662a63229aca2100c41664bd23
    sr3-rtx.map  28e3bbbe3e03e6b984edad1271128448
    sr3-rtx.ini  a8602a03565b964a2702d3b3180becfc   atlasSnoop=1

### It froze AGAIN at 1 read per 120 frames - which is the finding

The bounded probe - one read every 120 frames, a 60-read session budget, big surfaces capped at
three attempts - still froze the game. **At that rate the cost cannot be the problem.** So it is
not an allocation or a stall:

**Remix's D3D9 cannot service `GetRenderTargetData` on a surface it owns and is actively using.**

That is consistent with everything measured, and it completes the story:

- our own StretchRect read never hung, because it resolves OUR render target, not the game's;
- `SelfTestCopyPath` passed for the same reason - it read back a surface we had just written;
- and if **SR3's own compositor reads back its render target the same way**, it gets nothing under
  this stack and then faithfully writes the zeroes it just read. Which is exactly what the atlas
  snoop caught coming out of the game's own mapped memory: `mean 0.0`, measured by plain memcpy
  from the game's pointer, with no readback API involved at all.

That single mechanism explains the black character, why the game is correct without Remix, why no
configuration touched it, and why every read we attempted returned zero.

### Safety, and a design error of mine

`rtContentProbe` now has its own key, defaulting OFF, and is documented as DO NOT ENABLE. Sharing
a gate with `atlasSnoop` meant the harmless snoop could not be turned on without the dangerous
probe - which is how the second freeze happened at all.

Two freezes in one session, both from a diagnostic. The standing rule stands and needs no
restating: a shim must degrade, not abort its host, and a probe has to be the safest code in the
file, not the least careful.

    sr3-rtx.asi  372dbb3a59f976bcdf2130d732f16ebb
    sr3-rtx.map  2e9b15d11112c3e27d38b892bd35bbaf
    sr3-rtx.ini  84510480348e014af7e2b18e2ce59909   atlasSnoop=1  rtContentProbe=0

### Where this leaves the character texture

If the cause is the game's own readback failing inside Remix, the shim cannot reach it: the
failing call is between SR3 and Remix, and its result is consumed by the game's compositor before
anything we hook sees a pixel. The remaining options are a Remix-side fix or an asset replacement,
not a shim change.

**Not decided.** Put to the user with performance as the alternative.

## 2026-08-29 - THE COMPOSITOR WORKS. The user's lead was right and my conclusion was wrong.

User, pushing back on "this is a Remix limitation": *"when i had vertex capture on, i could see my
character being textured. maybe only capture the characters and npcs and vehicles."*

Tested `rtx.useVertexCapture = True` together with `hiddenPassMode=0`:

    ATLAS SNOOP: 3 captured, 1 copies uploaded | 11.4 draws/frame bound a snooped copy
      atlas 1: 1280x768   mean  14.3
      atlas 2: 2048x1024  mean 182.7      <- THE CHARACTER TEXTURE
      atlas 3: 1024x512   mean 169.6      <- its normal map

**182.7 is exactly the mean of `8E4C8047F62D947A`**, the historical atlas decoded from the 08-13
captures showing skin, a face with makeup, arms, legs and the fleur-de-lis tattoo. Measured by
memcpy out of the game's own mapped memory, with no graphics API in between.

So SR3's compositor is not defeated by this stack at all. **The pixels are reachable, and the
"Remix cannot service the readback, nothing can be done" conclusion was wrong.**

### Why I got there, and it is not the missing-control failure

Every earlier reading was taken with `hiddenPassMode=2` in force. There are TWO independent skip
mechanisms and I only ever varied one at a time:

| | effect |
|---|---|
| `rtx.useVertexCapture=False` | Remix declines shader-driven draws |
| `hiddenPassMode=2` | THE SHIM skips ~2000 draws/frame before the device sees them |

The state in which the atlas was provably populated - captures 08-13..08-18 - had capture ON and
the shim not in skip mode. When capture=True was tested on 08-29 it was tested WITH the shim still
skipping, judged visually, with no instrument, and closed on "still black".

**A two-variable system tested one variable at a time, and the conclusion drawn as though the
space had been covered.** That is a different error from the missing controls and the wrong-time
measurements, and it is the one that produced a false "impossible".

### The bisect

Step 2 restores `hiddenPassMode=2` with capture still ON:

    mean stays ~182  -> vertex capture is the lever, the shim's skipping is irrelevant, and this
                        configuration ships as it stands
    mean falls to 0  -> the SHIM's skipping destroys the composite, and a targeted rule must spare
                        exactly those draws - which is what compositeToTexturePass attempted
                        blind, and can now be aimed with this mean as its falsifier

    sr3-rtx.ini  951deffebb739d8b6ce48b18447c7a82   hiddenPassMode=2, atlasSnoop=1
    rtx.conf     d7e9c7ec79bc7208166f00ba8a56565a   useVertexCapture=True

### Bisect step 2: the SHIM's skipping is the culprit, not vertex capture

    step 1  capture=True, hiddenPassMode=0   ->  atlas 2048x1024 CAPTURED, mean 182.7
    step 2  capture=True, hiddenPassMode=2   ->  atlas NEVER LOCKED at all

In step 2 the game did not composite the texture - the snoop saw no top-level lock on it, only the
unrelated 1280x768 one. So `hiddenPassMode=2` prevents the composite from happening, and vertex
capture is NOT the lever.

**That relocates the bug into our own code**, which is the best possible outcome: it is reachable,
and rule 1 already describes it - *a draw whose RESULT the engine reads can never be SKIPPED*. The
engine reads this one back as a texture, exactly as `compositeToTexturePass` argued in the first
place. That rule was right in principle, aimed at a plausible population, and shipped with no way
to tell whether it had worked. The atlas mean is that missing falsifier.

### Step 3

`screenSpaceMode=0` with `hiddenPassMode=2` unchanged: only the screen-space/composite quads pass
through, every other prepass stays skipped.

    atlas mean ~182  -> targeted fix found; everything else keeps its current disposition
    atlas mean 0     -> a different skip rule is responsible and the frame dump names it

Caveat on step 2: that run was short (~600 frames, log 22 KB) and its last report shows 0
draws/frame. The user did see their character, so a character was drawn, but the run should be
longer next time. Recorded so the conclusion is not treated as firmer than the sample.

### Also unresolved and important

In step 1 the atlas WAS captured at 182.7 and 11.4 draws/frame bound our uploaded copy - and the
user still reported black skin. Either that report referred to step 2, or the correct texture is
reaching Remix and something downstream still renders it black. The next run must be reported with
BOTH the number and what was on screen, because those two possibilities need completely different
work.

    sr3-rtx.ini  b9c935812c96d35c5830b1f26cc1e0b5   screenSpaceMode=0

### The atlas is PARTIALLY composited, and that narrows the target sharply

User, looking at the character texture: *"it looks like we broke the way the character renders and
the textures get baked because i see that the texture has some stuff in it but the skin part is
black."*

The 18:21 run agrees numerically:

    atlas 2: 2048x1024  mean 3.8 of 255      (fully composited reads 182.7)

Mostly black, some content. So the composite is MULTI-PASS and only some passes survive - which is
also the original report, *"only the top of the player is colored correctly"*. One layer is being
lost, not the whole bake.

**That run used `screenSpaceMode=2`** - its settings line says so, and there is only one such line
in the log, so it is not a stale entry. The game must have been launched before the step-3 ini
landed. Step 3 is therefore still untested.

### Reading the atlas instead of inferring from a mean

A mean says how much is missing, never WHICH region. The snoop already holds the pixels, so the
two largest captures are now written to `chartex-atlas1-WxH.raw` / `atlas2`. Bounded to two, ~6 MB
each, written once.

With the picture I can see exactly which part of the layout is black - skin, tattoos, garments -
and each corresponds to a different composite pass. That turns "a layer is missing" into "THIS
layer is missing", which is the difference between another bisect and a targeted rule.

    sr3-rtx.asi  21a11f8062a77d83650b0b3d68556437
    sr3-rtx.map  4c0c1e29df13c24ae9cff12ce8d39c98
    sr3-rtx.ini  b9c935812c96d35c5830b1f26cc1e0b5   screenSpaceMode=0 (STILL UNTESTED)

## 2026-08-29 - THE CHARACTER TEXTURE IS FIXED

    settings: ffp=1 hiddenPassMode=2 screenSpaceMode=0 ... | rtx.useVertexCapture = True

    ATLAS SNOOP: 3 captured, 1 copies uploaded | 26.0 draws/frame bound a snooped copy
      atlas 2: 2048x1024  mean 182.7 of 255
      atlas 3: 1024x512   mean 169.6 of 255

Dumped and looked at: `chartex-atlas2-2048x1024.raw` is the complete character texture - skin,
the face with makeup, the eye, arms, legs, the floral tattoos and the fleur-de-lis. Identical to
`8E4C8047F62D947A` from the 08-13 captures.

**`screenSpaceMode=0` is the fix**, with `hiddenPassMode=2` unchanged - so every other prepass is
still skipped and only the screen-space/composite quads pass through.

### What was actually wrong, start to finish

SR3 bakes each character into one 2048x1024 texture using a chain of screen-space composite quads.
The shim classified those as post/composite quads and, under `hiddenPassMode=2`, SKIPPED them.
The bake never ran, the game read back nothing, wrote zeroes into the texture its character shader
samples, and every character rendered black.

**It was rule 1 all along** - *a draw whose RESULT the engine reads can never be SKIPPED, only
hidden or passed through* - and `compositeToTexturePass` on 2026-08-28 was aimed at exactly this.
It failed only because it was aimed at steady-state frames, where no character is being composited,
and because there was no falsifier that could tell whether it had worked.

### Why it took so long, honestly

Four distinct failure modes, none of them about being wrong on the mechanism:

| failure | instance |
|---|---|
| measurement without a control | `usage=0x200` read as RENDERTARGET; blank LockRect readbacks read as "never written" |
| right quantity, wrong TIME | compositeToTexturePass measured in frames that could not contain a spawn; the fill probe hooked after the fill; the RT probe read during loading |
| a two-variable space tested one variable at a time | `useVertexCapture` and `hiddenPassMode` are independent skips; every test held one at its broken value and the conclusion was drawn as though the space had been covered |
| a diagnostic with no ceiling | the RT content probe froze the game twice |

The third produced a false "this is impossible, wait for Remix". **The user rejected that
conclusion and supplied the observation that broke it** - that the character had been textured with
vertex capture on. That was the decisive evidence, and it came from them, not from the tooling.

### Preserved

    configs/sr3-rtx.ini.WORKING-character-texture.bak
    configs/rtx.conf.WORKING-character-texture.bak

### Still to settle

- Is `rtx.useVertexCapture = True` still required, or does `screenSpaceMode=0` alone suffice? That
  matters: capture-off was adopted for good reasons and capture-on may reintroduce duplicate
  geometry and cost performance.
- What `screenSpaceMode=0` costs - it was set to 2 because a passed-through composite chain can
  paint the rasterised frame over the path-traced world.

### Is capture-on actually required? Isolating it

`screenSpaceMode=0` is only known to work WITH `rtx.useVertexCapture = True`, because both were
changed on the way here. They are independent mechanisms and the question matters: capture-off was
the unblock that took four sessions, and capture-on is expected to reintroduce duplicate geometry
and cost performance.

This run changes exactly ONE variable back - capture to False, `screenSpaceMode=0` and
`hiddenPassMode=2` untouched:

    atlas mean ~182  -> capture is NOT required. screenSpaceMode=0 alone is the whole fix, and the
                        project keeps capture-off with everything it bought.
    atlas mean 0/low -> capture-on IS required for the bake, and its costs have to be measured and
                        managed - z-fighting, duplicate geometry, performance.

The atlas mean is the falsifier and it is measured by memcpy out of the game's own memory, so
this is a decision made on a number rather than on how the frame looks.

    rtx.conf  80ea312f4ede5ccc9db3cfa7ec910aa6   useVertexCapture=False

### Capture-on IS required - isolated, one variable

    capture=True,  screenSpaceMode=0, hiddenPassMode=2  ->  atlas mean 182.7
    capture=False, screenSpaceMode=0, hiddenPassMode=2  ->  black again

So `rtx.useVertexCapture` is load-bearing for the bake, and with the earlier `ffp=0` result - shim
inert, capture off, still black - the whole picture resolves:

**TWO independent mechanisms were each breaking the bake.**

| mechanism | what it did |
|---|---|
| `rtx.useVertexCapture=False` | Remix declines the game's shader-driven draws, so the composite quads never execute |
| `hiddenPassMode=2` + `screenSpaceMode=2` | the SHIM skipped those same quads before the device saw them |

Either one alone is sufficient to produce a black character. That is exactly why every
single-variable test came back negative and why the space looked closed: each test fixed one and
left the other broken. It is the clearest possible illustration of why a two-variable system
cannot be bisected one variable at a time.

This also means capture-off, adopted over four sessions for good reasons, silently broke every
render-to-texture SR3 performs - not just characters. That cost is now known and was not before.

### The user's check: is the GAME itself being rendered correctly?

> *"can we make sure that the game fully and properly renders when we switch off the path tracing?
> i think we are breaking the game and that is resulting in what we are seeing."*

Right instinct, and Remix has the switch for it - from its own description, read out of
`.trex/d3d9.dll`:

    rtx.enableRaytracing - "Globally enables or disables ray tracing. When set to false the
                            original game should render mostly as it would without Remix."

Set False with the shim FULLY ACTIVE (`ffp=1`, `screenSpaceMode=0`, capture on), which separates
"are we damaging SR3's own output" from "is the path-traced image right". The reference already
exists: `d3d9.dll` renamed away with `ffp=0` renders the character correctly.

| result | meaning |
|---|---|
| game renders correctly | the shim and Remix's rasteriser leave SR3's own output intact; anything still wrong lives in the path-traced path |
| game renders wrongly | we ARE breaking the game, and whatever is broken here also explains the path-traced result |

    rtx.conf  b5dabca361d2ea023a6c7d338d3d6fbf   capture=True, enableRaytracing=False

### "Most of the map is not rendered" with ray tracing off - and the test was contaminated

User, with `rtx.enableRaytracing = False` and `ffp=1`: most of the map missing, same symptoms.

**That result cannot be read as damage, because it is what the shim is designed to do.** With
`hiddenPassMode=2` the shim skips ~2000 draws a frame precisely so Remix does not see duplicates,
and converts others to fixed function. With ray tracing ON that is invisible - Remix renders the
conversions. With ray tracing OFF the skipped draws are simply gone from the screen and the
converted ones rasterise as untextured fixed-function geometry.

I proposed that test with `ffp=1` without thinking through what the shim does to a rasterised
frame. The observation is real; the inference "we are breaking the game" does not follow from it.

### The clean form

`ffp=0` returns at the first line of `Classify`: nothing skipped, nothing converted, shim inert.
With ray tracing still off, that is the actual question:

| result | meaning |
|---|---|
| map renders correctly | the shim is not damaging SR3's output and Remix's rasteriser is sound; the fault is in what we DO to draws |
| map still broken | Remix damages SR3's rendering with the shim inert and ray tracing off - a much larger finding that reframes everything downstream |

Note the reference already collected: `d3d9.dll` renamed away with `ffp=0` renders correctly. So
this run isolates Remix's own rasteriser as the only remaining difference.

    sr3-rtx.ini  0f51f473be7bbe67eb8e5d9963315532   ffp=0

### Remix is NOT breaking the game

User, with `ffp=0` (shim inert) and `rtx.enableRaytracing = False`: *"the game now renders almost
entirely correct. its exactly as when we first install rtx remix. there is some zfigting on the
ground and water and shadows are invisible."*

So Remix's rasterised passthrough is sound, and the residual z-fighting and missing shadows are the
stock Remix baseline rather than anything this project introduced. **The shim is not damaging SR3's
rendering. The fault is entirely in what we DO to draws** - which is the tractable case, and one
whose fix is already identified.

That also retires the question the user raised, with an actual reference frame rather than an
argument.

### The working configuration, reassembled

    ffp=1  hiddenPassMode=2  screenSpaceMode=0  skipDeferredGBuffer=1  dedupAll=1  atlasSnoop=1
    rtx.useVertexCapture = True   rtx.enableRaytracing = True

Both halves of the bake are satisfied here: Remix executes the game's shader draws (capture on) and
the shim stops skipping the composite quads (`screenSpaceMode=0`). This is the state that measured
`atlas 2048x1024 mean 182.7` with the full character texture, tattoos and all, dumped and looked at.

    sr3-rtx.ini  6891bbc483dceb4bb8cbf757c6ac33a4
    rtx.conf     d7e9c7ec79bc7208166f00ba8a56565a

### What to expect, and what remains

Capture-on was turned OFF four sessions ago because it makes Remix reconstruct shader-driven draws,
which doubled geometry and caused z-fighting. Those costs are expected to return, and they are now
a KNOWN price for a working character rather than an unexplained regression. The tools for them
already exist and are proven - `dedupAll`, `skipDeferredGBuffer`, `hiddenPassMode` - and the atlas
mean is the guard that says immediately if any of them breaks the bake again.

## 2026-08-29 - "extract information, do not break the game's render"

User, after seeing the world drop out at some angles: *"we need to make sure that instead of
breaking the game render, we are just extracting information."*

That is the right architectural statement for this shim and it should have been the design rule
from the start. Every `Disp::Skip` removes a draw from SR3's own framebuffer and every `Convert`
replaces one. Both are destructive, and the `ffp=0` run proved the game renders correctly when
left alone.

### Where the current state stands

    ATLAS: 2048x1024 mean 182.7 | 27.5 draws/frame bound a snooped copy    <- CHARACTER FIXED
    FFP converted 384/frame (17.3%)                                        <- baseline is 31.6%
    SKIPPED entirely 1632/frame

The character texture problem is solved and holding. What is broken is world coverage: 17.3% is
inside the band the docs record for both black-world regressions (11-16%), which is exactly the
"most of the world stops rendering at some angles" being reported.

### The assumption the whole conversion machinery rests on has never been re-tested

From the top of YOUR-INSTRUCTIONS.md:

> "Without it, Remix does not path-trace SR3 at all - measured 2026-08-17 by running with ffp=0,
> which produced a completely unmodified rasterised game."

**That was measured on 2026-08-17 and never re-checked.** Vertex capture is the mechanism by which
Remix raytraces shader-driven draws, and its state has changed twice since. If capture-on lets
Remix raytrace the game directly, the shim need not convert or skip anything at all - the user's
principle becomes achievable outright, the black-world failure class disappears with the rules that
cause it, and the character stays fixed because the bake needs only capture-on.

Testing it costs one ini line, and it is the single highest-leverage unexamined assumption in the
project.

| result | consequence |
|---|---|
| the world path-traces | the conversion machinery is unnecessary; the architecture collapses to "capture on, skip nothing, snoop the atlas" |
| flat rasterised game | conversion IS required and the 08-17 measurement stands; the work is raising coverage from 17.3% toward 31% without skipping the composite quads |

    sr3-rtx.ini  259720dc7436ab1f799f7f407025c4f3   ffp=0, capture ON, raytracing ON

### The conversion machinery IS required - clean negative, and worth the run

`ffp=0` with `rtx.useVertexCapture = True` and ray tracing ON produced the **unmodified rasterised
game**. So vertex capture alone does not make Remix raytrace SR3, the 2026-08-17 measurement still
stands, and the shim's conversion is load-bearing.

That was the single largest unexamined assumption in the project and it is now re-verified under
current conditions rather than inherited from a week-old note.

Also observed in that state: low frame rate and wrong exposure. Both are Remix rasterising the
game with the shim inert, so neither is attributable to the shim, and neither is representative of
the path-traced configuration.

### On "render normally and just do not display it"

The right shape, and the shim already holds the pieces - `Hook_SetRenderTarget` records slot 0's
size and `HookDevice` records the back-buffer size, so "is this draw writing the screen" is
answerable. The obstacle is a recorded dead end, not a missing capability:

> **Skipping the composite quad.** It is the only draw that writes the back buffer, so the image
> freezes at a healthy 56 fps.

So the final composite cannot simply be dropped. Worth returning to, with that constraint in mind,
once coverage is healthy.

### The live problem: coverage, and it should be diagnosed from the failure

    FFP converted 384/frame (17.3%)    <- baseline 31.6%; both black-world regressions sat at 11-16%
    not converted: untextured 1110, other-camera 261, screen-space 82, vertex-format 71,
                   ortho 16, mirrored 15
    SKIPPED entirely 1632/frame

`untextured 1110/frame` dominates. But a steady-state average cannot say why the world vanishes AT
A PARTICULAR ANGLE, and this project has repeatedly gone wrong by reasoning from frame averages
about a condition that only occurs sometimes.

**The dump has to be aimed at the failure.** `captureKey=0x78` (F9) re-arms the frame dump and
`dumpFrame=60` makes it land about a second later, so pressing F9 while the world is missing
captures the frame that is actually broken. That is how the head bugs were finally caught.

    sr3-rtx.ini  62bcf336afbfb8d2ce283adf3fe26c57   ffp=1, capture ON, screenSpaceMode=0

## 2026-08-29 - turning capture back ON invalidated a set of decisions made FOR capture-off

User: *"why are you regressing in the project. we wrote all the facts in the project files. why not
see those?"*

Correct, and the answer was in this file. `rtx.useVertexCapture = False` was not one setting; it
was a PREMISE, and at least three decisions were taken because of it. Turning capture back on for
the character bake invalidated all of them at once, and they were carried forward unexamined.

| decision | why it exists (quoted from YOUR-INSTRUCTIONS.md) | status with capture ON |
|---|---|---|
| `forceOcclusionVisible=1` | *"SR3 does its own GPU occlusion culling and reads the depth prepass back ... Capture-off IS a global skip"* - 5330 draws/frame ON vs 1426 OFF, 73% of the game's own submission gone | **wrong.** The prepass is no longer skipped, the game culls correctly on its own, and forcing every query visible only makes it submit geometry it would have culled. The same section names the price: *"the game no longer culls anything, so more geometry is submitted than it would normally draw"* |
| `hiddenPassMode=2` | *"became viable"* because capture-off already removed unconverted draws | still required, and now doing that job alone |
| `skipDeferredGBuffer` returning PassThrough | *"pass-through submits them to the device exactly as before ... and with vertex capture off Remix never sees them"* | **the premise is gone.** With capture ON, Remix DOES see passed-through draws, so this no longer hides the character G-buffer copy from the path tracer |

Also recorded, and relevant to what to expect: *"no configuration with `rtx.useVertexCapture` ON
and no duplicate copies"*, and capture-off was *"the only mechanism that removes unconverted
draws"*.

### Changed

`forceOcclusionVisible=0`. It is a compensation for a condition that no longer exists, and it is a
direct candidate for the low frame rate - it forces SR3 to submit geometry it would correctly cull.

### Method note

This is the failure the "Read the code you already have before instrumenting" section already
describes: *"The occlusion-culling breakthrough was sitting in a comment above `hiddenPassMode` the
whole time."* Same section, same setting, and this time the fact was in the handoff document rather
than a comment. I proposed another run before re-reading what a change of premise invalidated.

    sr3-rtx.ini  e2a867a5e0a6dc87121deb97e06095bc   forceOcclusionVisible=0

## 2026-08-29 - reading the file instead of re-deriving it

User: *"again you are not checking everything we have already done and documented ... we did force
occlusion visible because everything that is in the view frustum gets culled because the game
thinks the camera is occluded, which it is."*

Correct on both counts.

**`forceOcclusionVisible=1` restored.** I had removed it on the reasoning that it only compensates
for capture-off being a global skip. That is not the whole reason: the game's occlusion queries
report the camera as occluded - which it genuinely is - so everything in the view frustum gets
culled. The setting is required regardless of capture state. Reverted immediately.

### What the file already says, and what it settles

From "The hiding problem", corrected 2026-08-21:

> "No texture-tag mechanism suppresses a vertex-captured draw. **There is no configuration with
>  vertex capture ON and no duplicates**, and looking for one is a dead end."

and from a real frame in the same section: **242 composite draws a frame**.

So with capture ON the ONLY mechanism that keeps a draw away from Remix is `Disp::Skip` - never
reaching the device. `screenSpaceMode=0` passes all 242 through, and every one is reconstructed by
Remix and painted over the path-traced world. **That is the reported "some angles in some areas
make most of the world stop rendering", and it was predictable from this file without a run.**

### compositeToTexturePass re-enabled, and the 08-28 revert was premature

The narrow form of what `screenSpaceMode=0` did wholesale: pass through only the composites whose
render target is NOT screen-sized, leaving the 2560x1440 resolve chain and the back-buffer
composite skipped. Measured on 08-28: 357 screen-sized untouched, 41 off-screen SKIP -> PASS.

It was reverted as *"fired exactly as measured and fixed nothing"*. **That verdict was reached with
`rtx.useVertexCapture = False`** - under which the bake is broken by capture alone, so no
disposition rule of ours could have fixed it. The rule was never given a condition in which it
could succeed.

Two independent causes again, and the same trap as before: a correct rule tested while the other
cause was still broken, then discarded.

### The configuration now

    ffp=1  hiddenPassMode=2  screenSpaceMode=2  compositeToTexturePass=1
    forceOcclusionVisible=1  atlasSnoop=1   rtx.useVertexCapture = True

Each element is doing one documented job: capture ON so the bake executes;
`compositeToTexturePass` so the off-screen composites survive the shim; `screenSpaceMode=2` so the
242-draw screen chain stays skipped and Remix cannot reconstruct it; `forceOcclusionVisible=1`
because the game culls the frustum otherwise.

    sr3-rtx.ini  c1c80e108eafa7a6e0e9f43d70336483

## 2026-08-29 - cameraOnly: the architecture that adds instead of removing

User: *"why cant we render everything properly off screen so anything that writes into composit
quads can still function but are not being shown to us. and then we path trace the stuff we need
without touching the game render."*

The answer is that we can, and the reason it was never tried is a measurement fault.

### The confound

`ApplyTransforms` is the only caller of `SetTransform(D3DTS_VIEW/PROJECTION)` for the scene, and
it is invoked from exactly ONE place - inside the conversion path. So `ffp=0` removes the geometry
conversion **and the camera** at the same time.

From docs/shader-map.md:

> "Remix reads worldToView/viewToProjection from SetTransform and NEVER from shader constants.
>  SR3 renders through shaders, so wherever it doesn't also set the fixed-function transforms,
>  Remix has no camera."

So the 2026-08-17 test behind the whole conversion architecture -

> "ffp=0 with capture on produces a rasterised game and no path tracing at all ... The
>  fixed-function conversion IS the path-traced world, entirely."

- could not distinguish **"Remix needs our converted geometry"** from **"Remix needs a camera, and
only the conversion path sets one"**. I repeated that exact test on 2026-08-29, with the same
confound, and drew the same conclusion. Two runs spent re-confirming an ambiguity instead of
resolving it.

### What cameraOnly does

Watches c28 (`projTM`, a FUSED view-projection) and c48 (`IR_World2View`, the pure view matrix) -
both invariant across all 7,276 shaders in the corpus - rebuilds the pair, and calls SetTransform
once per camera CHANGE. Then returns `Disp::PassThrough` for every draw, ahead of every rule that
can skip, hide or convert.

Geometry comes from `rtx.useVertexCapture = True`.

**Nothing is skipped, hidden or replaced.** This is the only configuration in the project that
cannot black out the world or break the character bake, because it never removes anything - which
is precisely the constraint the user has been stating.

    CAMERA ONLY ...: N camera changes applied over M draws, R refused for unusable constants

| result | consequence |
|---|---|
| the world path-traces | the architecture works; the skip/hide/convert machinery and its entire failure class become unnecessary |
| flat rasterised, N>0 | a camera is not sufficient, Remix does need the converted geometry, and the 08-17 conclusion stands on its own merits - now actually tested |
| N = 0 | c28/c48 never held a usable pair and the mode did nothing |

    sr3-rtx.asi  b28a5b2cdac60f7c4e59ad1b1beb7af5
    sr3-rtx.map  ba0d36e841dec43859523bf281d8e1ec
    sr3-rtx.ini  b61ac0737063df9f19dd8cfbbc779b89   cameraOnly=1

## 2026-08-29 - cameraOnly WORKS. The 08-17 premise is disproven.

User: *"the world does path trace but we have the issues that we had before like the shapes around
our character, the world not being textured right."*

**The world path-traces with the shim touching not one draw.** So the conclusion this entire
architecture was built on -

> "ffp=0 with capture on produces a rasterised game and no path tracing at all ... The
>  fixed-function conversion IS the path-traced world, entirely."   (2026-08-17)

- is wrong. Remix never needed the converted geometry. It needed a CAMERA, and `ApplyTransforms`
was only ever called from inside the conversion path, so every test of that premise removed both
at once.

That is the largest single correction in the project's history, and it was found by reading the
call sites rather than by running the game.

### The two remaining symptoms are the two problems the conversion was built to solve

Both already diagnosed in this file, neither requiring geometry conversion to fix:

| symptom | documented cause |
|---|---|
| "the world not being textured right" | *"Remix cannot read SHORT2 texcoords and DISCARDS them"* - `[rtx-interleaver] Unsupported texcoord buffer format (80)`, VkFormat 80 = `R16G16_SSCALED` |
| "shapes around our character" | the geometry prepass reaching Remix as a second surface |

### The texcoord fix now runs in cameraOnly, and it is not a modification of the render

`InstallFloatUV` factored out of the convert path and called for pass-through draws too. Checking
what it writes before reusing it mattered:

    // RAW short values widened to float, NOT scaled.
    dst[i * 2 + 0] = static_cast<float>(t[0]);

A SHORT2 declaration delivers exactly those raw values to a shader, so the game's own vertex
shader reads identical numbers through either declaration. **It is a change of representation, not
of the render** - which is what makes it legitimate under "do not touch the game render", and the
only thing cameraOnly does to a draw.

    sr3-rtx.asi  72b03b959498d6606963eecc10c8c524

## 2026-08-30 - reading SR3's own pass structure instead of inferring it

User: *"why cant we render everything properly ... we should know exactly how the game renders ...
start disassembling the exe."*

Done, statically, with radare2 + pefile. Results in `docs/engine-map.md`; the operative findings:

**SR3 is a command-buffer renderer with a dedicated render THREAD.** The game thread produces
command blocks into a ring; a consumer at `0x0049DE20` takes a 24-byte descriptor, sets the block
bounds, and runs an opcode loop at `0x0049DED2`:

    opcode = *readPtr;  if (opcode < 74)  table[opcode]();   readPtr advanced by the handler

The dispatch table is 74 entries at `0x013509F8`; draws are opcodes 37, 41, 42, 43; a draw command
is 28 bytes. `SetRenderTarget` is op 9. **Ops 55/56/72 call `GetRenderTargetData`** - the engine
issues GPU->CPU readbacks as render commands, which is the mechanism behind the character bake.

**All four draw handlers test one global byte, `0x03395EA4`**, and skip the draw when it is set.
It is written at `0x0047CEBB` as `sete cl` from a visibility test at `0x0047C800`. So SR3 decides
per object whether it is visible and disables drawing with a single flag. That is the engine's own
culling, at instruction level, and it is exactly what the user said: everything in the frustum gets
culled because the game believes the camera is occluded. **`forceOcclusionVisible` is not a
workaround for capture-off; it is what keeps this flag clear.** Removing it on 08-29 was wrong.

**This also explains the multi-threading** the project measured but never accounted for - "the game
locks vertex buffers from more than one thread", 272 locks a frame, proven by a crash dump. There
is a producer thread filling buffers and a consumer thread submitting them.

### The classification axis this project has been approximating

Every D3D9 call the shim sees comes from a handler running inside one command block, and the block
is identified by `[0x02E5D648]` - readable at any draw. Two draws in the same block are in the same
submission unit. That is a fact the engine writes down, not an inference from samplers, render
targets or shader output signatures.

`screenSpaceMode`, `skipDeferredGBuffer`, `compositeToTexturePass` and every prepass test are each
attempts to reconstruct that information from the flattened stream. Three of them have blacked out
the world.

### Shipped: the reader

Read-only, two pointer reads per draw. Guarded the way engine-map.md's own lesson demands - *"verify
a static map against the live process before writing to it"* - after that document recorded a base
mismatch caught by exactly such a check:

- the host module must be `SaintsRowTheThird.exe`;
- its base must be `0x00400000` (`RELOCS_STRIPPED`, no `DYNAMIC_BASE`, confirmed live 08-15);
- every address must sit in a committed readable page, by `VirtualQuery`.

Any failure disables the feature permanently and says so. Nothing is written.

    COMMAND BLOCKS ...: N blocks/frame over F frames, D draws attributed
      this frame: N blocks
        block 0x........ :  1234 draws  (opcodes a..b)
      engine draw kill-switch (0x03395EA4) was SET on K draws

    sr3-rtx.asi  55b5a6afe4d463e8eaef76a05fbb2b85
    sr3-rtx.map  a826a8256961cc1308a197242e927145

### The crash was a Vulkan OOM inside Remix, and cameraOnly is why

    err: Size: 33554432                          (a 32 MB allocation failed)
    err: Heap 1: 33536 MB allocated, 32970 MB used, 31571 MB total

No `sr3-rtx-crash.dmp` was written because the fault was not in our process. **33.5 GB allocated
against a 31.5 GB heap.**

Structural, not a leak: `cameraOnly` skips nothing, so with vertex capture on Remix builds geometry
for EVERY draw in the game - 2,320 a frame including every prepass, composite and UI quad. The
capture-off section already measured capture-on at 5,330 draws/frame reaching Remix; cameraOnly
removes even the filtering that remained.

**So the architecture is right and incomplete.** "Add a camera, touch nothing" gives a path-traced
world, but Remix cannot be handed the entire submission stream. It needs selective filtering of
draws Remix does not need - without removing draws the ENGINE reads back, which is the constraint
rule 1 states and which the command-block listing exists to satisfy.

Also of note: **no "Unsupported texcoord buffer format (80)" warning appeared in that run at all**,
only `color0 format (37)`. Whether that is because `InstallFloatUV` converted them or because
vertex capture never reads the input declaration is not yet established.

### Two bugs of mine in that build

**1. The command-block listing was always empty.** `ResetCommandBlocks()` was called near the top of
`Hook_Present` and the frame report runs LATER in that same function, so the listing was wiped
before it printed - "this frame: 0 blocks" while the game submitted 2,320 draws a frame. Same class
as the destructive bucket-pop caught in the DIFFUSE_COLOR report before shipping; this one shipped.
Now cleared on the first draw of a frame instead.

**2. `InstallFloatUV` ran on every pass-through draw in cameraOnly** - 2,320 a frame and 6,252 D3D9
vertex buffers created across the 32->64-bit bridge, against ~700 on the convert path. A
contributor to the OOM, though not the dominant one. Moved behind `cameraOnlyFloatUV`, default OFF.

    sr3-rtx.asi  2e569b585ba5e7ddff87e75f7cf0b778
    sr3-rtx.map  f0234595fd05c13ddf3307c83c8db5f1
    sr3-rtx.ini  b0fcafc70dcee0f515fe27f217fbcb3f

### Lookahead built - rule 1 becomes a lookup instead of an inference

The three remaining ring-descriptor fields carry no pass label (two are write-only globals with a
single reference each; the third is a completion callback), and the blocks are 16 KB pool chunks
0x4000 apart - allocation, not passes. That line is closed.

What the command buffer DOES offer is lookahead: the block is fully built before the render thread
starts it. Per-opcode sizes were extracted statically from each handler's own advance of the read
pointer - most constant, six computed from a count inside the command - and are tabulated in
docs/engine-map.md.

So `GetRenderTargetData` (ops 55/56/72) is now VISIBLE AHEAD OF EXECUTION. A block containing one
is a block the engine reads back, and nothing in it may be skipped. That is rule 1 stated by the
engine rather than guessed at from samplers and render targets - the guessing that produced three
black-world regressions.

    BLOCK SCAN ...: N blocks walked, C commands | D draws, T SetRenderTarget, R GetRenderTargetData
      X blocks contain a READBACK - those may never be skipped | Y walks stopped early

`Y` is the honesty check: unresolved opcodes (7, 43, 56, 60, 72) stop a walk rather than guess a
size, because a wrong size desynchronises every command after it. If more than half the walks stop
early the report says so and the counts are lower bounds.

    sr3-rtx.asi  f9b98e8550f46104d2f2cf5a6a3e7573
    sr3-rtx.map  5d0caaabca8f98106c6205b78caa369f

## 2026-08-30 - session close: what this session established

Documented across `docs/YOUR-INSTRUCTIONS.md` (rewritten "STATE, 2026-08-30" section),
`docs/engine-map.md` (two RE addenda) and `docs/HANDOFF-PROMPT.md`.

### Findings, in order of how much they change the project

1. **The founding premise was false.** "The fixed-function conversion IS the path-traced world,
   entirely" (2026-08-17) could not have been established by the test that produced it:
   `ApplyTransforms` is the only caller of `SetTransform` for the scene and runs solely inside the
   conversion path, so `ffp=0` removed the camera along with the geometry. Remix reads the camera
   only from `SetTransform`. Given a camera plus vertex capture, **Remix path-traces SR3 with the
   shim converting nothing** - confirmed on screen.

2. **The character texture is solved, with two independent causes.** SR3 bakes each character into
   one 2048x1024 X8R8G8B8 DYNAMIC texture by locking each mip level's SURFACE. Capture-off alone
   blacks it out; the shim skipping the composite quads alone blacks it out. Testing one at a time
   is why it looked impossible for two days. Verified at `mean 182.7`, dumped and looked at.

3. **SR3's renderer is reverse-engineered.** Command-buffer architecture with a dedicated render
   thread, 74-opcode dispatch table at `0x013509F8`, per-opcode command sizes, the dispatcher at
   `0x0049DE20`, and one global byte `0x03395EA4` that disables all four draw commands - the
   engine's own culling, and the instruction-level proof that `forceOcclusionVisible` is
   load-bearing. Ring descriptors carry no pass identity; that question is closed.

4. **Lookahead exists now.** A command block is fully built before execution, so
   `GetRenderTargetData` is visible ahead of time and rule 1 becomes a lookup.

### Open

- `cameraOnly` OOMs Remix (33.5 GB) because it filters nothing. The block scan's readback count is
  the candidate filter and has not been read yet.
- Whether the SHORT2 texcoord conversion is needed in `cameraOnly` - no format-80 warning appeared
  in that run, but the cause is unconfirmed.
- Performance, untouched all session.

### Not to be repeated

`rtContentProbe` froze the game twice (default OFF, do not enable). `d3d9.allowDiscard=False` and
`d3d9.apitraceMode=True` are verified-parsed negatives. Restoring the 08-18 rtx.conf changed
nothing. With `d3d9.dll` renamed away the game renders correctly, so the game, save, character mods
and display settings are all cleared.

State is deployed and hash-verified; backup at `D:\SR3RTXREMIXCOMP-backup-2026-08-29`.


## 2026-08-30 15:47-15:52 - cameraOnly run with the block walker: the readback filter is empty, the render-target filter is not

Run: ~10,800 frames, ~5 minutes, clean exit. NO OOM - the previous 33.5 GB blowup was
`cameraOnlyFloatUV` creating 6,252 float-UV buffers on pass-through draws. Gated off, it is gone.
Remix log has no memory diagnostic at all this run; exit is the normal dxvk teardown message.

### The block walk is validated
    200,839 blocks walked | 132,204,771 commands | 0 walks stopped early
`CommandSize()` covers every opcode SR3 actually emits in gameplay. The walk never desynchronised.
The unresolved opcodes (7, 43, 56, 60, 72) are never emitted during play, so they cost nothing.
This makes the lookahead reader trustworthy as a source of facts, not just a probe.

### FINDING: the engine never reads back a render target during gameplay
    0 GetRenderTargetData (ops 55/56/72) in 132,204,771 commands
    0 blocks contain a READBACK
"A draw whose result the engine reads can never be skipped" is TRUE and VACUOUS. Nothing is read
back, so the rule constrains nothing and cannot be used as the cameraOnly filter. Closed by
measurement. Do not re-derive this.

Corollary, and this is the useful half: the character atlas is NOT built by readback. The fill probe
this same run shows texture LockRect 2, SURFACE LockRect 36, UpdateTexture 0, UpdateSurface 0,
StretchRect 0, ColorFill 0. It is a CPU->GPU upload per mip surface, then sampled on the GPU. The
constraint on skipping is GPU-side sampling, not CPU readback.

### FINDING: the engine declares its own pass structure, ahead of execution
    797,612 SetRenderTarget (op 9) = ~74 per frame
Op 9 is 0x0C bytes and sits in the same walk. Because a block is fully built before the render
thread executes it, every draw can be attributed to the render target the ENGINE names for it,
before it runs. That is the pass identity that screenSpaceMode, hiddenPassMode, skipDeferredGBuffer
and every prepass heuristic have been reconstructing from samplers and shader output counts - each
of which has blacked out the world at least once. This is the replacement for all of them.

### Performance in cameraOnly
    frame 27.0 ms avg (37 fps), worst 154 ms | shim 0.49 ms avg = 1.8% of frame, worst 1.9 ms
    inside real Present 4.56 ms avg | 93 frames >=33 ms of 600, 2 >=100 ms
Down from ~57% of frame time when converting. Converting nothing is nearly free; the remaining cost
is Remix and the game.

### CORRECTION to docs/engine-map.md
0x02E5D644 holds the PREVIOUSLY dispatched opcode, not the current one. Evidence: across every
block listing, the opcode range seen during draws is 20..35 (SetStreamSource 35,
SetVertexShaderConstantF 24, ops 20/34) and NEVER 37/40/41/42, which are the draw opcodes
themselves. The global is updated at the end of a loop iteration.

### Other state this run
- engine draw kill-switch 0x03395EA4 SET on 0 draws - forceOcclusionVisible is holding.
- blocks are 16 KB apart (0x4000) and carry 46..302 draws each; ~22-32 blocks/frame.
- atlas snoop captured 6 atlases with real pixels (2048x1024 mean 182.7, 1024x512 mean 169.6) but
  uploaded 0 copies and 0 draws bound one. Expected: in cameraOnly nothing is converted, so the
  game binds its own atlas and the snoop is correctly inert.
- VB LOCK HOOK 77,829 invalidations deferred, 22,949 coalesced, 0 allocations failed.

## 2026-08-30 - the black world at certain angles is the CAMERA, not the skip rules

### The elimination
User reported, for the cameraOnly run above: world goes black at some angles. In cameraOnly the
shim converts nothing, skips nothing, hides nothing - Classify returns PassThrough ahead of every
rule and the report confirms it: converted 0/frame, skipped 0/frame, hidden 0/frame, prepasses
hidden 0/frame, demoted 0/frame. The shim removed NOTHING for ~10,800 frames and the world still
went black.

So the classification rules are eliminated as the cause. Four sessions blamed them - screenSpaceMode,
hiddenPassMode, skipDeferredGBuffer, the prepass rules - and this is the first run that could tell
the difference, because it is the first one in which none of them executed.

### The cause
Setting the camera is the only thing the shim does in cameraOnly, and it was doing it 11.5 times a
frame: 124,566 DISTINCT view/projection pairs over 10,800 frames, pushed into SetTransform
indiscriminately in draw order. Shadow cascades, the water reflection, cubemap faces, the main view.
Remix's CameraManager has to choose one. Its log shows the strain from its own side:

    warn: [RTX] CameraManager: FOV of a camera changed between frames
    info: Camera cut detected on frame 1416 / 2471 / 2508

At some angles it settles on a camera whose frustum does not contain the scene.

### The fix - cameraMainViewOnly=1, built and deployed, NOT YET RUN
Three discriminators, each from state the ENGINE sets, none inferred from shader names or
render-target indices (both recorded dead ends):
  1. SIZE - render target is the back buffer's size. Shadow atlases, the light buffer and the
     half-res post chain are not. g_rt0Width/Height already carry this from the SetRenderTarget
     hook, so no extra bridge round trip.
  2. PERSPECTIVE - IsPerspective() on _34/_44. Full-res post and UI quads are orthographic.
  3. HANDEDNESS - RotationDeterminant(view) > 0. A water reflection mirrors and flips it; this is
     already documented and already used by skipMirrored for the same reason.

Failure mode chosen deliberately: reject everything in a frame and nothing is set, so Remix keeps
the last camera. A stale camera lags; it does not black out.

### The control ships with the change
Per-frame census of every distinct camera: RT size, perspective/ortho, mirrored/upright, draw
count, accepted/declined. A gate that picks the WRONG camera and one that picks NONE are both a
black world from the outside, and only the listing separates them. cameraMainViewOnly=0 restores
push-everything for an A/B without a rebuild.

Census reset is keyed on g_frames at the top of ApplyCameraOnly - the first draw of a frame, NOT
Present. Resetting in Present wipes the listing before the report prints; that bug shipped once
already for the command-block listing and cost a session of "0 blocks" readings.

### Deployed
    sr3-rtx.asi  7975119e177575cb3d879540751aa7ca
    sr3-rtx.map  73b7dbe9aedf72212de5778935f728ff
    sr3-rtx.ini  698d80fef8500a0c71270d18fc972e59
    ffp=1 cameraOnly=1 cameraMainViewOnly=1 hiddenPassMode=2 screenSpaceMode=2
    compositeToTexturePass=1 forceOcclusionVisible=1 atlasSnoop=1 rtContentProbe=0
Source backup: src/sr3-rtx/sr3rtx.cpp.before-main-view-camera


## 2026-08-31 - the camera gate WORKS; and Remix's own binary closes the tagging route for good

### The camera gate: verified, one camera accepted
User ran the cameraMainViewOnly build. The census, one frame:

    MAIN-VIEW GATE ON: accepted 9,581,420 draws | declined - not back-buffer sized 2,190,420,
                       orthographic 0, mirrored 0
      cam 0: rt  128x128  ORTHO       upright      3 draws -> declined
      cam 1: rt 2560x1440 perspective upright   5613 draws -> ACCEPTED
      cam 2: rt 4096x4096 ORTHO       upright      8 draws -> declined
      cam 3: rt 4096x4096 ORTHO       upright      7 draws -> declined
      cam 4: rt 4096x4096 ORTHO       upright    165 draws -> declined
      cam 5: rt  512x288  perspective MIRRORED   765 draws -> declined
      cam 6: rt  400x288  perspective upright     14 draws -> declined
      cam 7: rt  400x288  perspective MIRRORED    22 draws -> declined
      cam 8: rt  400x288  perspective MIRRORED   207 draws -> declined

Exactly one camera accepted, and it is unambiguously the right one. The "which camera" problem is
solved. Note the by-product: this is also a complete, engine-stated PASS TAXONOMY BY RENDER TARGET -
shadow cascades are 4096x4096 ortho, reflections are 512x288/400x288 mirrored, the scene is
2560x1440 perspective. 18.6% of all draws are off-screen passes.

### But the user's symptom moved to the geometry
"camera only renders everything. it also renders all the geometry from shaders. so just like
before, the camera is blocked and the character is surrounded with shapes."

Correct, and it follows directly from vertex capture being ON: Remix reconstructs EVERY draw it is
handed, including shadow-cascade geometry, reflection geometry, light volumes and full-screen
composite quads. A full-screen quad reconstructed as world geometry sits directly in front of the
camera - "the camera is blocked". Light volumes and prepass geometry around the player - "shapes
around the character".

### RE result: Remix declines a draw for exactly THREE reasons
Scanned the 182 MB `.trex/d3d9.dll` for its compatibility messages. The complete set:

    Trying to raytrace an occlusion query. Ignoring.
    Trying to raytrace an unsupported primitive topology [N]. Ignoring.
    Trying to raytrace but not detecting a valid camera.

There is no "this draw is tagged, do not raytrace it". This CONFIRMS FROM THE BINARY what runs
58-61 established empirically: no texture tag suppresses a vertex-captured draw. `ignoreTextures`
visualises as a pink/black checkerboard, `hideInstanceTextures` does not apply, clearing all eight
stages does nothing. Both directions now agree. **Do not spend another run looking for a tag that
hides geometry - it does not exist.**

Full texture-category list in the runtime, for the record: `ignoreTextures`, `hideInstanceTextures`,
`ignoreTransparencyLayerTextures`, `uiTextures`, `worldSpaceUiTextures`,
`worldSpaceUiBackgroundTextures`, `skyBoxTextures`, `terrainTextures` (via `rtx.terrain*`),
`decalTextures`, `dynamicDecalTextures`, `nonOffsetDecalTextures`, `singleOffsetDecalTextures`,
`particleTextures`, `beamTextures`, `animatedWaterTextures`, `lightmapTextures`,
`playerModelTextures`, `playerModelBodyTextures`, `raytracedRenderTargetTextures`,
`rayPortalModelTextureHashes`. 210 `rtx.*` options total.

### RE result: D3DPERF markers are a dead end
The exe imports D3DPERF_BeginEvent/EndEvent/GetStatus/SetOptions but has ZERO call sites for all
four (checked every `call dword [IAT]` in .text against the IAT addresses). Pulled in by a static
lib, never called. See engine-map.md.

### RE result: the full render-pass class tree, from RTTI
`rl_d3d_shadow_render_pass`, `rl_d3d_base_render_pass`, `rl_d3d_xray_render_pass`,
`rl_d3d_motion_blur_mask_render_pass`, `rl_d3d_batched_pass`, `rl_composite_pass`,
`rl_d3d_render_to_texture_pass` plus eight renderers - all with resolved vtables (engine-map.md).

The catch, and it is decisive: **passes run on the MAIN thread and record commands; the render
thread executes them.** At a draw hook the stack holds the dispatcher, not the pass. So the class
tree cannot serve as per-draw identity without pass identity being carried into the command stream,
and the ring descriptors were already traced and carry none. The render TARGET is what is actually
available per draw - and per the census above, it is sufficient.

## 2026-08-31 - the magenta/black surfaces: 16 dead marker-era hashes were painting real textures

User's lead: "before turning off vert capture we observed the game is rendering the verts correctly
but is also overlaying the magenta texture. now the magenta texture is black. i think we assigned
that texture to the world."

Correct, and it was a live config bug rather than an architecture problem.

### What was there
    rtx.ignoreTextures      = 16 hashes
    rtx.hideInstanceTextures = 1 hash (-0x978271113F293CE4, also present in ignoreTextures)
Per Remix's own description, every object using an ignored texture renders as the IGNORED MATERIAL -
a pink/black checkerboard, which reads as magenta at distance and as black under a dark exposure.
Six of the sixteen hashes have NO recorded reason anywhere in the docs. Collisions were rife:
-0x196FBE2CAB23CB16 was in worldSpaceUi + ignore + particle; -0x6AC1CFA187D094AA in worldSpaceUi +
ignore; -0x978271113F293CE4 in ignore + hideInstance.

### Why it survived the 2026-08 stale-tag cleanup, and why that reason is now void
That cleanup removed ten stale category lines but DELIBERATELY kept rtx.ignoreTextures, recorded as:

    "carries the marker hash 0x978271113F293CE4 ... It is load-bearing for ~1,863 marked draws a
     frame; removing it puts the magenta prepass shells back on screen."

That is no longer true. The marker subsystem was deleted when hiddenPassMode=2 became viable, and
the current log proves the shim marks nothing at all:

    MARKED with the marker texture 0/frame (refused as unsafe 0/frame, 0 SetTexture calls/frame)

So the list marks no draw of ours; all 16 entries are REAL GAME TEXTURES rendering as ignored
material. Note also the sign flip: run43's log has 0x978271113F293CE4 POSITIVE, the deployed conf
has it NEGATIVE - different 64-bit values - so the retained entry may not even be the marker.

### Change
Both lines removed. Config only, no rebuild, instantly reversible.
    backup  configs/rtx.conf.before-dead-marker-untag.bak
    before  d7e9c7ec79bc7208166f00ba8a56565a
    after   421b35520b662159df313db2a6df3205   (master and deployed both)

Deliberately left alone, to keep this a single-variable test: skyBoxTextures, worldSpaceUi*,
particleTextures, and rtx.ignoreLights (-0x645CF1DD53FF6357, part of the same smear, but lights
currently work).

### The general lesson, which has now bitten twice
A tag added for a mechanism that is later deleted does not become inert - it keeps acting on
whatever textures it names. When a subsystem is removed, its CONFIG residue must be removed with
it. The 2026-08-12 entry records the same failure mode ("user's in-game texture clicking silently
tagged 100+ textures ... invisible effects, felt like nothing happened").

## 2026-08-31 - BASELINE RESTORED. cameraOnly cannot produce a judgable image, and I kept asking for runs from it

User: "i cannot check any of that. things are either rendered wrong or are blocking the camera...
basically in a similar position to how we started the project."

That is a process failure on my side, not a new symptom. cameraOnly=1 passes EVERY draw through
with vertex capture ON, so Remix reconstructs shadow geometry, reflection geometry, light volumes
and gameplay collision shapes all at once. It was built to test ONE architectural claim (does Remix
need only a camera - yes) and it answered that. It was never a configuration anyone could look at
and judge a texture change from. I left the project sitting in it and then asked for evaluations
that the image could not support.

RULE: an experimental mode that degrades the image must be reverted to the last verified-good
config as soon as it has answered its question. Do not stack a second experiment on top of a
configuration whose output cannot be assessed.

### Restored
configs/sr3-rtx.ini <- configs/sr3-rtx.ini.WORKING-character-texture.bak
(previous experimental ini preserved at configs/sr3-rtx.ini.before-baseline-restore.bak)

    ffp=1 convertSkinned=1 hiddenPassMode=2 screenSpaceMode=0 compositeToTexturePass=0
    atlasSnoop=1 forceOcclusionVisible=1 injectLights=1 rtContentProbe=0
    cameraOnly absent -> 0.  cameraMainViewOnly is inert when cameraOnly=0 (IsMainSceneCamera is
    reached only from ApplyCameraOnly), so the new build is safe to leave deployed.

screenSpaceMode=0 is the recorded fix for the black character skin - composite quads are not
skipped at all - which is why compositeToTexturePass=0 in this snapshot is coherent rather than a
regression: it is the narrower gate for the same protection and is redundant here.

### The single change on top of the baseline
rtx.conf keeps the dead-marker untag from earlier today: rtx.ignoreTextures (16 hashes) and
rtx.hideInstanceTextures (1) removed. Verified by diff against
configs/rtx.conf.before-dead-marker-untag.bak - EXACTLY those two keys, nothing else. An earlier
claim in this session that four keys were dropped was wrong; it came from diffing against the
WORKING snapshot rather than the untag backup. worldSpaceUi* are present, 77 lines.

### Deployed
    sr3-rtx.asi  7975119e177575cb3d879540751aa7ca   (unchanged - no rebuild)
    sr3-rtx.ini  b9c935812c96d35c5830b1f26cc1e0b5
    rtx.conf     421b35520b662159df313db2a6df3205
So this run varies exactly one thing against a state whose character texture was previously
verified: the removal of the dead ignore tags.

## 2026-08-31 - the untag WORKED, and the remaining wrong texturing is ours: DYNAMIC vertex buffers

User: "player skin is textured. the top is textured right but not shoes and other stuff. underwear
has extra geometry. the wrong texturing is also reflected in with ray tracing off as well."

### Confirmed: the dead ignore tags were painting the character
Skin went from black to textured with NO code change - only the removal of rtx.ignoreTextures (16
hashes) and rtx.hideInstanceTextures (1). The 2026-08 decision to keep that list, on the grounds
that it was "load-bearing for ~1,863 marked draws a frame", was carrying 16 real game textures into
Remix's ignored material long after the marker subsystem that justified it had been deleted.

### "Also with ray tracing off" is the decisive clue
It rules Remix out entirely. Whatever is wrong is in the RASTERISED draw, which means it is ours.

### Cause, from the run's own counters
    SHORT2 texcoords converted to a float2 stream: 314 draws/frame | failures: 83737 convert
    conversion failures by reason: 0.0 layout, 5.8/frame DYNAMIC source, 0.0 desc, 0 create, 0 lock

EVERY failure is one branch: UvBufferFor() refuses any source vertex buffer carrying
D3DUSAGE_DYNAMIC. Those draws keep raw SHORT2 texcoords, and SHORT2 is NOT normalised - a stored
16384 is a UV of 16384, so the texture tiles into noise. Dynamic vertex buffers are what the engine
uses for per-frame-written character parts: cloth, shoes, accessories (CLOTH: 21.0 draws/frame in
the same run). "Top textured right, shoes not" is precisely that split.

Note the counter design failure that hid this: five reasons were tracked but only the TOTAL was
printed on the main line, and the breakdown line was conditional. 83,737 read as a scary aggregate
for months when it was one branch refusing 5.8 draws a frame.

### Both objections in that refusal were already answered elsewhere in the file
    "a DYNAMIC buffer must not be read back"  - correct, and the fix does not. RegisterSnoop /
        SnoopCopy read the copy the VB lock hook already makes of the game's OWN write. Exactly
        the mechanism the morph stream has used since 2026-08-28.
    "a conversion of it would be stale immediately" - InvalidateUvBuffers(vb) is ALREADY wired
        into Hook_VBUnlock (line ~1642), so the converted copy is dropped the moment the game
        refills the buffer. The staleness guard existed; this path predates it.

### Change: convertDynamicUV (default ON)
DYNAMIC sources are now registered with the snoop and converted from the snooped copy. The first
draw on a newly registered buffer finds nothing and WAITS - counted as g_uvDynWaiting, deliberately
NOT as a conversion failure, so "we cannot do this" and "we have not seen the data yet" can never
again be summed into one number. New report line:

    DYNAMIC sources (ON): N converted from the snooped copy, M waiting for the game's next write
    ... plus an explicit warning if registered-but-never-filled, which would mean the snoop is not
        seeing these locks and the whole approach is wrong.

Set convertDynamicUV=0 in sr3-rtx.ini to restore the old refusal with no rebuild.

### Deployed
    sr3-rtx.asi  bef8135eafa77cc6f1c1e00c178fd370
    sr3-rtx.map  5845ffce1c2da94d46e43107d775e3e2
    sr3-rtx.ini  b9c935812c96d35c5830b1f26cc1e0b5   (unchanged baseline)
    rtx.conf     421b35520b662159df313db2a6df3205   (unchanged, keeps the untag)
Source backup: src/sr3-rtx/sr3rtx.cpp.before-dynamic-uv

### STILL OPEN - not addressed by this change
"underwear has extra geometry". Extra geometry is the duplicate-draw problem: with vertex capture
ON, every draw the shim does NOT convert is reconstructed by Remix alongside the converted copy.
This run: 430/frame converted of 1,930 (22.3%), and "not converted: untextured 987, vertex-format
99, other-camera 164, screen-space 49". Do not conflate it with the UV fix - different cause,
different fix.

## 2026-08-31 - dynamic UV attempt 1 FAILED as designed, and the warning caught it in one run

    DYNAMIC sources (ON): 0 converted from the snooped copy, 24772 waiting for the game's next
                          write  <- registered but NEVER filled: the snoop is not seeing these locks
    failures: 0 convert   (was 83737)

The refusal is gone but nothing converted. The warning that shipped with the change named the
failure outright, so this cost one run and no guessing. That is the counter design paying for
itself - contrast the lumped 83,737 that hid the original branch for months.

### Why it failed - read out of the snoop's own comments, not from another run
`SnoopCopy(vb, 0, whole declared buffer, &fresh)` was wrong twice over:

1. SIZE. Hook_VBUnlock grows c.data only to `offset + size` of what the game actually wrote -
   its own comment: "Grows to what the game actually writes, which is a fraction of most buffers'
   declared size." So a whole-buffer request trips `offset + len > c.data.size()` and returns
   false immediately.
2. FRESHNESS. Hook_VBLock clears EVERY fresh bit on D3DLOCK_DISCARD. After a partial refill most
   of the buffer is legitimately stale, so requiring all-fresh over the whole buffer can never
   pass for a ring the game refills a slice at a time.

Both facts were already written down in this file. I asked for the whole buffer because the static
path does; a dynamic buffer is not the same object.

### Attempt 2 - convert the SLICE the draw reads
- Range comes from g_curDrawFirstVertex / g_curDrawVertexCount, the same pair the skinning path
  uses via VertexRangeFits.
- SnoopCopy(vb, first*stride, count*stride) - only those bytes, freshness required only there.
- UvKey now takes (first, count), used for DYNAMIC sources only. A dynamic buffer is a RING: two
  draws in one frame legitimately read different vertices from it, and keyed on the pointer alone
  the second draw would be served the first's coordinates. That is exactly the bind-pose cache
  defect ("each character had the wrong head"), whose key had to become
  {vb, offset, stride, minIndex, count}. Static buffers pass 0/0 and keep the old key.
- Entries stay indexed per vertex as stream 0 is (the uv stream binds at offset 0 with its own
  stride), so entry i always means stream-0 vertex i. Unfilled entries are zeroed rather than left
  as whatever the allocator returned.

### Deployed
    sr3-rtx.asi  748c2134fd95ec8efaa8c42879adab23
    sr3-rtx.map  c6dd47323484fa0d82cc6c3457fcf8a5
    ini / rtx.conf unchanged (b9c935.../421b35...)
Source backup: src/sr3-rtx/sr3rtx.cpp.before-dynamic-uv-slice

Falsifier unchanged and still explicit: "N converted, M waiting", with the never-filled warning if
registration still yields nothing.

## 2026-08-31 - dynamic UV attempt 2 WORKS at the mechanism level

    DYNAMIC sources (ON): 47323 converted from the snooped copy, 21 waiting
    SHORT2 ... 354 draws/frame | 49589 buffers converted (3.3 MB held, 1 flush,
                                 47294 invalidated by the game) | failures: 0 convert

The 21 "waiting" are the initial registrations, exactly as the design predicts (a newly registered
buffer has no snooped data until the game next writes it). 0 conversion failures, down from 83,737
- every one of which was the DYNAMIC refusal.

Invalidation is working and is load-bearing here: 47,294 invalidations means each dynamic slice is
rebuilt as the game rewrites it, which is the whole reason the cached conversion is not stale.

### Cost, recorded now so it is not discovered later as a mystery
    frame 28.0 ms (36 fps) | shim 15.53 ms = 55.4% of frame, worst 171 ms
    134 frames >=33 ms, 8 >=100 ms of 600 | inside real Present 3.94 ms
    uv buffer arena flushed at 48 MB (flush #1)
55% is in line with the conversion path's historical ~57%, so this change did not obviously make
it worse - but it does create and destroy thousands of D3D9 vertex buffers a second across the
bridge. If this needs optimising the fix is buffer REUSE (keep the allocation, refill it) rather
than create/release per invalidation. Not done yet: correctness first.

## 2026-08-31 - UV fix works but was NOT the cause; what the frame dump and the atlas actually show

User on attempt 2: "no change at all", nothing else changed either.

So the SHORT2/DYNAMIC theory is FALSIFIED as the cause of the wrong-textured shoes. The fix is kept
because it repaired a real defect (83,737 refusals -> 0, 47,323 conversions from the snoop), but it
must not be credited with anything visual. Do not re-propose it.

### What the frame dump says about the player (dump at frame 1448, player at 96.9 145.7 29.7)
    211 draws total: 155 SKIP, 54 CONVERT, 2 PASS
    SKIP reasons: 102 "prepass: shader samples nothing", 52 "prepass: only normal/stipple/depth",
                  1 orthographic - all legitimate prepass, none of them material draws.

The player BODY is ONE mesh drawn 35 times:
    v=7977 first=0 vb=49186410 bones=58[0..57] pal=58@d5021 gens=1  tex0=3BD233F0
    ... with 35 DIFFERENT primitive counts (p=84 .. p=3408)
i.e. one 7,977-vertex, 58-bone skinned mesh, one texture, and 35 index ranges - one per material
slot / clothing item. The other ~19 converted draws are separate meshes (head, hair, accessories)
on their own vertex buffers.

Note for anyone reading the dump: `ps='...'` is the shader's FIRST sampler, `rank=` is the rank of
the CHOSEN albedo. `ps='Blend_MapSampler' rank=100` therefore means a Diffuse map WAS found and
chosen - it does NOT mean the blend map is being bound as albedo. I misread this once today.

### The character atlas is CORRECT - dumped and viewed
chartex-atlas2-2048x1024.raw converted to PNG and inspected: skin, face with makeup, eye, arms,
legs, floral tattoos, fleur-de-lis, and striped sock/shoe bands ALL PRESENT and correctly
composited. mean 182.7, 6310/6311 sampled bytes non-zero.

So the atlas content is not the defect, and the atlas snoop is not implicated. Closed.

### Correction to a misleading counter: "atlas 1" is the TITLE SCREEN
    atlas 1: 1280x768 mean 14.3 of 255
This is not a character atlas at all. Dumped and viewed: it is the SAINTS ROW THE THIRD / THQ /
Volition logo screen. Its low mean is correct for a black background with logos, and it should not
be read as "an atlas that failed to fill". The atlas tracker admits a 1280x768 UI texture on size
alone. Worth tightening so this number stops looking like evidence of a defect.

### Where to look next, from this run's own counters
    uv tiling matched to the albedo map 3.5/frame, no pair for it 80.1/frame
80 draws a frame carry a tiling constant that cannot be matched to the albedo map, so no UV
transform is applied to them. With one atlas shared by 35 material slots, a missing per-slot UV
transform would put some slots on the wrong region of a texture that is itself perfect - which is
exactly the shape of "the top is right, the shoes are not". UNVERIFIED.

## 2026-08-31 - THE REMIX API IS CALLABLE FROM THE SHIM. This changes the architecture.

User: "can we fix some of these data reads with remix logic and skip the engine? and reconstruct
them ourselves?"

Yes. Verified from the binaries, not assumed.

    .trex\d3d9.dll  (64-bit runtime)  exports  remixapi_InitializeLibrary
    d3d9.dll        (32-bit BRIDGE CLIENT, the one the game loads, OUR process)
                    exports  remixapi_InitializeLibrary, remixapi_RegisterCallbacks

The bridge forwards the API, so a 32-bit ASI in the game process can call it. Header is already in
the tree: tools/vibe-re/rtx_remix_tools/dx/remix-comp-proxy/deps/bridge_api/remix/remix_c.h

### What the interface gives us (remixapi_Interface, remix_c.h:681)
    CreateMaterial / DestroyMaterial      - WE choose the albedo. Explicitly.
    CreateMesh / DestroyMesh              - hand over geometry directly
    SetupCamera                           - a real camera entry point
    DrawInstance                          - mesh + transform, per instance
    CreateLight / DrawLightInstance       - real lights
    SetConfigVariable
    dxvk_RegisterD3D9Device               - ATTACH TO THE GAME'S EXISTING DEVICE
    Startup / Present / Shutdown

### Why this dissolves most of the project's standing problems
Every hard problem in this project comes from one constraint: the ONLY way to tell Remix about
geometry was to fake a fixed-function draw, or let vertex capture reconstruct one. That constraint
is what produced:

  - the FFP conversion path and its 64-bone ceiling (MaxVertexBlendMatrixIndex = 8)
  - "which sampler is the albedo" - AlbedoRank, blanked/white materials, moved-off-stage-0
  - the SHORT2 texcoord problem (Remix discards VkFormat 80)
  - vertex capture ON reconstructing EVERY draw: the shapes around the character, blocked camera
  - the camera derivation from c28/c48 and the cameraMainViewOnly gate
  - skipping draws at all, which is what broke culling and blacked the world

With CreateMesh/CreateMaterial/DrawInstance none of those apply. We already CPU-skin the character
(51 skinned draws/frame, 95 meshes cached) - we would simply hand Remix the result instead of
re-expressing it as fixed function. We already reflect every shader (7,276 of them) and know which
sampler is the diffuse map - we would name it in a material instead of hoping Remix guesses stage 0.

And nothing is skipped: the game renders exactly as shipped, we describe a PARALLEL scene. That is
precisely the architecture the user has been asking for since "we need to make sure that instead of
breaking the game render, we are just extracting information".

Vertex capture can then go OFF - not to suppress anything, but because we no longer depend on it.

### Unverified, and must be tested before committing to this
1. Whether the BRIDGE's implementation is complete or returns NOT_AVAILABLE for some entry points.
   The 32-bit client exports the symbol; that is not proof every function is forwarded.
2. Whether dxvk_RegisterD3D9Device accepts the device Remix itself created for the game.
3. Whether API-submitted instances coexist with the game's own draws in one frame.

### Proposed order, smallest falsifiable step first
STEP 1: initialize the API and call SetupCamera ONLY, replacing ApplyCameraOnly's SetTransform
        hack. Tiny, reversible, and it answers unknowns 1 and 2 outright. If the camera works
        through the API, everything else is worth building.
STEP 2: one CreateMaterial + CreateMesh + DrawInstance for a single known object.
STEP 3: characters (we already have the skinned vertices and the atlas).
STEP 4: world, lights, decals, particles - the user's own scene contract.

Do NOT start at step 3.

## 2026-08-31 - Remix API step 1 BUILT: initialize + register the device, nothing else

Structures transcribed from bridge_api/remix/remix_c.h, API version 0.5.2 (REMIXAPI_VERSION_MAKE
0.5.2 = 0x50002). __stdcall throughout. The 21-member remixapi_Interface is declared in full even
though step 1 calls two of it - one wrong member and every pointer after it is garbage.

Implemented:
  RemixApiInit(dev)         GetModuleHandleW(L"d3d9.dll") - the BRIDGE CLIENT, never .trex, and
                            GetModuleHandle not LoadLibrary (a second d3d9 means two runtimes).
                            InitializeLibrary -> log rc. Then QueryInterface for
                            IDirect3DDevice9Ex (SR3 creates via CreateDeviceEx) and
                            dxvk_RegisterD3D9Device -> log rc.
                            Explicitly checks for NULL entry points and says so: "the bridge
                            exports the symbol but does not implement the interface".
  RemixApiSetCamera(v,p)    SetupCamera with REMIXAPI_CAMERA_TYPE_WORLD.
  RemixApiCameraTick()      same derivation and same IsMainSceneCamera gate as cameraOnly, so a
                            visual change can only be the API and not a different camera.
                            Called from Classify ahead of every rule.

Settings: remixApi=1 (default ON, image-neutral), remixApiCamera=0 (default OFF).
Both documented in configs/sr3-rtx.ini; step 1b is an ini flip with no rebuild.

Report line:
    REMIX API: InitializeLibrary <rc> | RegisterD3D9Device <rc> | entry points <ALL PRESENT|INCOMPLETE>
      attached to the game's device. SetupCamera: N calls, M failed, last rc <rc>

Error codes are named, not numeric: SUCCESS, REGISTERING_NON_REMIX_D3D9_DEVICE,
INCOMPATIBLE_VERSION, NOT_INITIALIZED etc.

### Deployed
    sr3-rtx.asi  c96199fcc5553946b49a5337767f0d6f
    sr3-rtx.map  70c2001c594ef7fa1c9afef806cdd562
    sr3-rtx.ini  27ccbedb171712d39088b086ec676bfb
    rtx.conf     421b35520b662159df313db2a6df3205  (unchanged)
Source backup: src/sr3-rtx/sr3rtx.cpp.before-remix-api

### The falsifier
INCOMPATIBLE_VERSION on init means the bridge implements a different API version - recoverable by
changing kRemixVersion. NULL entry points, or INCOMPLETE, means the bridge stubs the interface and
steps 2-4 are impossible through it; that is a real possible outcome and the log says it outright
rather than leaving it to be inferred from a picture.

## 2026-08-31 - Remix API step 1 result: GATED, not absent. The bridge said so itself.

    remix api: InitializeLibrary FAILED rc=11 (NOT_INITIALIZED)

Not a stub and not a timing problem. bridge32.log, line 20, at 14:40:31:

    err: Remix API is not enabled. This is currently an experimental feature and must be
         explicitly enabled in the `bridge.conf`. Please set `exposeRemixApi = True` if you
         are sure you want it enabled.

The bridge diagnosed itself. Confirmed independently: the string `exposeRemixApi` is present in the
32-bit bridge client d3d9.dll, and bridge32.log line 2 records it trying to open
"D:\SR3RTXREMIXCOMP\Saints Row 3\.trex\bridge.conf" - a file that did not exist.

### Change: created .trex/bridge.conf with exposeRemixApi = True
    configs/bridge.conf -> Saints Row 3/.trex/bridge.conf   db55f8142db1db21eb4bdf256f5c4ae5
Config only for the gate itself; no rebuild was needed for that.

LESSON, and it is the second time today the answer was already written down: read the BRIDGE logs,
not only remix-dxvk.log and our own. bridge32.log carried the exact remedy, in an err line, from
the first run that called the API.

### Also fixed: a misleading line I wrote myself
The step-1 report said "entry points INCOMPLETE - the bridge stubs part of the interface" whenever
the pointers were null. But a FAILED init leaves the interface all zeroes, so that message asserted
stubbing on evidence that showed nothing of the kind. Now three distinct states:
  - init failed        -> "not obtained - init failed, so nothing is known about them"
  - init OK, non-null  -> "ALL PRESENT"
  - init OK, null      -> "INCOMPLETE - init succeeded but entry points are NULL", the only case
                          that is actually evidence of stubbing
and rc==11 now prints the exposeRemixApi remedy directly in our own log.

### Deployed
    sr3-rtx.asi  55daca0a00372c6efef4d2f8007b622a
    sr3-rtx.map  c95e4802b50e3f1cf9524482264d76ea
    bridge.conf  db55f8142db1db21eb4bdf256f5c4ae5   (NEW)
    sr3-rtx.ini  27ccbedb171712d39088b086ec676bfb   rtx.conf 421b35520b662159df313db2a6df3205
Still remixApi=1 / remixApiCamera=0, so the image is unaffected either way.

## 2026-08-31 - STEP 1 ANSWERED: the mesh path is AVAILABLE. SetupCamera is not. Registration is unnecessary.

With exposeRemixApi = True in .trex/bridge.conf:

    remix api: InitializeLibrary OK. entry points:
        SetupCamera    = 00000000   <- NOT implemented
        CreateMesh     = 72968B30   AVAILABLE
        CreateMaterial = 729683F0   AVAILABLE
        DrawInstance   = 72968FE0   AVAILABLE
        CreateLight    = 72967AB0   AVAILABLE
        RegisterD3D9Device = 72969900
    remix api: RegisterD3D9Device rc=1 (GENERAL_FAILURE)

and bridge32.log line 23 explains the last one outright:

    err: [remixapi_dxvk_RegisterD3D9Device] Not yet supported. Device used by Remix API defaults
         to most recently created by client application.

### What this means
1. The API is REAL through this bridge. CreateMesh / CreateMaterial / DrawInstance / CreateLight -
   everything steps 2-4 need - are implemented.
2. Explicit registration is NOT SUPPORTED and NOT REQUIRED. The API already targets the game's
   device, because the game's is the most recently created one. GENERAL_FAILURE here is not a
   failure to attach.
3. SetupCamera is NOT implemented. Step 1b is impossible and is abandoned - which costs nothing,
   because the camera is already solved: the cameraMainViewOnly gate picks exactly one camera
   (verified 2026-08-31, 5,613 draws on the 2560x1440 perspective view, all others declined).

### Two defects in my own step-1 code, both fixed
a) The warning tested `!SetupCamera || !CreateMesh || !DrawInstance` and therefore announced
   "steps 2-4 are not possible through it" on the strength of ONE null pointer that steps 2-4 do
   not use. Capabilities are now tracked per function group: g_remixMeshReady (CreateMesh +
   CreateMaterial + DrawInstance) and g_remixCameraAvail (SetupCamera), reported separately.
b) g_remixDeviceReady was set from the RegisterD3D9Device return code, so a call the bridge does
   not implement would have disabled a working API. It is now derived from what is actually
   callable.

### Deployed
    sr3-rtx.asi  (rebuilt)   bridge.conf db55f8142db1db21eb4bdf256f5c4ae5
    sr3-rtx.ini  27ccbedb171712d39088b086ec676bfb   rtx.conf 421b35520b662159df313db2a6df3205
Source backup: src/sr3-rtx/sr3rtx.cpp.before-remix-capabilities

### Next: STEP 2 - one CreateMaterial + CreateMesh + DrawInstance for a single known object.
Not started. Do not jump to characters.

## 2026-08-31 - STEP 2 BUILT: one material, one mesh, one instance, entirely through the Remix API

Structures transcribed from remix_c.h: RemixHardcodedVertex (64 bytes - position[3], normal[3],
texcoord[2], color, 7 pads, all declared because the size is what matters), MaterialInfo,
MeshInfoSurfaceTriangles, MeshInfo, InstanceInfo, Transform (float[3][4], translation in the last
column). StructTypes: MATERIAL_INFO=2, MESH_INFO=12, INSTANCE_INFO=13.

What it does, once per frame from Present:
  - builds ONCE: a 24-vertex / 12-triangle cube (4 verts per face so each face has its own
    normal) and an emissive material;
  - then DrawInstance every frame. DrawInstance is a per-frame submission like a draw call, not a
    persistent registration - issue it once and the cube exists for exactly one frame.

Design choices that are about DIAGNOSIS, not aesthetics:
  - GREEN, because magenta already means "Remix ignored material" here and would be ambiguous the
    instant it appeared.
  - EMISSIVE (intensity 200), because a merely-lit cube could be invisible for lighting reasons,
    and "invisible because unlit" is indistinguishable from "the API did nothing" - the exact
    confusion step 2 exists to remove.
  - 3 units in front of the camera, from the inverse of the recorded main-scene view, so it cannot
    be mistaken for world geometry.

No textures on the material: remixapi_Path is a wchar_t FILE PATH, not a D3D9 texture. Texturing
API meshes from the game's own atlases is a separate problem and deliberately not in step 2.

RemixApiCameraTick now records the main-scene view UNCONDITIONALLY (it used to return early unless
remixApiCamera was on). Step 2 needs the view to place the cube, and SetupCamera is not implemented
by this bridge anyway, so there is nothing to gate.

Build error caught and fixed: g_remixLastView/g_remixLastViewValid were declared in the step-2
block, which the patch inserted AFTER RemixApiCameraTick, so both were undeclared at first use.
Moved beside g_remixCamView with a note saying why they must precede both users.

### Deployed
    sr3-rtx.asi  0aaa60594b5404baf1edf099f64a2e90
    sr3-rtx.map  ed4cadb9c5af4194b8a3c6c7018937a0
    sr3-rtx.ini  fed219fdc1b410051a861a7b0650d0e0
    rtx.conf 421b35520b662159df313db2a6df3205 | bridge.conf db55f8142db1db21eb4bdf256f5c4ae5
Source backup: src/sr3-rtx/sr3rtx.cpp.before-remix-step2

### The three outcomes
  cube VISIBLE                      -> API geometry reaches the path tracer. Steps 3-4 are on.
  DrawInstance rc=0 but NOT visible -> submissions accepted then discarded: transform convention
                                       or camera association is wrong. Recoverable.
  DrawInstance rc!=0                -> named in the log by RemixErrName.

## 2026-08-31 - STEP 2 SUCCEEDED. The cube is visible. This is the turning point.

User: "the cube is visible but it was not bright green. it was gray/black"

VISIBLE is the result that matters, and it settles the architectural question outright:

  * geometry described entirely through the Remix API, with NO D3D9 draw behind it, reaches the
    path tracer and is rendered;
  * the transform convention is right - remixapi_Transform float[3][4] with translation in the
    last column put the cube exactly where intended, 3 units in front of the camera;
  * the camera association works with no SetupCamera at all (the bridge does not implement it),
    because the API attaches to the most recently created device, which is the game's;
  * DrawInstance-per-frame from Present is the correct submission point.

The constraint that produced nearly every hard problem in this project - that geometry could only
reach Remix as a faked fixed-function draw or a vertex-capture reconstruction - is GONE.

### Why it was grey/black, and it is not a mystery
remixapi_MaterialInfo has NO albedo field. It carries textures (as wchar_t FILE PATHS), emission,
sprite-sheet and filter/wrap settings - nothing else. The surface properties live in
remixapi_MaterialInfoOpaqueEXT: albedoConstant, opacityConstant, roughnessConstant,
metallicConstant. I did not chain it, so albedo fell to its default and the cube rendered with the
default surface. The API was working perfectly the whole time.

Emission also did not show, which is worth noting rather than explaining away: emissiveIntensity
200 with emissiveColorConstant green produced no glow. Possibly emission needs the opaque EXT
present too, possibly it needs a texture, possibly the intensity is in units this does not expect.
Deliberately NOT resolved by guessing - the rebuilt cube sets BOTH albedoConstant and emission, so
the next run separates them:
    lit green   -> albedoConstant landed, emission did not
    glowing     -> emission landed
    grey again  -> neither constant is honoured, and the problem is not which struct carries them

### Change
RemixMaterialInfoOpaqueEXT transcribed (sType 5) and chained through MaterialInfo::pNext with
albedoConstant {0.05,1,0.05}, opacityConstant 1.0, roughnessConstant 0.4, metallicConstant 0.

### Deployed
    sr3-rtx.asi  (rebuilt)   ini fed219fdc1b410051a861a7b0650d0e0
    rtx.conf 421b35520b662159df313db2a6df3205 | bridge.conf db55f8142db1db21eb4bdf256f5c4ae5
Source backup: src/sr3-rtx/sr3rtx.cpp.before-opaque-ext

### STEP 3 is now unblocked, and the open question changes shape
Characters: we already CPU-skin them (51 skinned draws/frame, 95 meshes cached) and we already know
which sampler is the diffuse map from shader reflection. What we do NOT yet have is a way to give
an API material the game's OWN texture: remixapi_Path is a FILE PATH, so either
  (a) dump the atlas to disk once and point the material at it, or
  (b) find whether a D3D9 texture can be handed over some other way.
That is the next real unknown, and it should be settled before building step 3.

## 2026-08-31 - the invisible-but-glowing cube: alphaTestType 0 means NEVER

User: "the cube is invisible but i do see a green light coming from where it should be."

That is two findings, not one:
  1. EMISSION WORKS. The green light proves the material is live, emitting, and correctly placed.
     The earlier "emission did not land" reading was wrong - it never landed because the SURFACE
     was gone, not because emission failed.
  2. The surface was alpha-tested away.

### Cause
remixapi_MaterialInfoOpaqueEXT::alphaTestType is a raw `int` in remix_c.h with no enum beside it.
Remix mirrors VkCompareOp: NEVER = 0, ALWAYS = 7. The 64-bit runtime carries the default string
"AlphaTestType alpha_test_type = Always". The struct was zero-initialised, so the material said
alpha test NEVER PASSES - every pixel discarded, while the emissive property still contributed
light to the scene. Exactly the reported symptom.

### The general lesson, which cost two runs
The FIRST cube (no opaque EXT chained) was VISIBLE. Chaining the extension made it invisible.
Supplying an extension REPLACES Remix's defaults wholesale: inside these structs a zero is an
INSTRUCTION, not an absence. Zero-initialising a Vulkan-style extension struct and filling in only
the interesting fields is safe only where 0 is a valid default, and here it is not.

Every field of the opaque EXT is now set explicitly, including the ones that happen to be zero,
with a comment saying why.

### Deployed
    sr3-rtx.asi  (rebuilt, alphaTestType = 7 ALWAYS)
    ini fed219fdc1b410051a861a7b0650d0e0 | rtx.conf 421b35520b662159df313db2a6df3205
    bridge.conf db55f8142db1db21eb4bdf256f5c4ae5
Source backup: src/sr3-rtx/sr3rtx.cpp.before-alphatest

Expected next run: a solid green cube, lit AND glowing. If it is solid but grey, albedoConstant is
not being honoured and the alpha test was the only problem. If it is still invisible, the alpha
test was not the cause and opacityConstant or the blend path is.

## 2026-08-31 - STEP 2 COMPLETE, and the texture route answered

User: "it works. i see a lit green cube."

Step 2 is proven end to end:
  geometry submitted through the API reaches the path tracer | transform convention correct
  (float[3][4], translation in the last column) | albedoConstant works via MaterialInfoOpaqueEXT |
  emission works | alpha test understood | DrawInstance-per-frame from Present is the right
  submission point | no SetupCamera needed - the API binds to the most recently created device.

### The texture question, answered from the header and the runtime
remixapi_Path is a `const wchar_t*` FILE PATH. There is NO route for a live D3D9 texture:
dxvk_GetVkImage goes the other way (IDirect3DSurface9 -> VkImage, for reading Remix's OUTPUT), and
no MaterialInfo extension takes an image handle. The runtime carries both ".dds" and ".png", plus
"Please make sure all replacement textures have mip-maps" and "A suboptimal replacement texture
detected", so mips are expected.

So: the game's own textures reach an API material only by being WRITTEN TO DISK.

### Built: the atlas -> DDS -> API material test
- WriteBgraDds(): uncompressed BGRA8 DDS with a full box-filtered mip chain. DDS rather than PNG
  because it needs no compression library - header plus raw pixels.
- ALPHA FORCED TO 255. The atlas is X8R8G8B8, so its alpha byte is undefined; passing it through
  could hand Remix a fully transparent texture. That would look exactly like the alphaTestType bug
  already paid for once today, so it is pre-empted rather than diagnosed later.
- Hooked to the atlas capture, gated on w>=2048 && h>=1024 so it takes the CHARACTER atlas and not
  the 1280x768 title screen that the tracker also admits.
- The cube now waits for the atlas (bounded at 1200 frames), then wears it: albedoTexture = the
  DDS path, albedoConstant white so the texture is not tinted, emission off so a glow cannot wash
  out the thing being checked. No atlas by 1200 frames -> the green constant cube, so a missing
  atlas degrades the test rather than deleting it.

### Deployed
    sr3-rtx.asi  (rebuilt)  ini fed219fdc1b410051a861a7b0650d0e0
    rtx.conf 421b35520b662159df313db2a6df3205 | bridge.conf db55f8142db1db21eb4bdf256f5c4ae5
Source backup: src/sr3-rtx/sr3rtx.cpp.before-atlas-texture
Stale sr3-remix-atlas.dds deleted before the run so its presence proves this run wrote it.

### Also found, and it matters more than the texture route
REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BONE_TRANSFORMS_EXT (14) carries up to
REMIXAPI_INSTANCE_INFO_MAX_BONES_COUNT = 256 bone transforms per instance, and
remixapi_MeshInfoSkinning carries bonesPerVertex + blendWeights + blendIndices. SR3 needs 58 bones.
So Remix can do the SKINNING ITSELF: hand it the bind-pose mesh once and per-frame bone transforms.
That retires both the shim's CPU skinning and the 64-bone fixed-function ceiling
(MaxVertexBlendMatrixIndex = 8) that has shaped this project from the start. Step 3 should use it.

## 2026-08-31 - STEP 2 COMPLETE (lit green cube), and the texture question answered

User: "it works. i see a lit green cube."

alphaTestType = 7 (ALWAYS) fixed it. Step 2 is fully proven:
    API geometry reaches the path tracer          - the cube renders
    transform convention correct                  - float[3][4], translation in last column
    camera association works with NO SetupCamera  - the API uses the game's device implicitly
    DrawInstance-per-frame from Present           - correct submission point
    albedoConstant works (MaterialInfoOpaqueEXT)  - green
    emission works                                - proven by the light cast while the surface
                                                    was alpha-tested away

### The texture question - ANSWERED from the header and the runtime
remixapi_Path is `const wchar_t*`, a FILE PATH. There is no texture-handle route:
  - MaterialInfo carries albedo/normal/tangent/emissive as PATHS only;
  - dxvk_GetVkImage(IDirect3DSurface9*, uint64_t* out) goes the WRONG WAY - it reads Remix's
    output into a VkImage, it does not accept a texture for a material;
  - no other StructType in the API takes an image. The full extension list is 24 entries and the
    only material extensions are PORTAL, TRANSLUCENT, OPAQUE and OPAQUE_SUBSURFACE.
The runtime contains both '.dds' and '.png', plus "Please make sure all replacement textures have
mip-maps" and "A suboptimal replacement texture detected".

So: the game's own textures reach an API material THROUGH A FILE, and that is the only route.

### Also found, and it matters more than the texture question
REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BONE_TRANSFORMS_EXT = 14
    remixapi_InstanceInfoBoneTransformsEXT { sType, pNext, const remixapi_Transform* , count }
    REMIXAPI_INSTANCE_INFO_MAX_BONES_COUNT = 256
with remixapi_MeshInfoSkinning { bonesPerVertex, blendWeights, blendIndices } on the surface.
SR3 needs 58 bones. So Remix can do the skinning itself from a bind-pose mesh plus per-frame bone
transforms - which retires BOTH our CPU skinning AND the fixed-function 64-bone ceiling
(MaxVertexBlendMatrixIndex = 8) that has shaped this project since the beginning.

### Built: the texture test, reusing the proven cube
  WriteBgraDds(path, w, h, bgra)  uncompressed BGRA8 DDS with a FULL MIP CHAIN (box filter).
                                  DDS rather than PNG because it needs no compression library.
                                  ALPHA FORCED TO 255 - the atlas is X8R8G8B8 so its alpha byte is
                                  undefined, and shipping that through would produce an invisible
                                  texture, i.e. exactly the alpha failure already paid for today.
  WriteAtlasDdsOnce()             called from the atlas capture, for p.w>=2048 && p.h>=1024 only -
                                  the 1280x768 capture is the TITLE SCREEN, identified earlier.
                                  Absolute path beside the exe (Remix resolves it; a relative path
                                  depends on a working directory this shim does not control).
  cube material                   if the DDS exists: albedoTexture = it, albedoConstant white,
                                  emission ZERO (a glowing surface would wash out the test).
                                  Otherwise the green constant, so a missing atlas degrades the
                                  test rather than removing it. Bounded wait of 1200 frames.

### Deployed
    sr3-rtx.asi  ad5e6ab82136c8a5a7421f17bbf8c735
    sr3-rtx.map  8ca1173055d9a1af81576be21ca967b8
    ini fed219fdc1b410051a861a7b0650d0e0 | rtx.conf 421b35520b662159df313db2a6df3205
    bridge.conf db55f8142db1db21eb4bdf256f5c4ae5
Source backup: src/sr3-rtx/sr3rtx.cpp.before-atlas-texture
Stale Saints Row 3/sr3-remix-atlas.dds deleted before the run so its presence proves this run wrote it.

### Outcomes
  cube wears the character atlas -> the file route works; step 3 is fully unblocked
  cube is white/untextured        -> Remix did not load the DDS; check its log for a texture
                                     complaint, and the header/mips are the suspects
  cube is still green             -> no atlas was captured in time; not a texture failure

## 2026-08-31 - the texture test did not run: a race, not a texture failure

User: "the cube is still glowing green."

That is the third outcome listed for this test - no atlas was available when the cube was built -
and the log confirms it exactly:

    remix api: wrote the character atlas as ...\sr3-remix-atlas.dds (2048x1024 BGRA8 + mips)
    STEP 2 CUBE: built (green constant - no atlas was available) | DrawInstance 1024 calls, 0 failed

### The DDS writer is CORRECT and is not implicated
The file is 11,184,940 bytes. A 2048x1024 BGRA8 mip chain is 12 levels -
8388608+2097152+524288+131072+32768+8192+2048+512+128+32+8+4 = 11,184,812 - plus a 128-byte
header = 11,184,940 EXACTLY. Nothing about the format has been tested yet either way.

### The actual defect: a one-shot build against a resource that appears on the game's schedule
The cube waited up to 1200 frames for the atlas and then built green. But the character atlas is
only captured once a CHARACTER IS ON SCREEN, which is well after the loading screens that the first
1200 frames are spent in. The bounded wait did not fix the race, it just hid it behind a timeout.

### Change: rebuild once when the atlas arrives late
If the cube is already built, untextured, and the atlas becomes ready, destroy the untextured
material+mesh and rebuild with the texture. Guarded:
  - runs from Present BEFORE this frame's DrawInstance, so nothing has been submitted against the
    old handles yet this frame;
  - the old handles are released only AFTER the new pair is successfully created;
  - if the textured rebuild fails, the working green pair is restored rather than leaving no cube.

### Deployed
    sr3-rtx.asi  (rebuilt)   map (rebuilt)
    ini fed219fdc1b410051a861a7b0650d0e0 | rtx.conf 421b35520b662159df313db2a6df3205
    bridge.conf db55f8142db1db21eb4bdf256f5c4ae5
Source backup: src/sr3-rtx/sr3rtx.cpp.before-cube-rebuild

## 2026-08-31 - the cube vanished: the HASH is the identity, and I reused it

User: "i do not see any cube." DrawInstance reported 630 calls, 0 failed, SUCCESS - so the
submission was fine and the object simply did not exist.

Remix's own log names it:

    info: Ignoring repeated material registration (handle=5923289857198653441)

5923289857198653441 = 0x5233C0BE00000001, the material hash hard-coded in RemixBuildCube.

### What actually happened
remixapi_MaterialInfo::hash and remixapi_MeshInfo::hash ARE the identity. Registering a second,
different material under the same hash is refused and the EXISTING registration is handed back. So:
  1. the textured material was never registered - the green one was returned instead;
  2. the code then destroyed "the old" material and mesh;
  3. that destroyed the object the new handle aliased.
Result: no cube at all, with every return code SUCCESS. A destroy of an aliased handle cannot be
detected from return codes, which is why this looked like a rendering failure and was not one.

### Fixes
1. g_remixBuildGen, incremented per build, added to both hashes. Each rebuild is now a genuinely
   distinct object. (Mesh hash also moved off 0x...00000002 to 0x...10000002 so material and mesh
   hashes cannot collide with each other across generations.)
2. The old pair is NO LONGER DESTROYED. Two objects one frame apart on a test path are not worth
   the risk that just cost a run; leaking one material and one mesh, once, is the cheaper mistake.
3. The build log now reports the generation, both hashes, and WHICH material was built - it
   previously said "albedoConstant green" unconditionally, including for the textured build, which
   would have misled the next reading of it.

Also in the Remix log, and NOT yet attributed:
    [RTX-Compatibility-Info] Texture 0 without valid hash detected, skipping drawcall.
    [RTX-Compatibility-Info] Trying to bind a texture to a mesh without UVs. Was this intended?
The cube does carry UVs (texcoord is written per vertex), so if that line refers to our mesh it is
a real finding about how the API expects them. Deliberately left open rather than guessed at - the
next run separates it, because a cube that renders textured makes both lines someone else's.

### Deployed
    sr3-rtx.asi  (rebuilt)  ini fed219fdc1b410051a861a7b0650d0e0
    rtx.conf 421b35520b662159df313db2a6df3205 | bridge.conf db55f8142db1db21eb4bdf256f5c4ae5
Source backup: src/sr3-rtx/sr3rtx.cpp.before-hash-fix


## 2026-08-31 - MILESTONE: the Remix API path is proven end to end

    remix api: STEP 2 built gen 1 - hashes 00000002/10000003 (albedoConstant green + emissive green)
    remix api: STEP 2 built gen 2 - hashes 00000003/10000004 (albedo = the character atlas from disk)
    STEP 2 CUBE: built (wearing the CHARACTER ATLAS from disk) | DrawInstance 451 calls, 0 failed

A cube described entirely through the Remix API, with no D3D9 draw behind it, rendered in the path
traced image wearing the game's OWN character texture. No "Ignoring repeated material registration"
this run.

### Every mechanism now PROVEN, and none of it needs re-deriving
| mechanism | state |
|---|---|
| API reachable from the 32-bit shim | bridge client d3d9.dll exports remixapi_InitializeLibrary |
| gate | `.trex/bridge.conf` -> `exposeRemixApi = True` (bridge log said so itself) |
| device attach | NOT needed. dxvk_RegisterD3D9Device is unimplemented; the API defaults to the most recently created device, which is the game's |
| camera | NOT needed. SetupCamera is unimplemented; the game's own camera is used |
| geometry | CreateMesh + DrawInstance, per frame from Present |
| transform | remixapi_Transform float[3][4], translation in the LAST COLUMN |
| base colour | MaterialInfoOpaqueEXT::albedoConstant - the base MaterialInfo has NO albedo field |
| emission | MaterialInfo::emissiveIntensity + emissiveColorConstant |
| alpha | OpaqueEXT::alphaTestType mirrors VkCompareOp. 0 = NEVER = invisible. USE 7 = ALWAYS |
| identity | MaterialInfo::hash / MeshInfo::hash ARE the identity. Reusing one is refused and the OLD object is returned |
| textures | remixapi_Path is a wchar_t FILE PATH. Uncompressed BGRA8 DDS with a full mip chain loads correctly |

### Traps paid for, in order, so they are not paid for again
1. Supplying an extension REPLACES Remix's defaults wholesale. Inside these structs a zero is an
   INSTRUCTION, not an absence - a zeroed alphaTestType means "never draw this".
2. The hash is the identity. A rebuild must use a NEW hash, or the new registration is silently
   ignored and every return code still says SUCCESS.
3. Do not destroy a handle that may alias a live registration. Leaking a test object is cheaper.
4. A one-shot build against a resource the GAME produces on its own schedule is a race. The
   character atlas does not exist until a character is on screen, long after frame 1200.
5. X8R8G8B8 sources have an undefined alpha byte. Force it to 255 when writing a DDS.

### Left open, deliberately, and now someone else's problem
    [RTX-Compatibility-Info] Texture 0 without valid hash detected, skipping drawcall.
    [RTX-Compatibility-Info] Trying to bind a texture to a mesh without UVs. Was this intended?
Two occurrences, unchanged from runs before the API existed, and the cube renders textured - so
neither refers to our mesh.

### STEP 3 - characters. The architecture it enables
The reason this matters is not the cube. It is that the constraint behind nearly every hard problem
in this project is gone:

  - "which sampler is the albedo" (AlbedoRank, blanked/white materials, moved-off-stage-0) becomes
    a NAMED texture path. We already reflect all 7,276 shaders and know the diffuse sampler.
  - the 64-bone fixed-function ceiling (MaxVertexBlendMatrixIndex = 8) is gone:
    INSTANCE_INFO_BONE_TRANSFORMS_EXT carries up to 256 bones, and SR3 needs 58.
  - SHORT2 texcoords stop mattering: we write float UVs into the vertex struct ourselves.
  - vertex capture can go OFF - not to suppress anything, but because we no longer depend on it,
    which removes the duplicate reconstruction that IS the shapes around the character and the
    blocked camera.
  - nothing is skipped. The game renders exactly as shipped; we describe a parallel scene.

Order: 3a submit ONE character mesh through the API alongside the existing path (expect a DOUBLED
character - that is the proof it landed); 3b turn vertex capture off and stop converting, so the
API scene is the only one. Do not do 3b first - a black screen would then have two candidate
causes.

## 2026-08-31 - STEP 3a BUILT: one character mesh through the Remix API

Deliberately ALONGSIDE the existing path, not instead of it. A DOUBLED character is the proof it
landed; replacing the old path in the same change would make a missing character mean either "the
API failed" or "the old path was removed correctly", and that ambiguity has cost whole sessions.

### How it works
The CPU-skinned vertices are ALREADY what Remix wants: SkinnedVertex is {pos[3], nrm[3], uv[2]}
and remixapi_HardcodedVertex is those same fields plus a colour and padding. They are already in
WORLD space (the dump's at(96.9 145.7 29.7) is the skinned centroid), so the instance transform is
identity - no matrix convention to get wrong.

  capture   in SkinAndBind, just before the Unlock, for the first draw with >= 4000 vertices.
            Reading back through the mapped pointer is exactly what the existing centroid
            accumulation already does, so it is no new risk.
  indices   read ONCE from the game's index buffer in the draw hook, which is the only place
            startIndex and primitiveCount exist. Handles INDEX16 and INDEX32.
  rebasing  out[i] is the skinned vertex for original index (baseVertex + minIndex + i), and the
            game's indices address (baseVertex + idx), so the captured-array index is
            (idx - minIndex). Anything outside the captured window becomes a degenerate triangle
            rather than dropping the whole mesh.
  material  names the character atlas DDS. This is the FIRST geometry in this project whose albedo
            we CHOSE instead of hoping Remix would infer it from texture stage 0.
  hashes    a distinct family (0x5233C0DE...) from the cube's, plus the generation counter.
  alpha     alphaTestType = 7. A zero means NEVER; that cost a run today.

### Build error caught: the same declaration-ordering trap as before
The per-frame DrawInstance tick lives in the Remix API block, which precedes SkinnedVertex - so the
captured-vertex vector cannot be declared beside the handles it is used with. Only the handles moved
early, with a comment saying why the two halves of step 3a sit on opposite sides of that type.

### Deployed
    sr3-rtx.asi  75c4bffc601a1ed427c591f3ad566b80
    sr3-rtx.map  18847f84a9b05784939c6e5eb69f2137
    sr3-rtx.ini  0522311c7684be32b391d3955dfd9f0b
    rtx.conf 421b35520b662159df313db2a6df3205 | bridge.conf db55f8142db1db21eb4bdf256f5c4ae5
Source backup: src/sr3-rtx/sr3rtx.cpp.before-step3a

### Expected
    STEP 3a CHARACTER: N verts, M triangles | DrawInstance ... - expect a DOUBLED character
A frozen second copy of one body part, in the pose it had when captured, wearing the atlas. Frozen
is CORRECT at this stage: the mesh is captured once and never updated. Animation is step 3c, via
INSTANCE_INFO_BONE_TRANSFORMS_EXT (256 bones; SR3 needs 58).

## 2026-08-31 - step 3a: submitted successfully, rendered nowhere anyone would look

    STEP 3a CHARACTER: 7977 verts, 3408 triangles | DrawInstance 3863 calls, 0 failed, SUCCESS
    STEP 2 CUBE: built (wearing the CHARACTER ATLAS from disk) | 3863 calls, 0 failed

7977/3408 matches the big body draw in the frame dump exactly, so the capture and the index
rebasing both worked. The cube still renders, so the API is healthy. The mesh exists and is drawn.

### Cause: the capture and the build did not share their preconditions
RemixBuildCharacter required g_atlasDdsReady. The CAPTURE in SkinAndBind did not - it took the
FIRST skinned draw of the run with >= 4000 vertices. That happens during loading, long before the
player character exists in the world, so the mesh was frozen at a pose and a POSITION from that
moment. Submitted correctly, rendered correctly, and nowhere the player would ever look.

RULE: a capture and the build that consumes it must share their preconditions. Gating only the
consumer leaves the producer free to sample the wrong moment, and every return code still reads
SUCCESS.

### Changes
1. The capture is now gated on g_atlasDdsReady, exactly as the build is.
2. The build logs the mesh's world-space CENTROID and EXTENT, and the frame report repeats the
   position. Without it, "submitted successfully but invisible" and "submitted successfully and
   sitting 400 metres away" are the same log line - and they were, which is what made this cost a
   run. The report already prints the camera position, so the comparison needs no arithmetic.

### Deployed
    sr3-rtx.asi  (rebuilt)  ini 0522311c7684be32b391d3955dfd9f0b
    rtx.conf 421b35520b662159df313db2a6df3205 | bridge.conf db55f8142db1db21eb4bdf256f5c4ae5
Source backup: src/sr3-rtx/sr3rtx.cpp.before-3a-gate

## 2026-08-31 - step 3a: the mesh was CORRECT and UNPLACED. Skinned output is OBJECT space.

    mesh   at (-0.0 1.2 0.0), extent 0.56 x 1.86 x 0.39
    camera at (90.8 145.4 21.4)

A correctly sized character - 1.86 units tall - sitting at the WORLD ORIGIN. Geometry, indices,
material and submission were all right; only the placement was missing.

### The wrong claim this was built on
I asserted "the CPU-skinned vertices are already in WORLD space, so the instance transform is
identity", and wrote it into the code, the ini and the worklog. It is FALSE. They are in OBJECT
space; SR3 places them with objTM.

I read the frame dump's main-line `at(96.9 145.7 29.7)` as the skinned centroid. The bind-pose
figure printed right beside it - `bind=1.23x1.79x0.34 at(-0.00 1.19 -0.01)` - is what our captured
mesh actually matches, to within rounding. The evidence for the correct answer was on the same line
as the evidence I misread.

### Fix
objTM (3 constant registers at kRegObjTM) is captured AT THE SAME MOMENT as the vertices - a
transform from a later frame would place this pose where the character has since moved to - and
used as the instance transform.

The layouts map across with a straight memcpy: SR3's objTM is a row-major float3x4, row r is
m[r*4..r*4+3] with translation in the fourth component, and remixapi_Transform is float[3][4] with
translation in the last column. The skinning code already documents SR3's half of that.

### Diagnostic improved
The build now logs BOTH the object-space centroid and the position objTM places it at. Logging only
the former is what made "unplaced" look like "broken" for a whole run.

### Deployed
    sr3-rtx.asi  (rebuilt)  ini 0522311c7684be32b391d3955dfd9f0b
    rtx.conf 421b35520b662159df313db2a6df3205 | bridge.conf db55f8142db1db21eb4bdf256f5c4ae5
Source backup: src/sr3-rtx/sr3rtx.cpp.before-objtm

## 2026-08-31 - STEP 3a SUCCEEDED: character geometry through the Remix API, placed and textured

    object space centroid (-0.0 1.2 0.0), extent 0.56 x 1.86 x 0.39
    objTM places it at (96.9 146.9 29.7)
    camera                (96.2 147.7 35.2)
    STEP 3a CHARACTER: 7977 verts, 3408 triangles | DrawInstance 1416 calls, 0 failed, SUCCESS

User: "i see some polyguns from my character."

(96.9 146.9 29.7) matches the player position from the 2026-08-31 frame dump - (96.9 145.7 29.7) -
so the placement is right. Character geometry described ENTIRELY through the Remix API, with no
D3D9 draw behind it, rendering in the path traced image at the correct world position wearing the
character atlas.

"Some polygons" is CORRECT at this stage and is not a defect: the body is ONE 7,977-vertex mesh
drawn 35 times with different index ranges - one per clothing/body slot - and this captures a
single range. 3408 triangles of roughly 15,000 across all 35, so about a quarter of the body, in
one frozen pose.

### What is now proven end to end
  geometry     CreateMesh from CPU-skinned vertices - the layout already matched
  indices      read once from the game's IB, rebased by (idx - minIndex)
  placement    objTM captured with the vertices, straight memcpy into remixapi_Transform
  material     albedo NAMED as the character atlas DDS - not inferred from texture stage 0
  submission   DrawInstance per frame from Present

### Remaining, in order
  3b  all 35 slots, not one. Each slot is a separate index range over the same vertex buffer, so
      it is 35 meshes sharing one vertex array - or one mesh with 35 surfaces, which
      remixapi_MeshInfo::surfaces_count already supports.
  3c  LIVE, not frozen: re-upload the skinned vertices each frame, or better, hand Remix the bind
      pose plus per-frame bone transforms via INSTANCE_INFO_BONE_TRANSFORMS_EXT (256 bones, SR3
      needs 58) and let it skin.
  3d  then, and only then, turn vertex capture off and stop converting, so the API scene is the
      only one. Doing this before 3b/3c would leave a broken image with several candidate causes.

### Worth measuring at 3b
How many indices fall outside the captured window and become degenerates. The rebasing maps them
to 0 rather than dropping the mesh, which is the safe choice but is silent - if that count is
large, part of "some polygons" is collapsed triangles rather than a partial slot.

## 2026-08-31 - STEP 3b BUILT: all of the character's slots as surfaces of one mesh

SR3's body is ONE 7,977-vertex buffer drawn 35 times with different INDEX RANGES, one per
clothing/body slot. remixapi_MeshInfo::surfaces_count takes an array of surfaces, each with its own
indices and material - exactly the shape of the problem. One mesh, N surfaces, one shared vertex
array, one material.

### Two phases, because a slot list is only complete when its frame ends
  COLLECT  RemixCollectCharacterSlot(), from the draw hook, during the CAPTURE FRAME ONLY. Matches
           on g_stream0 == the captured vertex buffer, plus minIndex and vertex count. Copies the
           game's index buffer ONCE on the first slot - a character IB is ~90 KB, so copying it
           whole costs one lock instead of one per slot, and every later slot reads the copy.
  BUILD    RemixBuildCharacter(), from Present, on the first frame AFTER the capture frame, when
           no further slots can arrive.

### The out-of-range count is now reported
Promised at the end of step 3a and delivered: indices outside the captured vertex window are
collapsed to degenerate triangles (safe - dropping the mesh would be worse), and the count is
printed against the total. If it is a large fraction, the missing geometry is collapsed triangles
rather than an honestly partial capture. That distinction was previously invisible.

### Build error, third time for the same reason
g_apiCapVB / g_apiCapFrame were declared with the builder but written by SkinAndBind, which
precedes it. Moved, with a comment. The step-3 state is inherently split around SkinnedVertex and
the Present-time tick, and that keeps catching me out.

### Deployed
    sr3-rtx.asi  (rebuilt)  ini 0522311c7684be32b391d3955dfd9f0b
    rtx.conf 421b35520b662159df313db2a6df3205 | bridge.conf db55f8142db1db21eb4bdf256f5c4ae5
Source backup: src/sr3-rtx/sr3rtx.cpp.before-step3b

### Expected
    STEP 3b CHARACTER: N surfaces (M slots seen), 7977 verts, T triangles at (x y z) | ...
                       | K indices collapsed as out-of-range
A frozen FULL character - all slots - standing where the capture happened, wearing the atlas.

## 2026-08-31 - step 3b: half the body wrong. baseVertex, dropped when the builder was split.

    36 surfaces from 36 slots, 7977 verts, 19359 triangles, placed at (96.9 146.9 29.7)
    0 indices collapsed as out-of-range
User: "its the whole body with like half the polygons missing at random."

All slots collected, EVERY index in range, placement correct - and still half the polygons wrong.
That combination is the finding: it rules out an out-of-BOUNDS rebasing and leaves an
off-by-a-per-slot-amount one. A wrong index that is still inside [0, 7977) is invisible to a range
check, which is why the diagnostic read 0 while the mesh was half nonsense.

### Cause
Splitting the single-slot builder into collect + build dropped baseVertex from the collect
signature. Every slot was then rebased as (idx - minIndex), which is correct ONLY for slots whose
baseVertex equals the captured draw's. SR3's 36 body slots do not all share one baseVertex, so the
rest landed on valid but wrong vertices.

Step 3a did not show this because it used a single draw, where baseVertex cancels out.

### Fix: rebase absolutely
    captured vertex 0 is at absolute index  capBaseVertex + capMinIndex   (g_apiCapFirstVertex)
    a slot's index idx is at absolute index slotBaseVertex + idx
    so the captured-array index is        (slotBase + idx) - g_apiCapFirstVertex
Computed in long long, and out-of-range now means genuinely outside the captured window rather
than "a different window".

minIndex is deliberately no longer required to match when collecting a slot - the absolute
rebasing handles any window, so a slot with a different one is now handled instead of skipped. The
vertex COUNT still has to match, as a cheap "same body mesh" test.

### Deployed
    sr3-rtx.asi  (rebuilt)  ini 0522311c7684be32b391d3955dfd9f0b
    rtx.conf 421b35520b662159df313db2a6df3205 | bridge.conf db55f8142db1db21eb4bdf256f5c4ae5
Source backup: src/sr3-rtx/sr3rtx.cpp.before-basevertex

## 2026-08-31 - step 3b: the numbers say the mesh is COMPLETE, so stop reading the picture as the mesh

    36 surfaces from 36 slots, 7977 verts, 19359 triangles, 0 collapsed as out-of-range
Identical before and after the baseVertex fix - so those 36 slots DO share one baseVertex and that
fix was a no-op. It is kept because the absolute rebasing is correct by construction, but it must
not be credited with anything.

19,359 triangles against 7,977 vertices is a complete closed body's worth (roughly 2 triangles per
vertex). Every index is in range. All 36 slots are present. The mesh is not missing anything.

### The overlap, which every number was blind to
The API copy is placed at (96.9 146.9 29.7). The game's OWN character stands at (96.9 145.7 29.7).
The same spot. Two copies of the same surfaces occupying one volume in a path traced scene z-fight,
and that reads exactly as "the whole body with like half the polygons missing at random".

Step 3a was designed around "a DOUBLED character is the proof it landed" - and the doubling is
precisely what made the result unreadable, because the two copies are not side by side, they are
INTERPENETRATING. That was a flaw in the test design, not in the code under test.

### Change: remixApiCharacterOffset, default 3.0
Translates the API instance on X so it stands BESIDE the real character. Purely a diagnostic:
step 3d removes the game's copy entirely and the offset goes back to 0. Until then the two must
not share a volume or neither can be judged. Set remixApiCharacterOffset=0 to overlay them again.

Build error: added a second statement to a brace-less `if`, orphaning its `else`. Braced.

### Deployed
    sr3-rtx.asi  (rebuilt)  ini 0522311c7684be32b391d3955dfd9f0b
    rtx.conf 421b35520b662159df313db2a6df3205 | bridge.conf db55f8142db1db21eb4bdf256f5c4ae5
Source backup: src/sr3-rtx/sr3rtx.cpp.before-offset

## 2026-08-31 - step 3b: the offset did NOT fix it, so the z-fight theory was wrong

User, with the copy standing beside the real character: "it is missing alot of polygons and is not
textured or textured properly. its just a mesh with a bunch of holes."

The overlap hypothesis is FALSIFIED. Separating the two copies changed nothing, so the mesh really
is broken rather than merely interpenetrating. Recorded as a wrong call: the numbers (36/36 slots,
0 out-of-range, a plausible triangle count) were consistent with a complete mesh and I let that
outweigh a direct visual report a second time.

### Found by reading the code, not the picture: the UVs are RAW SHORTS
kShortUVScale = 1/1024 exists in this file, and SR3's vertex shaders end with
`mul o1.xy, r0, 0.0009765625`. The fixed-function path reproduces that with a uv TEXTURE MATRIX
rather than touching the vertices - the frame report even prints "last scale 0.00098, 0.00098".
An API mesh has no texture matrix, so handing Remix the raw values tiles the atlas ~1024x, which
reads as untextured. Now baked into the vertex at build time.

That explains the texture. It does NOT explain holes, so:

### Diagnostic added rather than another guess
The build now audits the captured vertex data itself, independently of anything Remix does:
  - how many captured vertices are ALL-ZERO, i.e. never written by the skinning loop;
  - the position range on each axis;
  - the uv range after scaling.
If more than a quarter are zero it says so outright: every triangle touching one collapses to the
origin and reads as a hole, and that would mean the CAPTURE is incomplete rather than the index
rebasing - which every previous counter was blind to, because a zeroed vertex is perfectly
in-range.

### Deployed
    sr3-rtx.asi  (rebuilt)  ini 0522311c7684be32b391d3955dfd9f0b
    rtx.conf 421b35520b662159df313db2a6df3205 | bridge.conf db55f8142db1db21eb4bdf256f5c4ae5
Source backup: src/sr3-rtx/sr3rtx.cpp.before-uvscale

## 2026-08-31 - the holes: the mesh arrays were LOCAL and died at the end of the builder

The vertex audit came back completely clean, and that is what made this findable:

    0 of 7977 vertices are all-zero
    pos x[-0.33 0.22] y[-0.07 1.79] z[-0.22 0.17]     - a proper character bounding box
    uv  u[0.010 0.994] v[0.009 0.986]                 - textbook 0..1 after the x1/1024 fix

Sound data, correct rebasing, 36/36 slots, correct placement. Which leaves only the DESCRIPTION.

### Cause
remixapi_MeshInfoSurfaceTriangles holds POINTERS - vertices_values, indices_values - and nothing
in the header says CreateMesh copies them. The character built its vertex and index arrays as
LOCAL std::vectors inside RemixBuildCharacter, so they were freed the moment it returned, and every
DrawInstance afterwards read released memory.

The step-2 cube has worked since its first correct build precisely because its arrays are static
globals (g_remixCubeVerts / g_remixCubeIdx). The difference between the working case and the broken
one was sitting in the file the whole time.

A complete, sound, correctly rebased mesh reading as "a bunch of holes" and smeared texture is
exactly what use-after-free looks like here, and no counter can see it: every number describes the
data at BUILD time, when it was still alive.

### Fix
g_apiCharVerts, g_apiCharSlotIndices and g_apiCharSurfaces are now globals that outlive the call.
The surface array is still populated only AFTER every inner index vector is final - taking a
pointer into a vector that is later resized is the same mistake one level down.

### Also fixed this round
UVs are scaled by kShortUVScale (1/1024) when building API vertices. SR3's bind-pose uvs are raw
shorts; the game's shaders apply `mul o1.xy, r0, 0.0009765625` and the fixed-function path uses a
uv texture matrix. An API mesh has neither, so the scale is baked into the vertex. Confirmed by the
audit: uvs now span 0.010..0.994.

### Deployed
    sr3-rtx.asi  (rebuilt)  ini 0522311c7684be32b391d3955dfd9f0b
    rtx.conf 421b35520b662159df313db2a6df3205 | bridge.conf db55f8142db1db21eb4bdf256f5c4ae5
Source backup: src/sr3-rtx/sr3rtx.cpp.before-lifetime

## 2026-08-31 - TEXTURED. Remaining: holes (duplicate slots) and body-only (by design)

User: "it is textured but it still has a lot of holes and is only the body. no hair. no clothes."

TEXTURED is the win: the x1/1024 uv scale and the array-lifetime fix both landed. The audit
confirms it independently - uvs span 0.010..0.994 and no captured vertex is zeroed.

### "only the body" is SCOPE, not a defect
The capture takes ONE vertex buffer. From the 2026-08-31 frame dump the player is the body
(vb=49186410, 7977 verts) PLUS about 19 separate meshes on their own buffers - 49186B50, 49186EF0,
491864F8, 49187718, 3CA094C8 and others - which are the head, hair, clothing and accessories. They
were never in scope for 3b and their absence is expected. Capturing them is step 3e.

### The holes: duplicate index ranges, and the evidence was already in our own log
The shim's skinning report has said "19 exact duplicates dropped/frame" all along - SR3 submits the
same skinned geometry more than once a frame, and the FFP path has always deduped it. The slot
collector had no such filter, so an identical index range entered the mesh twice and two coplanar
copies of a surface z-fight against each other.

36 collected slots with ~19 duplicates is about half of them, which also matches the earlier
"half the polygons missing at random" that I misattributed to overlap with the game's own character
and then to baseVertex. Both of those were wrong; this is the same symptom's actual cause.

No counter could show it because every slot is individually valid - correct range, correct base,
in-bounds indices. Only the RELATIONSHIP between slots is wrong.

### Fix
Slots are deduped on (startIndex, primitiveCount, baseVertex) at collection time, and the dropped
count is reported beside the kept one.

### Deployed
    sr3-rtx.asi  (rebuilt)  ini 0522311c7684be32b391d3955dfd9f0b
    rtx.conf 421b35520b662159df313db2a6df3205 | bridge.conf db55f8142db1db21eb4bdf256f5c4ae5
Source backup: src/sr3-rtx/sr3rtx.cpp.before-slotdedup

## 2026-08-31 - the holes: four theories dead, so print the mesh instead of inferring it

    36 surfaces from 36 slots (0 duplicate index ranges dropped), 7977 verts, 19359 triangles
    0 out-of-range | vertex audit clean | textured correctly

Four causes have now been proposed for the holes, from the picture, and all four are dead:
  1. overlap with the game's own character   - offsetting it aside changed nothing
  2. a baseVertex rebasing mistake           - the fix was a measurable no-op
  3. out-of-range indices                    - 0
  4. duplicate index ranges                  - 0 dropped
Every aggregate number describes a healthy mesh. That is the point: the aggregates cannot see this,
and I kept proposing mechanisms instead of printing the structure.

METHOD NOTE, and it is the same one this project recorded in August: when a symptom survives more
than two hypotheses, stop hypothesising and measure the thing directly. Four is too many.

### Added: the slot structure, in full
At build time the log now prints, for every slot: startIndex, triangle count, baseVertex, the index
range it covers, and whether that range OVERLAPS another slot's. Plus the number of distinct
vertices referenced by any slot at all.

Two specific things it can show that nothing so far could:
  - ranges that overlap WITHOUT being identical. LOD variants or nested submissions z-fight exactly
    like duplicates while defeating the equality test that reported 0.
  - a referenced-vertex count well below 7977, which would mean the slots simply do not cover the
    body and the holes are geometry never submitted at all.

### Deployed
    sr3-rtx.asi  (rebuilt)  ini 0522311c7684be32b391d3955dfd9f0b
    rtx.conf 421b35520b662159df313db2a6df3205 | bridge.conf db55f8142db1db21eb4bdf256f5c4ae5
Source backup: src/sr3-rtx/sr3rtx.cpp.before-slotdump

## 2026-08-31 - THE HOLES: SR3's character slots are TRIANGLE STRIPS. Found by printing the structure.

The slot dump answered it in one run, after four hypotheses from the picture had failed. The ranges
tile the index buffer EXACTLY when a slot consumes (prims + 2) indices instead of (prims * 3):

    slot  0: start   741, 3408 tris ->  741 + 3408 + 2 = 4151 = slot  8's start
    slot  8: start  4151,  250 tris -> 4151 +  250 + 2 = 4403 = slot  9's start
    slot  9: start  4403,  628 tris -> 4403 +  628 + 2 = 5033 = slot 10's start
    slot 35: start 18464, 2972 tris -> 18464 + 2972 + 2 = 21438 = slot  5's start

Four independent confirmations, exact. These are D3DPT_TRIANGLESTRIP draws.

### What that did
Reading prims*3 indices per slot ran THREE TIMES TOO FAR - into the following slots' data - and
then formed triangles from consecutive STRIP vertices as though they were independent triples.
Hence holes everywhere, while every aggregate stayed healthy: the indices were all real, all in
range, all distinct, and the vertex data was provably sound.

Hook_DrawIndexedPrimitive is handed the primitive type as its FIRST parameter, and every version of
this feature ignored it. The "36 of 36 slots overlap" line was the strip length being misread, not
a real overlap.

### Fix
Slots record their D3DPRIMITIVETYPE. Strips read prims+2 indices and expand to a triangle list with
alternating winding, dropping degenerate joins (a repeated index, used to stitch strips together)
rather than emitting slivers. Lists keep the old path. Anything else is skipped rather than guessed
at. The count of strip slots and dropped degenerates is reported.

### The method point, recorded because it cost four runs
Every one of the four dead hypotheses was proposed from the SCREENSHOT. The one that worked came
from printing the data structure and doing arithmetic on it. The project's own August rule -
"when a symptom survives more than two hypotheses, stop hypothesising and measure" - was right, and
I passed it by two.

### Deployed
    sr3-rtx.asi  (rebuilt)  ini 0522311c7684be32b391d3955dfd9f0b
    rtx.conf 421b35520b662159df313db2a6df3205 | bridge.conf db55f8142db1db21eb4bdf256f5c4ae5
Source backup: src/sr3-rtx/sr3rtx.cpp.before-strips

## 2026-08-31 - STEP 3b COMPLETE: the character copy is correct. Now the tint.

User: "now the copy is right. the texture tint is probably not correct."

The triangle-strip fix closed the holes. A character mesh described ENTIRELY through the Remix API -
geometry, indices, placement and a NAMED albedo texture - now renders correctly beside the game's
own copy. That is step 3b done.

### The tint, which is the customisation
SR3 multiplies its diffuse result by Tint_color (c37 in the cloth family); the full model is
already documented above ShaderInfo in this file - three Diffuse_Color_a/b/c weighted by the
Pattern_Map channels at gamma 2.2, then `mul oC0, r1, c37`. The API material was built with
albedoConstant WHITE, so that multiply was simply dropped.

The shim has measured this all along without acting on it:
    DIFFUSE_COLOR: 64.5 draws/frame carry it (30.2 skinned) | NON-WHITE 11.88/frame,
                   45 distinct values seen

### Change: one material per distinct tint, assigned PER SURFACE
Tint_color is captured per slot at collection time from g_psConst[tintColorReg]. At build, slots
are grouped by colour (within 0.004) and one material is created per distinct value, then assigned
to that slot's surface. remixapi_MeshInfoSurfaceTriangles carries a material per SURFACE precisely
so one mesh can do this.

Building one material per slot would be 36 registrations of mostly the same colour; building one
for the whole mesh is what threw the customisation away. Capped at 24 distinct tints, with a white
fallback for slots whose shader declares no Tint_color.

The build logs how many distinct tints were found and the first eight values, so "the tint is
wrong" can be separated from "the tint was never read" without another guess.

### Deployed
    sr3-rtx.asi  (rebuilt)  ini 0522311c7684be32b391d3955dfd9f0b
    rtx.conf 421b35520b662159df313db2a6df3205 | bridge.conf db55f8142db1db21eb4bdf256f5c4ae5
Source backup: src/sr3-rtx/sr3rtx.cpp.before-tint

### Remaining for a complete character
  - hair, clothes and accessories: ~19 more meshes on their own vertex buffers (step 3e)
  - live animation: BONE_TRANSFORMS_EXT, 256 bones against SR3's 58 (step 3c)
  - then remove the game's own copy and drop the diagnostic offset (step 3d)

## 2026-08-31 - STEP 3e BUILT: the whole character, one vertex buffer at a time

The tint landed ("i think it is now correct"), so step 3b is closed and the remaining gap is that
the copy is BODY ONLY. From the frame dump the player is the body plus about nineteen separate
meshes on their own vertex buffers - head, hair, clothing, accessories.

### Approach: repeat the working pipeline, do not refactor it
Capture one buffer, build it, FILE it, reset, and pick up the next buffer on a later frame. The
capture/collect/build path that now works is untouched; only its bookkeeping changed. About twenty
meshes therefore take roughly forty frames to accumulate, which is invisible in play.

### Two things that had to be right
1. PER-MESH STORAGE. remixapi_MeshInfoSurfaceTriangles keeps POINTERS to the vertex and index
   arrays - the lesson that cost a run when those arrays were function locals. Reusing one set of
   buffers for the next build would free the previous mesh's data out from under Remix in exactly
   the same way, one level up. Each finished mesh gets its own ApiMeshStore.
2. MEMBERSHIP. The size floor drops from 4000 to 64, because 4000 was chosen to find the BODY and
   would exclude most of what is missing. Parts are instead grouped by objTM TRANSLATION: every
   piece of one character shares it, so NPCs and props are excluded without needing to identify
   them. The first capture must still be a >=4000-vertex mesh, which fixes the reference position
   on the body rather than on a stray accessory.

### Robustness: a failure now skips, it does not stop
Every permanent `g_apiCharFailed = true` inside the builder became ApiSkipCurrentCapture(): the
buffer is remembered in a skip list and the next one is tried. Previously one accessory with an
index format we could not read would have set the flag and cost the other nineteen meshes. Six
failure sites plus two out-of-memory sites rewritten; zero permanent failures remain.

### Deployed
    sr3-rtx.asi  (rebuilt)  ini 0522311c7684be32b391d3955dfd9f0b
    rtx.conf 421b35520b662159df313db2a6df3205 | bridge.conf db55f8142db1db21eb4bdf256f5c4ae5
Source backup: src/sr3-rtx/sr3rtx.cpp.before-step3e (and .before-skipfail)

### Expected
    STEP 3e CHARACTER: N meshes, T triangles total | latest: ...
The copy beside the player gains parts over the first second or so - head, hair, clothes - until
it stops growing. Still frozen; animation is step 3c.

## 2026-08-31 - step 3e: parts mis-placed and mis-textured. Two independent faults, both mine.

User: "the parts are not textured correctly and are not placed correctly".

### Placement: a capture-WINDOW bug, not a transform bug
The first 3e captured ONE buffer per frame - build, reset, take the next one later. That freezes
each part at the objTM and in the POSE it had on ITS OWN frame, so the instant the player moves the
parts scatter and each is mid-a-different-animation. The transform code was right all along.

Fixed: one capture FRAME takes every part, into a pending table. Builds are drained afterwards, one
per Present, by loading each pending entry into the globals the builder already reads - so the
build path itself is unchanged.

### Texture: every part was pointed at the BODY's atlas
Only the body uses 3BD233F0. Head, hair, clothing and accessories carry their own textures -
4923C980, 16381958, 4923F2A0 and about sixteen more in the frame dump.

Fixed: DumpTextureDds() writes ANY game texture to a DDS, cached by pointer, and each part's
material names its own. Compressed formats (DXT1/3/5) are copied through UNCHANGED - a DDS carries
DXT blocks natively, so the fourCC is set and the bytes written, which avoids a block decoder and
keeps the texture bit-exact. A8R8G8B8/X8R8G8B8 are written as BGRA8 with alpha forced to 255.
Anything else, or a texture that cannot be locked (DEFAULT pool), logs the reason and falls back to
the atlas rather than failing the part.

### The declaration order finally restructured, as promised
This was the FOURTH build broken by step-3 declaration order, each previous fix moving one more
global earlier. The cause is structural: the capture runs in SkinAndBind, the build near the draw
hook, and the per-frame DrawInstance tick far above both - so a type used by all three has exactly
one legal home. ApiSlot and ApiPending are now together in a commented block immediately before
SkinAndBind, with a note saying only raw handles may live earlier and why.

### Deployed
    sr3-rtx.asi  (rebuilt)  ini 0522311c7684be32b391d3955dfd9f0b
    rtx.conf 421b35520b662159df313db2a6df3205 | bridge.conf db55f8142db1db21eb4bdf256f5c4ae5
Source backups: .before-pending, .before-texdump, .before-reorder
Stale sr3-remix-tex-*.dds deleted before the run, so any present afterwards were written by it.

## 2026-08-31 - only the body again: a window that opens MID-FRAME can only collect the tail of one

User: "i do not see any parts now at all. its just a textured body".

The one-frame capture opened its window at the instant the body was recognised - and SR3 draws the
body LATE, so every part drawn earlier in that same frame had already gone past. The window was
real, it just started too late in the frame to contain anything but its own trigger.

### Fix: a scouting frame
The frame that finds the body no longer captures. It records the reference position and schedules
the real window for g_frames + 1, which is then collected WHOLE - body included, and in draw order
from the first draw of the frame.

Two details that follow from the split:
  - the membership tolerance widens from 0.25 to 4.0 (squared), because the character may move
    between the scouting frame and the capture frame;
  - the FIRST part taken on the capture frame re-anchors the reference position, so every later
    part is matched against a position from the SAME frame rather than the previous one.

Both logged: "character found at (x y z) - capturing every part of it on frame N", then
"capture window closed - N parts of the character taken in one frame". Those two lines separate
"the window never opened" from "the window opened and found nothing", which the previous build
could not distinguish.

### Deployed
    sr3-rtx.asi  (rebuilt)  ini 0522311c7684be32b391d3955dfd9f0b
    rtx.conf 421b35520b662159df313db2a6df3205 | bridge.conf db55f8142db1db21eb4bdf256f5c4ae5
Source backup: src/sr3-rtx/sr3rtx.cpp.before-window

## 2026-08-31 - one part built, no errors: the post-build reset was killing the drain

    remix api: character found at (96.9 145.7 29.7) - capturing every part of it on frame 1354
    remix api: capture window closed - 10 parts of the character taken in one frame
    STEP 3b - character mesh built: 2 surfaces, 1080 verts, 576 triangles, extent 0.10 x 0.14 x 0.14
    (no skips, no failures, no further builds)

The scouting-frame window WORKED - 10 parts captured in one frame, which is the thing the previous
run could not do. But only the first was ever built.

### Cause
The post-build reset ended with `g_apiCapFrame = 0xFFFFFFFFu;` and the builder's third line is

    if (g_apiCapFrame == 0xFFFFFFFFu || g_frames <= g_apiCapFrame) return;   // window still open

so completing ONE mesh set the sentinel that makes every later call return immediately. No skip, no
failure, no log - exactly how it presented, and why the symptom looked like a capture problem when
the capture had already succeeded.

That line was correct in the ORIGINAL sequential design, where each build ended one capture and
began another. Under the pending table the window frame is history that must be kept. The same line
was in ApiSkipCurrentCapture and had the same effect; both are now commented rather than deleted,
because the reason is not obvious from the code.

The two diagnostic lines added last round are what made this a five-minute diagnosis instead of
another guess: "window closed - 10 parts" proved the capture was fine, so only the drain was left.

### Deployed
    sr3-rtx.asi  (rebuilt)  ini 0522311c7684be32b391d3955dfd9f0b
    rtx.conf 421b35520b662159df313db2a6df3205 | bridge.conf db55f8142db1db21eb4bdf256f5c4ae5
Source backup: src/sr3-rtx/sr3rtx.cpp.before-drainfix

## 2026-09-01 - 6 of 10 parts built. The 4 failures were a call-ORDER bug.

    capture window closed - 10 parts of the character taken in one frame
    6 meshes built, including the BODY: 36 surfaces, 7977 verts, 13524 triangles
    5 textures dumped: 4 compressed copied through (256x256 and 64x64), 1 BGRA8 2048x1024 atlas
    pending part 0, 6, 8, 9: "no usable slots"

The drain fix worked and the per-part texture dumping works - compressed formats copy through
bit-exact, which was the right call over writing a block decoder.

### Why four parts had no slots
RemixCollectCharacterSlot ran at the TOP of Hook_DrawIndexedPrimitive, which is BEFORE
Classify -> SkinAndBind captures the part. So on the first draw of any new vertex buffer there was
no pending entry to attach the slot to, and the slot was dropped. A part drawn ONCE therefore ended
with zero slots and was skipped.

That also explains the pattern exactly: the parts that survived are the ones drawn more than once,
and the body - drawn 36 times - was never at risk.

### Fix
The collect call moves to immediately AFTER g_origDrawIndexedPrimitive. The capture has happened by
then, and g_stream0 / g_curPS still describe the game's draw because EndFFP - which restores state -
runs after it.

### Deployed
    sr3-rtx.asi  (rebuilt)  ini 0522311c7684be32b391d3955dfd9f0b
    rtx.conf 421b35520b662159df313db2a6df3205 | bridge.conf db55f8142db1db21eb4bdf256f5c4ae5
Source backup: src/sr3-rtx/sr3rtx.cpp.before-collectorder

## 2026-09-02 - STEP 3e COMPLETE (10/10 parts), and STEP 3c BUILT: the character is now LIVE

    capture window closed - 10 parts of the character taken in one frame
    10 meshes built, 21,990 triangles, 9 textures dumped
The call-order fix took it from 6 to 10. Step 3e is done.

### Step 3c - Remix does the skinning now
A frozen character is not a finished one, so the mesh is now uploaded ONCE in its BIND POSE with
skinning data, and only the bones and objTM move afterwards.

  mesh      remixapi_MeshInfoSkinning on every surface: bonesPerVertex 4, blend weights and bone
            indices taken from the shim's existing decoded bind pose (BaseVertex already carries
            pos/nrm/uv/w[4]/idx[4], so nothing new had to be decoded).
            Weights are NORMALISED here - SR3's shader divides by their sum and Remix will not.
  instance  REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BONE_TRANSFORMS_EXT (=14), chained on pNext, up to
            256 bones; SR3 uses 58 and the palette is 64.
  per frame every frame the game draws one of our captured parts, its objTM AND its bone palette
            are snapshotted in the draw hook. That is the whole of the animation - no per-frame
            CreateMesh.

The layouts line up exactly, so this is a copy rather than a conversion: SR3's bone palette is a
row-major float3x4 in three registers per bone with translation in the fourth component of each
row, and remixapi_Transform is float[3][4] with translation in the last column - the same
convention the instance transform already proved.

Consequence worth noting: for anything on the API, the shim's CPU skinning becomes unnecessary.
That is currently a large part of the 15 ms shim cost.

### Deployed
    sr3-rtx.asi  (rebuilt)  ini 0522311c7684be32b391d3955dfd9f0b
    rtx.conf 421b35520b662159df313db2a6df3205 | bridge.conf db55f8142db1db21eb4bdf256f5c4ae5
Source backup: src/sr3-rtx/sr3rtx.cpp.before-step3c

### Still open on the character
  - eyes render wrong. All 10 meshes build, so it is a MATERIAL problem: alphaTestType is forced
    to 7 (always pass) with a fully opaque material on every part, which would flatten the
    alpha-blended or two-layer shader eyes normally use.
  - the diagnostic X offset of 3 is still standing the copy beside the game's own character; it
    goes to 0 when the game's copy is removed (step 3d).

## 2026-09-02 - step 3c CRASHED the game. Skinning gated OFF; the working character is restored.

    *** CRASH: 0xC0000005 at 713080CE (d3d9.dll +0x780CE)
        access violation reading address 4BD17000
        shim state: frame 1160 draw 6222
    minidump: Saints Row 3/sr3-rtx-crash.dmp

The log shows the capture and the slot walk completing normally and then nothing - 10 parts taken,
the audit clean, the slot structure printed, ZERO meshes built. The crash is inside the BRIDGE
during the first CreateMesh, so Remix read past one of the two skinning arrays.

### Response: restore the working build first
remixApiSkinning is a new setting, DEFAULT OFF. With it off the character is submitted
already-skinned and frozen - exactly the 10-part state that worked - so the game is playable again
while the contract is worked out. The whole skinning path is kept behind the switch rather than
reverted, because it is one ini flip from being testable again.

### Two hardening changes kept regardless of the eventual cause
1. The FULL 64-bone palette is always sent, initialised to IDENTITY at build time. maxBone is
   computed only over NON-ZERO weights, so a vertex carrying a high bone index with a zero weight
   can legitimately exceed maxBone+1 - and an index beyond boneTransforms_count would have Remix
   read past the array. Sending the whole palette removes the possibility rather than reasoning
   about it. This is a plausible cause of the crash on its own.
2. A surface only claims skinning when BOTH arrays are exactly 4 x vertexCount.

### What is NOT yet known
remix_c.h says the count must be bonesPerVertex * vertexCount, which is what was passed
(4 x 1080 = 4320 for the part being built). If it still crashes with the guards above, the next
suspect is the bridge's own marshalling of those arrays across the 32-to-64-bit boundary rather
than anything in this file - and that would be checked by shrinking to a single tiny test mesh
rather than by another full run.

### Deployed
    sr3-rtx.asi  f52e13c90241598c6fc8270d2d819c1d
    sr3-rtx.map  4c0727a20ea1c23328aa4d73d1748855
    sr3-rtx.ini  807553e47b726a8468d755be3f186112   (remixApiSkinning=0)
    rtx.conf 421b35520b662159df313db2a6df3205 | bridge.conf db55f8142db1db21eb4bdf256f5c4ae5
Source backup: src/sr3-rtx/sr3rtx.cpp.before-skingate

## 2026-09-02 - materials read from the DRAW instead of hardcoded

"the character is almost fully correct but we need to do better work."

The named defect is the eyes, and the cause was structural rather than specific: EVERY part was
given one hardcoded material - fully opaque, alphaTestType 7 (ALWAYS), one roughness, one
opacityConstant. That is correct for the body and wrong for anything with a cutout or a blend,
which is precisely what eyes use.

### The mapping, which is exact rather than approximate
Remix's AlphaTestType mirrors VkCompareOp (NEVER=0 .. ALWAYS=7) and D3D9's D3DCMPFUNC runs
NEVER=1 .. ALWAYS=8. So the conversion is (func - 1), and alpha test DISABLED maps to ALWAYS - the
old hardcoded 7, which is why the body always looked right and everything else did not.

### Change
Each slot now records the game's real state at the moment of its draw:
    D3DRS_ALPHATESTENABLE / ALPHAFUNC / ALPHAREF / ALPHABLENDENABLE / SRCBLEND / DESTBLEND
and the material key becomes (tint, alphaTestType, alphaRef, blendOn) instead of tint alone. Alpha
is part of a material's IDENTITY, not a property of the mesh: two slots of one part legitimately
differ only in whether they are cut out, and a shared opaque material is what loses them.

Blended draws are given opacityConstant 0.85 rather than a blendType_value, because that
enumeration is not in the header and guessing it is how the alphaTestType=0 invisibility happened.
Stated as a deliberate approximation rather than left implicit.

### Measurement shipped with it
The build now lists every distinct material: tint, alpha test function BY NAME, reference value,
and whether blending is on. If the eyes are still wrong, that line says whether their draw was
read as cut-out, blended or opaque - which separates "we read the state wrongly" from "we read it
correctly and Remix needs something else".

### Deployed
    sr3-rtx.asi  (rebuilt)  ini 807553e47b726a8468d755be3f186112 (remixApiSkinning=0)
    rtx.conf 421b35520b662159df313db2a6df3205 | bridge.conf db55f8142db1db21eb4bdf256f5c4ae5
Source backup: src/sr3-rtx/sr3rtx.cpp.before-material

## 2026-09-02 - "its just got the body albedo": the texture was stored per PART, not per SLOT

User, on the eyelashes/eyelids: "its just got the body albedo".

That is the atlas fallback, and the evidence was already in the log and not chased: 9 textures
dumped for 10 parts.

### Cause
One albedo was stored per PART, taken from g_curPS.albedoStage on its FIRST draw:
  - a shader with no ranked albedo gave null, and the part fell back to the BODY's atlas;
  - and slots within one part that legitimately use different textures could never both be right,
    even when the first one resolved.
The material was already per SURFACE. The texture had no business being per part.

### Change
ApiSlot carries its own albedo, recorded at slot-collection time as the ranked stage if the shader
has one and otherwise whatever is ACTUALLY BOUND at stage 0 - never the body's atlas, which is
never right for a part that simply has no ranked sampler. The material key becomes
(texture, tint, alphaTestType, alphaRef, blendOn), and each material names its own dumped DDS.
The cap rises from 24 to 32 distinct materials.

### Measurement
The material listing now prints the texture pointer per material and marks any that has none:
    mat N: tex 0x........, tint (...), alphaTest GREATER_EQ ref 128, blend off
    mat N: tex 0000000000, ...  <- NO texture of its own, using the fallback
So "wrong texture" and "no texture found" are now different lines rather than the same picture.

### Deployed
    sr3-rtx.asi  (rebuilt)  ini 807553e47b726a8468d755be3f186112 (remixApiSkinning=0)
    rtx.conf 421b35520b662159df313db2a6df3205 | bridge.conf db55f8142db1db21eb4bdf256f5c4ae5
Source backup: src/sr3-rtx/sr3rtx.cpp.before-slottex
Stale sr3-remix-tex-*.dds cleared, so the dumps present after a run are this run's.

## 2026-09-02 - eyelashes fixed; and the material listing exposed a tint of 5.0

    STEP 3e CHARACTER: 10 meshes, 21990 triangles | 11 textures | 0 materials without a texture
User: "the eyelashes are textured correctly now."

Per-slot textures closed that out. 11 textures for 10 parts is the expected shape once slots within
one part may differ.

### What the new listing showed, unprompted
    mat 0: tex 4A9D7EF8, tint (1.000 1.000 1.000), alphaTest ALWAYS ref 0, blend off
    mat 1: tex 4A9D7EF8, tint (5.000 5.000 5.000), alphaTest ALWAYS ref 0, blend ON
Every part has exactly two materials, and the second carries a tint of 5.0 with blending ON.

albedoConstant is a COLOUR - 0..1. Five is not a base colour, it is a multiplier, and handing it
to albedoConstant coats the surface in blown-out white. A 5x multiplier on a BLENDED pass is
almost certainly an additive glow or sheen rather than a second opaque skin.

This is the value the user was asked to judge as "the tint" and approved as probably correct. It
was not: it looked plausible because the opaque material underneath it is right. Only printing the
number made it visible.

### Change
- albedoConstant is clamped to [0,1], and the listing marks any value that was clamped, so the raw
  reading stays visible rather than being silently corrected.
- The listing now prints D3DRS_SRCBLEND / D3DRS_DESTBLEND per material, with the enumeration in a
  comment: src ONE / dst ONE is additive, SRCALPHA / INVSRCALPHA is ordinary alpha.

That last line decides the next step honestly: if the blended slot is ADDITIVE it should be an
emissive material or dropped, not a translucent skin over the character - and that is a decision to
take from the blend factors, not from the picture.

### Deployed
    sr3-rtx.asi  (rebuilt)  ini 807553e47b726a8468d755be3f186112 (remixApiSkinning=0)
    rtx.conf 421b35520b662159df313db2a6df3205 | bridge.conf db55f8142db1db21eb4bdf256f5c4ae5
Source backup: src/sr3-rtx/sr3rtx.cpp.before-tintclamp

## 2026-09-02 - the blend FLAG is not the blend. The FACTORS are.

The listing added last round answered its own question in one run:

    11 materials: blend ON (src 2 dst 1)   = D3DBLEND_ONE / D3DBLEND_ZERO
     1 material:  blend ON (src 5 dst 6)   = SRCALPHA / INVSRCALPHA
    10 materials: blend off

src=ONE, dst=ZERO is the IDENTITY blend: result = source. D3DRS_ALPHABLENDENABLE is set, but the
factors make it an ordinary OPAQUE write. So eleven of the twelve "blended" slots are not
translucent at all, and giving them opacityConstant 0.85 thinned the entire character. Exactly one
slot is genuinely alpha-blended.

Reading the enable flag alone was the error; the flag is necessary but not sufficient.

### Change
    identityBlend = (srcBlend == D3DBLEND_ONE && destBlend == D3DBLEND_ZERO)
    translucent   = alphaBlendEnable && !identityBlend
Only genuinely blending factors produce a translucent material now.

### Note on the 5.0 tint, kept clamped
Those same eleven slots carry Tint_color = 5.0. With src=ONE/dst=ZERO they are plain opaque draws
with a 5x multiplier, so the constant is an intensity rather than a base colour - albedoConstant
cannot exceed 1 and the clamp stands. Recorded rather than resolved: if the character later reads
as too dark in those slots, this constant is the first suspect and the raw value is in the log.

### Deployed
    sr3-rtx.asi  (rebuilt)  ini 807553e47b726a8468d755be3f186112 (remixApiSkinning=0)
    rtx.conf 421b35520b662159df313db2a6df3205 | bridge.conf db55f8142db1db21eb4bdf256f5c4ae5
Source backup: src/sr3-rtx/sr3rtx.cpp.before-blendfactors

## 2026-09-03 - white clothes, bracelets, piercings: the customisation RECIPE, not a missing texture

User: "the players underwear, bracelets, piercings, etc are white. the only thing textured on the
real player was the top while the copy only has the body textured correctly."

Note the first half: THE REAL PLAYER IS ALSO MOSTLY UNTEXTURED. This is not an API-path defect. Both
paths bind a texture where the game computes a colour.

### What those draws actually are
Frame dump, player position, by first sampler:
    35 Blend_MapSampler      rank=100
    17 Diffuse_MapSampler    rank=100
     3 IR_GBuffer_DSF_Data   rank=80      <- rank 80 is Pattern_Map
A Pattern_Map is a MASK, not a colour. SR3's clothing and accessories have no diffuse texture
holding their colour at all: it is three CONSTANTS - Diffuse_Color_a/b/c - weighted by the
pattern's r/g/b channels at gamma 2.2, with a selector that sends some texels to a desaturated
branch so trim and skin escape tinting, and the whole result multiplied by Tint_color. That recipe
is already documented above ShaderInfo, read out of ir_sr3npcclothfull_c shader[8].

Binding the raw pattern as albedo is exactly why these items are white.

### Change
1. A slot whose shader declares Diffuse_Color_a/b/c is marked as CUSTOMISED, and its three colours
   are captured with it.
2. For those slots the shim's existing ClothAlbedo() is asked to GENERATE the albedo - pattern
   combined with the three colours. Where it succeeds that is the correct texture and beats
   anything bound.
3. Where it cannot - it only handles the family where the pattern IS the albedo, and the player's
   own clothing samples a Diffuse_Map through a SECOND UV SET which cannot be folded into one
   texture - the material takes Diffuse_Color_a as its albedoConstant instead of Tint_color.

Point 3 is an APPROXIMATION and is labelled as one in the code: the real recipe weights all three
colours per texel and a single constant cannot express that. It is the difference between a white
bracelet and a roughly right one, not correctness. The log reports the split:
    CUSTOMISED items - N albedos GENERATED from pattern x Diffuse_Color, M use Diffuse_Color_a as
    a flat approximation
so how much is properly generated versus approximated is visible rather than assumed.

### The honest remaining gap
The player's clothing family needs the two UV sets reconciled before its albedo can be generated
properly. That is a real piece of work and is NOT done here - ProbeClothUV in this file is the
existing note on it.

### Deployed
    sr3-rtx.asi  04d907dc4d603f96a7fff03c6afdb5d4
    sr3-rtx.map  ddde2193c3014e2b8e30550774e3cff2
    ini 807553e47b726a8468d755be3f186112 (generateCloth=1, remixApiSkinning=0)
    rtx.conf 421b35520b662159df313db2a6df3205 | bridge.conf db55f8142db1db21eb4bdf256f5c4ae5
Source backup: src/sr3-rtx/sr3rtx.cpp.before-cloth

## 2026-09-03 - REGRESSION: the top went black. The fix was applied wider than the fault.

User: "the top of the copy is now black."

Mine. Diffuse_Color_a was applied as albedoConstant to EVERY slot carrying the cloth recipe -
including the top, which already had a real Diffuse_Map at rank 100 and rendered correctly. Its
Diffuse_Color_a is near-black, because for that shader the colour comes from the texture and the
constant is not the base colour at all. So the top went black.

The fault was never "cloth slots have the wrong constant". It was "slots whose chosen albedo is a
PATTERN have no colour". Widening the fix past its case broke a working surface.

### Two guards, both narrowing
1. The approximation applies only when the slot's chosen albedo ranks <= 80 - a Pattern_Map bound
   as base colour, which is the case that renders white. A slot with a genuine Diffuse_Map (100)
   keeps its texture and its tint, untouched.
2. A near-black constant is never an improvement on white. If Diffuse_Color_a reads as unset
   (luminance <= 0.02) the surface is left alone, and the count is reported:
       N customised slots had a near-BLACK Diffuse_Color_a and were left with their own texture

Guard 2 would have prevented this regression on its own, which is why it is in rather than just
guard 1.

### Still true and NOT caused by this work
"the original clothes ... are not textured correctly even with ray tracing off." The GAME's own
render has the same defect, so the customisation recipe is unimplemented in both paths, not broken
in the API path. The player clothing family still needs its two UV sets reconciled before its
albedo can be generated properly - unchanged, and still the real remaining work.

### Deployed
    sr3-rtx.asi  (rebuilt)  ini 807553e47b726a8468d755be3f186112
    rtx.conf 421b35520b662159df313db2a6df3205 | bridge.conf db55f8142db1db21eb4bdf256f5c4ae5
Source backup: src/sr3-rtx/sr3rtx.cpp.before-clothfix

## 2026-09-03 - BAKING the pixel shader's output. The custom shader, finally, and for the right reason.

User: "why cant we save or get what the pixel shader computes?" - and the criticism that the
material tweaking was regressing rather than solving. Both correct.

### Why no amount of material work could have fixed the clothes
Remix NEVER RUNS A PIXEL SHADER. It picks one bound texture as the albedo and applies its own PBR
model. SR3's clothing colour exists only as the OUTPUT of a pixel shader - three constants weighted
per texel by a Pattern_Map at gamma 2.2, a selector branch, then x Tint_color. There is no texture
holding it and no constant equal to it. Choosing a better texture or a better constant was
therefore structurally incapable of working, and the Diffuse_Color_a approximation that blacked the
top was that dead end reaching its natural end.

### The method
Run the game's own pixel shader into an offscreen target whose SCREEN SPACE IS UV SPACE. Every
texel of the output then holds exactly the colour that shader computes for that texel. Not an
approximation of the recipe - the recipe, executed by the same shader with the same constants and
the same textures. The engine already uses this technique itself: the 2048x1024 character atlas is
a bake.

The cloth pixel shader writes only oC0, which is what makes it bakeable.

### What had to be built: a vs_3_0 assembler
The game provides no vertex shader that maps UV to clip space, so one is assembled directly as
tokens. It does NOT patch the game's shader: in SM3.0 vertex inputs bind by DECLARATION USAGE
rather than register number, so a standalone shader declaring dcl_texcoord v0 receives the texture
coordinates from the game's own vertex declaration.

    position.x = u * 2/1024 - 1
    position.y = 1 - v * 2/1024      (texture space runs down, clip space up)
    position.zw = (0, 1)
The 1/1024 is folded into the literal because SR3's texcoords are RAW SHORTS - the same scale the
uv work established earlier.

Seven texcoord outputs are written with the same uv, because the cloth pixel shader declares
v0..v6 and which one carries the pattern's coordinates is not yet known. A wrong guess there shows
as a mis-sampled bake, not a crash.

### THIS BUILD ONLY VALIDATES THE ASSEMBLER
Hand-assembled bytecode is exactly the kind of thing that fails silently, so nothing is baked yet.
The shader is created and D3D9 is asked to validate it:
    bake: uv-bake vertex shader accepted by D3D9 (N tokens)          -> the assembler is correct
    bake: D3D9 REJECTED the hand-assembled uv-bake vertex shader ... -> it is not, and baking
                                                                        disables itself
D3D9 validates the token stream, so this is a real check and not a self-assessment. The bake pass
is only worth building on top of a shader that is known good.

### Deployed
    sr3-rtx.asi  9de699dfc2c2dcf1d8fc3dbb62af6a7f
    sr3-rtx.map  ca569231ff50dd9a7b3c99a0468edbe8
    ini 807553e47b726a8468d755be3f186112 (remixApiClothAlbedo=0 - the generated-albedo path that
        blacked the top is OFF; bakeShaderAlbedo defaults on but only builds the shader)
Source backup: src/sr3-rtx/sr3rtx.cpp.before-bake

## 2026-09-03 - the GPU bake crashes Remix. Twice, same address. Off.

    bake: uv-bake vertex shader accepted by D3D9 (75 tokens)   <- the assembler is CORRECT
    Exception 0xc0000005 at 00007FFAD8FF5A39 (NvRemixBridge.exe)   <- both runs, same address

No bake ever completed. The crash is in Remix's 64-bit server, not in the shim.

Tried and did NOT help: wrapping the bake draw in an occlusion query. Remix's own log says it
ignores draws issued inside one ("Trying to raytrace an occlusion query. Ignoring."), so this
should have kept the bake invisible to it. Same crash, same address - which means the fault is not
the DRAW being captured. The remaining suspects are the render-target switch itself
(SetRenderTarget to a texture we created) or GetRenderTargetData, both of which Remix intercepts.

What IS established and worth keeping:
  - the hand-assembled vs_3_0 is valid; D3D9 accepted it (75 tokens).
  - the approach is sound in principle - the engine bakes its own character atlas the same way.
  - it cannot be done through the Remix device.

bakeShaderAlbedo is off. The game is stable and the character is unchanged.

### Where this leaves the clothes
Remix never runs a pixel shader, so the computed colour has to be produced SOMEWHERE. The GPU route
through the Remix device is now closed. The remaining route is a CPU bake: the recipe is fully
documented above ShaderInfo and ClothAlbedo() already implements it for the family where the pattern
IS the albedo. The player's own clothing needs its two UV sets reconciled first - that is the real
outstanding work, and it is reverse engineering rather than another material switch.

## 2026-09-03 - clothes and hair now render correctly through the Remix API

### The clothes: a CPU bake of SR3's customisation recipe
The GPU bake was abandoned - driving a render target through the Remix device crashed its 64-bit
server twice at the same address, even with the draw hidden inside an occlusion query. The hand
assembled vs_3_0 that maps UV to clip space WAS valid (D3D9 accepted it, 75 tokens); the fault is
in Remix intercepting the render-target switch or the readback.

So the recipe is evaluated on the CPU instead. What it took, in the order the faults appeared:
  1. the second UV set. The cloth shaders declare three texcoord inputs and sample the pattern
     with TEXCOORD1. Only set 0 was ever decoded.
  2. the bind pose is cached from the FIRST draw on a buffer, which is a prepass with a reduced
     declaration and no TEXCOORD1 - so uv2 was always empty. Now re-decoded from the material
     draw, which is the only point where the buffer and a full declaration are both in hand.
  3. a fallback to TEXCOORD0 for the three parts whose shaders carry no second set at all. The
     corset was one of them, which is why it kept showing its segmentation map raw.
  4. Tint_color = 5.0 is not a colour. It multiplied a correct dark maroon albedo into pure
     magenta. Components above 1 are now treated as "no tint".
  5. DOUBLE GAMMA. The recipe works in linear light - both branches raise the pattern to 2.2 -
     but the DDS is 8-bit and Remix samples it as sRGB, so the 2.2 was applied twice. That is the
     "too dark with black noise" report exactly: an 8-bit encoding of a linear value has almost
     no precision left in the shadows. Now encoded back to sRGB on write.

### The hair
    slot 0: albedo 'Diffuse_MapSampler', first 'Diffuse_MapSampler', HAIR 1694 tris
Hair's chosen albedo is its Diffuse_Map, and for hair that map is smooth DIRECTIONAL data - the
blue/green/purple image on screen. It is not a colour, so binding it beat the colour constant.
Hair now takes Hair_Spec_Color2 with NO texture at all. Strand detail is a later problem.

The hair shader is LIT (it samples the L-buffer), so it is deliberately NOT baked - that would burn
the game's lighting into the albedo and Remix would light it twice.

### Also fixed: black texture dumps
A DEFAULT-pool or render-target texture can return SUCCESS from LockRect and hand back ZEROES. A
512x512 dump was black end to end and was being shipped as an albedo. An empty dump is now
discarded and the slot falls back, with the texture named in the log.

### What made the difference this session
Logging what each slot IS - its albedo sampler by name, its first sampler, cloth/hair - instead of
identifying parts from the screenshot and then guessing which shader they used. The hair was found
in one line after several runs of guessing.

## 2026-09-04 - what the GAME CODE says about customisation (verified, not inferred)

Reading the exe instead of guessing which shader file applies.

### The colours are plain 8-bit sRGB, divided by 255
0x008FCE60 and 0x00951A60 both set Diffuse_Color_a/b/c BY NAME, and immediately unpack the value:
    movzx ecx, al / movzx edx, ah / movzx eax, [esp+0x52]
    divsd xmm1, [0x12a2d88]        ; the constant decodes to exactly 255.0
So a customisation colour is the 8-bit swatch the player picked, over 255. Not linear, not
premultiplied, no hidden scale.

CONSEQUENCE, and it settles an argument that cost several runs: the pow(c, 2.2) in
ir_at_sr3pccloth is that SHADER moving an sRGB constant into linear light for its OWN lighting
maths. It is not part of the colour's identity. Remix lights from an sRGB albedo, so the colour
must be handed over AS AUTHORED, with no pow. That also explains why the two player cloth variants
disagree - ir_sr3pccloth uses the same constants directly (no pow, Diffuse_Color at c14,
Pattern_Map at s0) while ir_at_sr3pccloth pows them (c11, s2).

### The material is set by NAME, which is the model the shim already uses
0x00951A60 sets Diffuse_Color_a/b/c, "Hair", and Sphere_Map through one call (0xd9e8b0) taking a
name string. The shader's own constant table then maps that name to a register. So reflecting by
name per draw - which ReflectShader already does at line 868 for Pattern_MapSampler - is correct
by construction, and the varying registers between variants were never a bug.

### What the exe CANNOT answer
Which texture is bound to which sampler STAGE for a given garment. "Pattern_Map" does not appear as
a string in the exe at all - it lives in the material data files, not in code. Diffuse_Map,
Specular_Map, Normal_Map, Sphere_Map and Decal_Map do appear, but the binding is data-driven.

So the remaining gap genuinely needs a runtime probe rather than more disassembly, and that is now
established rather than assumed.

### Verified inventory
  colours are byte/255 sRGB                         exe, 0x008FCE60
  no engine-side gamma or scale on them             exe, same block
  parameters bound by name                          exe, 0x00951A60 -> 0xd9e8b0
  Pattern_Map register varies (s0 vs s2)            corpus, two files
  Diffuse_Color register varies (c11 vs c14)        corpus, two files
  shim resolves both by name already                sr3rtx.cpp:868
  the pattern we read is 32x32 SOLID BLUE           runtime, mean 85 = (0+0+255)/3
  the pattern lock succeeds                         runtime, "8 ok, 0 failed"

## 2026-09-04 - the clothes are too dark, and it is NOT the recipe

Traced r1.x in the lerp: `mov_pp r1.xz, c6` with `def c6, 1, 0.5, -0.5, ...` so r1.x = 1.0, and
`mad r5.xyz, r6.z, r5, c6.x` with `r5 = c3 - r1.x` is exactly lerp(1, C, p.b). The implementation
matches the shader instruction for instruction.

Bake #2 computes (84 0 0) from colour C = (0.40 0 0) and a diffuse of 0.82. That IS what the
shader produces. So the albedo is right and the image is still dark, which places the fault in how
REMIX LIGHTS this mesh rather than in what we hand it.

Added clothBrightness (a percentage, default 100) and labelled it in both the code and the ini as
a TUNING KNOB rather than a fix, with the evidence for why the recipe is not at fault written
beside it. Whatever value looks correct is a measurement of the lighting gap - a factor of two
would point at our API instances not receiving the same lights as the game's own geometry.

Also added clothUseDiffuse (default 1) so the diffuse multiply can be A/B'd from the ini.

### Session summary
Everything from this session is written up as "STATE, 2026-09-04" at the top of
docs/YOUR-INSTRUCTIONS.md: the Remix API contract and its six paid-for traps, the character
pipeline, the customisation facts verified from the exe and the shaders, what is open, the routes
that are closed and must not be reopened, and the method lessons that actually paid.


================================================================================================
SESSION, 2026-09-04 .. 2026-09-07 - CAPTURE OFF, THE UI REBUILT, CLOTH COLOUR SOLVED,
                                    THE GAME'S OWN ASSETS UNPACKED
================================================================================================

The headline: `rtx.useVertexCapture = False` is now the configuration, the character renders
correctly through the GAME's own draw rather than through the Remix API, and the customisation
colour system is solved for every garment except two.

------------------------------------------------------------------------------------------------
1. THE ONE TECHNIQUE THAT UNLOCKED EVERYTHING
------------------------------------------------------------------------------------------------
Remix's refusal is about the VERTEX shader, not the draw. Its own message says so:

    [RTX-Compatibility-Info] Skipping draw call with shader usage as vertex capture is not enabled.

Vertex capture exists to capture vertex-shader output, so a vertex shader is the thing it cannot
handle. THE PIXEL SHADER WAS NEVER THE PROBLEM. A draw re-issued with fixed-function vertex
processing and the game's own pixel shader still bound is a combination Remix executes with
capture off.

Proved by accident, on the menu video. Nulling the pixel shader on the HUD turned the video
BLACK AND WHITE - Bink is Y/Cr/Cb across three L8 stages (1280x720, 640x360, 640x360) and stage 0
alone is luma. Keeping the shader brought the colour back, and that was the proof.

Applied in three places:
  the HUD              DrawPrimitiveUP draws rebuilt as D3DFVF_XYZRHW quads
  the character skin   the atlas composites re-issued as a full-target quad
  (the world already had its own fixed-function conversion)

------------------------------------------------------------------------------------------------
2. THE UI - A WHOLE DRAW ENTRY POINT WAS NEVER HOOKED
------------------------------------------------------------------------------------------------
The complete frame dump of 5,200 draws contained NO HUD at all. D3D9 has FOUR draw entry points
and this shim hooked two. DrawPrimitiveUP (slot 83) carries ~13-30 draws a frame; every one had
been invisible to the shim since the fork - unclassified, uncounted, absent from every dump.

The HUD is 56 of them, into the BACK BUFFER, after the frame's final composite:

    zw=1 zt=0 blend=1 | no projTM | Diffuse_MapSampler | 6 textures | 6-48 vertices

Depth test OFF is what "draw on top of everything" means. The other 20 UP draws are depth-TESTED
Orbital_map quads in the world passes and must not be touched; zt separates them exactly.

Format, asked of the DEVICE rather than our own tracking (SetFVF is slot 89 and is also unhooked,
so a cached declaration could describe another mesh entirely):

    stride 28 | float4 POSITION | float2 TEXCOORD0 | d3dcolor COLOR0
    vtx0 pos 2560.0    0.0   uv 1,0   FFFFFFFF
    vtx1 pos 2560.0 1440.0   uv 1,1   FFFFFFFF

2560x1440 is the screen: the positions are ALREADY SCREEN PIXELS, which is exactly what D3D9
wants from a D3DFVF_XYZRHW vertex. So the rebuild is a 28-byte field reorder - no projection to
invert, no space to convert. FVF field order is position, diffuse, texcoord; the game stores
texcoord BEFORE colour, so the two must swap or the colour bytes land in the uv.

STILL OPEN: the in-game HUD is not visible and sub-menu text/backgrounds are missing. The menu
video works. With capture off, anything PASSED THROUGH is simply not drawn, and the pass census
was added to name what is being lost. This work is parked at the user's request.

------------------------------------------------------------------------------------------------
3. THE CHARACTER SKIN - AND HOW IT WAS VERIFIED
------------------------------------------------------------------------------------------------
Capture-off blacked the character. Measured on both sides rather than inferred:

    capture ON    atlas 2048x1024 mean 182.7    atlas 1024x512 mean 169.6
    capture OFF   atlas 2048x1024 mean   0.0    atlas 1024x512 mean   0.0
    (atlas 1280x768 reads 14.3 in BOTH - it is CPU-written through LockRect, which is what
     proves the other two are GPU composites)

The probe named the four draws that build them: screen-space quads, 6 verts, 2 triangles, running
a real pixel shader over two DXT5 textures the same size as the target. A 1:1 blit - so the
game's vertices are not needed at all; a full-target XYZRHW quad reproduces them.

AFTER THE FIX:  ATLAS COMPOSITE re-issued: 4 done, 0 failed
                atlas 2048x1024 mean 182.7    atlas 1024x512 mean 169.6

Identical to the pre-breakage values, not merely non-zero. That is what makes it conclusive: a
wrong quad or a wrong half-texel offset would give non-zero-but-different.

Gated on aspect ratio, not size alone. The size test also admitted the 1280x720 IR_GBuffer_Depth
resolve; the post chain is screen-shaped (16:9) and the atlases are 2:1, which separates them with
nothing needing to be observed first.

------------------------------------------------------------------------------------------------
4. CLOTH CUSTOMISATION - SOLVED FOR EVERY GARMENT BUT TWO
------------------------------------------------------------------------------------------------
See the section "THE WORKING CLOTH COLOUR FORMULA" at the top of YOUR-INSTRUCTIONS.md - it is the
authoritative copy and is confirmed correct by the user.

The bug: ClothAlbedo() refused any material whose albedo was not the Pattern_Map itself, which
declined all FOURTEEN of the player's items and left them showing raw untinted maps. White
bracelets, a white choker and a green beanie were those shipped maps rendered untinted - not a
colour bug at all.

Three garment shapes, and they need three treatments:
  1. pattern IS the albedo            -> resolve in the pattern's own texture space (the corset)
  2. separate diffuse + FLAT pattern  -> one colour for the surface, multiplied into the diffuse
                                         in ITS space. No mesh, no uv assumption. FIXED.
  3. separate diffuse + VARYING pattern on a SECOND uv set -> the underwear and the bra. OPEN.

Traps paid for here:
  - clothBrightness is ALREADY normalised at parse time; dividing by 100 again scaled every
    garment down by a hundred (result mean 0.3 of 255, predicted 43, measured 42.8 after the fix);
  - the cloth cache key must include the DIFFUSE. ClothKey(pattern,col) ^ diffuse*prime collapses
    to the pattern-space key when diffuse is null, so registering a UV-space bake there overwrote
    the corset and the shoes - the shoes went BLACK because their bake covers 11% of its texture;
  - there must be ONE pipeline. A second one gave the right hue and the wrong brightness.

------------------------------------------------------------------------------------------------
5. THE GAME'S OWN ASSETS - THREE CONTAINER FORMATS UNPACKED
------------------------------------------------------------------------------------------------
tools/vpp.py and tools/peg.py, written this session, produce game-textures/ - 3,467 bitmaps,
697 items, with a README explaining the naming and the recipe.

VPP_PC / STR2_PC v6, index entry at 0x800, 24 bytes:
    name offset | pad | DATA OFFSET | UNCOMPRESSED size | COMPRESSED size | pad
The bundled tools/vpp_extract.py had these fields off by one slot and wrote zero-length files for
customize_player.vpp_pc.

THREE different storage layouts, and each needs its own read:
    flags 0x0000  uncompressed          slice at the data offset
    flags 0x4803  compressed+CONDENSED  ONE zlib stream for the whole block, offsets index the
                                        DECOMPRESSED result (.str2_pc)
    flags 0x4801  compressed            per-entry streams at an ALIGNED RUNNING CURSOR, decoded as
                                        RAW DEFLATE - they carry a zlib header but NO adler32
                                        trailer (shaders.vpp_pc)
In all three the stored "compressed size" is NOT a stream length. Do not trust it as one.

PEG / GEKV v13: header 24 bytes, ENTRY STRIDE 72. A single-bitmap PEG cannot reveal the stride -
24 + 72 lands exactly where the name table starts either way - so it only showed itself on a
three-bitmap PEG, where the names came out as garbage. Formats seen: 400 = DXT1, 402 = DXT5.

What the assets say:
  _n 564  _d 338  _dp 126  _p 68  _sb 57  _flow 46  _dob 36
  A white or grey DIFFUSE map is CORRECT - it is the detail layer, shipped to be tinted.
  Patterns are a SHARED LIBRARY of 22 selectable decals in clothes/pat/ - pat_star01,
  pat_saints01, pat_llheart01 ... applied OVER garments, which is the entire reason a second uv
  set exists.
  The player's underwear pattern is pat_llheart01, confirmed by rendering it beside the runtime
  capture. It is a TILING heart; the "devil horns" are the corners of neighbouring hearts.

------------------------------------------------------------------------------------------------
6. HAIR
------------------------------------------------------------------------------------------------
A hair item ships THREE maps - _dob, _flow, _n - and NO _d at all. So what the shader calls
Diffuse_Map for hair is the FLOW map, which is why it looks like green/teal/magenta directional
data. Colour comes from a separate 128x128 swatch per menu choice (hair_black_01_sb ...).

The strands are in the Dob_Map, measured per channel:
    R  min 16  max 222   THE STRANDS
    G  min 247 max 248   flat, carries nothing
    B  identical to R
One greyscale channel duplicated; the green it appears to be is just G at 247 in an RGB view.

    hair albedo = chosen colour * (dob.R / 255) * clothTintScale

This is NOT baking the hair shader - that shader samples the L-buffer and baking it would burn the
game's lighting into the albedo. This multiplies by an authored, unlit mask. Remix still lights.

------------------------------------------------------------------------------------------------
7. THE REMIX API CHARACTER IS OFF, AND WHY
------------------------------------------------------------------------------------------------
The character's DrawInstance loop lives INSIDE RemixApiTestTick(), which returns early on
!remixApiTestCube. Turning off the step-2 test cube therefore silently stopped the character being
submitted - `DrawInstance 0 calls` against 10 built meshes and 21,990 triangles, for several runs.
The gates are separated now.

It is left OFF (remixApiCharacter=0). Everything that fixed the real character - the atlas
composite, the cloth colours, the hair - lives in the SHARED path. The API copy was frozen
(skinning crashes Remix's server), duplicated, and a second path to keep in sync; several of this
session's bugs came from exactly that. remixApiCharacter=1 restores it, no rebuild.

------------------------------------------------------------------------------------------------
8. THE UNDERWEAR AND THE BRA - WHAT IS PROVEN, AND WHAT IS LEFT
------------------------------------------------------------------------------------------------
Same system, same class. THREE shortcuts are closed BY MEASUREMENT, not by opinion:

  - the pattern is read at MANY texels, not one:
        ONE-POINT declined: TEXCOORD1 VARIES - u spans 0..1904, v spans -192..1881
  - uv1 is NOT an affine function of uv0:
        u = 0.0416*u0 +1590.0 (worst residual 1598.4)
        v = 0.2303*v0 +1292.5 (worst residual 1543.9)   against a span of ~1900. Not a near miss.
  - and from the shader itself, THE ALBEDO IS NOT ON TEXCOORD0:

        ir_at_sr3pccloth_bs[8]:  texld_pp r4, r4, s0   ; s0 <- COMPUTED from TEXCOORD6,
                                                       ;       clamped against c2/c3, scale 512
                                 texld_pp r5, v1, s2   ; s2 <- TEXCOORD1     the pattern
        ir_sr3pccloth_bs [6]:    texld_pp r6, v1, s0   ; s0 <- TEXCOORD1

"Albedo is TEXCOORD0, pattern is TEXCOORD1" is NOT A RULE OF THIS ENGINE. It is a coincidence that
holds for some variants. The CPU baker rasterises into TEXCOORD0 space regardless, which is a
concrete reason its islands need not land where the mesh samples them.

The deployed build reflects the real mapping out of each shader's texld instructions and reports
    UV SET PER SAMPLER (from the shader's own texld): s0<-... s2<-...
THAT RUN HAS NOT HAPPENED YET. It is the next thing to read.

------------------------------------------------------------------------------------------------
9. METHOD - WHAT ACTUALLY WENT WRONG THIS SESSION
------------------------------------------------------------------------------------------------
Four mistakes, all the same shape: a conclusion drawn from something that did not establish it.

  - ABSENCE FROM A CAPPED LOG IS NOT EVIDENCE. The 64x64 pattern was missing from a probe that
    only prints a few entries, and that was read as "the draw is not converted". It was converted
    all along. Two builds were spent on code downstream of a gate that did not exist.
  - A PREDICATE USED AS A GATE MUST EXPLAIN ITS REFUSALS. ClothPatternIsSampledAtOnePoint returned
    false silently, which is indistinguishable from "this draw never happened".
  - DIAGNOSTICS THAT ASSERT CONCLUSIONS THEY NEVER REACHED. The slot dump computed strip ranges
    with the triangle-LIST formula and then declared "coplanar duplicates that z-fight"; the
    UV1-vs-UV0 probe reported "INDEPENDENT unwraps" when it had actually just given up on a
    degenerate span. Both sent a reader chasing something that was never there.
  - CHECK REACHABILITY BEFORE WRITING THE FIX. Twice: the one-point sampling, and the hair strand
    generator, which was put on the Remix API path while that path was not drawing at all.

And what worked, every time: LOOKING AT THE THING. The contact sheet that showed cloth-9 was EMPTY
rather than scrambled; rendering pat_llheart01 beside the runtime capture; the Dob_Map channel
statistics; disassembling the shader instead of reasoning about it. Reasoning from summary numbers
- a mean, a coverage percentage, a uv range - lost every time it was tried.

------------------------------------------------------------------------------------------------
10. AN AUDIT FOUND THREE BUGS NO SYMPTOM HAD SHOWN
------------------------------------------------------------------------------------------------
  - DrawHudFixedFunction set D3DRS_LIGHTING and ten texture stage states through g_orig* under
    g_internal, which bypasses the hooks - so the shim's SHADOW copies kept the game's values
    while the device held ours. ShadowGetRS/ShadowGetTSS are what the classifier reads. The
    lighting one was LIVE: outside the pixel-shader guard, ~28 draws a frame, never restored.
  - The bake bridge released only the old `generated` and then AddRef'd the pattern again,
    leaking a pattern reference on every re-bake.
  - Two doc claims stated the opposite of current behaviour as settled fact.

================================================================================================


================================================================================================
SESSION, 2026-09-08 .. 2026-09-09 - THE GARMENT TAXONOMY, READ OUT OF THE SHADERS
================================================================================================

Where it ended: every customised item on the character renders in its correct colour EXCEPT the
decals on the bra and the underwear, and the last defect chased was a bug of mine in the atlas
padding rather than anything about the game.

------------------------------------------------------------------------------------------------
1. THE SAMPLER -> TEXCOORD MAPPING IS NOT FIXED, AND THE SHIM ASSUMED IT WAS
------------------------------------------------------------------------------------------------
Since the fork this shim has assumed "albedo is TEXCOORD0, pattern is TEXCOORD1". Disassembling
every cloth shader (tools/cloth_uv_table.py, output in docs/cloth-uv-map.md) shows that is a
coincidence which holds for some variants:

    ir_at_sr3pccloth_*  [8][9]   Pattern s2 <- TEXCOORD1   Diffuse s0 <- computed from TEXCOORD6
    ir_sr3pccloth_*     [6]      Pattern s0 <- TEXCOORD1   Diffuse s3 <- computed from TEXCOORD5
    ir_sr3npcclothfull_*[8][9]   Pattern s0 <- TEXCOORD0   (no diffuse sampled)
    ir_sr3npccloth_glow_*        Pattern s0 <- TEXCOORD3   (no diffuse sampled)

The player's own clothing is `ir_sr3pccloth_*` shader [6], identified by matching the runtime
report `pattern at stage 0, albedo stage 3` against the table - one variant, no ambiguity.

ReflectShader now reads this out of the `texld` instructions (D3DSIO_TEX, 0x42) instead of
assuming it, and carries a one-step DATAFLOW TRACE so a coordinate built through a clamp chain
still resolves: for every temporary, the input register that last fed it. `samplerUvDirect`
records whether an answer was READ from a texld source or TRACED through temporaries, because a
traced answer is an inference and a caller may want to treat it differently. Runtime confirmed the
static reading exactly:

    s0=Pattern_MapSampler<-TEXCOORD1  s3=Diffuse_MapSampler<-TEXCOORD5*traced

------------------------------------------------------------------------------------------------
2. kShortUVScale = 1/1024 IS CONFIRMED, AND THE BAKER'S SPACE WAS ALWAYS RIGHT
------------------------------------------------------------------------------------------------
From the vertex shader of the same effect:

    def c1, 0.0009765625, ...        exactly 1/1024
    mul o6.xy, c1.x, v1              v1 = the mesh's TEXCOORD0 attribute
    dcl_texcoord5 o6                 so o6 IS pixel-shader TEXCOORD5

The albedo's coordinate is the mesh's own TEXCOORD0 attribute times 1/1024. A hardcoded constant
that had been a guess since session 1 is now read out of the game's own bytecode.

This RETRACTS a claim made earlier in this session - that the baker rasterises into the wrong
space. It does not. The space was right; the failure was elsewhere.

------------------------------------------------------------------------------------------------
3. THE CLAMP WAS A DEAD END, AND WAS RETRACTED TWICE
------------------------------------------------------------------------------------------------
The pixel shader clamps its albedo coordinate through ClampU1/ClampV1, and that looked like the
explanation for two messages. It is not:

  - only TWO of the four Clamp constants exist in this shader, and an all-or-nothing check in the
    probe reported "declares no clamp constants" when two were sitting there - a partial truth
    turned into a total absence;
  - both read 0.0000 at runtime, which makes the window degenerate;
  - and the SAMPLER STATE settles it: addressU=1, addressV=1, i.e. D3DTADDRESS_WRAP. The game
    TILES the albedo. Measured from the device, not inferred from arithmetic.

So the wrap added to the CPU baker on 2026-09-06 was right, and the "the game pins, we tile"
reading was wrong. The one durable thing the clamp taught: a CONVERTED draw has no pixel shader
at all - BeginFFP nulls it - so whatever the shader did with its coordinate is simply gone.

------------------------------------------------------------------------------------------------
4. THE TAXONOMY - THREE WAYS A PATTERN RELATES TO ITS GARMENT
------------------------------------------------------------------------------------------------
Measured per draw from the vertex stream, and this is what finally organised the problem:

    ONE-POINT      uv1 is CONSTANT across the draw. Every vertex reads the same pattern texel, so
                   the whole surface takes one colour however detailed the pattern is.
    AFFINE         uv1 = s*uv0 + o. The pattern rides the garment's own unwrap, so it can be
                   resolved PER TEXEL in the albedo's texture space with no mesh at all.
                   The backpack fits u = 1.0000*u0 + 0.0 with a worst residual of EXACTLY 0.0.
    INDEPENDENT    two genuinely separate unwraps. The bra and the underwear: worst residuals
                   ~1700 and ~1000 against a span of ~1900.

For the INDEPENDENT case there is a real structural limit, not a missing trick. The albedo is a
TILING detail map (the underwear's uv0 spans about 3.7 tiles) and the pattern is a DECAL placed
independently, so one albedo texel is visited by many surface points carrying different pattern
colours. NO single texture in the albedo's space can hold that. Four separate attempts at a
shortcut failed because they were looking for something that does not exist.

What ships instead is a named approximation: the garment takes the pattern's DOMINANT texel
(53% and 62% of the map respectively, both the blue background). That selects colour C, which for
these two is teal and magenta - the correct garment colours, confirmed against the unmodded
screenshot. The decal is lost; the alternative was white, which is wrong in every respect.

------------------------------------------------------------------------------------------------
5. THE PADDING, AND THE BUG THAT MADE IT LOOK LIKE THE GAME'S FAULT
------------------------------------------------------------------------------------------------
These diffuse maps are mostly EMPTY: 88.7% of the underwear's and 74.0% of the bra's is black
padding around a few bright islands. A converted draw has no pixel shader, so the shader's clamp
is gone and the raw uv wanders across the whole map - into the padding, which multiplied to black.
The user saw black square patches on both.

The fix is to pad: dilate the islands outward so a sample that strays gets the island's colour.
That took three tries and the last two failures were mine:

  a. A FLAT FILL with one average removed the black but left every island with a visible border,
     because the islands carry shading and a flat fill does not. Reported as "similar color to the
     main parts" - still visible.
  b. DILATION then spread BLACK. It decided "is this texel filled?" from the DIFFUSE being above
     8, but propagated the GENERATED colour - and a diffuse of 10 through gamma 2.2 times a colour
     times a scale of 2 is about 1 of 255. The near-black rim of every island became a seed. The
     dumped texture shows it exactly: dark wedges radiating from the island edges.

  Now only a texel carrying real COLOUR may seed or spread.

THE DIAGNOSTIC WAS THE WORSE HALF. It reported "0 texels left unreached", which was TRUE - every
empty texel really was reached, with black. A statement that is accurate and tells you nothing is
more dangerous than one that is wrong; two further hypotheses were built on top of it before the
artifact contradicted it. The report is now measured on the RESULT ("N% of the FINAL texture is
still black") rather than on the loop counter.

------------------------------------------------------------------------------------------------
6. HOW THAT BUG WAS ACTUALLY CAUGHT - THE REMIX CAPTURE
------------------------------------------------------------------------------------------------
Worth recording as a technique. A Remix capture (Ctrl+Shift+P, lands in rtx-remix/captures/)
writes out every texture Remix received. Ours are findable by SIZE - they are the only
uncompressed BGRA8 at the garment's dimensions:

    29F520D5  128x256  the bra        39.7% black
    4FD02CBB  256x128  the underwear  19.4% black
    80238026  256x128  the armband     0.9% black

and our own dump of the same textures measured 39.7% and 19.4% - identical. That comparison
eliminated Remix, the binding, and any second draw in one step, and pointed the finger squarely
back at the generator. The armband at 0.9% is why it looked right while the other two did not.

------------------------------------------------------------------------------------------------
7. THE REMIX API CHARACTER IS OFF, AND WAS SILENTLY OFF BEFORE THAT
------------------------------------------------------------------------------------------------
The character's DrawInstance loop lives INSIDE RemixApiTestTick(), which returns early on
!remixApiTestCube. Turning off the step-2 test cube therefore stopped the character being
submitted - `DrawInstance 0 calls` against 10 built meshes and 21,990 triangles, for several runs,
silently. The gates are separated now.

It is deliberately left OFF. Everything that fixes the real character - the atlas composites, the
cloth colours, the hair - lives in the SHARED fixed-function path.

------------------------------------------------------------------------------------------------
8. HAIR
------------------------------------------------------------------------------------------------
A hair item ships _dob, _flow and _n and NO _d at all, so what the shader calls Diffuse_Map for
hair is the FLOW map. The strands are in the Dob_Map's RED channel (R 16..222, G flat at 247,
B identical to R - one greyscale channel duplicated). Hair albedo is now

    chosen colour * (dob.R / 255) * clothTintScale

on the SHARED path (HairAlbedo), after first being written on the API path where it could never
have taken effect.

------------------------------------------------------------------------------------------------
9. METHOD - THE FAILURES WERE ALL ONE SHAPE
------------------------------------------------------------------------------------------------
  - ABSENCE FROM A CAPPED LOG IS NOT EVIDENCE. A 64x64 pattern missing from a probe that prints
    only a few entries was read as "the draw is not converted". It was converted all along.
  - A DIAGNOSTIC MUST NOT ASSERT A CONCLUSION IT DID NOT REACH. Three did: the slot dump's
    "coplanar duplicates", the UV1 probe's "INDEPENDENT unwraps" when it had given up on a
    degenerate span, and the pad's "0 texels left unreached".
  - AN ALL-OR-NOTHING CHECK TURNS A PARTIAL TRUTH INTO A TOTAL ABSENCE (the clamp constants).
  - MEASURE THE RESULT, NOT THE PROCESS. A loop counter says what the loop did; only the artifact
    says what came out.
  - CHECK REACHABILITY BEFORE WRITING THE FIX. Twice - the one-point sampling, and a diagnostic
    placed inside the API character collection after that path had been switched off.

  And what worked every time: LOOKING AT THE THING. The contact sheet that showed a bake was empty
  rather than scrambled; pat_llheart01 rendered beside the runtime capture; the Dob_Map channel
  statistics; the disassembly; and finally the dumped texture whose black wedges named the seeding
  bug in one glance.

------------------------------------------------------------------------------------------------
10. TEST METHODOLOGY WORTH REUSING
------------------------------------------------------------------------------------------------
The user reconfigured the character to occupy nearly all 13 wardrobe slots, deliberately including
BOTH a bra and underwear, and supplied matched unmodded/modded screenshots. That single run
produced three garments of the hard class side by side with their real inputs and separated them
cleanly - the backpack (AFFINE, fixed), the underwear and the bra (INDEPENDENT). Asking for a
deliberately constructed outfit was worth more than any number of incidental runs.

================================================================================================


---

# SESSION 2026-09-09 (evening) .. 2026-09-10 - THE BRA AND THE UNDERWEAR, SOLVED

Where it started: both garments rendered with "black square patches", a padding pass had been
built to hide them, and the decals (a cat on the underwear, a star on the bra) were written off
as a structural limit of the INDEPENDENT class. Where it ended, confirmed by the user on screen:
both silhouettes correct, the cat and the star in place in the right colours, the star on the
left cup only (as in the game), the backpack's stomach yellow, the underwear's panel cyan.

Every step below was a measurement that killed a hypothesis, until the one that did not.

## 2026-09-09 - five hypotheses, each closed by one measurement

The premise on entry was that a CONVERTED draw loses the game's pixel-shader clamp, so the raw
uv "wanders into the diffuse's empty atlas space". Read the shader instead of the register names:

    ir_sr3pccloth_c ps[6]:
      mov_pp r1.xz, c6               ; c6.x = 1.0
      add_pp r0.w, r1.x, -c13.x      ; 1 - ClampV1
      mul_pp r2.w, r0.w, c9.y        ; * 512
      mad_pp r0.w, r0.w, c9.y, c9.z  ; * 512 + 1   -> window [-(1-V1)*512, (1-V1)*512+1]
      abs_pp r0.w, c13.x
      cmp_pp r3.y, -r0.w, v5.y, r2.w ; ClampV1 == 0 -> take v5.y UNCLAMPED

Runtime: `ClampU1 0.00000  ClampV1 0.00000`. V is bypassed outright, U is pinned to [-512, 513].
**The clamp is declared and inactive.** The sampler is WRAP (measured). The game wraps exactly
as the conversion does. First hypothesis dead; the log line that had asserted "the game PINS the
coordinate" was rewritten to print the windows instead of a verdict.

Then the coordinate. `vs[2]` (2340 bytes - the permutation actually bound, identified by
`GetFunction` size) does `mul o6.xy, c1.x, v1`: the diffuse is fetched at `TEXCOORD0 * 1/1024`,
no offset, no tiling - the very matrix the fixed-function path applies. Same on all three probed
garments, same pixel shader (2976 bytes). Second hypothesis (a different permutation) dead.

Then WHERE the mesh lands. A COVERAGE probe wrapped every vertex's uv0 the way the sampler does
and looked up the texel:

    256x256 garment    0.3% of vertices on a black texel     <- control: TEXCOORD0 is its unwrap
    underwear 256x128 65.7%
    bra       128x256 63.6%

The overlays showed the two broken garments' vertices sprinkled evenly across the map. A SCAN of
every short2 the stride can hold found nothing better than TEXCOORD0 (the bra's best candidate
was the position field). Third hypothesis (another uv set) dead. A walk of the INDEX BUFFER -
`base=0 minIndex=0 numVertices=271`, all 271 referenced, none outside - killed the fourth (a
loose NumVertices range sweeping in other garments' vertices). And `uv1 != uv0` on every one of
those 271 vertices, so INDEPENDENT was real, not an artefact of the range.

So: the game fetches, through exactly our coordinate, a texture that is 88.7% black, lands two
thirds of the mesh on the black, and shows no black. Every term of the shader's output is
proportional to that fetch. The only thing left was the texture's MEANING.

## 2026-09-09 - it was the alpha. The dump had been lying about it.

    texld_pp r3, r3, s3      ; the Diffuse_Map fetch - r3.w is its ALPHA
    ...
    lrp_pp r3.xyz, v4.w, c38, r0    ; only .xyz is touched after that
    mul_pp oC0, r3, c37             ; oC0.w = diffuse.a * Tint_color.w

The garment is a TEMPLATE MESH cut to shape by its texture's alpha, and the game blends it
(`alphaBlend=1 src=5 dst=6`, measured; the control garment draws `src=2 dst=1`). Two thirds of
the mesh on "black" was correct and expected: that is the invisible part of the template. The
game files agree - every `cm_unwr_f_*` is 256x128 with 81-90% transparent alpha, every
`cm_bra_f_*` 128x256 with 63-85%.

Why it had never been seen: `WriteBgraDds` wrote `cur[i*4+3] = 255`. Every dump of these maps
reported "alpha==255: 100%" while the same bytes measured 90% transparent in the probe. A day of
looking at pictures of the cutout, with the one channel that explained it fabricated by the tool
that made the pictures. Fixed; the dump now carries the buffer's own alpha.

The fix, `clothCutout=1`: the generated texture already carried the alpha (the diffuse arrives
as DXT5 at draw time and its alpha decoded correctly); the DILATION had been writing `0xFF000000`
over every filled texel, and the converted draw took TFACTOR alpha. Now the dilation keeps each
texel's alpha (colour only, for bilinear edges) and BeginFFP alpha-tests the draw at 128 when it
binds a generated texture that carries a cutout - the same save/restore path a shader texkill
uses, and the signal Remix reads as "cutout" rather than "glass". The DXT1 decoder's
punch-through texel (index 3 of a 3-colour block) is fixed to alpha 0 on the same switch; it was
wrong, it just was not what these two garments hit. User: both silhouettes correct.

## 2026-09-10 - the decal, through the mesh

`vs[2]`: `mul o2.xy, r2, c1.x` with `r2 = c6/c7 * v4` - the pattern is sampled at
`TEXCOORD1 * Pattern_Map_Tiling / 1024`, wrapped. A DECAL SPACE probe measured whether a texture
baked in THAT space could also carry the cutout, which lives in uv0: 46 of the underwear's 63
visible vertices and 214 of the bra's 248 share a wrapped uv1 cell with cut template geometry.
It cannot. TEXCOORD1 as the sampled set is closed.

But the same numbers showed that the "3.7 tiles of uv0 overlap" behind the structural-limit
story was mostly the CUT template (208 of 271 vertices). The visible panels sit on distinct
islands. So `clothMeshDecal=1` (`BakeDecal`): for every triangle with a visible vertex,
rasterise it into the diffuse's space, interpolate its uv1 barycentrically, and record which
pattern texel each visible albedo texel shows. Cut triangles never write. Everything else stays
as it was - uv0 space, alpha intact, TEXCOORDINDEX 0, one stage, nothing new for Remix.

    underwear  142 tris rasterised, 221 skipped | 78.9% of visible texels resolved | 0 conflicts
    bra        662 rasterised, 670 skipped      | 91.4%                            | 433 conflicts

The generated textures had the cat on the front panel and the star on the cup. In the wrong
colours: cyan and white - the pattern's own texels.

## 2026-09-10 - the colour chain. The desaturation branch was never in this shader.

`colourFromPatternTexel` classifies a mixed texel - cyan (0,1,1), white (1,1,1) - as
"near-grey, no customisation" and returns it raw; its weighted sum was only ever verified on
pure single-channel patterns. ps[6] has no such branch:

    add_pp r5.xyz, -r1.x, c3        ; C - 1
    mad_pp r5.xyz, r6.z, r5, c6.x   ; lerp(1, C, blue)
    lrp_pp r7.xyz, r6.y, c2, r5     ; lerp(that, B, green)
    lrp_pp r5.xyz, r6.x, c1, r7     ; lerp(that, A, red)

White -> C by blue, -> B by green, -> A by red. On a pure-channel texel the chain and the sum
agree, which is why every flat-pattern garment had been right and nobody noticed. On the decals
they diverge: cyan -> B (magenta cat, yellow star), white -> A (yellow eyes) - the user's
screenshot exactly. `colourFromPatternTexelChain` is used on the per-texel paths (mesh decal and
AFFINE); the backpack's cyan stomach, the same bug on the AFFINE path, became yellow with it.
The one-point/dominant `pick` stays on the old function: the two agree on every texel it is
ever given. User: colours correct.

## 2026-09-10 - the star on both cups: two surfaces, one island, four attempts

The bra's diffuse has ONE cup island; both cups map onto it with mirrored uv0, and the game
shows the star on the LEFT cup only. So the left cup wants the star at island region P and the
right cup wants background there. One texture, one answer per texel: the left cup's triangles
wrote first and both cups showed its star. The 433 "conflict" texels were exactly the star -
the conflict map (`sr3-remix-decal-2-conflicts.dds`) painted nothing else on the whole bra.

The second cup has to get its own texels. The mechanism, `clothDecalTiles=1`:

  - the generated texture is laid out as TILES side by side, each with its own decal map;
  - the skinned copy - `SkinAndBind` already writes a private `{pos, nrm, uv}` vertex per draw
    every frame, uv in raw short units - shifts each tile's vertices' u0 by whole tiles;
  - the draw's texture matrix is scaled by 1/width-in-tiles at bind time;
  - cut geometry keeps its within-tile position and every tile carries the same alpha, so it
    stays invisible wherever it lands.

Three ways of choosing WHICH triangles go to tile 1 were refused by their own guards, each
refusal stated in the log:

  1. Connected components: `1 connected components ... 433 conflict texels across 0 component
     pairs`. The cups join through the band. Dead.
  2. uv0 winding sign (mirrored halves wind oppositely): `176 between OPPOSITE windings, 257
     within the SAME winding`. Most of the disagreement was same-winding - triangles of the
     other cup that happen to wind the same way at the star's points. Dead.
  3. A colouring of the TRIANGLE CONFLICT GRAPH (each disagreeing texel is an edge between the
     two triangles; each triangle takes the lowest tile holding nothing it disagrees with;
     non-conflicting triangles stay in tile 0 because any tile is correct for them): `35
     disagreeing triangle pairs over 68 triangles -> 2 tiles` - and then `visible triangles
     crossing a tile seam: 47`, refused, because the cup island touches the texture's edge and
     the panel wraps across it.
  4. The same colouring, with each tile laid out as ENOUGH SIDE-BY-SIDE COPIES of its image that
     its panel, shifted to the start of its region, never leaves it:

        433 conflict texels = 35 pairs over 68 triangles -> 2 tiles laid out as 4+2 copies =
        6 texture widths. Tris per tile: 882 / 34; 18 seam vertices duplicated; 916 list
        triangles; refused: 0.

     The seam vertices are duplicated into the skinned copy behind the draw's own vertices with
     the other tile's shift, a private INDEX32 triangle list references them (the strip is
     dissolved), and the draw hook swaps that buffer in for the one call. User: fixed.

## 2026-09-10 - the colour curve: why a linear scale could never make cyan

The underwear's panel colour reaches the shader as C = (0.059, 0.220, 0.298). G/B = 0.74 is
blue, and a linear scale (clothTintScale x2) preserves hue by construction. The game shows cyan
because ps[6] ends `mul oC0, r3, c37` with Tint_color = (5,5,5) - measured - into an HDR target
it then tonemaps: G and B saturate together. `clothColourCurve=1` applies Reinhard, x/(1+x), to
Tint*C per channel, with the Tint read from the shader constant. Against every colour the user
had judged: underwear light cyan, bra fuchsia, armband and backpack yellow, bracelets blue, and
mid-range colours within a few percent of the x2 brightness the corset was calibrated at.
Player-family generator only; hair and the NPC path unchanged. User: fixed.

Also measured this run: `Diffuse_Color c14 = (1,1,1,1)` on every garment. The generator has
never applied it, and it did not need to.

## What is now true

    every garment on the 2026-09-09 outfit renders correctly: silhouettes, colours, decals.
    INDEPENDENT is NOT a structural limit. The mesh relates the two unwraps per texel.
    the colour formula for the player family is the shader's own lerp chain; the weighted-sum
      with a desaturation branch belongs to the NPC family and coincides only on pure texels.
    the customisation colour goes through Reinhard on the measured Tint, not a linear x2.
    a template garment is cut by its texture's ALPHA; the converted draw must alpha-test it.
    a texel can be claimed by two surfaces; tiles + seam duplicates + a private list resolve it.

## Method, this stretch

  - Five hypotheses died to five measurements, in order, each one line in the log. The cause was
    the thing every measurement had been treating as background: a channel.
  - The tool that made the pictures had fabricated the channel. "Look at the thing" includes
    checking that the thing you are looking at is the thing.
  - Three of my own guards refused my own fix, and each refusal said exactly why. Reading the
    refusal reason beat re-theorising every time.
  - A diagnostic that asserts a verdict ("the game PINS the coordinate") sent a day's work after
    a clamp that was never active. Print the numbers.
  - The 3-colour slots: Diffuse_Color_a/b/c, selected by the pattern's R/G/B through the chain.
    Wardrobe slot order is not constant order - the underwear's first wardrobe slot is `c`.

## Deployed at the end of the session

    sr3-rtx.asi  9c0cfaf958174e3bcc0b91c81bf5be1e   CONFIRMED on screen by the user
    sr3-rtx.ini  08450a3d1f807cfb31dec4f7dd2f73a7
    clothCutout=1  clothMeshDecal=1  clothDecalTiles=1  clothColourCurve=1
    (clothAlbedoPercent=200 remains the x2 fallback when clothColourCurve=0)

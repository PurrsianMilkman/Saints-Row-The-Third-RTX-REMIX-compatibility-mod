# Research: paths to full SR3 Remix compatibility

> **SUPERSEDED IN PART — read `YOUR-INSTRUCTIONS.md` first.** Written 2026-08-13, before the
> SR2-proxy port. Its conclusion that "nobody converts the shaders to fixed function wholesale"
> and that rewriting draws as fixed function is "not what any successful deferred-game port did"
> is **wrong for this project**: BRAGme/sr2-rtx-remix-proxy does exactly that on the same engine
> family, and we now do too. Measured 2026-08-17: with the conversion switched off Remix does not
> path-trace SR3 at all. The prior art survey and the Remix mechanism notes here are still useful.

*2026-08-12, substantially revised 2026-08-13. Sources at bottom.*

> **2026-08-13 update — read `docs/shader-map.md` first.** Offline shader RE succeeded and
> supersedes several assumptions below. Corrections:
> - SR3 uses Volition **Inferred Lighting** (SIGGRAPH 2009 / GDC 2012), not classic deferred.
>   There is no D3D9 light list to scrape; lights are volume draws into a low-res irradiance
>   buffer, one named shader per light type.
> - **Camera**: Remix reads `worldToView`/`viewToProjection` only from
>   `SetTransform(D3DTS_VIEW/PROJECTION)`, never from shader constants. SR3 keeps them at fixed
>   registers (`projTM` c28, `objTM` c32, `IR_World2View` c48), as **separate** matrices — so a
>   hook can rebuild them and call `SetTransform`. `rtx.fusedWorldViewMode` is a dead end.
> - **Lights**: `SetLight`/`LightEnable` are converted to ray-traced lights by the **stock**
>   runtime (`RtxContext::addLights`), works over the 32-bit bridge. Preferred over the Remix API,
>   whose `SetupCamera` is commented out in the bridge client and whose `exposeRemixApi` is
>   experimental (known crash: rtx-remix issue #736).
> - **Prior art**: none for any Saints Row title. Templates: `Clippy95/SR.MixFix` (MIT, this exact
>   game) for the ASI; `softsoundd/dxvk-remix-mirrorsedge` if a runtime fork is needed.
> - The "outdoors is path traced" claim below is **withdrawn** — see worklog retractions.

## Where we stand after session 1

Working already, with zero game patching:
- Outdoor world renders through the path tracer (menu flyover + street gameplay): geometry,
  materials, emissive surfaces (traffic lights actually cast light), pedestrians, vehicles.
- Vertex capture handles this engine's outdoor pipeline (huge — this was the open question).
- HUD/menus clean after UI texture tagging; DLSS + Ray Reconstruction active; sessions stable.

Broken:
- **Interiors are pitch black.** Log shows `Trying to raytrace but not detecting a valid camera`
  → indoors, Remix's heuristic for extracting the view/projection from shader constants fails,
  so (almost) nothing enters the ray-traced scene. Confirmed it is NOT a lighting problem:
  a camera-attached sphere fallback light (radiance 50) changed nothing.
- No real game lights anywhere (deferred shading — expected; needs light injection or relighting).
- Cosmetic: red firefly noise outdoors; one floating garbled quad indoors (likely a
  render-target texture drawn as geometry).

## The proven blueprint for deferred DX9 games: xoxor4d's gta4-rtx

GTA IV is SR3's closest analog (2008-era, fully shader-based, deferred) and has a working
community compatibility mod. Its architecture — **interception and translation, not shader
rewriting**:

1. **Ultimate ASI Loader** loads a game-specific compatibility ASI into the process.
2. The ASI hooks the game's renderer and **re-submits world geometry as fixed-function-style
   draws** that Remix understands natively (better perf and reliability than vertex capture).
3. **Game lights are translated into Remix lights** at runtime (dxvk-remix exposes a public
   C API — `remix_c.h` — for injecting lights/meshes/materials from a mod).
4. A shader-fork component (their FusionFix fork) exposes normal maps to the capture path.
5. Optionally a **custom dxvk-remix fork** carries game-specific fixes upstream won't take.

Key lesson: nobody "converts the shaders to fixed function" wholesale. You convert the
*submission path* at the engine-hook level and let the original shaders die off, or leave
them running for passes Remix ignores.

## Approach ladder for SR3 (cheapest first)

1. **Runtime tuning (no code)** — still unexhausted:
   - Dev menu → Rendering → Debug Display → *Geometry Hash* view indoors: is interior geometry
     in the RT scene at all, or fully absent?
   - Game Setup → Step 2: Parameter Tuning — camera/matrix options. Conf options to try:
     `rtx.fusedWorldViewMode` (games with fused World-View matrices), `rtx.camera.*` overrides,
     `rtx.capture.correctBakedTransforms`, anti-culling (`rtx.antiCulling.object.enable`).
   - Toggle vertex capture on/off indoors and compare skip messages in the log.
2. **Engine-level render minimization (no code)** — done: display.ini triage profile
   (post/SSAO/shadows/reflections/MSAA off, lighting Low).
3. **SR3 compatibility ASI (the real project, gta4-rtx model)**:
   - Reverse the renderer in `SaintsRowTheThird.exe` (x32dbg + Ghidra; no symbols, but the
     Volition Core engine's data formats are well documented by the saintsrowmods community,
     and SR3 tooling — Minimaul's tools — can unpack `packfiles\*.vpp_pc` to reach shader
     binaries and materials).
   - Hook the world/character batch submission; emit Remix-friendly draws (or feed geometry
     through the Remix C API directly).
   - Enumerate the engine's light list (deferred renderers keep one) and inject via Remix API —
     this solves relighting semi-automatically, like gta4-rtx's "all game lights translated".
4. **Targeted shader work (only where 3 needs it)**:
   - Dump SM3 bytecode (dxvk shader dump) to map which vertex shaders drive world/skinned
     geometry and which constant registers hold the WVP matrices — feeds both the camera fix
     and any hook-level matrix math.
   - Patching simplified shaders back into packfiles is possible but is a last resort.

## Dead ends (don't spend time)

- The `_DX11` executable — permanently out of scope for Remix.
- `d3d9.shaderModel = 0` style forced-FF fallback — SR3 has no fixed-function fallback path.
- Rewriting all shaders as fixed function — not what any successful deferred-game port did.

## Before building anything: check prior art

Search the **RTX Remix Showcase Discord** compatibility table + hash databases for
"Saints Row" — if someone has an SR3 entry, camera findings, or a WIP compat mod, it changes
the plan. xoxor4d's `remix-comp-projects` collection is also worth scanning for a template
project structure.

## Sources

- https://github.com/xoxor4d/gta4-rtx — GTA IV RTX compatibility mod (architecture blueprint)
- https://github.com/xoxor4d/p2-rtx — Portal 2 compat mod (same author, source-available)
- https://github.com/xoxor4d/remix-comp-projects — collection of compat mod templates
- https://github.com/NVIDIAGameWorks/rtx-remix/wiki/Compatibility — official compat guidance + debug steps
- https://github.com/NVIDIAGameWorks/dxvk-remix — runtime source; `RtxOptions.md`; Remix C API (`remix_c.h`)
- https://docs.omniverse.nvidia.com/kit/docs/rtx_remix/1.2.4/docs/introduction/intro-compatibility.html
- https://developer.valvesoftware.com/wiki/RTX_Remix — background on FF vs shader pipelines
- https://github.com/NVIDIAGameWorks/rtx-remix/issues/86, /issues/278 — black screen / camera detection reports

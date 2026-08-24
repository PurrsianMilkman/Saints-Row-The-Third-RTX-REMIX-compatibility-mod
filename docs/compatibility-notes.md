# SR3 × RTX Remix — Compatibility Assessment
> **PARTLY SUPERSEDED — read `YOUR-INSTRUCTIONS.md` first.** The facts about the game build are
> still correct, and "SR3 has no fixed-function fallback" remains true of the *game*. But the
> conclusion that "everything depends on Remix's vertex capture path" is no longer how this
> project works: the shim now BUILDS a fixed-function path by re-issuing draws itself. Vertex
> capture is what handles the draws we do NOT convert, and that is currently the source of the
> duplicated geometry rather than the solution.

## The game, as found in `Saints Row 3\`

| Fact | Detail |
|---|---|
| Version | GOG (Inno Setup uninstaller, `goggame-1430740694.*`), DRM-free — ideal for Remix |
| Executables | `SaintsRowTheThird.exe` (DX9, 32-bit, **our target**), `SaintsRowTheThird_DX11.exe` (32-bit, incompatible with Remix) |
| Launcher | `game_launcher.exe` picks the renderer — bypass it, launch the DX9 exe directly |
| Engine | Volition "Core" engine: fully programmable pipeline, SM3.0 minimum, deferred lighting, HDR post chain |
| Existing mods | ASI loader (`dinput8.dll`), `SRTT.MixFix.x86.asi` (QOL/fixes: FPS uncap, particle crash fix, black-bar removal), ZMenu trainer (disabled: `.asi.bak`), `zmods_twitch.dll` |
| Data | `packfiles\*.vpp_pc` / `.str2_pc` archives; textures are `cvbm/cpeg` formats (irrelevant for Remix — replacements are hash-based at the D3D9 level) |

The 32-bit DX9 exe means Remix attaches via the **bridge**: the runtime's 32-bit `d3d9.dll`
goes next to the exe and forwards to the 64-bit renderer in `.trex\` (`NvRemixBridge.exe`).
The standard runtime release zip contains both — copy its contents beside the exe.

## Why SR3 is in the hardest compatibility class

1. **No fixed-function fallback.** Min spec is an SM3 GPU; every draw uses vertex+pixel shaders.
   The `d3d9.shaderModel = 0` trick (forcing games with FF fallbacks down to fixed function)
   cannot work here. Everything depends on Remix's **vertex capture** path, which injects
   capture code into vertex shaders to recover final transformed geometry. Vertex capture has
   improved a lot since runtime 0.2 (captured normals, better perspective-divide precision) but
   is still officially "experimental for simple vertex shaders" — SR3's skinning/instancing
   shaders will stress it.
2. **Deferred lighting.** What Remix sees per frame is mostly G-buffer writes plus fullscreen
   lighting/composite quads. Consequences:
   - No D3D9 lights are ever submitted → set a **fallback light** or the path tracer renders black.
   - The fullscreen passes (lighting resolve, SSAO, bloom, tonemap, DoF, god rays) appear as
     screen-covering geometry with render-target textures → must be pushed into ignore/UI
     categories or the output is garbage.
   - Actual scene lighting must be authored from scratch in the Remix Toolkit (Phase 3).
3. **GPU skinning** for peds/player → vertex capture required for characters, and animated
   meshes tend to have unstable draw-call hashes (matters for replacements later, less for triage).
4. **Known-fragile DX9 path on modern Windows** — the DX9 exe is the original 2011 code path;
   MixFix's `ParticleCrashFix` already papers over one crash. Expect Remix to surface more.

Realistic best case: geometry + textures render through the path tracer with manual relighting.
Plan for the possibility that some passes (water, reflections, character shadows) stay broken
and get documented as known issues.

## In-game settings for triage (`configs\display.remix.ini`)

The current `display.ini` is hostile to Remix. Deploy script swaps it (original backed up):

| Setting | Was | Triage value | Why |
|---|---|---|---|
| `MSAA_Level` | 8 | **0** | MSAA breaks Remix outright |
| `SSAO_Level` | 3 | 0 | fullscreen AO pass = garbage input |
| `PostProcess` | 2 | 0 | kills bloom/tonemap/DoF/god-ray passes at the source |
| `MotionBlur` | true | false | post pass |
| `Reflections` | 2 | 0 | render-to-texture passes confuse capture |
| `ShadowDetail` | 4096 | 0 | shadow map passes; RT shadows replace them |
| `LightingDetail` | 2 | 1 | simplest deferred path the engine offers |
| `VSync` | true | false | let DXVK/Remix own presentation |
| `Fullscreen` | false | false (keep) | windowed = survivable debugging |

Turning passes off in-game is far more reliable than filtering them in rtx.conf — always
prefer the engine switch when one exists.

## First-boot triage checklist (Phase 1)

1. Launch via `tools\launch.ps1`. Three outcomes:
   - Crash on boot → check `.trex\` logs in the game dir; try latest vs. previous runtime release.
   - Boots, black/garbage screen → normal for this class of game, proceed.
   - Boots and renders something → excellent, proceed.
2. **Alt+X** → Remix menu → Developer Settings. Confirm the runtime is actually attached
   (if Alt+X does nothing, the ASI loader's `dinput8.dll` may be eating input — test with it
   temporarily renamed).
3. Enable **vertex capture** (Developer Settings → Geometry) if geometry is missing. This is
   the make-or-break switch for SR3.
4. Set **fallback light** to "No lights present" (or Always) so the scene isn't black.
5. Use the debug view modes (e.g. "Is Instance / Geometry Hash") to see what Remix recognizes.
6. Texture categorization pass — Alt+X → Game Setup: tag HUD/menus as **UI**, skybox as **Sky**,
   any surviving fullscreen-pass textures as **Ignore**, particles/decals into their categories.
   Save → writes hashes into `rtx.conf` in the game dir → `tools\pull-conf.ps1` to version it.
7. Record everything in `docs\worklog.md`: runtime version, what rendered, what didn't,
   which options mattered. Compatibility work is 90% bookkeeping.

## Experiment backlog (when stuck)

- Newest runtime ↔ older releases (vertex capture behavior shifts between versions).
- ASI mods off entirely (rename `dinput8.dll`) to rule out interference; re-enable after.
- ZMenu (re-enable `.asi.bak`) once stable — time-of-day freeze and teleport are genuinely
  useful for reproducible captures.
- Interiors and cutscenes early — deferred games often behave differently indoors.
- Check the RTX Remix Showcase Discord compatibility table + `hash-database` threads for prior
  SR3 attempts; someone may have a MixFix-style Remix patch or a verdict already.
- Nuclear option if the deferred path is unworkable: an ASI/patch that forces the forward/lowest
  render path or stubs specific passes (this is what per-game "RemixFix" plugins do for other
  deferred titles). That's a reverse-engineering project — scope it only if triage stalls.

## Option-name caveat

`rtx.conf` option names drift between runtime versions. The installed runtime ships its own
authoritative list (`RtxOptions.md` / docs in the release package) — trust that over anything
in this repo, and prefer setting options through the in-game UI (which writes correct names),
then `pull-conf.ps1`.

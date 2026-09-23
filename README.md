# Saints Row The Third RTX REMIX Compatibility Mod

**A compatibility shim that makes the original (2011, DX9) Saints Row: The Third render through
[NVIDIA RTX Remix](https://github.com/NVIDIAGameWorks/rtx-remix), so the game can be path traced,
captured, and eventually re-authored with PBR assets.**

Saints Row: The Third never calls `SetTransform`, so RTX Remix has no camera and cannot path-trace
it. This shim recovers the engine's matrices from vertex-shader constants and gives Remix a scene
it can trace.

> **Correction, 2026-08-30.** Earlier versions of this README said the fixed-function conversion
> *was* the path-traced world, on the evidence that `ffp=0` produced a rasterised game. That
> conclusion was wrong, and it is worth stating plainly because it stood for weeks: the only code
> publishing a **camera** to Remix lived inside the conversion path, so `ffp=0` removed the camera
> and the conversion *at the same time*. The A/B had no control. With `cameraOnly=1` — camera
> published, nothing converted — the world path-traces fine. What Remix actually needed was the
> camera; the conversion is how draws are filtered down to something it can afford.

---

## Disclaimer

Disclaimer: this is made with AI. I tried to give credit to everyone. if i missed anything, please tell me.

this is created with Claude opus and some fable 5 through visual studio code.

the sr2 rtx proxy and some vibe reverse engineering tools were used in this project.

I hope to be able to find contributors to help finish this project.

saints row the third has been one of my favorite games and i think it could use a facelift with this mod.

the ultimate goal is to get everything working and replace the assets with the remastered version since they are already PBR.

Thanks to everyone has made this project possible with their work on previous projects!

and thanks to RTX REMIX and Nvidia!

---

## Status: work in progress

The path-traced world renders, is textured and is lit. Characters render with correct skin, hair and
correct customisation colours on every garment. As of v0.1.6 it runs at **roughly twice the frame
rate** and no longer crashes after eight minutes. **The sky and the in-game HUD are still missing** —
see [Known issues](#known-issues) before you install, so you know what you are getting.

**Install: [INSTALL.md](INSTALL.md)** · **What changed: [CHANGELOG.md](CHANGELOG.md)** ·
**Credits: [CREDITS.md](CREDITS.md)** · **Contributing: [CONTRIBUTING.md](CONTRIBUTING.md)**

### v0.1.6 found two bugs in RTX Remix itself

This project now runs a **patched Remix runtime**. Both bugs are in NVIDIA's shipped build *and* in
`origin/main`, and neither is SR3-specific — any game that skins on the GPU through Remix hits both.

- **A staging-buffer memory leak.** A slice of `RtxStagingDataAlloc` is acquired and never released
  on one path. On SR3 that reached **13.7 GB across 428 × 32 MB blocks** and crashed the game after
  about eight minutes. With the fix: 5 blocks, 160 MB.
- **The skinning kernel reads normals as floats only**, so packed normals arrive as garbage — hard,
  faceted shading on every GPU-skinned character.

The patches are in [`remix-fork-patches/`](remix-fork-patches/), with the trade-off explained: the
stock runtime works and gives you everything the mod does, it just also gives you the crash and the
faceted shading.

### The technique that unlocked v0.1.4

**Remix's refusal is about the vertex shader, not the draw.** Its own message says so —
`Skipping draw call with shader usage as vertex capture is not enabled` — because vertex capture
exists to capture *vertex-shader* output. The pixel shader was never the problem.

So a draw re-issued with **fixed-function vertex processing** while **the game's own pixel shader
stays bound** is a combination Remix executes happily with capture off:

| what it fixed | how |
|---|---|
| **the character's skin** | the atlas composites re-issued as a full-target quad with their own pixel shader kept |
| the menu video | proved the rule: nulling the pixel shader turned it greyscale, because Bink is Y/Cr/Cb across three stages and stage 0 alone is luma |
| the HUD's draws — *found*, not yet fixed | a whole D3D9 entry point, `DrawPrimitiveUP`, had never been hooked, so 13–30 draws a frame were invisible to the shim. They are now rebuilt as `D3DFVF_XYZRHW` quads (a 28-byte field reorder, since the positions are already screen pixels) — but the in-game HUD is still not visible. See [Known issues](#known-issues). |

### The Remix programmatic API (v0.1.2, now optional)

The shim can also describe geometry to Remix **directly** — creating meshes, naming materials and
choosing textures — instead of dressing draws up as fixed function. That is how characters were
built in v0.1.2, gated behind `exposeRemixApi = True` in `.trex\bridge.conf`.

It ships **off** in v0.1.4 (`remixApiCharacter=0`), because the technique above makes the game's
own character render correctly — which also retires the duplicate character and the frozen one.
The API path remains available and is still the route for describing arbitrary geometry to Remix.

## What it does

RTX Remix path-traces **fixed-function** geometry natively. Anything a game draws through vertex
and pixel shaders, Remix has to *reconstruct* from shader output — and that reconstruction is the
source of nearly every defect on a target like this one: duplicated meshes, quads welded to the
camera, stale albedo, wrong UVs.

Saints Row: The Third is a late-DX9, fully shader-driven engine (Volition Core, Shader Model 3.0)
with deferred "inferred lighting" and heavy post-processing. It **never calls `SetTransform`**, so
Remix had no camera at all and was simply rasterising the whole game.

`sr3-rtx.asi` is a D3D9 shim that sits on the device vtable and does two things.

**For the world**, it re-issues eligible draws as fixed function: it recovers the engine's real
matrices from vertex-shader constant registers, hands them to D3D9 through `SetTransform`, binds
the real albedo map to stage 0, draws, and restores. Remix then receives unambiguous geometry.

**For characters**, it re-issues the draw with fixed-function *vertex* processing while leaving the
game's own *pixel* shader bound, and generates the customisation textures itself — the game builds
clothing colour per texel from a pattern map and three constants, which no texture-stage
arrangement can express.

That required reverse-engineering a fair amount of the engine — including, eventually, the
renderer itself: the command buffer, the render thread, a 74-opcode dispatch table, and the
engine's own draw kill-switch, all in [docs/engine-map.md](docs/engine-map.md). Some of what had
to be established along the way:

- **the matrix registers** — `projTM` (VIEW·PROJ) at c28, `objTM` (world) at c32, `IR_World2View`
  at c48, the 64-bone palette at c52, three registers per bone;
- **the UV formula**, `uv = raw * tiling / 1024`, where the tiling uniforms live at *different
  registers in different shaders* and must be read from each shader's CTAB;
- **skinning** — the skinned vertex buffer is STATIC and holds the bind pose; all animation lives in
  the c52 constants, invisible to Remix. Characters were skinned on the CPU until v0.1.6, which
  moved them onto the GPU through fixed-function vertex blending (a side stream of `FLOAT3` weights
  and `UBYTE4` indices, palette in `WORLDMATRIX(0..255)`). Remix hashes geometry *before* skinning,
  so asset identity stays stable;
- **alpha cutouts** are done with `texkill` inside 403 pixel shaders, never via
  `D3DRS_ALPHATESTENABLE`, so a render-state rule cannot see them;
- **Remix discards SHORT2 texcoords** (`VkFormat 80 = R16G16_SSCALED`) — which is why the world
  rendered as flat material colour until the UVs were converted to a float2 stream;
- **SR3's per-texel material recipes**, disassembled out of the shaders. The NPC and player
  families use *different* rules — a weighted sum with a desaturation branch versus a lerp chain —
  and the player's tint is a Reinhard tonemap at ×5, not a brightness scale, which is why every
  linear correction preserved the hue and never produced the game's saturated colours;
- **which UV set feeds which sampler**, per shader and per draw. "Albedo is TEXCOORD0, pattern is
  TEXCOORD1" is not a rule — see [docs/cloth-uv-map.md](docs/cloth-uv-map.md);
- that a garment's second UV set can be **unrelated** to its first, and that the *mesh* is what
  relates them: rasterise the triangles into the diffuse's space and interpolate.

Full reverse-engineering results: [docs/shader-map.md](docs/shader-map.md) and
[docs/YOUR-INSTRUCTIONS.md](docs/YOUR-INSTRUCTIONS.md). The complete run-by-run history, including
every measurement and every failed approach, is in [docs/worklog.md](docs/worklog.md).

## Known issues

Stated plainly, because a compatibility mod that hides its gaps wastes everyone's time.

| issue | status |
|---|---|
| **No sky.** The `rfg-skybox` family (~50 draws/frame) is passed through rather than converted, and pass-through draws are skipped now that vertex capture is off. | open — needs conversion; the dome is 343 verts one unit from the camera, so it needs care |
| **No in-game HUD.** Its draws are found and rebuilt now, but it does not appear, and sub-menu text and backgrounds are missing. The menu video works. Parked. | open — **v0.1.4 reported this as fixed and it was not** |
| **Other outfits.** Every colour mechanism is general, but only one outfit is confirmed on screen. A garment needing more than 4 tiles, or with a visible panel wider than 12 texture widths, is refused by its own guard and falls back to a single tile. | open |
| **Misplaced buildings.** Rare, sticks for a few seconds, angle- and location-dependent, seen while flying. | open — **the top correctness bug, and unattributed.** The shim's own data is correct and a 60-frame ring recording found no object moving, so whether the patched runtime is involved is not yet known |
| **Body skin and head colours do not match** on characters. | open — probably the most visible remaining fault |
| **Decal flicker** in Remix's Geometry Hash view. Proven *not* to be a hash change. | open |
| **The Remix-API character path**, when enabled, is frozen and stands beside the game's own copy — handing Remix `MeshInfoSkinning` crashed its 64-bit server. It ships **off**, so this is not something you will see. | open, but not in the shipped config |
| **First-time per-buffer conversions run on the game's render thread** when content streams in — worst spike 219 ms. They belong on a worker. | open |
| **Performance.** The occlusion-query hook fabricates a "visible" answer to every query first (still necessary - see below), but the shim now runs its own occlusion test on a worker thread and refines that answer to 0 pixels for a box entirely behind opaque geometry (`occlusionCull=1`, `occlusionDryRun=0`). Some geometry the game would have culled itself still reaches the path tracer. | partially addressed |

Fixed and no longer a concern: z-fighting / doubled world, the crash on fast movement, the black
sky, white surfaces, frozen character animation, the camera-blocking particle plane, the flat
untextured world, over-tiled roads, churning geometry hashes, unreadable texcoords on DYNAMIC
vertex buffers (fixed by a per-draw ring, `convertDynamicUV=1`), black cars, objects popping out of
existence as they entered the frustum, car parts and glass drifting in rhythm with character
animation (v0.1.1), the black character texture (v0.1.2), the character skin and menu video (v0.1.4),
hair with no strand detail (v0.1.4), and — in v0.1.5 — **the player's clothing colours, on every garment**.

In v0.1.6: the ~8-minute crash from a Remix memory leak, hard faceted shading on GPU-skinned
characters, the raster overlay that stopped path tracing, stretched bracelets and glasses lenses,
car parts floating with animation in the new GPU path, and roughly half the frame time.

Release history and what changed in each: **[CHANGELOG.md](CHANGELOG.md)**.

## Requirements

- An NVIDIA RTX GPU (20-series minimum; 30/40-series strongly recommended) and a recent driver.
- **The original 2011 Saints Row: The Third.** Steam or GOG both work. *Saints Row: The Third
  Remastered is a different, DX11/DX12 game and cannot be used.*
- The **DX9** executable, `SaintsRowTheThird.exe`. The `_DX11` exe can never work with Remix.

## Install

See **[INSTALL.md](INSTALL.md)** for the full step-by-step, or grab the
[latest release](../../releases/latest) and follow the `INSTALL.md` inside it.

## Repository layout

```
├── src/sr3-rtx/         the shim: sr3rtx.cpp + build.ps1
├── build/               the built sr3-rtx.asi (committed so a release needs no local build; NOT byte-reproducible - the PE timestamp changes on every compile, and the toolchain is whatever `vswhere -latest` finds)
├── configs/
│   ├── sr3-rtx.ini          shim settings — every one documented with the measurement behind it
│   ├── rtx.conf             the Remix config (texture categorisation, options)
│   ├── user.conf            Remix quality/performance settings (wins over rtx.conf)
│   ├── dxvk.conf            d3d9.maxEnabledLights = 64
│   ├── bridge.conf          exposeRemixApi = True — goes in .trex\, unlocks the Remix API
│   └── display.remix.ini    Remix-safe in-game display settings (MSAA off, post off)
├── tools/               install/deploy/launch scripts and the RE tooling
│   ├── install-runtime.ps1  download + install the Remix runtime into the game dir
│   ├── deploy-conf.ps1      push configs/ into the game dir (backs up originals)
│   ├── pull-conf.ps1        pull the in-game-tuned rtx.conf back into configs/
│   ├── launch.ps1           launch the DX9 exe
│   ├── vpp.py               read VPP_PC / STR2_PC v6 containers
│   ├── peg.py               read PEG (GEKV v13) texture containers -> PNG
│   ├── fxo_scan.py          parse .fxo_pc shaders' CTABs -> named constants + registers
│   └── fxo_disasm.py        disassemble one shader out of a .fxo_pc
├── docs/
│   ├── HANDOFF-PROMPT.md    start here if you are picking the project up
│   ├── YOUR-INSTRUCTIONS.md current state, engine facts, and the dead ends not to retry
│   ├── engine-map.md        the renderer: command buffer, opcodes, dispatch table
│   ├── cloth-uv-map.md      which UV set feeds which sampler, per shader
│   └── worklog.md           the run-by-run history
├── remix-fork-patches/  patches to RTX Remix itself — two upstream bug fixes
├── engine-control/      a SEPARATE plugin, sr3-engine.asi — see below
└── srttr-hair/          a SEPARATE mod, the SRTTR hair reshape — see below
```

### Two sub-projects that ship separately

**`engine-control/`** builds `sr3-engine.asi`, which hooks **the engine's own render-command
dispatch table** at `0x013509F8` rather than the D3D9 device vtable. SR3 is a command-buffer
renderer: a producer thread writes command blocks into a ring, a render thread consumes them and
dispatches each through a 74-entry function-pointer table — and that table sits in writable
`.data`. Hooking there puts our code *inside* the engine, one level above D3D9, where a command is
still a command rather than six loose arguments. It shares no state with `sr3-rtx.asi`; either can
be removed without affecting the other, and the ASI loader loads both.

**`srttr-hair/`** is an unrelated mod that reshapes *Saints Row: The Third Remastered*'s hair
meshes back onto the 2011 game's silhouette — the Remaster re-authored every style puffier, with
stray strand cards over the crown. Only vertex positions change; vertex counts, UVs, bone weights,
index buffers and textures stay byte for byte as SRTTR shipped them. Its tools are versioned here;
its game data is not.

Not in the repo, by design: the game copy, the Remix runtime, and extracted game assets (`re/`) —
none of those are ours to redistribute. `tools/vpp_extract.py` regenerates the last one.

## Building the shim

Needs the MSVC Build Tools (x86 toolchain). It is a single translation unit and builds in seconds:

```powershell
powershell -File src\sr3-rtx\build.ps1
```

Output lands in `build\sr3-rtx.asi`.

## Reverse engineering

```powershell
# Unpack the shader library (1,693 files) and map every named shader constant
py -3 tools\vpp_extract.py "Saints Row 3\packfiles\pc\cache\shaders.vpp_pc" re\shaders
py -3 tools\fxo_scan.py re\shaders re\shader_constants.csv
```

That produces 52,990 named constants across 7,276 shaders. `tools/fxo_disasm.py <file> <index>`
disassembles an individual shader.

`tools/vpp.py` and `tools/peg.py` read the game's texture containers — `VPP_PC`/`STR2_PC` v6
archives and the `GEKV` v13 PEG format, where a texture is a *pair* of files: one holding the
directory, one holding the pixels. Together they extract the 3,467 customisation bitmaps the game
ships, which is what asset replacement will be authored against.

**Extracted game assets are not in this repository** — they are Volition/Deep Silver art, and
`game-textures/`, `player-textures/` and `re/` are all gitignored. Regenerate them from your own
copy of the game.

## Roadmap

1. **Attribute the misplaced buildings.** The top correctness bug. The control run — stock runtime,
   memoization back on, same flight path — has not been done.
2. **Character body/head colour mismatch**, the most visible remaining fault.
3. **The in-game HUD.** Its draws are found and rebuilt; it still does not appear. Parked.
4. **Convert the sky** — the last population lost when vertex capture was turned off.
5. **Report both Remix bugs upstream**, and restore index memoization properly by giving each
   memoized copy its own host-visible buffer instead of carving it from the staging allocator.
6. **Confirm the colour work on other outfits.** Every mechanism is general; only one outfit has
   been checked on screen.
7. Move first-time per-buffer conversions off the render thread.
8. Scene captures into the RTX Remix Toolkit; proper sun/sky and key lights, replacing the
   fallback light.
9. **The goal: replace assets with the Saints Row: The Third Remastered versions, which are
   already PBR.** The plan and its two substitution points are in
   [docs/asset-replacement-plan.md](docs/asset-replacement-plan.md).

## Contributing

Help is genuinely wanted — see [CONTRIBUTING.md](CONTRIBUTING.md). The two best places to start
are the sky and the UI, and [docs/YOUR-INSTRUCTIONS.md](docs/YOUR-INSTRUCTIONS.md) is a complete
handoff: current state, established engine facts, and a list of dead ends *not* to retry.

## Credits

This project stands on other people's work — see **[CREDITS.md](CREDITS.md)** for the full list.
The short version:

- **[NVIDIA RTX Remix](https://github.com/NVIDIAGameWorks/rtx-remix)** and
  [dxvk-remix](https://github.com/NVIDIAGameWorks/dxvk-remix) — the runtime this exists to serve,
  and on top of [DXVK](https://github.com/doitsujin/dxvk).
- **[BRAGme/sr2-rtx-remix-proxy](https://github.com/BRAGme/sr2-rtx-remix-proxy)** (MIT) — the
  project this is a port of. [BRAGme](https://github.com/BRAGme),
  [xoxor4d](https://github.com/xoxor4d) (`remix-comp-base`, the original) and
  [Kim2091](https://github.com/Kim2091) (who adapted it). The fixed-function conversion approach
  is theirs, and it is the single most important idea here.
- **[Ekozmaster/Vibe-Reverse-Engineering](https://github.com/Ekozmaster/Vibe-Reverse-Engineering)**
  (MIT) — [Ekozmaster](https://github.com/Ekozmaster) (Emanuel Kozerski),
  [Kim2091](https://github.com/Kim2091), [Night1099](https://github.com/Night1099) and
  [Hemry81](https://github.com/Hemry81). Four of the biggest unblocks in this project came out of
  its `retools.search strings`.
- **[ThirteenAG](https://github.com/ThirteenAG)** — Ultimate ASI Loader.
- **Volition** — for the game, and for an inferred-lighting renderer that was a pleasure to read.

**If credit is missing or wrong, open an issue and it will be fixed.**

## License

[MIT](LICENSE), matching the SR2 proxy this is derived from.

This is an unofficial fan project. It is not affiliated with or endorsed by Volition, Deep Silver,
THQ Nordic, Koch Media, or NVIDIA. Saints Row is a trademark of its respective owners. No game
assets are distributed here.

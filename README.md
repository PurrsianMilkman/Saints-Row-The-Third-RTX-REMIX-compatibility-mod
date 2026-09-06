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

The path-traced world renders, is textured and is lit. **The sky and the HUD are still missing**,
and characters are mid-rebuild onto a new architecture — see [Known issues](#known-issues) before
you install, so you know what you are getting.

**Install: [INSTALL.md](INSTALL.md)** · **What changed: [CHANGELOG.md](CHANGELOG.md)** ·
**Credits: [CREDITS.md](CREDITS.md)** · **Contributing: [CONTRIBUTING.md](CONTRIBUTING.md)**

### The architecture changed in v0.1.2

Characters are no longer faked as fixed-function draws. They are described to Remix directly
through its **programmatic API** — the shim creates the meshes, names the materials and chooses
the textures, while the game renders untouched alongside. Ten parts, ~22,000 triangles, correct
geometry, placement, per-slot textures, customisation colours and hair.

The API is gated behind `exposeRemixApi = True` in `.trex\bridge.conf`, which is why the install
now has an extra step. `remixapi_InitializeLibrary` is exported by the **32-bit bridge client**
`d3d9.dll` — the one the game loads, in the game's own process.

This matters beyond characters: it is a route to describing *anything* to Remix directly, rather
than dressing draws up as fixed function and hoping Remix reconstructs them the way we meant.

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

**For characters**, since v0.1.2, it skips that entirely and describes them to Remix through the
programmatic API — meshes, materials and textures, named directly. That sidesteps reconstruction
altogether, and it is where the project is heading generally.

That required reverse-engineering a fair amount of the engine — including, eventually, the
renderer itself: the command buffer, the render thread, a 74-opcode dispatch table, and the
engine's own draw kill-switch, all in [docs/engine-map.md](docs/engine-map.md). Some of what had
to be established along the way:

- **the matrix registers** — `projTM` (VIEW·PROJ) at c28, `objTM` (world) at c32, `IR_World2View`
  at c48, the 64-bone palette at c52, three registers per bone;
- **the UV formula**, `uv = raw * tiling / 1024`, where the tiling uniforms live at *different
  registers in different shaders* and must be read from each shader's CTAB;
- **skinning** — the skinned vertex buffer is STATIC and holds the bind pose; all animation lives
  in the c52 constants, invisible to Remix, so characters are skinned on the CPU before submission;
- **alpha cutouts** are done with `texkill` inside 403 pixel shaders, never via
  `D3DRS_ALPHATESTENABLE`, so a render-state rule cannot see them;
- **Remix discards SHORT2 texcoords** (`VkFormat 80 = R16G16_SSCALED`) — which is why the world
  rendered as flat material colour until the UVs were converted to a float2 stream;
- **SR3's per-texel material recipes**, disassembled out of the shaders, because NPC clothing
  colour is computed per texel from a mask plus three constants and no texture-stage arrangement
  can express it. The generator is verified byte-exact against the shader.

Full reverse-engineering results: [docs/shader-map.md](docs/shader-map.md) and
[docs/YOUR-INSTRUCTIONS.md](docs/YOUR-INSTRUCTIONS.md). The complete run-by-run history, including
every measurement and every failed approach, is in [docs/worklog.md](docs/worklog.md).

## Known issues

Stated plainly, because a compatibility mod that hides its gaps wastes everyone's time.

| issue | status |
|---|---|
| **No sky.** The `rfg-skybox` family (~50 draws/frame) is passed through rather than converted, and pass-through draws are skipped now that vertex capture is off. | open — needs conversion; the dome is 343 verts one unit from the camera, so it needs care |
| **No HUD.** The UI is shader-drawn and never converted, so it disappears with vertex capture off. | open |
| **The API character stands beside the game's own.** The Remix-API copy is deliberately offset 3 units so the two cannot z-fight while the pipeline is being built. Removing the game's copy is the last step. | by design, for now |
| **The API character is frozen.** Handing Remix `MeshInfoSkinning` crashed its 64-bit server inside `CreateMesh`, so skinning is off (`remixApiSkinning=0`). | open |
| **Clothes read too dark.** The recipe is proven correct against both the exe and the shader — a bake computes exactly what the shader produces — so the fault is in how Remix lights these meshes. `clothBrightness` is a tuning knob, not a fix. | open |
| **Hair has no strand detail** — flat colour only. | open — the colour itself is now correct |
| ~11 draws/frame still have unreadable texcoords — their source vertex buffer is DYNAMIC, so the per-buffer UV conversion cannot cache them. | open — needs a per-draw ring |
| **Shim time grows across a session**, ~24% → ~44% of frame time, worst-case stalls around 300 ms. | open |
| **Performance.** The occlusion-query hook deliberately answers "visible" to every query, so the game submits more geometry than it normally would. Functionality was prioritised over frame rate. | by design, for now |

Fixed and no longer a concern: z-fighting / doubled world, the crash on fast movement, the black
sky, white surfaces, frozen character animation, the camera-blocking particle plane, the flat
untextured world, over-tiled roads, churning geometry hashes, black cars, objects popping out of
existence as they entered the frustum, car parts and glass drifting in rhythm with character
animation (v0.1.1), and the black character texture (v0.1.2).

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
├── build/               the built sr3-rtx.asi (committed, so releases are reproducible)
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
│   ├── vpp_extract.py       unpack Volition VPP_PC v6 packfiles (format documented in-file)
│   ├── fxo_scan.py          parse .fxo_pc shaders' CTABs -> named constants + registers
│   └── fxo_disasm.py        disassemble one shader out of a .fxo_pc
└── docs/
    ├── HANDOFF-PROMPT.md    start here if you are picking the project up
    ├── YOUR-INSTRUCTIONS.md current state, engine facts, and the dead ends not to retry
    ├── engine-map.md        the renderer: command buffer, opcodes, dispatch table
    └── worklog.md           the run-by-run history (~11,000 lines)
```

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
python tools\vpp_extract.py "Saints Row 3\packfiles\pc\cache\shaders.vpp_pc" re\shaders
python tools\fxo_scan.py re\shaders re\shader_constants.csv
```

That produces 52,990 named constants across 7,276 shaders. `tools/fxo_disasm.py <file> <index>`
disassembles an individual shader.

## Roadmap

1. **Finish the API character** — get skinning past Remix's server crash so it animates, solve the
   lighting gap that makes clothes read dark, then remove the game's own copy and drop the
   diagnostic offset.
2. **Convert the sky** and **convert the UI** — the two populations lost when vertex capture was
   turned off.
3. Performance: the growing shim time and its stalls.
4. Scene captures into the RTX Remix Toolkit; proper sun/sky and key lights, replacing the
   fallback light.
5. **The goal: replace assets with the Saints Row: The Third Remastered versions, which are
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

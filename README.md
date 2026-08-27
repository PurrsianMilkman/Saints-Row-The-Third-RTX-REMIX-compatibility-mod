# Saints Row The Third RTX REMIX Compatibility Mod

**A compatibility shim that makes the original (2011, DX9) Saints Row: The Third render through
[NVIDIA RTX Remix](https://github.com/NVIDIAGameWorks/rtx-remix), so the game can be path traced,
captured, and eventually re-authored with PBR assets.**

Without this shim, Remix does not path-trace Saints Row: The Third at all. With it, the game is
path traced.

That is a measurement, not a claim: setting `ffp=0` in `sr3-rtx.ini` makes the shim a passive
observer — it still logs, still injects lights, converts nothing — and with it the world is
**rasterised**, with no path tracing at all. Remix's capture button does nothing, because there is
no ray-traced scene to capture. The fixed-function conversion *is* the path-traced world, all of
it. See [docs/evidence/](docs/evidence/).

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

## Status: playable alpha

The path-traced world renders, is textured, animates, and is lit. **The sky and the HUD are
currently missing** — see [Known issues](#known-issues) before you install, so you know what you
are getting.

**Install: [INSTALL.md](INSTALL.md)** · **Credits: [CREDITS.md](CREDITS.md)** ·
**Contributing: [CONTRIBUTING.md](CONTRIBUTING.md)**

## What it does

RTX Remix path-traces **fixed-function** geometry natively. Anything a game draws through vertex
and pixel shaders, Remix has to *reconstruct* from shader output — and that reconstruction is the
source of nearly every defect on a target like this one: duplicated meshes, quads welded to the
camera, stale albedo, wrong UVs.

Saints Row: The Third is a late-DX9, fully shader-driven engine (Volition Core, Shader Model 3.0)
with deferred "inferred lighting" and heavy post-processing. It **never calls `SetTransform`**, so
Remix had no camera at all and was simply rasterising the whole game.

`sr3-rtx.asi` is a D3D9 shim that sits on the device vtable and re-issues every eligible draw as
fixed function: it recovers the engine's real matrices from vertex-shader constant registers,
hands them to D3D9 through `SetTransform`, binds the real albedo map to stage 0, draws, and
restores. Remix then receives unambiguous geometry and path traces it.

That required reverse-engineering a fair amount of the engine. Some of what had to be established
along the way:

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
| **Hair renders white.** Neither hair texture carries the colour, and every character colour constant measures (1,1,1). `Hair_Spec_Color1/2` are the remaining candidates. | open |
| **Player clothing colour** uses a different recipe from the NPC one, with the pattern on a second texture coordinate set. | open — NPC clothing works |
| ~11 draws/frame still have unreadable texcoords — their source vertex buffer is DYNAMIC, so the per-buffer UV conversion cannot cache them. | open — needs a per-draw ring |
| **Shim time grows across a session**, ~24% → ~44% of frame time, worst-case stalls around 300 ms. It tracks skinned-geometry volume, so it may just be a busier district. | open — largest problem after the sky and the HUD |
| **Performance.** The occlusion-query hook deliberately answers "visible" to every query, so the game submits more geometry than it normally would. Functionality was prioritised over frame rate. | by design, for now |

Fixed and no longer a concern: z-fighting / doubled world, the crash on fast movement, the black
sky, white surfaces, frozen character animation, the camera-blocking particle plane, the flat
untextured world, over-tiled roads, churning geometry hashes, black cars, objects popping out of
existence as they entered the frustum, and — as of v0.1.1 — car parts and glass drifting in rhythm
with character animation.

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
│   ├── dxvk.conf            d3d9.maxEnabledLights = 64
│   └── display.remix.ini    Remix-safe in-game display settings (MSAA off, post off)
├── tools/               install/deploy/launch scripts and the RE tooling
│   ├── install-runtime.ps1  download + install the Remix runtime into the game dir
│   ├── deploy-conf.ps1      push configs/ into the game dir (backs up originals)
│   ├── pull-conf.ps1        pull the in-game-tuned rtx.conf back into configs/
│   ├── launch.ps1           launch the DX9 exe
│   ├── vpp_extract.py       unpack Volition VPP_PC v6 packfiles (format documented in-file)
│   └── fxo_scan.py          parse .fxo_pc shaders' CTABs -> named constants + registers
└── docs/                notes, shader map, the full worklog, and docs/evidence/
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

1. **Convert the sky** and **convert the UI** — the two populations lost when vertex capture was
   turned off. Both are ordinary conversion work.
2. Hair colour and player clothing colour.
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

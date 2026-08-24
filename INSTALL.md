# Installation

How to get Saints Row: The Third path tracing with RTX Remix. Read
[Known issues](README.md#known-issues) first — this is an alpha, and the sky and HUD are currently
missing.

Total time: about 15 minutes.

---

## 0. Before you start

**Hardware and drivers**

- An NVIDIA RTX GPU. 20-series is the minimum; 30- or 40-series is strongly recommended, because
  the shim currently submits more geometry than the game normally would (see step 7).
- A recent NVIDIA Game Ready driver.

**The right copy of the game**

- The **original 2011 Saints Row: The Third**. Steam and GOG both work.
- **Saints Row: The Third Remastered will not work.** It is a different, DX11/DX12 game. RTX Remix
  only attaches to DX8/DX9 titles.
- You will launch `SaintsRowTheThird.exe`. The `SaintsRowTheThird_DX11.exe` in the same folder can
  never work with Remix — ignore it completely.

**Work on a copy of the game.**

Strongly recommended. RTX Remix replaces `d3d9.dll` and drops a `.trex\` folder next to the
executable, and you will be editing `display.ini`. Copy the whole game folder somewhere else and
mod the copy — that way a bad experiment costs you nothing, and Steam/GOG never fights you over
changed files.

Throughout this document, **`<game>`** means the folder containing `SaintsRowTheThird.exe`.

**Other ASI mods conflict.** If you have SRTT.MixFix, ZMenu, Gentlemen of the Row, or any other
`.asi` in the game folder, rename them to `.asi.disabled` before you start. Get this working
first, then re-enable them one at a time if you want them.

---

## 1. Install the RTX Remix runtime

Download the latest runtime from
[NVIDIAGameWorks/rtx-remix/releases](https://github.com/NVIDIAGameWorks/rtx-remix/releases).
Take the plain runtime `.zip` — **not** the `debug` or `symbols` package.

> Developed and tested against **Remix runtime 1.5.2**. Newer versions should work, but if
> something behaves oddly, try 1.5.2 before reporting it.

Extract it and copy its contents into `<game>`, so that you end up with:

```
<game>\
├── SaintsRowTheThird.exe
├── d3d9.dll                  <- from Remix (the 32-bit bridge client)
├── NvRemixLauncher32.exe     <- from Remix
└── .trex\                    <- from Remix (the 64-bit renderer)
    ├── d3d9.dll
    ├── NvRemixBridge.exe
    └── ...
```

Some release zips nest everything inside one top-level folder — if so, copy the *contents* of that
folder, not the folder itself. `d3d9.dll` and `.trex\` must sit directly beside the `.exe`.

**Or do it with the script in this repo**, which downloads the latest release and flattens that
nesting for you:

```powershell
powershell -File tools\install-runtime.ps1
# or, if you already downloaded the zip:
powershell -File tools\install-runtime.ps1 -ZipPath "C:\path\to\remix-1.5.2-release.zip"
```

(The script expects the game at `Saints Row 3\` inside the repo. If your layout differs, copy the
files by hand — it is only two items.)

---

## 2. Install an ASI loader

`sr3-rtx.asi` is loaded by an ASI loader, which SR3 does not ship with.

Download **[Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases)** —
the **x86 / 32-bit** build, `Ultimate-ASI-Loader.zip`. Saints Row: The Third is a 32-bit game;
the x64 build will silently do nothing.

Rename the `dinput8.dll` from that zip (or `dinput8.dll` from the archive's contents) and place it
in `<game>`:

```
<game>\dinput8.dll
```

> **Do not use `d3d9.dll` as the proxy name.** That name is already taken by RTX Remix, and
> overwriting it will break the whole thing. `dinput8.dll` is the name this project uses and tests.

If you already have another ASI loader in place (many SR3 mod packs ship one), you can keep it —
just make sure it is the 32-bit one and that it is not named `d3d9.dll`.

---

## 3. Install the mod

From the release zip (or from `build\` and `configs\` in this repo), copy these into `<game>`:

| file | goes to | what it is |
|---|---|---|
| `sr3-rtx.asi` | `<game>\sr3-rtx.asi` | the shim itself |
| `sr3-rtx.ini` | `<game>\sr3-rtx.ini` | its settings — **required**, it is not optional |
| `rtx.conf` | `<game>\rtx.conf` | the Remix config: texture categorisation and options |
| `dxvk.conf` | `<game>\dxvk.conf` | raises the D3D9 light limit to 64 |

`sr3-rtx.ini` must sit beside the `.asi`. Without it the shim falls back to defaults and you will
not get the tested configuration.

---

## 4. Apply Remix-safe display settings

Copy `display.remix.ini` over `<game>\display.ini`. **Back up your original first:**

```powershell
copy "<game>\display.ini" "<game>\display.ini.pre-remix"
copy display.remix.ini "<game>\display.ini"
```

Or use `powershell -File tools\deploy-conf.ps1`, which backs up `display.ini` and deploys both it
and `rtx.conf`.

What matters in there, and why:

- **`MSAA_Level = 0`** — MSAA is a hard incompatibility with Remix. Never turn it back on.
- `SSAO_Level`, `ShadowDetail`, `Reflections`, `MotionBlur`, `PostProcess` all off — these are
  fullscreen passes that produce garbage input for a path tracer. Killing them at the engine level
  is cleaner than filtering them in `rtx.conf`.
- `LightingDetail = 1` — the simplest deferred path the engine offers.
- `Fullscreen = false`, `VSync = false` — let DXVK/Remix own presentation.
- `Preset = 5` — a custom preset, so the values above are actually honoured.

Adjust `ResolutionWidth` / `ResolutionHeight` / `RefreshRate` to your monitor.

> **The game rewrites `display.ini` whenever you change options in its own menu.** If you touch
> the in-game video settings, re-apply this file afterwards.

---

## 5. Launch

Run **`SaintsRowTheThird.exe` directly**.

```powershell
powershell -File tools\launch.ps1
```

**Do not launch through `game_launcher.exe`, the Steam/GOG play button, or the desktop shortcut**
if they route through the launcher — the launcher can start the DX11 executable, which Remix
cannot attach to.

The first launch is slow: DXVK compiles shaders and Remix builds its caches. Expect stutter for
the first minute or two, and expect the very first frames to take a few hundred milliseconds each.
This settles.

---

## 6. Check that it worked

**In game, press `Alt+X`.** The RTX Remix menu should open. If it does, Remix is attached.

**Check `<game>\sr3-rtx.log`.** The first lines should read something like:

```
sr3-rtx loaded (pid ...) - FFP conversion, ported from sr2-rtx-remix-proxy
settings: ffp=1 convertSkinned=1 hiddenPassMode=2 skipUntextured=1 screenSpaceMode=2 ...
CreateDeviceEx -> hooking device (backbuffer 2560x1440, aspect 1.778)
device reports MaxActiveLights = 64, using 64 light slots
query vtable patched from the first OCCLUSION query (GetData slot 7), forceOcclusionVisible=1
```

Then, once a scene is up, a periodic frame report. The line that tells you the conversion is
actually running is:

```
frame 1800 | draws NNNN/frame | FFP converted NNN/frame (NN.N%)
```

A non-zero "FFP converted" count is the whole ballgame — that is the path-traced world. If it
reads `0/frame`, the shim is loaded but converting nothing; see troubleshooting below.

**Visually**, you should see soft path-traced shadows and bounce lighting, and *no* sky and *no*
HUD. Missing sky and HUD are expected in this build — they are the two known gaps.

---

## 7. Performance

This build prioritises correctness over frame rate. The occlusion-query hook
(`forceOcclusionVisible=1` in `sr3-rtx.ini`) answers every one of the engine's GPU occlusion
queries with "fully visible", which is what stops objects vanishing as they enter the frustum —
but it also means the game submits geometry it would normally cull.

If you need frames, the levers, in order of effect:

1. Lower the resolution in `display.ini`. Path tracing scales with pixels.
2. Turn DLSS on in the Remix menu (`Alt+X`) if your GPU supports it.
3. In the Remix menu, reduce bounce count and sample counts.

`rtx.conf` in this repo is a *tuned* config, not a default one — every texture hash in it was
categorised by hand over dozens of runs. Prefer adjusting settings in the Remix menu and then
running `tools\pull-conf.ps1` to save them back, rather than hand-editing hashes.

---

## Troubleshooting

**The Remix menu (`Alt+X`) does not open.**
Remix is not attached. Check that `d3d9.dll` and `.trex\` are directly beside
`SaintsRowTheThird.exe`, and that you launched the DX9 exe rather than `_DX11` or the launcher.
Read `<game>\rtx-remix\logs\remix-dxvk.log` — Remix names what it refused and why.

**No `sr3-rtx.log` appears.**
The ASI loader is not loading the shim. Confirm `dinput8.dll` is the **32-bit** Ultimate ASI
Loader, that it is named `dinput8.dll` and not `d3d9.dll`, and that `sr3-rtx.asi` is in the same
folder as the exe.

**The game is path traced but everything is one flat colour, or geometry is missing.**
Confirm `sr3-rtx.ini` is present beside the `.asi`. Missing settings, not a missing shim, is the
usual cause.

**Everything renders as a pink-and-black checkerboard, or bright magenta.**
That is Remix's "ignored material" pattern. It means `rtx.useVertexCapture` has been turned back
on. The shipped `rtx.conf` sets `rtx.useVertexCapture = False`; check yours still does. This is
load-bearing — vertex capture makes Remix reconstruct every unconverted shader draw and render a
second copy of the world on top of the converted one, and no texture-tagging mechanism suppresses
it. (Nine of them were tried. See [docs/YOUR-INSTRUCTIONS.md](docs/YOUR-INSTRUCTIONS.md), "The
hiding problem".)

**Objects pop out of existence as you approach them.**
`forceOcclusionVisible=1` should be set in `sr3-rtx.ini`. The frame report's line
`occlusion queries: N read back/frame, M answered VISIBLE/frame` tells you whether the hook fired
— if M is 0, it never did.

**The game crashes on startup.**
Almost always another `.asi` conflicting. Disable everything except `sr3-rtx.asi` and retest.

**The game hitches badly.**
The first minutes are shader compilation and are expected. Persistent hitching is logged with a
breakdown: search `sr3-rtx.log` for `HITCH` — it attributes each stall to present, our draws,
other hooks, or the game/bridge itself.

**Something else.**
Open an issue with `<game>\sr3-rtx.log` and `<game>\rtx-remix\logs\remix-dxvk.log` attached.
Those two files together answer most questions.

---

## Uninstalling

Delete from `<game>`: `sr3-rtx.asi`, `sr3-rtx.ini`, `rtx.conf`, `dxvk.conf`, `dinput8.dll`,
`d3d9.dll`, `NvRemixLauncher32.exe`, the `.trex\` folder, the `rtx-remix\` folder, and the
`sr3-rtx*.log` files. Restore `display.ini` from `display.ini.pre-remix`.

Or, if you modded a copy of the game as recommended in step 0: delete the copy.

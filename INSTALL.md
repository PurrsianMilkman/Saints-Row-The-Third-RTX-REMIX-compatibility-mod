# Installation

About 15 minutes. `<game>` below means the folder containing `SaintsRowTheThird.exe`.

> **This is an alpha: there is no sky and no HUD yet.** Full list:
> [Known issues](README.md#known-issues).

## You need

- An **NVIDIA RTX GPU** (20-series minimum) and a recent driver.
- **Saints Row: The Third — the original 2011 game. Steam or GOG.**
- **It does not work on Saints Row: The Third Remastered.** That is a different game.

Mod a *copy* of the game folder, not your only install. Disable any other `.asi` mods first.

## 1. RTX Remix runtime

Download the plain runtime zip (not `debug` or `symbols`) from
[rtx-remix/releases](https://github.com/NVIDIAGameWorks/rtx-remix/releases) and copy its contents
into `<game>`, so `d3d9.dll` and `.trex\` sit directly beside the exe.

Tested against runtime **1.5.2**.

## 2. ASI loader

Download the **32-bit** [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases)
and put it in `<game>` named **`dinput8.dll`**.

Not `d3d9.dll` — that name belongs to Remix.

## 3. Mod files

Copy into `<game>`:

```
sr3-rtx.asi
sr3-rtx.ini      <- required
rtx.conf
dxvk.conf
```

## 4. Display settings

Back up `<game>\display.ini`, then copy `display.remix.ini` over it.

MSAA must be off — it is a hard incompatibility with Remix. The game rewrites this file if you
change video options in its menu, so re-apply it if you do.

## 5. Launch

Run **`SaintsRowTheThird.exe`** directly. Not `SaintsRowTheThird_DX11.exe`, not the launcher.

**Alt+X** opens the Remix menu. The first launch is slow while shaders compile — this settles.

## Did it work?

- **Alt+X** opens the Remix menu.
- `<game>\sr3-rtx.log` exists, and its frame report shows a non-zero **`FFP converted`** count.
  That count is the path-traced world.
- You'll see path-traced lighting, and no sky and no HUD. Those two are expected in this build.

## If it didn't

| symptom | cause |
|---|---|
| Alt+X does nothing | Remix isn't attached. Check `d3d9.dll` + `.trex\` are beside the exe, and that you launched the DX9 exe. |
| No `sr3-rtx.log` | The ASI loader isn't loading it. Check `dinput8.dll` is the **32-bit** build. |
| Flat untextured colours | `sr3-rtx.ini` is missing from `<game>`. |
| Pink/black checkerboard, or magenta | `rtx.useVertexCapture` got turned back on. It must stay `False`. |
| Objects vanish as you approach | `forceOcclusionVisible=1` must be set in `sr3-rtx.ini`. |
| Crash on startup | Another `.asi` conflicting. Disable everything except `sr3-rtx.asi`. |
| Low frame rate | Lower the resolution, and enable DLSS in the Remix menu. This build is not optimised yet. |

Still stuck? Open an issue and attach `<game>\sr3-rtx.log` and
`<game>\rtx-remix\logs\remix-dxvk.log`. Those two answer most questions.

## Uninstall

Delete `sr3-rtx.asi`, `sr3-rtx.ini`, `rtx.conf`, `dxvk.conf`, `dinput8.dll`, `d3d9.dll`,
`NvRemixLauncher32.exe`, `.trex\`, `rtx-remix\` and `sr3-rtx*.log` from `<game>`, and restore your
`display.ini` backup. Or just delete the copy you modded.

# Installation

## Requirements

**This is an alpha version. It is work in progress and not finished at all.**

- **RTX Remix runtime version 1.5.2:** https://github.com/NVIDIAGameWorks/rtx-remix/releases
- **Ultimate ASI Loader** (if you don't have it): https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases
- **Back up `display.ini`** (to restore your display settings after uninstall)

**Note on the runtime.** The stock Remix runtime works, but it has two bugs that this project found
and fixed: a memory leak that **crashes the game after about eight minutes**, and hard, faceted
shading on GPU-skinned characters. To avoid the shading bug on the stock runtime, set
`skinViaFixedFunction=0` in `sr3-rtx.ini`. To fix both properly, build a patched runtime — see
`remix-fork-patches/` in the repository.

## How to install

Copy these files next to `SaintsRowTheThird.exe`:

```
sr3-rtx.asi
sr3-rtx.ini
rtx.conf
dxvk.conf
user.conf
```

Copy this one into the `.trex` folder (it unlocks the Remix API, which characters need):

```
.trex\bridge.conf
```

Run the game. Go to **Options > Display**.

Set everything to **Off** except **Lighting Detail**. Set **Scene Detail** to whichever performs
best.

## To uninstall

Delete:

```
sr3-rtx.asi
sr3-rtx.ini
rtx.conf
dxvk.conf
user.conf
.trex\bridge.conf
```

Then restore your `display.ini` backup.

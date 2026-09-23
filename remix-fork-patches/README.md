# RTX Remix runtime patches

This project runs a **patched build of the RTX Remix runtime**, not the stock one. These are the
patches, against [NVIDIAGameWorks/dxvk-remix](https://github.com/NVIDIAGameWorks/dxvk-remix).

Two of them fix bugs that are present in NVIDIA's shipped build **and** in `origin/main`. They are
not SR3-specific — any game that skins on the GPU through Remix will hit both.

## The patches

| file | what it fixes |
|---|---|
| `staging-release.diff` | **A staging-buffer memory leak.** Remix acquires a slice of `RtxStagingDataAlloc` and never releases it on one path, so the allocator grows without bound. On SR3 that reached **13.7 GB across 428 × 32 MB blocks** and crashed the game after roughly eight minutes. With the fix plus `rtx.enableIndexBufferMemoization = False`: **5 blocks, 160 MB**, and host RAM 13.82 GB → 0.60 GB. |
| `skinning-normal-format.diff` | **The skinning kernel reads normals as floats only.** Anything supplying normals in a packed format gets garbage, which renders as hard, faceted shading on every GPU-skinned character. |
| `build-environment-workarounds.diff` | Local build-environment fixes. Not a bug fix — needed to get the tree building here. |
| `identity-pinning.diff` | Dormant. Kept because it was measured and ruled out, so nobody re-derives it. |

Both real bugs should be reported upstream; they have not been at the time of writing.

## Do I need this?

**The stock runtime works.** You get the path-traced world, the characters, the clothing colours —
everything the mod does. What you also get, without these patches, is:

- the game **crashing after ~8 minutes** as the staging allocator runs away, and
- **hard, faceted shading** on characters, if GPU skinning is enabled.

If you would rather not build a runtime, run with `skinViaFixedFunction=0` in `sr3-rtx.ini` to
avoid the shading bug, and expect to restart the game periodically.

## Building it

```powershell
git clone https://github.com/NVIDIAGameWorks/dxvk-remix.git
cd dxvk-remix
git apply ..\staging-release.diff
git apply ..\skinning-normal-format.diff
# and build-environment-workarounds.diff if your environment needs it

. .\build_common.ps1
meson compile -C _Comp64Release
```

Then copy `_Comp64Release\src\d3d9\d3d9.dll` over `<game>\.trex\d3d9.dll`. **Keep a copy of the
original first** — the shipped runtime is the control for any "is this our fault?" question, and
this project has needed it.

`meson install` is blocked in this tree; copy the artifacts by hand.

## Licensing

dxvk-remix is zlib/libpng (the DXVK parts, © Philip Rebohle and Joshua Ashton) plus MIT (NVIDIA's
additions). Both permit redistributing a modified build, provided altered versions are **plainly
marked as altered** and the notices travel with them.

These patches are diffs against upstream, so they carry no upstream code and none of that applies
to the files in this directory. If you distribute a **built** runtime with these applied, mark it
plainly as an altered version and ship the upstream licence files with it.

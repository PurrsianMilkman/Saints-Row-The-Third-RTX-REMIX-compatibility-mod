# RESUME PROMPT - Saints Row 3 RTX Remix

Continue the SR3 RTX Remix project at `D:\SR3RTXREMIXCOMP`.

## Read first, in this order

1. `docs/YOUR-INSTRUCTIONS.md` - the top section, **THE CLOTH COLOUR FORMULA**, confirmed
   correct on every garment 2026-09-10. Do not change colour output without keeping all of them
   right. Then **STATE, 2026-09-07**.
2. `docs/worklog.md` - the last entry, "SESSION 2026-09-09 (evening) .. 2026-09-10 - THE BRA AND
   THE UNDERWEAR, SOLVED". It is the record of how five hypotheses died and what was true.
3. `docs/cloth-uv-map.md` - which UV set feeds which sampler, per shader, and what the clamp
   registers actually do (nothing, at runtime).

## Where it stands

The game's OWN character renders with correct skin, correct customisation colours on EVERY
garment of the 2026-09-09 outfit - silhouettes, decals, the star on the left cup only - and hair
strand detail, with `rtx.useVertexCapture = False`. The Remix API character is OFF.

**The technique that unlocked it:** Remix refuses a draw for its VERTEX shader, not its pixel
shader. Fixed-function vertex processing + the game's own pixel shader executes with capture off.

**What the cloth work established** (all measured, all behind ini switches):
- the player shader's colour CHAIN (white -> C by blue -> B by green -> A by red) on per-texel
  paths, and Reinhard on the measured Tint_color (5) for the game's saturation - `clothColourCurve`;
- template garments are cut by their texture's ALPHA; keep it, alpha-test the draw - `clothCutout`;
- INDEPENDENT uv sets are related by the MESH: rasterise triangles into the diffuse's space with
  uv1 interpolated - `clothMeshDecal`;
- two surfaces claiming one island get separate TILES, chosen by colouring the triangle conflict
  graph, with seam duplicates and a private index list - `clothDecalTiles`.

## Open, in priority order

1. **The UI.** Parked at the user's request. In-game HUD invisible, sub-menus missing. Start
   from `DrawHudFixedFunction` and `uiKeepPixelShader` (the pixel shader must stay).
2. The sky, lost to capture-off, and the frustum popping.
3. Other outfits. Every mechanism is general, but only this outfit is confirmed. A garment whose
   conflict graph needs more than 4 tiles, or a visible panel wider than 12 texture widths of
   copies, is refused (`refused: N` in `CLOTH DECAL TILES`) and falls back to one tile.
4. The one-point/dominant `pick` still goes through the old sum+desaturation function. It agrees
   with the chain on every pure-channel texel; if a garment ever shows a raw pattern colour on a
   flat region, switch it to the chain.

## How to work here

- **Look at the thing - and check the tool that shows it.** The DDS writer fabricated alpha 255
  for a day. Convert to PNG, view it, and know which channels the picture actually carries.
- **Measure the RESULT, not the process.** Print numbers, not verdicts; the log line that said
  "the game PINS the coordinate" sent a day of work after a clamp that was never active.
- **Read the refusal.** Every guard that refuses a fix says why; three did this session, and the
  reason was the next fix each time.
- Every change gets an ini switch. Back up the source as `sr3rtx.cpp.before-<what>` first.
- Build: `src/sr3-rtx/build.ps1`. Deploy: copy `build/sr3-rtx.asi` and `configs/sr3-rtx.ini`
  into `Saints Row 3/`, hash-verify both. The ASI is LOCKED while the game runs.
- Diagnostics land in the game dir as `sr3-remix-hard-N-*.dds` (inputs, coverage, uv1 space),
  `sr3-remix-gen-N-*.dds` (generated textures, tiles side by side), `sr3-remix-decal-N-conflicts.dds`.
  Move them to `dumps/<date>/` when done; `dumps/` is gitignored.
- Asset tools: `tools/vpp.py`, `tools/peg.py`, `tools/cloth_uv_table.py`, `tools/fxo_disasm.py`
  (`py`, not `python`). Output in `game-textures/` and `re/cloth-shaders/`.

## Deployed

    sr3-rtx.asi  9c0cfaf958174e3bcc0b91c81bf5be1e   CONFIRMED on screen 2026-09-10
    sr3-rtx.ini  08450a3d1f807cfb31dec4f7dd2f73a7
    rtx.conf     88a5ecf49316c397062343dae54ea594   rtx.useVertexCapture = False

    ffp=1  hiddenPassMode=2  screenSpaceMode=0  forceOcclusionVisible=1
    remixApi=1  remixApiCharacter=0  remixApiTestCube=0
    compositeFfp=1  clothUniformFromDiffuse=1  hairStrandsFromDob=1
    clothDominantPattern=1  clothPadUnused=1  clothCutout=1  clothMeshDecal=1
    clothDecalTiles=1  clothColourCurve=1
    uiDemoteUP=1  uiConvertUP=1  uiKeepPixelShader=1
    clothAlbedoPercent=200 (the x2 fallback when clothColourCurve=0)  generateCloth=1

Backups: `D:\SR3RTXREMIXCOMP-backup-2026-09-10` and earlier.

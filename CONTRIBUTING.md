# Contributing

Help is genuinely wanted. This is a large reverse-engineering job and it is close to working —
the path-traced world renders, is textured, animates and is lit. What is left is a short and
*specific* list, not an open-ended one.

## Read this first

**[docs/YOUR-INSTRUCTIONS.md](docs/YOUR-INSTRUCTIONS.md)** is a complete handoff document: current
state, the engine facts that have been measured and can be trusted, and — importantly — a list of
**dead ends not to retry**. Several of those cost a week each. Reading it will save you more time
than anything else in this repository.

Then:

- [docs/sr2-fork.md](docs/sr2-fork.md) — the design being implemented, and what is still to port.
- [docs/shader-map.md](docs/shader-map.md) — the constant registers and the shader corpus.
- [docs/worklog.md](docs/worklog.md) — the run-by-run history. Long, but every conclusion in the
  other documents is traceable to a run in here.
- [docs/asset-replacement-plan.md](docs/asset-replacement-plan.md) — the plan for the end goal.

## Good places to start

Roughly in order of value:

1. **Convert the sky.** ~50 draws a frame from the `rfg-skybox` family (`rfg-skybox_s`,
   `-clouds_s`, `-clouds-2_s`, `-matte_s`, `-overhead_s`, `-simple_s`, `-stars`, `-meteors`).
   They are currently passed through, and pass-through draws are skipped now that
   `rtx.useVertexCapture` is off — so there is no sky. Care is needed: the dome is 343 vertices,
   36×9×36 units, sitting **one unit from the camera**, and treating it as ordinary lit geometry
   is what produced a black sky once already.
2. **Convert the UI.** The HUD is shader-drawn and never converted, so it disappears entirely.
   `screenSpaceMode` in `configs/sr3-rtx.ini` documents what has been measured about the
   screen-space quad population and which of them are genuinely authored 2D content.
3. **Hair colour.** Neither hair texture carries it and every character colour constant measures
   (1,1,1). `Hair_Spec_Color1`/`Hair_Spec_Color2` are the remaining candidates and have never been
   captured, because the constant probe's twelve slots fill with other materials first.
4. **Player clothing colour.** The recipe is disassembled and written down in
   `docs/YOUR-INSTRUCTIONS.md`; it needs TEXCOORD1 reconciled against TEXCOORD0.
5. **Shim time growing across a session** — ~24% to ~44% of frame time, with worst-case stalls
   around 300 ms. It tracks skinned-geometry volume (211k → 354k skinned vertices/frame), so it
   may be nothing but a busier district, but the stalls are visible and this is the largest open
   problem after the sky and the HUD. Nothing has been optimised yet — functionality was the
   priority.
6. **Testing on other hardware and other copies of the game.** This has been developed against one
   GOG copy on one machine. Bug reports with `sr3-rtx.log` and `rtx-remix\logs\remix-dxvk.log`
   attached are useful even with no fix attached.

## How this project works, and what it expects of a change

These are not style preferences. Each one is here because ignoring it cost real time.

**Measure before you fix.** A "freeze" in this project was diagnosed and fixed twice before anyone
timed it — the game was running at 56 fps and had never stalled. If you cannot state the
measurement, you are guessing.

**A setting in `sr3-rtx.ini` is an unsettled hypothesis.** Once it is measured, the behaviour moves
into the code with the measurement recorded beside it and the switch is deleted. A switch nobody
varies is a switch nobody re-checks, and stale switches interacting is what made the pre-fork
build unmaintainable.

**Write the falsifier into the change.** Every behaviour in the shim reports a counter that would
read zero if it never fired. `forceOcclusionVisible` logs
`occlusion queries: N read back/frame, M answered VISIBLE/frame` specifically so that "the hook
never fired" is distinguishable from "the hook fired and did nothing". Counters going up is not
the same as the population being covered — check both ends.

**Read Remix's own binary before flipping a Remix option.** `<game>\.trex\d3d9.dll` registers every
option by name with its description as an adjacent string:

```powershell
cd tools/vibe-re
py -3 -m retools.search "<game>/.trex/d3d9.dll" strings -f <keyword>
```

This is the single most productive tool in the project. It found the texcoord format bug, the real
semantics of `rtx.ignoreTextures`, the terrain-baker requirement and the vertex-capture behaviour.
Every one of those had previously been guessed at, and most of the guesses were wrong.

**Read `<game>\rtx-remix\logs\remix-dxvk.log` on any "Remix does not show X" question.** It names
what it refused and why. It is what found the texcoord format bug after three *correct* shim-side
disproofs had already been produced.

**A rule that hides draws must be argued against the whole shader corpus first**, because hiding a
real surface makes it invisible and the symptom appears somewhere unrelated. Every hiding rule in
the shim was checked against all 7,276 shaders before it was written, and the safety argument in
each case is that the category which could be wrongly hidden is empty. `re/shader_constants.csv`
(regenerate it with `tools/vpp_extract.py` + `tools/fxo_scan.py`) is how you check.

**A sampler name identifies a family, not a file.** `Depth_bufferSampler` is the particle system,
`IR_GBuffer_DepthSampler` is what *water* uses, `Depth_mapSampler` is projectors. A rule written
against "samples any depth buffer" would have deleted every water shader in the game.

**Verify every deploy by file hash.** A running game silently locks the `.asi`, so a copy can fail
and leave you testing the previous build while believing you are testing the new one.

**Don't reach for `rtx.ignoreTextures` as a substitute for understanding a pass.** It is not a
hide — Remix renders an ignored material as a pink-and-black checkerboard. Believing otherwise
cost this project six runs.

## Practical notes

- Build with `powershell -File src\sr3-rtx\build.ps1` (MSVC Build Tools, **x86** toolchain). One
  translation unit, builds in seconds.
- Deploy with `tools\deploy-conf.ps1`; pull Remix-menu changes back with `tools\pull-conf.ps1`.
  **Never hand-edit the game's `rtx.conf` and then pull** — Remix writes that file when you save
  in its UI, and only hashes Remix wrote itself have the correct sign.
- `python` on Windows is often the Microsoft Store stub. Use `py -3`.
- The game must be closed before deploying a new `.asi`.

## Pull requests

- Say what you measured, not just what you changed. A log excerpt in the PR body is ideal.
- Small and separable beats large and bundled — this codebase has been burned by changes whose
  effects could not be attributed.
- If something you tried turned out to be wrong, say so in the PR and it will go into the dead-ends
  table. Negative results are worth as much as fixes here; several of the entries in
  `docs/YOUR-INSTRUCTIONS.md` are the most valuable things in the repository.

## Issues

Bug reports should include:

- `<game>\sr3-rtx.log`
- `<game>\rtx-remix\logs\remix-dxvk.log`
- Your GPU, driver version, Remix runtime version, and which copy of the game (Steam/GOG)
- A screenshot, if it is a visual problem

Those two log files together answer most questions.

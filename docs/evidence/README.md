# Evidence

Every conclusion in `docs/` is supposed to be traceable to a measurement, and this is where the
measurements live: run logs, frame dumps, and the A/B screenshots that proved the shim turns the
path tracer on.

The logs are the shim's own diagnostic output — draw dispositions, counters, shader and constant
*names*, texture hashes, timing breakdowns. They contain no game content.

## What is deliberately NOT in this repository

Two categories of file that exist in a local working copy are gitignored, because publishing them
would mean redistributing content that is not ours:

**`docs/evidence/cloth/` — dumped game textures.** Character diffuse, `Dob_Map`, normal, sphere
and blend maps, and the clothing pattern maps, plus the generator's output beside them. These are
Volition / Deep Silver art assets extracted from a copy of the game. They were how the NPC
clothing recipe was verified byte-exact against the shader, and the worklog entries for
2026-08-20 refer to them.

To regenerate them from your own copy of the game, set `charTexDump=1` in `sr3-rtx.ini`, run the
game, and the dumps appear next to the executable. `clothDump=1` does the same for the pattern
and result pairs.

**Crash minidumps (`*.dmp`).** A minidump is a memory snapshot of the running
`SaintsRowTheThird.exe` process, so it embeds portions of the game's own executable — and it can
carry local filesystem paths. `tools/read_minidump.py` reads them. If you need to send one for a
bug report, attach it to the issue rather than committing it.

## The A/B proof

The claim that this shim is what makes RTX Remix path-trace the game at all rests on a controlled
A/B: identical scene, identical settings, the only variable being whether the conversion runs.

It is reproducible on your own machine rather than taken on trust. Set `ffp=0` in `sr3-rtx.ini`,
which makes the shim a passive observer — it still loads, still logs, still injects lights, and
converts nothing. The world is then **rasterised**: no path tracing, and Remix's capture button
does nothing, because there is no ray-traced scene to capture. Set it back to `ffp=1` and the
path-traced world returns.

So the fixed-function conversion is not *contributing to* the ray-traced scene. It is the entire
scene.

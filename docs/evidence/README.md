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

## The A/B that was wrong, and why it is kept here

For weeks this project claimed the fixed-function conversion **was** the path-traced world, on the
strength of an A/B: set `ffp=0`, the shim converts nothing, and the world comes back rasterised
with Remix's capture button doing nothing.

The observation was real. The conclusion was wrong, and it was corrected on 2026-08-30.

**The A/B had no control.** The only code publishing a **camera** to Remix lived inside the
conversion path. So `ffp=0` did not isolate the conversion — it removed the camera at the same
time, and Remix cannot trace anything without one. Two variables moved; one was credited.

The control that settles it is `cameraOnly=1`: publish the camera, convert nothing. The world
path-traces. What Remix needed was the camera. The conversion is how the draw list gets filtered
down to something Remix can afford to build — with everything converted and nothing skipped, it
builds geometry for every draw and exhausts Vulkan memory at 33.5 GB.

This is left in the repository rather than quietly deleted, because the shape of the mistake is
more useful than the fact: **a switch that turns off two things at once cannot tell you which one
mattered.** See `docs/YOUR-INSTRUCTIONS.md` for the current state.

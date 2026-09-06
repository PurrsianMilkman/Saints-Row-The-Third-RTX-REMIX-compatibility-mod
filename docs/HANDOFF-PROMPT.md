# Resume prompt - SR3 RTX Remix

Continue the project at D:\SR3RTXREMIXCOMP.

## Read in this order
1. docs/YOUR-INSTRUCTIONS.md - the top section, "STATE, 2026-09-04". Everything current is there:
   the Remix API contract and its traps, the character pipeline, what is verified, what is open,
   and the routes that are CLOSED and must not be reopened.
2. docs/engine-map.md - the engine reverse engineering (command buffer, opcodes, RTTI pass tree).
3. docs/worklog.md - only if you need the history of a specific decision. It is 11,000 lines.

## Where the project is
Characters are built through Remix's PROGRAMMATIC API, not the fixed-function conversion. Ten
parts, ~22,000 triangles, correct geometry, placement, per-slot textures, customisation colours
and hair. The game renders untouched alongside; the API copy stands 3 units to one side so the two
cannot z-fight.

## The three open items
1. Clothes read too dark. The recipe is PROVEN correct against both the exe and the shader, so the
   fault is in how Remix lights these meshes. clothBrightness is a tuning knob, not a fix.
2. The character is frozen - skinning crashes Remix's server (remixApiSkinning=0).
3. The game's own copy is still drawn; removing it retires the diagnostic offset.

## How to work here
- Verify against the exe and the shader corpus before building. The cloth recipe was read from the
  wrong shader variant twice; there are 34 of them and their registers differ.
- Log what a thing IS by name, not what it looks like. Four hypotheses from screenshots lost to one
  arithmetic check on a printed slot table.
- Ship the falsifier with the change. A counter that only counts successes is not a budget.
- Do not fix wider than the fault. Applying a colour constant to every cloth slot blacked a top
  that was already correct.
- Every deployed file has a hash in the STATE section. Back up before editing; the backups are
  src/sr3-rtx/sr3rtx.cpp.before-<what>.

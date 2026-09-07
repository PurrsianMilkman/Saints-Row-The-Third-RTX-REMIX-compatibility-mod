# engine-control worklog

Session log for the STANDALONE engine-control plugin (`sr3-engine.asi`). The main project's log
stays in `docs/worklog.md` and is not written to from here.

## 2026-09-07 - hooking the ENGINE's renderer, not D3D9. Stage 0: the dispatch table, inert.

New direction, asked for directly: control the game renderer, simplify it for Remix, and read game
data to verify customisation. All three land on the same lever, and it is not at the D3D9 boundary.

### What the disassembly settled before anything was written

1. **The 74-entry opcode dispatch table at 0x013509F8 is in `.data`, which is WRITABLE**
   (VA 0x012E1000, size 0x022415FC, characteristics 0xC0000040). So is every runtime global in the
   submission map. Hooking the engine's own render-command dispatch is a matter of writing 74
   pointers - no trampolines, no code patching, no VirtualProtect strictly needed.
   Dumped from file offset 0xF4EDF8: 58 live handlers all inside .text, 16 copies of the stub
   0x004BF550, and the dwords after entry 73 are 0/2/0x28/1 - small integers, so the table ends
   exactly where the dispatcher's `cmp esi, 0x4a` says it does.

2. **The kill-switch path advances the read pointer by the SAME amount as the draw path.**
   op 42: draw path `0x49D692 add [0x2e5d650],0x1c`, kill path `0x49D6B5 add [0x2e5d650],0x1c`.
   That makes the drop primitive for stage 1 trivial and safe - set 0x03395EA4, call the original,
   clear it. The engine skips its own draw, advances its own pointer, sets its own skipped flag
   (`or [eax+4], 0x80`). No size arithmetic on our side, and it covers **op 43**, whose size the
   2026-08-30 static extraction could never resolve.

3. **CORRECTION: 0x00D9E8B0 is a string HASHER, not a material-parameter setter.** The docs had it
   as "sets Diffuse_Color_a/b/c through one call taking a name string". The call site was right;
   the function was not. It walks the string, lowercases each byte via 0xEA739D, and runs
   `crc = (crc>>8) ^ table[(crc^ch)&0xFF]`. All 256 entries at 0x1320DA0 are byte-identical to a
   generated CRC-32 table (poly 0xEDB88320), checked entry by entry. Result stored with no final
   inversion, seed 0.

       parameter id = crc32_lowercase(name), init 0, no final XOR
       Diffuse_Color_a = 0xA7D143AC   Pattern_Map = 0x03B65AF7   Tint_color = 0x8F88517A

   This does not overturn "parameters are bound by name" - that stands. It supplies the runtime
   KEY: the engine compares 32-bit hashes, not strings. Which is why `Pattern_Map` never appears as
   a string in the exe, and it is the mechanism goal 3 needs.

### Shipped: stage 0, deliberately inert

`engineHooks` (ini, default 0 in code, set to 1 for this run). Thunks over all 74 entries; each
increments its opcode's counter and calls the original. Nothing dropped, rewritten or reordered.

Handlers take NO arguments - each reads its command from [0x02E5D650] - and end in a plain `ret`,
so `void __cdecl` is an exact match. ESI is live across the call in the dispatcher and is
callee-saved, so a C function already preserves it. The opcode is a TEMPLATE PARAMETER so the
linker's identical-COMDAT folding cannot collapse the 74 thunks into one and lose the attribution;
verified in the map file - 74 symbols at 74 distinct addresses.

**It refuses to write unless the live table matches the shipped exe entry for entry.** Not
ceremony: this document records a static map wrong by 0x81C000 caught by exactly this check before
anything was patched, and the game folder contains SRTT.MixFix.x86.asi (currently `.disabled`) which
would be the realistic way for the table to already be owned.

### The falsifier

The ENGINE HOOKS block in the frame report prints the per-opcode dispatch census, named where the
identity is established and as a bare number where it is not. A hook that installed but never runs
and a hook that never installed look identical from outside the process; only this separates them:

    "ZERO commands dispatched" -> the table was written but our thunks never ran. NOT live, and
                                  nothing else in the block is evidence.
    "not installed"            -> a check refused, and the line above says which.

It also counts dispatches of the 16 stub opcodes. The stub is a bare `ret` that never advances the
read pointer, so the engine emitting one would hang the render thread. Expected 0; non-zero would
be a real finding meaning the opcode map is incomplete.

### Verified before asking for a run
- the 74 entries compiled into the shim are byte-identical to a fresh dump of the exe.
- 74 thunk symbols at 74 distinct addresses in sr3-rtx.map - no COMDAT folding.
- build clean at /W3.

### Deployed
    sr3-rtx.asi  dc558f33513ac83ee540370440184068  (407040 bytes)
    sr3-rtx.ini  fa53f6c6d92d938905ee34e3dcaf0c75  (engineHooks=1)
    rtx.conf     88a5ecf49316c397062343dae54ea594
    bridge.conf  db55f8142db1db21eb4bdf256f5c4ae5  (exposeRemixApi = True)
Source backup: src/sr3-rtx/sr3rtx.cpp.before-engine-hooks
Ini backup:    configs/sr3-rtx.ini.before-engine-hooks.bak

### Next, if the census reads non-zero
Stage 1 the drop primitive; stage 2 pass-level filtering (segment blocks by op 9, and never touch a
block containing ops 55/56/72 - the readback rule, stated by the engine rather than inferred);
stage 3 the crc32 name dictionary and the hunt for the runtime table keyed by those hashes.

## 2026-09-07 - moved OUT of the main shim into a standalone plugin

The engine-hook code was first built inside `src/sr3-rtx/sr3rtx.cpp` and proved there (the run above).
It has now been moved to a standalone `sr3-engine.asi` so the main RTX Remix project can be worked on
without interference.

What was done to the main project, so it is exactly as it was:
  - `src/sr3-rtx/sr3rtx.cpp` restored byte-exact from `sr3rtx.cpp.before-engine-hooks` (verified
    with `diff`), rebuilt to 399,872 bytes - the same size as the pre-existing Sep 6 build - and
    redeployed.
  - `configs/sr3-rtx.ini` restored from `sr3-rtx.ini.before-engine-hooks.bak`; the `engineHooks`
    key is gone from it entirely.
  - `docs/worklog.md` restored; this log lives here instead.
  - `docs/engine-map.md` KEPT, deliberately. Everything added there is a fact about the exe, which
    is what that document is for, and two of its entries were wrong (op 37/40).

Not deployed. `sr3-engine.asi` is built but deliberately NOT copied into the game folder, so the
next run of the main project is unaffected. `engine-control\deploy.ps1` puts it there when wanted
and `-Remove` takes it back out; it touches only `sr3-engine.*`.

The main shim's `ScanCommandBlock()` still counts op 37 as a draw, so its BLOCK SCAN draw figure is
slightly overstated. Left for the main project to fix when convenient - nothing here depends on it.

## 2026-09-07 - stage 1: the drop primitive, through the engine's own kill-switch

### The primitive
To suppress a draw the plugin does NOT skip the handler and advance the read pointer itself:

    set 0x03395EA4  ->  call the original handler  ->  restore 0x03395EA4

Both paths through a draw handler advance the read pointer by the same amount (op 42: draw path
0x49D692, kill path 0x49D6B5, both `add [0x2e5d650], 0x1c`). So the ENGINE does the size arithmetic
and desynchronising the block is not possible from here. It also covers op 43, whose size was never
resolved statically, and leaves the engine's own skipped-draw flag (`or [eax+4], 0x80`) consistent -
because this is the exact path SR3 takes when its own culling hides an object.

The switch is restored to its PREVIOUS value, not to zero: the engine may have set it itself, and
clearing it would override the engine's own culling decision.

### The guard, and why it is not optional
The kill path dereferences [0x351F8F8] -> +0x50 -> +4 and WRITES there. A null or uninitialised
root would turn a working frame into a fault inside the engine. So the chain is validated as
committed AND writable at every level before the switch is armed, cached on the root pointer's
value (one load and a compare in the steady state) and re-validated whenever it changes. A draw
that fails validation is passed through untouched and counted as a refusal.

### The falsifier
`DROPS: mode=N (...) | X draws seen, Y dropped (Z%), R refused`. A black image cannot by itself
distinguish "the drop worked" from "the game stopped"; those three numbers can. For dropMode=1 the
reported percentage is checked against the expected 100/dropOneInN, which is arithmetic rather than
self-assessment. An explicit line fires if dropMode is on and nothing was dropped.

### Also added: the pass census, which is stage 2's input
A pass boundary is exactly a SetRenderTarget command (op 9). Its layout, read from the handler at
0x0049CB00: [opcode][index +4][surface +8], size 0x0C, dispatched to vtable+0x94 = slot 37. So the
render target a draw belongs to is READ FROM THE COMMAND rather than inferred from formats or
sampler names - which is what every pass-classification rule in the main shim has been
approximating. Draws are attributed per surface and reported most-drawn first.

`if constexpr` keeps all of this out of the 70 non-draw thunks entirely; only ops 40-43 carry the
drop path and only op 9 carries the pass bind.

### Deployed for the test
    sr3-engine.asi  5e6952782e1499f3616542755af8573e  (138240 bytes)
    sr3-engine.ini  a6c9754b5a3813267b63ebc4b49fab69  (dropMode=1, dropOneInN=2, passCensus=1)
    main shim untouched: sr3-rtx.asi 047eb783f94ffea0828dc45f2068b70f
Source backup: engine-control/src/sr3engine.cpp.before-stage1

## 2026-09-07 - stage 1 PROVEN, and the pass census was measuring the wrong thing

### The drop primitive works, exactly

    DROPS: mode=1 (one in N) | 5831974 draws seen, 2915987 dropped (50.0%), 0 refused

2,915,987 x 2 = 5,831,974. Exactly half, against an expected 100/dropOneInN = 50.0% - arithmetic,
not a self-assessment. **0 refused**, so the kill chain [0x351F8F8] -> +0x50 -> +4 validated on
every single attempt. No crash, no hang, no new dump, across 42,495,737 dispatched commands.

The read pointer never desynchronised. That is the result routing the drop through the engine's own
kill path was designed to guarantee, and it is now measured rather than argued.

### The census also confirmed, again, that op 40 is a draw
op 42 DrawIndexedPrimitive 5,366,272 and op 40 DrawPrimitive 295,543 - together 5,661,815, close to
the 5,831,974 draws seen (the remainder is op 41 DrawPrimitiveUP, visible in the early censuses at
2,661 before the world loaded). Op 37 never appears among the draws, as expected now that it is
known to be a vertex-texture bind.

### CORRECTION - the pass census keyed on the wrong thing
It keyed on the SURFACE alone and ignored the render-target INDEX. On an MRT engine that is wrong
twice over:

  - every MRT slot collapsed into one population. `surface 0x00000000` accumulated **51,893 binds**
    and 216,064 attributed draws - but that is not a pass at all, it is
    `SetRenderTarget(index>0, NULL)` unbinding the extra slots of the previous MRT set.
  - the "current pass" became whichever surface was bound LAST, so a sequence of
    slot0=A, slot1=B, draw attributed the draw to B - an auxiliary target - instead of to A.

So the first pass table is not trustworthy and is not being carried forward as evidence. Confirmed
from the handler at 0x0049CB00 that args are pushed right-to-left for SetRenderTarget(index,
surface), hence [cmd+4] = INDEX and [cmd+8] = SURFACE.

Fixed: rows are keyed on (index, surface); only a bind at INDEX 0 - the colour target - changes
which pass draws belong to; NULL binds are counted separately as slot unbinds; the table is 128
rows rather than 48, which was overflowing ("TABLE FULL - more exist").

This is the same class of fault as the op 37 mislabel: a plausible-looking table that was actually
measuring something else. Caught because the NULL surface with 51,893 binds and 216,064 draws made
no physical sense.

### Deployed for the next run
    sr3-engine.ini  dropMode=0 (the primitive is proven; a correct pass census wants a normal frame)
Source backup: engine-control/src/sr3engine.cpp.before-mrt-fix

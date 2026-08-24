# Vibe-RE toolkit, integrated 2026-08-19

Cloned to `tools/vibe-re/` from https://github.com/Ekozmaster/Vibe-Reverse-Engineering (verified:
`py -3 tools/vibe-re/verify_install.py` reports ALL REQUIRED CHECKS PASSED - radare2 6.1.0 bundled,
21 retools modules importing clean, frida 17.17, pefile, capstone, pyghidra. Ghidra itself and the
signature DB are the two optional pieces not installed).

Run everything from `tools/vibe-re/` with `py -3` - the `python` alias on this machine is the
Microsoft Store stub and fails.

## Why this matters to THIS project

The repository contains a skill, `dx9-ffp-port`, describing the exact task this project is: porting
a shader-driven DX9 game to fixed function for RTX Remix. Two of its documented pitfalls name bugs
we currently have open, which is worth more than any single tool:

- *"Bones mixed up between NPCs: stale WORLDMATRIX slots from a previous object. The game may need
  a game-specific reset hook at a per-object boundary."*
- *"Everything is white/black: albedo texture is on stage 1+, not stage 0."* (we already rank by
  CTAB sampler name, which is a stronger version of the same idea)

It also documents the #1 Remix porting mistake as a game uploading a pre-multiplied WorldViewProj -
which SR3 does NOT do, and this project settled independently: `projTM` is V*P at c28, `objTM` is
the world at c32, and `IR_World2View` is handed to us at c48.

## The four tool groups

| group | what it does | our use |
|---|---|---|
| `retools/` | static analysis of a PE: disassembly, decompilation, xrefs, call graphs, vtables, byte patterns, cached into SQLite via `index.py` / `query.py` | finding engine functions we currently only reach through D3D9 hooks - e.g. the per-object boundary the skinning-stability procedure needs |
| `livetools/` | Frida: breakpoints, register/memory reads, function tracing, stepping. Plus `gamectl.py`, which drives the game window with real `SendInput` | confirming a candidate function is called once per skinned object; `gamectl` can reproduce a symptom without a manual run |
| `graphics/directx/dx9/tracer/` | captures EVERY `IDirect3DDevice9` call for N frames with arguments, backtraces, shader bytecode and matrices | far beyond our own frame dump, which records one line per draw and only the fields we thought to add |
| `rtx_remix_tools/dx/remix-comp-proxy/` | the FFP proxy template our SR2 fork is descended from | a reference implementation to compare our decisions against |

## The constraint as first understood (superseded - see below)

**The DX9 tracer ships its own `d3d9.dll` proxy (`graphics/directx/dx9/tracer/bin/d3d9.dll`), and
Remix IS a `d3d9.dll`.** They cannot both sit in the game directory. So a tracer capture shows what
the GAME submits, with Remix absent - which is exactly right for questions about the engine ("what
does it upload before a skinned draw?") and useless for questions about Remix ("what does Remix do
with what we send?").

Our own shim is an ASI hooking the device vtable, so it is unaffected either way.

## Commands worth knowing

```bash
cd tools/vibe-re

# static
py -3 -m retools.index status                     # what is cached for this game
py -3 -m retools.query <...>                      # query cached facts, no re-scan
py -3 retools/ghidra_server.py                    # warm Ghidra, sub-second decompiles (optional)

# dynamic
py -3 -m livetools gamectl --exe SaintsRowTheThird.exe info
py -3 -m livetools gamectl --exe SaintsRowTheThird.exe keys "W W SPACE"
py -3 -m livetools trace <addr> --count 50

# dx9 tracer  (NOT usable on SR3 - see the applicability section at the end)
py -3 -m graphics.directx.dx9.tracer trigger --game-dir "<game>" --frames 2 --delay 5
py -3 -m graphics.directx.dx9.tracer analyze <file.jsonl> --hotpaths --resolve-addrs <game.exe>
py -3 -m graphics.directx.dx9.tracer analyze <file.jsonl> --callers SetVertexShaderConstantF
```

## The procedure this WOULD unlock (blocked - see above)

`SKILL.md` gives a concrete method for the class of bug we keep hitting - state leaking between
objects - and it is the first approach we have that does not depend on guessing:

1. capture 2+ frames with multiple skinned NPCs on screen;
2. `--hotpaths --resolve-addrs` to find the callers of bone-range `SetVertexShaderConstantF`;
3. `--callers SetVertexShaderConstantF`: the function appearing N times per frame, where N is the
   number of skinned objects, is the per-object boundary;
4. `livetools trace <addr> --count 50` to confirm the hit rate matches the NPC count;
5. `callgraph.py --up` + `decompiler.py` to confirm it loops over objects.

That is directly applicable to the double-draw and to anything else where we cannot tell one
character from another at the D3D9 boundary - which has now cost four dedup attempts.

## What actually applies to SR3 - measured 2026-08-19

The optimistic table above was written before any of it had been run against this game. Two of the
four groups do not apply, for reasons specific to SR3. Check this section before reaching for a
tool.

### The DX9 tracer cannot be deployed at all

This is worse than the "swaps out Remix's d3d9.dll" constraint originally recorded here. SR3
**statically imports five symbols** from `d3d9.dll`:

    D3DPERF_GetStatus, D3DPERF_EndEvent, D3DPERF_BeginEvent, D3DPERF_SetOptions, Direct3DCreate9

and resolves a sixth, `Direct3DCreate9Ex`, by name at runtime (both strings sit at `.rdata`
0x012A0110). The tracer's `src/d3d9.def` exports exactly one symbol, `Direct3DCreate9`. Dropping
it into the game directory fails at **load time on IAT resolution** - the game does not start.
Past that, SR3 prefers the Ex entry point, which the tracer does not wrap either.

Making it usable means adding the four `D3DPERF_*` forwards plus `Direct3DCreate9Ex` and full
`IDirect3D9Ex` / `IDirect3DDevice9Ex` wrappers to shared tooling. That is a project, not a
deployment step. **The "procedure this WOULD unlock" section above is therefore blocked at step 1.**

### The static D3D9 scanners find nothing, and that is not a bug in them

`find_skinning.py` on `SaintsRowTheThird.exe` reports no skinned declarations, no bone palettes,
no vertex-blend states, and suggests `[Skinning] Enabled=0`. SR3 has 221 skinned shader files and
this shim skins 69 draws a frame.

The scanners look for literal register numbers and declaration blobs compiled into the binary.
SR3 is **data-driven**: vertex declarations and constant register assignments come out of the
`.fxo_pc` shaders and packfile format tables, not out of immediates in the exe. `find_d3d_calls.py`
likewise found the IAT but **zero call sites** for `Direct3DCreate9` - the engine dispatches
indirectly.

**`re/shader_constants.csv` is this project's replacement for that whole group and is strictly
better**: 52,991 rows of real constant and sampler bindings parsed from the shaders themselves,
carrying the register numbers the scanners failed to find. Reach for it first.

### What DOES work

- **`retools.search strings` on any binary** - including Remix's own 190 MB `.trex/d3d9.dll`.
  Remix registers every option by name with its documentation as an adjacent string, so this is a
  searchable manual for the runtime. It is how we found that terrain baking requires
  `rtx.terrainBaker.material.replacementSupportInPS_fixedFunction = True` for fixed-function draws
  - the finding that cleaned up `rtx.conf`. Use it for any "what does this Remix option actually
  do?" question instead of guessing.
- **`livetools` / `gamectl`** - untested here so far, but nothing about SR3 blocks it.
- **The `dx9-ffp-port` skill as a written reference** - its pitfall list stays useful even with
  its tooling unavailable.

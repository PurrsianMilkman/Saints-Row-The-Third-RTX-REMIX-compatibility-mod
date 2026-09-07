# engine-control — SR3 engine renderer control

A **standalone** plugin, `sr3-engine.asi`, kept deliberately apart from the main RTX Remix shim so
that work on each can proceed without touching the other.

| | main project | this folder |
|---|---|---|
| binary | `sr3-rtx.asi` | `sr3-engine.asi` |
| hooks | D3D9 — the device vtable | the **engine's** render-command dispatch table at `0x013509F8` |
| source | `src/sr3-rtx/sr3rtx.cpp` | `engine-control/src/sr3engine.cpp` |
| build | `src/sr3-rtx/build.ps1` → `build/` | `engine-control/src/build.ps1` → `engine-control/build/` |
| config | `sr3-rtx.ini` | `sr3-engine.ini` |
| log | `sr3-rtx.log` | `sr3-engine.log` |

They touch different tables and share no state. Either can be deleted from the game folder without
affecting the other, and `dinput8.dll` (the ASI loader) loads both.

## Why hook the engine rather than D3D9

The main shim watches SR3 from outside: a flattened stream of D3D9 calls arrives, and engine intent
has to be reconstructed from samplers, render-target formats and shader reflection. Three separate
rules have blacked out the world by guessing wrong about which draws the engine reads back.

But SR3 is a command-buffer renderer. A producer thread writes command blocks into a ring; a render
thread at `0x0049DE20` consumes them and dispatches each command through a 74-entry function-pointer
table. That table is in **writable `.data`**, so hooking the engine's renderer is a matter of
writing 74 pointers — and our code then runs *inside* the engine, one level above D3D9, where a
command is still a command rather than six loose arguments.

## Use

```powershell
.\deploy.ps1            # build, deploy to the game folder, hash-verify
.\deploy.ps1 -Remove    # take it back out
```

Then run the game and read `Saints Row 3\sr3-engine.log`.

`engineHooks=0` in `sr3-engine.ini` makes the plugin completely inert without removing it.

## State

**Stage 0 — the hook, inert. Verified live 2026-09-07** (while the code still lived inside the main
shim; it has since been moved here and the main shim restored to its original state):

- `74/74` dispatch entries hooked, **123,307,264 commands dispatched over 4,800 frames**, no crash.
- **0 stub dispatches**, confirming the bare-`ret` reading — the engine cannot emit those 16 opcodes.
- op 42 dispatches (15,831,840) matched the D3D9 draws the main shim attributed (15,831,620) — two
  independent measurement paths agreeing.

Two results that de-risk pass filtering:

- **Zero `GetRenderTargetData` in 108 million commands** across 163,456 blocks. Rule 1 — "a block
  whose result the engine reads back may never be skipped" — does not bind anywhere in gameplay.
- **Zero walks stopped early**, so ops 7, 43, 56, 60 and 72 never occur; the size table is complete.

## Corrections this work produced

`docs/engine-map.md` had **op 37 as `DrawPrimitive`**. Read from the handlers:

| op | handler | what it actually is |
|---|---|---|
| 20, 21 | `0x0049D040`, `0x0049D0C0` | engine state shadow writes — **no D3D9 call at all** |
| **37** | `0x0049D510` | `SetTexture`, stage `[cmd+4] + 0x101` = `D3DVERTEXTEXTURESAMPLER0` — a **vertex** texture bind, not a draw |
| **40** | `0x0049D560` | **`DrawPrimitive`** (vtable+0x144), and it tests the kill-switch |
| 52, 53, 54 | `0x0049D810/830/850` | `Query::Issue(BEGIN)` / `Issue(END)` / `GetData` — 557/frame each |

**So the draw opcodes are 40, 41, 42, 43** — consecutive handlers, exactly the four that test the
kill-switch at `0x03395EA4`. Treating 37 as a draw would suppress a vertex-texture bind while
removing no geometry.

> The main shim's `ScanCommandBlock()` still counts op 37 as a draw, so its BLOCK SCAN draw figure
> is slightly overstated. That is a one-line fix in `sr3rtx.cpp` and is left for the main project to
> apply when convenient — nothing here depends on it.

## Next

- **Stage 1 — command control.** Drop a draw by setting `0x03395EA4`, calling the original handler,
  and clearing it. Both the draw path (`0x49D692`) and the kill path (`0x49D6B5`) advance the read
  pointer by the same `0x1C`, so the engine does its own size arithmetic and there is no chance of
  desynchronising the block. The kill path dereferences `[0x351f8f8] → +0x50 → +4`, so that chain
  must be non-null before the switch is armed.
- **Stage 2 — pass-level filtering.** Segment blocks by op 9 (`SetRenderTarget`), ~60 passes/frame,
  and drop whole passes Remix does not need.
- **Stage 3 — read game data by name.** A parameter's runtime id is `crc32_lowercase(name)`, init 0,
  no final XOR (hasher `0xD9E8B0`, CRC-32 table `0x1320DA0`, `tolower` at `0xEA739D`).
  `Diffuse_Color_a` = `0xA7D143AC`, `Pattern_Map` = `0x03B65AF7`.

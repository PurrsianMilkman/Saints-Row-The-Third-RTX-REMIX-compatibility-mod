# SR3 engine map — reverse-engineering `SaintsRowTheThird.exe`

> **STALE IN PART — read `YOUR-INSTRUCTIONS.md` first.** The `mode=1` runtime binary-patching
> plan this document was written to support was **deleted** in the SR2-proxy fork; the shim no
> longer patches the exe and the runtime verifier is gone. The static analysis itself — the D3D9
> wrapper layer, the call sites, the image-base correction, and the finding that the
> fixed-function renderers never execute — is still accurate and was verified live 12/12.

*2026-08-15. Produced by `tools/pe_analyze.py` and `tools/find_render_calls.py`.
Target: the DX9 exe, 16,257,024 bytes, x86, image base `0x400000`.*

**Headline: the engine funnels every D3D9 call through one thin wrapper layer at
`0x49c9xx–0x49d7xx`, and it has at least two additional renderers that use fixed-function
state.** That wrapper layer is the chokepoint for `mode=1` (engine patching): one hook per API
call, inside the engine, where engine-side context is still available.

## Method, and why the obvious approaches fail

| Approach | Result |
|---|---|
| Import table | **Dead end.** The exe imports only 5 D3D9 functions: `Direct3DCreate9` and four `D3DPERF_*`. Every device method goes through a COM vtable and appears nowhere in the IAT. |
| Byte-pattern scan for `FF 15` / `FF 90+disp32` | **Worthless.** x86 has no fixed instruction boundaries, so a regex over 12.7 MB of `.text` invents hits — it found 162 "calls" whose displacements were random bytes like `0x5de58b00` — and misses real ones. A null result from this method proves nothing. |
| Capstone linear sweep, direct calls only | Found **3** sites. MSVC rarely emits `call dword ptr [reg+off]` here. |
| Capstone + `mov reg,[obj+off]` | 9,444 hits, mostly **false positives** — a vtable offset is just an integer and collides constantly with ordinary struct field offsets. Leads only. |
| **Capstone + load→call correlation** | **344 confirmed sites.** Requires the loaded register to actually be `call`ed within 12 instructions, invalidated if anything overwrites it. This is the list to patch against. |

Dispatch is two-step (`mov reg,[obj+off]` … `call reg`), which is why the single-instruction
patterns find almost nothing. `tools/find_render_calls.py` runs all three passes and labels
which is trustworthy.

## 1. The D3D9 wrapper layer — `0x49c9xx–0x49d7xx`

Roughly 2 KB holding one thin forwarding function per API call. Addresses are the confirmed
call sites *inside* each wrapper:

| Address | Method | | Address | Method |
|---|---|---|---|---|
| `0x49c9fe` | BeginScene | | `0x49d398` | SetVertexShader |
| `0x49ca1e` | EndScene | | `0x49d3c8` | SetPixelShader |
| `0x49cb1d` | SetRenderTarget | | `0x49d3f8` | SetVertexDeclaration |
| `0x49cb51` | SetDepthStencilSurface | | `0x49d428` | SetIndices |
| `0x49cbe9` | Clear | | `0x49d46b` | SetStreamSource |
| `0x49cffd` | SetRenderState | | `0x49d4f0`, `0x49d533` | SetTexture |
| `0x49d1e5`, `0x49d231` | SetVertexShaderConstantF | | `0x49d594` | DrawPrimitive |
| `0x49d2c5`, `0x49d311` | SetPixelShaderConstantF | | `0x49d608` | DrawPrimitiveUP |
| | | | `0x49d690` | DrawIndexedPrimitive |
| | | | `0x49d720` | DrawIndexedPrimitiveUP |

The four draw entry points sit within `0x49d594..0x49d720` — a single ~400-byte region. That is
the engine's submission chokepoint.

## 2. A second renderer using FIXED-FUNCTION state — `0xe27000–0xe33000`

| Address | Method |
|---|---|
| `0xe27f7d` | BeginScene |
| `0xe27fc1`, `0xe313e0`, `0xe314a3`, `0xe31e42` | **SetMaterial** |
| `0xe30f4d` | **LightEnable** |
| `0xe3131d` | **SetLight** |
| `0xe32bed` | SetTextureStageState |
| `0xe325fd` | SetTexture |
| `0xe3270d` | SetVertexDeclaration |
| `0xe32e5d` / `0xe32f1d` / `0xe32f8d` / `0xe32fed` | DrawPrimitive / UP / Indexed / IndexedUP |
| `0xe3304d` | SetTransform |

`SetMaterial` + `SetLight` + `LightEnable` + `SetTransform` + `SetTextureStageState` is the
classic fixed-function pipeline. A third, similar cluster exists at `0xf5e5c6–0xf5ec8d`
(SetLight, LightEnable, SetTextureStageState, SetTexture).

**Measured 2026-08-15 — these renderers never run.** Every draw was attributed to its caller by
reading the return address inside the D3D9 draw hooks (no patching needed: the engine's
`call eax` sites go through the device vtable we already hook). Standing in a busy street with
pedestrians and traffic:

```
draws from wrapper 0x49cxxx   indexed=1567.8  prim=48.0  indexedUP=0.0  UP=57.1 /frame
draws from ff-renderer        (no lines - zero draws)
draws from ff-cluster         (no lines - zero draws)
```

**100% of rendering comes from the wrapper region.** The fixed-function code paths exist in the
binary but are dead in gameplay - legacy, editor or an unused video path. So `research.md`'s
"SR3 has no fixed-function fallback path" is correct in practice, and there is no shortcut
whereby Remix receives natively-consumable geometry. Everything must go through the wrapper.

## 3. The engine issues ~57 user-pointer draws per frame

Measured: `DrawPrimitiveUP` **57.1/frame**, `DrawIndexedPrimitiveUP` **0**. Exclusively the
non-indexed variant, all from the wrapper region.

This matters twice over. UP draws pass geometry as a plain memory array, so **the vertex data is
readable at the call with no buffer to lock** - the easiest possible geometry to inspect. And it
is the exact submission form the re-submission project must imitate, already working in-engine.

57/frame is a small, distinct population and a strong suspect for the HUD, the flickering planes
and the white plane welded to the camera: screen-space quads are what a renderer submits this
way. Positions near -1..1 would confirm clip space.

## Open questions

1. ~~ASLR~~ — **resolved.** `DllCharacteristics` = `0x8000`: no `DYNAMIC_BASE`. Characteristics
   `0x0123` sets `RELOCS_STRIPPED`, so the image *cannot* be relocated. It always loads at
   `0x400000` and every address here is valid verbatim at runtime — confirmed live.
2. ~~What the fixed-function renderers draw~~ — **answered 2026-08-15: nothing.** See below.
3. Whether the wrapper functions are `__stdcall`/`__fastcall` and their parameter layout, needed
   before hooking them.
4. The UV dequantization scale for `short2` texture coordinates (see `worklog.md`);
   `Object_instance_params_2` (c36) is the current suspect.

## Correction, 2026-08-15 — the first address table was wrong by `0x81C000`

The initial pass reported the image base as `0xc1c000` and every address was inflated by
`0x81C000`. Cause: for **PE32** the `ImageBase` field is at optional-header offset **28**;
offset **24** is `BaseOfData`. The tools read offset 24. The tell was visible in the output and
missed — `.rdata` had RVA `0xc1c000`, identical to the reported "image base".

It was caught by the runtime verifier on its first run, before anything was patched:

```
engine map: module base 0x00400000 (expected 0x00c1c000)
engine map: BASE MISMATCH - relocated despite stripped relocs. Not patching.
```

Both tools are fixed. The **bytes** captured for each site were always correct — file offsets
derive from section raw addresses and never depended on the base — so only the addresses moved.

Worth keeping as a rule: verify a static map against the live process before writing to it. The
check cost a few lines and caught an error that would have written a hook into arbitrary code.

---

# ADDENDUM 2026-08-29 — SR3 is a COMMAND-BUFFER renderer, and the engine has its own draw kill-switch

*Produced with radare2 6.1.0 (`tools/vibe-re/tools/radare2-6.1.0-w64`) + pefile against
`SaintsRowTheThird.exe`. No runs, no patching — static only.*

## The wrapper functions have ZERO direct callers

Scanning every `E8 rel32` in `.text` (12.7 MB) for calls to the DrawIndexedPrimitive wrapper at
**`0x0049D650`** returns **0 hits**. The wrapper is never called directly. It is reached through a
function-pointer table, and it reads its arguments out of a struct rather than off the stack:

```asm
0x0049d650  push esi
0x0049d651  mov  esi, [0x2e5d650]        ; COMMAND-BUFFER READ POINTER
0x0049d657  push 0x3393c30
0x0049d65c  call 0x4ad930
0x0049d661  cmp  byte [0x3395ea4], 0     ; <-- the engine's own DRAW KILL-SWITCH
0x0049d668  jne  0x49d6a5                ;     set => skip the draw entirely
0x0049d66a  ...  args from [esi+4] .. [esi+0x18]
0x0049d68a  mov  eax, [ecx + 0x148]      ; vtable slot 82 = DrawIndexedPrimitive
0x0049d690  call eax
0x0049d692  add  dword [0x2e5d650], 0x1c ; commands are 28 BYTES
```

| global | meaning |
|---|---|
| `0x02E5D650` | command-buffer read pointer, advanced by the command's own size (0x1C for a draw) |
| `0x03171B68` | the `IDirect3DDevice9*` |
| `0x03395EA4` | **byte. Non-zero suppresses the draw** |

## The render command set — 74 opcodes, dispatch table at `0x013509F8`

Each entry is a handler; 16 of the 74 are the unimplemented stub `0x004BF550`. Identified by the
device method the handler actually dispatches (`mov eax,[ecx+off]` immediately followed by
`call eax` — the loose "any `[reg+off]`" pattern is worthless here, exactly as this document
already warns):

| op | handler | command | | op | handler | command |
|---|---|---|---|---|---|---|
| 10 | `0x0049CB30` | SetDepthStencilSurface | | 33 | `0x0049D3E0` | SetVertexDeclaration |
| 11 | `0x0049CB60` | Clear | | 34 | `0x0049D410` | SetIndices |
| 12 | `0x0049CC00` | Clear | | 35 | `0x0049D440` | SetStreamSource |
| 14 | `0x0049CD10` | SetViewport | | 36 | `0x0049D4A0` | SetTexture |
| 16 | `0x0049CDF0` | SetViewport | | 37 | `0x0049D510` | **DrawPrimitive** |
| 17 | `0x0049CF90` | SetScissorRect | | 41 | `0x0049D5D0` | **DrawPrimitiveUP** |
| 19 | `0x0049D010` | SetSamplerState | | 42 | `0x0049D650` | **DrawIndexedPrimitive** |
| 24,25 | `0x0049D1C0/210` | SetVertexShaderConstantF | | 43 | `0x0049D6C0` | **DrawIndexedPrimitiveUP** |
| 26 | `0x0049D250` | SetVertexShaderConstantB | | 50 | `0x0049D760` | GetBackBuffer |
| 27,28 | `0x0049D2A0/2F0` | SetPixelShaderConstantF | | 51 | `0x0049D7C0` | StretchRect |
| 29 | `0x0049D330` | SetPixelShaderConstantB | | 55,56 | `0x0049D890/950` | **GetRenderTargetData** |
| 31 | `0x0049D380` | SetVertexShader | | 57 | `0x0049DA20` | SetRenderState |
| 32 | `0x0049D3B0` | SetPixelShader | | 58 | `0x0049DA60` | SetGammaRamp |

`SetRenderTarget` is op 9 (`0x0049CB00`, from the original table above).

**Note ops 55/56/72 — the engine issues `GetRenderTargetData` as a render command.** That is a
GPU->CPU readback in the command stream, and it is the mechanism behind the character-texture bake.

## The kill-switch, and why forceOcclusionVisible is load-bearing

**All FOUR draw handlers test the same byte** before submitting — `0x49D573`, `0x49D5E3`,
`0x49D663`, `0x49D6D3`. Two sites write it:

```asm
0x004ad598  mov byte [0x3395ea4], bl     ; render-state reset, clears it with a block of globals
...
0x0047ceb6  test bl, bl
0x0047ceb8  sete cl
0x0047cebb  mov byte [0x3395ea4], cl     ; SET when a visibility test returned FALSE
```

`bl` there is 1 only if the call at `0x0047C800` returned non-zero. That function reads
thread-local state (`fs:[0x2c]`), walks a context at `[esi+0x674]`, and does floating-point work
over a 0x5D4-byte frame before returning a bool — a visibility determination.

So, at instruction level: **SR3 decides per-object whether it is visible, and if not it sets one
global byte that makes every draw command a no-op.** That is the engine's own culling, and it is
exactly the behaviour recorded in YOUR-INSTRUCTIONS.md:

> "SR3 does its own GPU occlusion culling and reads the depth prepass back ... Capture-off IS a
>  global skip."   capture ON 5330 draws/frame -> capture OFF 1426 draws/frame.

and the user's statement that everything in the frustum gets culled because the game believes the
camera is occluded. `forceOcclusionVisible=1` answers the occlusion query so this test passes and
the byte stays clear. **It is not a workaround for capture-off; it is what keeps the engine's own
kill-switch off.** Removing it on 2026-08-29 was wrong and was reverted.

## What this changes

1. The engine's passes are **commands in a buffer**, not call-stack contexts. Attributing a draw to
   a pass by return address cannot work past the dispatcher; the pass identity lives in the
   command stream.
2. There is a single, engine-owned place where drawing is suppressed. Anything the shim does to
   suppress draws is a SECOND mechanism layered on one the engine already has.
3. `GetRenderTargetData` is a first-class render command, which is consistent with the character
   bake performing a readback and with the observation that reading a Remix-owned render target
   hangs.

## The dispatcher, found 2026-08-30 — a render THREAD consuming a command RING

One site in `.text` references the dispatch table base: **`0x0049DEDC`**. It sits inside a
consumer loop at `0x0049DE20`.

### Outer loop: take the next command block off a ring

```asm
0x0049de20  mov  ecx, 0x339385c
            call 0xfc0c80              ; wait on a sync object -> this is a CONSUMER THREAD
0x0049de35  cmp  word [0x339389e], 0   ; ring entry count
            jbe  empty
0x0049de3f  mov  eax, [0x339389c]      ; ring READ INDEX
            mov  ecx, [0x33938a0]      ; ring BASE
            and  eax, 0xffff
            lea  eax, [eax + eax*2]    ; *3
            lea  esi, [ecx + eax*8]    ; base + index*24  -> 24-byte descriptor
0x0049de59  mov  eax, [esi]            ; descriptor+0  = command block pointer
            mov  edx, [esi + 4]        ; descriptor+4  = block size
            add  edx, eax
            mov  [0x2e5d648], eax      ; block START
            mov  [0x2e5d64c], edx      ; block END
            mov  [0x2e5d650], eax      ; read pointer := start
            call GetCurrentThreadId
            mov  [0x2e5d658], eax      ; owning thread id
```

### Inner loop: execute the block, one opcode at a time

```asm
0x0049ded2  mov  esi, dword [eax]              ; OPCODE = first dword of the command
            cmp  esi, 0x4a                     ; 74 - the table size
            jge  done
            mov  ecx, [esi*4 + 0x13509f8]      ; handler = table[opcode]
            call ecx
            mov  eax, [0x2e5d650]              ; the HANDLER advanced the pointer itself
            mov  [0x2e5d644], esi              ; LAST OPCODE EXECUTED, kept in a global
            cmp  eax, [0x2e5d64c]
            jb   loop
```

A command is `[opcode][args...]`, and each handler advances `0x2e5d650` by its own size — 0x1C for
a draw, which is the opcode plus DrawIndexedPrimitive's six arguments.

### The complete submission map

| address | meaning |
|---|---|
| `0x013509F8` | opcode dispatch table, 74 entries |
| `0x0049DE20` | render-thread consumer loop (outer) |
| `0x0049DED2` | opcode dispatch loop (inner) |
| `0x0339385C` | sync object the render thread waits on |
| `0x0339389C` | ring read index (16-bit) |
| `0x0339389E` | ring entry count |
| `0x033938A0` | ring base; entries are **24 bytes** |
| `0x02E5D648` | current command block START |
| `0x02E5D64C` | current command block END |
| `0x02E5D650` | command read pointer |
| `0x02E5D658` | thread id that owns the current block |
| `0x02E5D644` | the PREVIOUSLY dispatched opcode (see correction below) |
| `0x03395EA4` | draw kill-switch (all four draw handlers test it) |
| `0x03171B68` | the `IDirect3DDevice9*` |

All valid verbatim at runtime: `RELOCS_STRIPPED`, no `DYNAMIC_BASE`, image base `0x400000`,
confirmed live on 2026-08-15.

### What this settles

**SR3 has a dedicated render thread.** The game thread produces command blocks into a ring; this
thread consumes them. That is the direct explanation for something this project measured but never
explained - *"the game locks vertex buffers from more than one thread"*, 272 buffer locks a frame
off the render thread, and a crash dump proving it. The producer fills buffers while the consumer
submits.

**A draw's PASS IDENTITY is the command block it belongs to.** Every D3D9 call the shim sees comes
from a handler running inside one block, and the block is identified by `[0x02E5D648]` - readable
at any draw, no heuristics, no sampler guessing, no render-target inference. Two draws in the same
block are in the same submission unit; two draws in different blocks are not.

That is the classification axis every rule in this shim has been approximating. `screenSpaceMode`,
`skipDeferredGBuffer`, `compositeToTexturePass` and the prepass tests are all attempts to recover,
from the flattened D3D9 stream, information the engine has already written down at a fixed address.

## The ring descriptor fields — traced 2026-08-30, and they carry NO pass identity

Measured first: `16.9 blocks/frame` over 6,321 frames, 12-138 draws each. Then the addresses:

    0x02D4A4D0  0x02D4E4D0  0x02D524D0  0x02D564D0  0x02D5A4D0  ...

**Exactly 0x4000 apart.** These are fixed-size **16 KB command buffers from a pool**; a new block
begins when the previous one FILLS. Block identity is allocation granularity, not a pass boundary,
and the claim in the previous section that "the block a draw belongs to IS its pass identity" is
**wrong** - draws in a block are only temporally adjacent.

The remaining three descriptor fields were traced to see whether any of them tags the block:

| field | what the dispatcher does with it | verdict |
|---|---|---|
| `+8` | `mov [0x3171b60], ebx` after the block finishes | **write-only.** One reference in the whole 12.7 MB image - the store itself. Nothing reads it |
| `+0xc` | `mov [0x3171b64], edx` after the block finishes | **write-only**, same - one reference, the store |
| `+0x10` | `if (edi) { ecx = edi; call 0xdd80f0 }` | a completion callback, signalled when the block is done |

And `0x0049E1C0`, called with the descriptor before the loop, computes `(desc - ringBase)/24` and
compares a 16-bit field in a parallel array at `0x033938A4` against the ring read index
`0x0339389C` - ring bookkeeping, locating the neighbouring slot. Not a label either.

### Conclusion: SR3 does not tag its render passes

The engine's command stream carries the opcode, the arguments, the block bounds and a completion
callback. **There is no per-block pass or context id.** A pass boundary in this engine is exactly
what it is at the D3D9 level - a `SetRenderTarget` command (op 9) - and the shim already sees that.

So the hoped-for shortcut does not exist, and this is worth recording as a closed question rather
than left as a promising direction. What the disassembly DID settle stands:

- the renderer is a command buffer consumed by a dedicated thread, which explains the
  multi-threaded buffer locking measured months earlier;
- `GetRenderTargetData` is a render command (ops 55/56/72), so engine-side readbacks are real;
- one global byte `0x03395EA4` disables all four draw commands, written from a visibility test -
  the engine's own culling, and why `forceOcclusionVisible` is load-bearing.

### The one capability the command buffer offers that D3D9 does not

The read pointer `0x02E5D650` and end `0x02E5D64C` bound a block that is ALREADY BUILT. Commands
can therefore be walked AHEAD of execution: at any draw, the rest of the block - including the next
`SetRenderTarget` - is readable. That is lookahead the streaming D3D9 view cannot provide, and it
would let a draw be classified by the pass it is IN and where that pass ENDS, rather than by
guessing from its samplers.

Whether that is worth building is a separate question: the shim already knows the CURRENT render
target, and lookahead only adds knowledge of the pass's extent.

## Per-opcode command sizes — extracted 2026-08-30, and the block is now walkable

Each handler advances the read pointer itself, so the sizes live in the handlers rather than in a
table. Extracted statically from every handler's own `add`/`mov` on `0x02E5D650`:

| size | opcodes |
|---|---|
| 4 | 0, 1, 2, 5, 6, 38, 62 |
| 8 | 3, 4, 8, 10, 15, 21, 31, 32, 33, 34, 52, 53 |
| 0x0C | **9 (SetRenderTarget)**, 16, 18, 22, 23, **37 (DrawPrimitive)**, 54, 57, 66, 73 |
| 0x10 | 19, 25, 28, **36 (SetTexture)**, 40, **55 (GetRenderTargetData)** |
| 0x14 | 11, 14, 17 |
| 0x18 | **35 (SetStreamSource)** |
| 0x1C | **42 (DrawIndexedPrimitive)**, 68 |
| 0x20 | 12, 50 |
| 0x34 | 51 |
| 0x604 | 58 (SetGammaRamp) |

Payload-carrying commands compute their own size from a count inside the command:

| opcode | size |
|---|---|
| 24, 27  `SetVertex/PixelShaderConstantF` | `0x0C + count*16`, count at `+8` |
| 26, 29  `SetVertex/PixelShaderConstantB` | `0x0C + count*4`, count at `+8` |
| 20 | `0x18 + [+0x14]` |
| 41  `DrawPrimitiveUP` | `0x14 + [+0x10]` |
| 61 | `0x08 + [+4]` |
| 67 | `0x18 + [+0x10]` |

Unresolved: 7, 43 (`DrawIndexedPrimitiveUP`, `0x24 + two registers`), 56, 60, 72. A walk that meets
one of these STOPS rather than guessing - a wrong size desynchronises everything after it, which is
a worse failure than a short answer.

### Why this is worth having

The block bounded by `[0x02E5D648, 0x02E5D64C)` is **already built** when the render thread begins
executing it. So at any draw the remainder of the block is readable, and that turns rule 1 -

> "A draw whose RESULT the engine reads can never be SKIPPED, only hidden or passed through."

- from an inference into a lookup. `GetRenderTargetData` is opcodes 55/56/72. **If a block contains
one, the engine reads back what that block drew and nothing in it may be removed.** Three separate
rules have blacked out the world by getting exactly this wrong from D3D9-level guessing.

Shipped as `scanCommandBlocks`, walking each block once, bounded by the end pointer and a 4096
command guard.



## Correction, 2026-08-30: `0x02E5D644` holds the PREVIOUS opcode

Measured over ~10,800 frames and 17.2 million draws. Read at the top of every draw hook, this
global never once held a draw opcode. Across every block listing the range seen is **20..35** -
`SetStreamSource` (35), `SetVertexShaderConstantF` (24), ops 20 and 34 - and **never 37, 40, 41 or
42**, which are the four draw opcodes themselves.

So the dispatcher updates it at the *end* of a loop iteration, not before the handler runs. During
a draw it names the command that preceded the draw, which is why the listing's "opcodes a..b"
column reads as a range of setup commands rather than as draw opcodes.

This does not affect the block-start pointer at `0x02E5D648`, which was and remains the
currently-executing block.

## The lookahead walk, validated 2026-08-30

    200,839 blocks walked | 132,204,771 commands | 0 walks stopped early

Zero early stops means `CommandSize()` covers every opcode SR3 emits during gameplay and the walk
never desynchronised. The opcodes left unresolved by static analysis (7, 43, 56, 60, 72) are never
emitted in play, so they cost nothing. The reader can be trusted as a source of facts.

### Finding: the engine never reads back a render target during gameplay

    0 GetRenderTargetData (opcodes 55/56/72) in 132,204,771 commands
    0 blocks contain a readback

The rule "a draw whose result the engine reads can never be skipped" is **true and vacuous**.
Nothing is read back, so it constrains nothing and cannot serve as a draw filter. Closed by
measurement; do not re-derive.

The useful corollary: the character atlas is **not** built by readback. The same run measured
texture `LockRect` 2, **surface `LockRect` 36**, `UpdateTexture` 0, `UpdateSurface` 0,
`StretchRect` 0, `ColorFill` 0. It is a CPU-to-GPU upload per mip surface, then sampled on the
GPU. The constraint on skipping a draw is GPU-side *sampling*, not CPU readback.

### Finding: the engine declares its own pass structure, ahead of execution

    797,612 SetRenderTarget (opcode 9) = ~74 per frame

Op 9 is 0x0C bytes and falls inside the same validated walk. Because a block is fully built before
the render thread executes it, every draw can be attributed to the render target the **engine**
names for it, before it runs. That is the pass identity `screenSpaceMode`, `hiddenPassMode`,
`skipDeferredGBuffer` and every prepass heuristic have been reconstructing from samplers and shader
output counts - each of which has blacked out the world at least once.

Note that for the *camera* question this lookahead is not needed: `g_rt0Width/Height` from the
`SetRenderTarget` hook already gives the same fact per draw, at no extra bridge cost.


## SR3's render-pass class hierarchy, recovered from RTTI (2026-08-31)

The exe ships MSVC RTTI, so the renderer's whole class tree is named in the binary. Resolved
TypeDescriptor -> CompleteObjectLocator -> vtable for each. **These are static addresses in a
RELOCS_STRIPPED image based at 0x400000, so they are valid verbatim** - but verify the module base
at runtime before trusting any of them (the rule from the earlier base-mismatch catch).

| class | vtable | what it is |
|---|---|---|
| `rl_d3d_base_render_pass` | `0x012A1780` | the ordinary scene pass |
| `rl_d3d_shadow_render_pass` | `0x012A1AEC` | shadow map generation |
| `rl_d3d_xray_render_pass` | `0x012A1550` | the see-through-walls pass |
| `rl_d3d_motion_blur_mask_render_pass` | `0x012A1514` | motion-blur mask |
| `rl_d3d_batched_pass` | `0x012A17B8` | batched draws |
| `rl_d3d_render_to_texture_pass` | (base `rl_render_to_texture_pass`) | render-to-texture |
| `rl_composite_pass` | `0x01293ACC` | composite |

Renderers, same treatment: `rl_d3d_scene_renderer` `0x012A142C`, `rl_d3d_terrain_renderer`
`0x012A170C`, `rl_d3d_particle_renderer` `0x012A010C`, `rl_d3d_primitive_renderer` `0x012A1058`,
`rl_d3d_floating_decal_renderer` `0x012A16A4`, `rl_d3d_reflected_light_renderer` `0x012A1658`,
`rl_d3d_raycast_renderer` `0x012A161C`, `rl_d3d_render_texture_manager` `0x0129FC80`.

Abstract bases sit at `0x0125D0CC` (`rl_base_render_pass`), `0x01293958` (`rl_shadow_render_pass`),
`0x0125D060` (`rl_xray_render_pass`), `0x0125D03C` (`rl_motion_blur_mask_render_pass`).

### The catch: passes do not run on the render thread

This taxonomy is real but it is **not directly readable at draw time**. SR3's passes execute on the
main thread and *record* commands; the render thread later executes those command blocks through the
74-opcode table, and that is where every D3D9 call the shim sees comes from. So at a draw hook the
stack contains the dispatcher, not `rl_d3d_shadow_render_pass::execute`. The pass classes cannot be
used as a per-draw identity without a way to carry pass identity into the command stream - and the
ring descriptors were already traced and carry none.

**What IS available at draw time and is engine-stated: the bound render target.** See below.

### `D3DPERF_BeginEvent` - a dead end, do not chase it

The exe imports `D3DPERF_BeginEvent`, `D3DPERF_EndEvent`, `D3DPERF_GetStatus` and
`D3DPERF_SetOptions` from `d3d9.dll`. That looks like named GPU pass markers, which would be an
ideal zero-risk pass identity. **It is not: all four have ZERO call sites.** Scanned every
`call dword [IAT slot]` in `.text` against each import's IAT address (`0x0101C518`, `0x0101C514`,
`0x0101C510`, `0x0101C51C`) - none. They are pulled in by a static library and never called. The
retail build emits no markers.

## The render target IS the pass identity, and it is available per draw

From the camera census of the 2026-08-31 run, one frame, every distinct camera with its target:

| render target | projection | handedness | draws | what it is |
|---|---|---|---|---|
| 2560x1440 | perspective | upright | 5,613 | **the main scene** |
| 4096x4096 | ortho | upright | 8 / 7 / 165 | shadow cascades |
| 512x288 | perspective | MIRRORED | 765 | reflection |
| 400x288 | perspective | MIRRORED | 22 / 207 | reflection |
| 400x288 | perspective | upright | 14 | reflection setup |
| 128x128 | ortho | upright | 3 | probe |

Across the run: 9,581,420 draws on the accepted camera against 2,190,420 declined for a
non-back-buffer target - 18.6% of draws are off-screen passes. `g_rt0Width/Height` already carries
this from the `SetRenderTarget` hook at no extra bridge cost, and it needs no shader names and no
render-target indices, both of which are recorded dead ends.


---

# ADDENDUM 2026-09-07 — the dispatch table is WRITABLE, and `0xD9E8B0` is a string hasher, not a setter

*Static only, against `SaintsRowTheThird.exe` with pefile + capstone. No runs.*

## 1. The opcode dispatch table can be hooked

The 74-entry table at `0x013509F8` — the one the render thread indexes at `0x0049DED2` — sits in
**`.data`**, which is `READ|WRITE`:

| section | VA | virtual size | characteristics |
|---|---|---|---|
| `.text` | `0x00401000` | `0x00C1AAF0` | `0x60000020` R-X |
| `.rdata` | `0x0101C000` | `0x002C436A` | `0x40000040` R-- |
| **`.data`** | **`0x012E1000`** | **`0x022415FC`** | **`0xC0000040` RW-** |

Every runtime global in this document's submission map is in that same writable section: the read
pointer `0x02E5D650`, the block bounds `0x02E5D648/64C`, the ring base `0x033938A0`, the device
pointer `0x03171B68` and the kill-switch `0x03395EA4`.

Dumped from file offset `0xF4EDF8`, the table holds **58 live handlers — all inside `.text` — and
16 copies of the stub `0x004BF550`**. The dwords following entry 73 are `0, 2, 0x28, 1`: small
integers, not pointers, so the table ends exactly where the dispatcher's `cmp esi, 0x4a` says.

**Calling convention.** Handlers take no arguments — each reads its command from `[0x02E5D650]` —
and end in a plain `ret`, not `ret N`. `void __cdecl` is therefore an exact replacement. The
dispatcher keeps the opcode in `ESI` across the call, and `esi` is callee-saved in the MSVC x86
ABI, so an ordinary C function already preserves it.

**The stub is a bare `ret`** (`0x004BF550`, then `int3` padding). It never advances the read
pointer, so if the producer ever emitted one of those 16 opcodes the render thread would spin on
that command forever. They are therefore never emitted — and a hook that counts them turns that
inference into a measurement.

## 2. The kill-switch does the pointer arithmetic for us

The `DrawIndexedPrimitive` handler's two paths, in full:

```asm
0x0049D661  cmp  byte [0x3395ea4], 0     ; kill-switch
0x0049D668  jne  0x49d6a5
            ... six args from [esi+4..+0x18], call vtable+0x148 ...
0x0049D692  add  dword [0x2e5d650], 0x1c ; DRAW path advances by 0x1C
0x0049D699  mov  dword [0x3395e7c], 0
0x0049D6A3  pop  esi
0x0049D6A4  ret
0x0049D6A5  mov  ecx, [0x351f8f8]        ; KILL path
0x0049D6AB  mov  eax, [ecx + 0x50]
0x0049D6AE  or   dword [eax + 4], 0x80   ; the engine's own "draw was skipped" flag
0x0049D6B5  add  dword [0x2e5d650], 0x1c ; ...advances by the SAME 0x1C
0x0049D6BC  pop  esi
0x0049D6BD  ret
```

**Both paths advance the read pointer identically.** So the way to suppress a draw is to set
`0x03395EA4`, call the original handler, and clear it again: the engine skips its own draw,
advances its own pointer, and sets its own skipped flag. No size arithmetic on our side, no chance
of desynchronising the block — and it works for **op 43**, whose size the static extraction in the
previous addendum could never resolve.

The kill path dereferences `[0x351f8f8] → +0x50 → +4`, so that chain must be non-null before the
switch is armed.

## 3. CORRECTION — `0x00D9E8B0` is a string hasher, not a material-parameter setter

`YOUR-INSTRUCTIONS.md` records "0x00951A60 sets Diffuse_Color_a/b/c … through one call (0xd9e8b0)
taking a name string". The call site is right; what the function *is* was not. Disassembled, it
walks the string one byte at a time:

```asm
0x00D9E8E8  call  0xea739d                  ; tolower  (if 'A'..'Z' add 0x20)
0x00D9E8F1  movzx ecx, al
0x00D9E8F6  xor   ecx, esi
0x00D9E8F8  and   ecx, 0xff
0x00D9E8FE  shr   esi, 8
0x00D9E901  xor   esi, [ecx*4 + 0x1320da0]  ; table lookup
...
0x00D9E914  mov   [edx], esi                ; stored raw - NO final inversion
```

That is the standard reflected CRC-32 inner loop, `crc = (crc >> 8) ^ table[(crc ^ ch) & 0xFF]`.
All 256 entries at `0x1320DA0` are **byte-identical to a generated CRC-32 table** (polynomial
`0xEDB88320`), checked entry by entry. Signature is thiscall
`Hash(this, const char* name, unsigned seed, unsigned maxLen)`; call sites pass seed `0` and
maxLen `-1`, and the result is stored without a final XOR.

So the identity of a named engine parameter is:

    id = crc32_lowercase(name)      init 0, no final XOR

| name | id |
|---|---|
| `Diffuse_Color_a` | `0xA7D143AC` |
| `Diffuse_Color_b` | `0x3ED81216` |
| `Diffuse_Color_c` | `0x49DF2280` |
| `Diffuse_Map` | `0x69B48F91` |
| `Pattern_Map` | `0x03B65AF7` |
| `Specular_Map` | `0xE848C9CA` |
| `Normal_Map` | `0x2808EB90` |
| `Tint_color` | `0x8F88517A` |

At `0x008FCE60` the three colour names are hashed **once** and cached, guarded by a bitmask at
`0x025F5B7C` (`test byte [0x25f5b7c], bl` / `or dword [0x25f5b7c], ebx`) with the hash objects at
`0x025F5B70/74/78`.

This does **not** overturn "parameters are bound by name" — that conclusion stands, and reflecting
by name per draw is still correct by construction. What it adds is the *runtime key*: the engine
compares 32-bit hashes, not strings, so any parameter id seen at runtime can be turned back into a
name by hashing a candidate list, and any name can be looked up without a string search. That is
the mechanism goal 3 needs, and `Pattern_Map` not appearing as a string in the exe is consistent
with it — a data-driven binding stores the hash, not the text.


## Correction, 2026-09-07 - op 37 is NOT DrawPrimitive, and the query triple is now named

The opcode table above assigned op 37 to `DrawPrimitive`. Reading the handlers themselves - after
the stage 0 hook census showed several thousand commands a frame with no established identity -
corrects it, and names four more:

| op | handler | what it actually is |
|---|---|---|
| 20 | `0x0049D040` | **engine state shadow.** Writes `[id*4 + 0x33958E0]` and `[((w1<<4)+w0)*4 + 0x3395970]`, ORs bits into `0x3395E70/74`, sets flag `0x3395E79`. **Calls no D3D9 method at all** |
| 21 | `0x0049D0C0` | the same two shadow arrays, from a pointed-to block rather than inline. Also no D3D9 call |
| **37** | `0x0049D510` | **`SetTexture` (vtable+0x104 = slot 65), stage = `[cmd+4] + 0x101`.** `0x101` is `D3DVERTEXTEXTURESAMPLER0`, so this binds a **VERTEX texture**. It issues no draw and does not test the kill-switch |
| **40** | `0x0049D560` | **`DrawPrimitive`** (vtable+0x144 = slot 81). Tests the kill-switch. Size `0x10` |
| 52 | `0x0049D810` | `IDirect3DQuery9::Issue` (vtable+0x18 = slot 6) with `D3DISSUE_BEGIN` |
| 53 | `0x0049D830` | `IDirect3DQuery9::Issue` with `D3DISSUE_END` |
| 54 | `0x0049D850` | `IDirect3DQuery9::GetData` (vtable+0x1C = slot 7), 4 bytes, flags 0. On failure writes `0xFFFFFFFF` to `[cmd+8]`, else the result |

**So the draw opcodes are 40, 41, 42, 43** - handlers `0x0049D560/5D0/650/6C0`, consecutive, and
exactly the four that test the kill-switch at `0x03395EA4`. Op 37 was never one of them. The shim
had inherited `{37, 40, 41, 42, 43}`; treating 37 as a draw would suppress a vertex-texture bind
while removing no geometry, which is precisely the kind of fault that is invisible until a drop is
actually attempted. Fixed in `EngineOpIsDraw()` before stage 1.

Ops 52/53/54 are dispatched at an **identical 557.3 per frame** - they are one occlusion query,
issued and read back 557 times a frame. That is the engine's own occlusion culling appearing in the
command stream, and it is what `forceOcclusionVisible` currently intercepts at the D3D9 boundary.

## First run of the stage 0 hook, 2026-09-07 - measured

`74/74` entries hooked, **123,307,264 commands dispatched over 4,800 frames** (~25,700 a frame),
no crash, and the table still held our thunks at exit. The predictions all held:

- **stub opcodes dispatched: 0**, confirming the bare-`ret` reading - the engine cannot emit them.
- **op 42 dispatches (15,831,840) match the D3D9 draws the shim attributed (15,831,620)** - two
  independent measurement paths agreeing to within a sampling window.
- the kill-switch was **set on 0 draws**, consistent with `forceOcclusionVisible=1`.

Two results that matter for filtering:

1. **Zero `GetRenderTargetData` in 108 million commands across 163,456 blocks.** Rule 1 - "a block
   whose result the engine reads back may never be skipped" - is the constraint that blacked out
   the world three times, and in gameplay **it does not bind anywhere**. 
2. **Zero walks stopped early**, so ops 7, 43, 56, 60 and 72 - the five whose sizes were never
   resolved - simply never occur. The size table is complete for gameplay.

~59.5 `SetRenderTarget` per frame, so roughly 60 passes a frame is the unit stage 2 will filter.

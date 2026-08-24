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

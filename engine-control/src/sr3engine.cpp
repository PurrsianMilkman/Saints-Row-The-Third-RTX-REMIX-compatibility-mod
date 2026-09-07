// sr3-engine.asi - SR3 engine renderer control, as a STANDALONE plugin.
//
// This is deliberately NOT part of sr3-rtx.asi. It hooks the ENGINE's own render-command dispatch
// table; the main shim hooks D3D9. They touch different tables, keep separate logs and separate
// ini files, and either can be removed without affecting the other - so the main RTX Remix work
// can carry on while this is being developed.
//
// WHAT IT HOOKS, and why here rather than at D3D9
//
// SR3 is a command-buffer renderer. A producer thread writes command blocks into a ring; a render
// thread at 0x0049DE20 consumes them and dispatches each command through a 74-entry
// function-pointer table:
//
//   0x0049DED2  mov esi, [eax]                ; opcode = first dword of the command
//               cmp esi, 0x4a                 ; 74 - the table size
//               jge done
//               mov ecx, [esi*4 + 0x13509f8]  ; handler = table[opcode]
//               call ecx
//               mov eax, [0x2e5d650]          ; the HANDLER advanced the read pointer itself
//               mov [0x2e5d644], esi          ; ESI IS LIVE ACROSS THE CALL
//
// That table is in WRITABLE .data (VA 0x012E1000, characteristics 0xC0000040), so hooking the
// engine's renderer is a matter of writing 74 pointers. Our code then runs INSIDE the engine, one
// level above D3D9, where a command is still a command rather than six loose arguments.
//
// CALLING CONVENTION. Handlers take NO arguments - each reads its command from [0x02E5D650] - and
// end in a plain `ret`, not `ret N`. `void __cdecl` is an exact match. ESI is live across the call
// in the dispatcher and is callee-saved in the MSVC x86 ABI, so an ordinary C function preserves it.
//
// VERIFIED STATICALLY against the shipped exe, and then live on 2026-09-07: 74/74 entries hooked,
// 123.3M commands dispatched over 4,800 frames, no crash, 0 stub dispatches.

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

// ---------------------------------------------------------------------------------------------
// logging - its own file, so nothing here interleaves with the main shim's log

wchar_t g_logPath[MAX_PATH]{};
CRITICAL_SECTION g_logLock;
bool g_logReady = false;

void LogInit(HMODULE self) {
    InitializeCriticalSection(&g_logLock);
    GetModuleFileNameW(self, g_logPath, MAX_PATH);
    wchar_t* slash = wcsrchr(g_logPath, L'\\');
    if (slash) wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - g_logPath), L"sr3-engine.log");
    FILE* f = nullptr;
    if (_wfopen_s(&f, g_logPath, L"w") == 0 && f) fclose(f);
    g_logReady = true;
}

void Log(const char* fmt, ...) {
    if (!g_logReady) return;
    char line[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    EnterCriticalSection(&g_logLock);
    FILE* f = nullptr;
    if (_wfopen_s(&f, g_logPath, L"a") == 0 && f) {
        fprintf(f, "%s\n", line);
        fclose(f);
    }
    LeaveCriticalSection(&g_logLock);
}

// ---------------------------------------------------------------------------------------------
// settings - its own ini, so the main shim's sr3-rtx.ini is never touched

struct Settings {
    bool     engineHooks   = true;   // the whole point of this plugin, so on by default HERE
    unsigned reportSeconds = 10;     // how often the census is written
    unsigned installDelayMs = 3000;  // let the engine finish its own init before we write
    // STAGE 1. 0 = off, 1 = drop one draw in every dropOneInN, 2 = drop every draw.
    // Mode 2 is the unambiguous proof: if the primitive is correct the image empties out and the
    // game keeps running at a normal frame rate. A hang or a crash would mean the read pointer
    // desynchronised, which is precisely what routing the drop through the engine's own kill
    // path is designed to make impossible.
    unsigned dropMode      = 0;
    unsigned dropOneInN    = 2;
    bool     passCensus    = true;   // count draws per render target, via op 9
};
Settings g_settings;

wchar_t g_iniPath[MAX_PATH]{};

void LoadSettings(HMODULE self) {
    GetModuleFileNameW(self, g_iniPath, MAX_PATH);
    wchar_t* slash = wcsrchr(g_iniPath, L'\\');
    if (slash) wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - g_iniPath), L"sr3-engine.ini");
    auto num = [](const wchar_t* key, int def, const wchar_t* path) {
        return static_cast<int>(GetPrivateProfileIntW(L"sr3-engine", key, def, path));
    };
    g_settings.engineHooks    = num(L"engineHooks", 1, g_iniPath) != 0;
    g_settings.reportSeconds  = static_cast<unsigned>(num(L"reportSeconds", 10, g_iniPath));
    g_settings.installDelayMs = static_cast<unsigned>(num(L"installDelayMs", 3000, g_iniPath));
    g_settings.dropMode       = static_cast<unsigned>(num(L"dropMode", 0, g_iniPath));
    g_settings.dropOneInN     = static_cast<unsigned>(num(L"dropOneInN", 2, g_iniPath));
    g_settings.passCensus     = num(L"passCensus", 1, g_iniPath) != 0;
    if (g_settings.reportSeconds < 1) g_settings.reportSeconds = 1;
    if (g_settings.dropOneInN < 2) g_settings.dropOneInN = 2;
    if (g_settings.dropMode > 2) g_settings.dropMode = 0;
}

// ---------------------------------------------------------------------------------------------
// the engine map. Absolute addresses: the exe is RELOCS_STRIPPED with no DYNAMIC_BASE and always
// loads at 0x400000 - but that is VERIFIED below rather than assumed, because engine-map.md
// records a static map that was wrong by 0x81C000 and was caught by exactly this kind of check.

constexpr uintptr_t kExpectedBase      = 0x00400000;
constexpr uintptr_t kAddrDispatchTable = 0x013509F8;
constexpr uintptr_t kAddrReadPtr       = 0x02E5D650;
constexpr uintptr_t kAddrBlockStart    = 0x02E5D648;
constexpr uintptr_t kAddrBlockEnd      = 0x02E5D64C;
constexpr uintptr_t kAddrDrawDisabled  = 0x03395EA4;
constexpr unsigned  kEngineOpCount     = 74;
constexpr unsigned  kEngineStubHandler = 0x004BF550;

// The table exactly as it stands in the shipped exe, dumped from file offset 0xF4EDF8.
// The control: compared against the live table before a single entry is written.
constexpr unsigned kExpectedDispatch[kEngineOpCount] = {
    0x0049C940, 0x0049C950, 0x0049C960, 0x0049C980, 0x0049C9A0, 0x0049C9F0,
    0x0049CA10, 0x0049CA30, 0x0049CAA0, 0x0049CB00, 0x0049CB30, 0x0049CB60,
    0x0049CC00, 0x004BF550, 0x0049CD10, 0x0049CEC0, 0x0049CDF0, 0x0049CF90,
    0x0049CFE0, 0x0049D010, 0x0049D040, 0x0049D0C0, 0x0049D160, 0x0049D180,
    0x0049D1C0, 0x0049D210, 0x0049D250, 0x0049D2A0, 0x0049D2F0, 0x0049D330,
    0x004BF550, 0x0049D380, 0x0049D3B0, 0x0049D3E0, 0x0049D410, 0x0049D440,
    0x0049D4A0, 0x0049D510, 0x0049D550, 0x004BF550, 0x0049D560, 0x0049D5D0,
    0x0049D650, 0x0049D6C0, 0x004BF550, 0x004BF550, 0x004BF550, 0x004BF550,
    0x004BF550, 0x004BF550, 0x0049D760, 0x0049D7C0, 0x0049D810, 0x0049D830,
    0x0049D850, 0x0049D890, 0x0049D950, 0x0049DA20, 0x0049DA60, 0x004BF550,
    0x0049DA90, 0x0049DAD0, 0x0049DB30, 0x004BF550, 0x004BF550, 0x004BF550,
    0x0049DB40, 0x0049DB60, 0x0049DB80, 0x004BF550, 0x004BF550, 0x004BF550,
    0x0049DBB0, 0x0049DDB0,
};

// Names read out of the handlers themselves, not guessed. Where an opcode's identity is not
// established it prints as its number - the "log what a thing IS, by name" rule.
//
// Ops 37 and 40 are the 2026-09-07 CORRECTION to docs/engine-map.md, which had 37 as DrawPrimitive:
//   op 37  0x0049D510  vtable+0x104 = slot 65 SetTexture, stage = [cmd+4] + 0x101.
//                      0x101 is D3DVERTEXTEXTURESAMPLER0, so it binds a VERTEX texture.
//   op 40  0x0049D560  vtable+0x144 = slot 81 DrawPrimitive, and it DOES test the kill-switch.
const char* EngineOpName(unsigned op) {
    switch (op) {
        case  9: return "SetRenderTarget";
        case 10: return "SetDepthStencilSurface";
        case 11: case 12: return "Clear";
        case 14: case 16: return "SetViewport";
        case 17: return "SetScissorRect";
        case 19: return "SetSamplerState";
        case 20: case 21: return "engine state shadow (no D3D9)";
        case 24: case 25: return "SetVertexShaderConstantF";
        case 26: return "SetVertexShaderConstantB";
        case 27: case 28: return "SetPixelShaderConstantF";
        case 29: return "SetPixelShaderConstantB";
        case 31: return "SetVertexShader";
        case 32: return "SetPixelShader";
        case 33: return "SetVertexDeclaration";
        case 34: return "SetIndices";
        case 35: return "SetStreamSource";
        case 36: return "SetTexture";
        case 37: return "SetTexture(vertex sampler)";
        case 40: return "DrawPrimitive";
        case 41: return "DrawPrimitiveUP";
        case 42: return "DrawIndexedPrimitive";
        case 43: return "DrawIndexedPrimitiveUP";
        case 50: return "GetBackBuffer";
        case 51: return "StretchRect";
        case 52: return "Query::Issue(BEGIN)";
        case 53: return "Query::Issue(END)";
        case 54: return "Query::GetData";
        case 55: case 56: return "GetRenderTargetData";
        case 57: return "SetRenderState";
        case 58: return "SetGammaRamp";
        default: return nullptr;
    }
}

// The four handlers that test the kill-switch at 0x03395EA4 are 0x0049D560/5D0/650/6C0 - opcodes
// 40, 41, 42, 43, consecutively. That is the draw set, and it is what a drop may target.
// Treating 37 as a draw would suppress a vertex-texture bind while removing no geometry at all.
bool EngineOpIsDraw(unsigned op) {
    return op == 40 || op == 41 || op == 42 || op == 43;
}

// ---------------------------------------------------------------------------------------------
// the hook

typedef void(__cdecl* EngineOpFn)();

EngineOpFn g_engOrig[kEngineOpCount] = {};
EngineOpFn g_engThunk[kEngineOpCount] = {};
unsigned g_engOpCount[kEngineOpCount] = {};

bool     g_engHooked = false;
unsigned g_engHookedCount = 0;
unsigned g_engMismatches = 0;
unsigned g_engStubDispatched = 0;

// ---------------------------------------------------------------------------------------------
// STAGE 1 - the drop primitive, and a pass census
//
// THE DROP. To suppress a draw we do NOT skip the handler and advance the read pointer ourselves.
// We set the engine's own kill-switch, call the original handler, and put the switch back:
//
//     set 0x03395EA4  ->  call original  ->  restore 0x03395EA4
//
// Both paths through a draw handler advance the read pointer by the same amount - op 42's draw
// path at 0x49D692 and its kill path at 0x49D6B5 both `add [0x2e5d650], 0x1c`. So the engine does
// its own size arithmetic and there is no way for us to desynchronise the command block. It also
// covers op 43, whose size was never resolved statically, and it leaves the engine's own
// skipped-draw bookkeeping (`or [eax+4], 0x80`) consistent, because this is the exact path SR3
// takes when its own culling decides an object is not visible.
//
// THE GUARD. That kill path dereferences [0x351F8F8] -> +0x50 -> +4 and WRITES there. If the root
// is null or not yet initialised, arming the switch would turn a working frame into a crash inside
// the engine. So the chain is validated with VirtualQuery, and re-validated whenever the root
// pointer's value changes - one compare in the common case. If it does not validate, the draw is
// passed through untouched and the refusal is counted.
//
// The switch is restored to its PREVIOUS value, not to zero: the engine may have set it itself,
// and clearing it would be us overriding the engine's own culling decision.

constexpr uintptr_t kAddrKillChainRoot = 0x0351F8F8;

enum DropMode {
    kDropOff     = 0,   // nothing is dropped; the primitive never fires
    kDropOneInN  = 1,   // drop one draw in every N - visible, stable, and unambiguous
    kDropAll     = 2,   // drop every draw - the black-image proof that the primitive is correct
};

unsigned g_drawsSeen     = 0;
unsigned g_dropsDone     = 0;
unsigned g_dropsRefused  = 0;
unsigned g_dropSeq       = 0;

unsigned g_killChainRootSeen = 0;
bool     g_killChainOk       = false;

// Readable AND writable, at every level the kill path touches.
bool ProbeWritable(uintptr_t a, SIZE_T n) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    const DWORD w = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if (!(mbi.Protect & w)) return false;
    const uintptr_t end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return a + n <= end;
}

// Validates [0x351F8F8] -> +0x50 -> +4 the way the kill path walks it. Cached on the root value,
// so the steady-state cost is a load and a compare.
bool KillChainOk() {
    if (!ProbeWritable(kAddrKillChainRoot, 4)) return false;
    const unsigned root = *reinterpret_cast<const volatile unsigned*>(kAddrKillChainRoot);
    if (root == g_killChainRootSeen) return g_killChainOk;
    g_killChainRootSeen = root;
    g_killChainOk = false;
    if (!root || !ProbeWritable(root + 0x50, 4)) return false;
    const unsigned mid = *reinterpret_cast<const volatile unsigned*>(root + 0x50);
    if (!mid || !ProbeWritable(mid + 4, 4)) return false;
    g_killChainOk = true;
    return true;
}

// Should this particular draw be dropped? Kept branch-cheap; called once per draw.
bool ShouldDropDraw() {
    switch (g_settings.dropMode) {
        case kDropAll:    return true;
        case kDropOneInN: return g_settings.dropOneInN > 1 &&
                                 (++g_dropSeq % g_settings.dropOneInN) == 0;
        default:          return false;
    }
}

// ---------------------------------------------------------------------------------------------
// pass census - what stage 2 will filter on
//
// A pass boundary in this engine is exactly a SetRenderTarget command (op 9). Its layout, read
// from the handler at 0x0049CB00: [opcode][index at +4][surface at +8], size 0x0C, dispatched to
// vtable+0x94 = slot 37. So the render target a draw belongs to is readable at the command, with
// no inference from formats or sampler names.

// CORRECTED after the first stage 1 run. The first version keyed on the SURFACE alone and ignored
// the render-target INDEX, which is wrong in two ways on an MRT engine:
//
//   - every MRT slot collapsed into one population, so `surface 0x00000000` accumulated 51,893
//     binds. Those are not a pass; they are `SetRenderTarget(index>0, NULL)` unbinding the extra
//     slots of the previous MRT set.
//   - the "current pass" became whichever surface was bound LAST, so a bind sequence of
//     slot0=A, slot1=B, draw attributed the draw to B - the auxiliary target - rather than to A.
//
// Confirmed from the handler at 0x0049CB00: args are pushed right-to-left for
// SetRenderTarget(index, pRenderTarget), so [cmd+4] is the INDEX and [cmd+8] the SURFACE.
//
// A pass is therefore defined by what is bound at INDEX 0 - the colour target. Binds to slots 1..n
// are recorded so the MRT shape is visible, but they do not change which pass draws belong to.
constexpr unsigned kMaxPasses = 128;
struct PassRow {
    unsigned surface = 0;
    unsigned index   = 0;
    unsigned binds   = 0;
    unsigned draws   = 0;
};
PassRow  g_passes[kMaxPasses];
unsigned g_passCount    = 0;
unsigned g_passOverflow = 0;
unsigned g_curPass      = 0xFFFFFFFF;   // index into g_passes, not a surface
unsigned g_nullBinds    = 0;            // SetRenderTarget(*, NULL) - slot unbinds

// Called from the op 9 thunk BEFORE the original runs, because the handler advances the pointer.
void NotePassBind() {
    const unsigned cmd = *reinterpret_cast<const volatile unsigned*>(kAddrReadPtr);
    if (!cmd) return;
    const unsigned rtIndex = *reinterpret_cast<const volatile unsigned*>(cmd + 4);
    const unsigned surface = *reinterpret_cast<const volatile unsigned*>(cmd + 8);
    if (!surface) { ++g_nullBinds; if (rtIndex == 0) g_curPass = 0xFFFFFFFF; return; }

    for (unsigned i = 0; i < g_passCount; ++i) {
        if (g_passes[i].surface == surface && g_passes[i].index == rtIndex) {
            ++g_passes[i].binds;
            if (rtIndex == 0) g_curPass = i;
            return;
        }
    }
    if (g_passCount >= kMaxPasses) {
        ++g_passOverflow;
        if (rtIndex == 0) g_curPass = 0xFFFFFFFF;
        return;
    }
    const unsigned slot = g_passCount++;
    g_passes[slot].surface = surface;
    g_passes[slot].index   = rtIndex;
    g_passes[slot].binds   = 1;
    g_passes[slot].draws   = 0;
    if (rtIndex == 0) g_curPass = slot;
}

inline void NotePassDraw() {
    if (g_curPass < kMaxPasses) ++g_passes[g_curPass].draws;
}

// One thunk per opcode. The opcode is a TEMPLATE PARAMETER, so every instantiation is a distinct
// function with its own constant baked in - which is what stops the linker's identical-COMDAT
// folding (/OPT:ICF, on by default) from collapsing all 74 into one and losing the per-opcode
// attribution that is the entire point of the census.
//
// Deliberately minimal. This runs on the render thread for EVERY command - about 25,700 a frame -
// so it is one increment and a call through the saved original. No logging, no locking, no
// allocation on this path. The counter is a plain non-atomic increment: the dispatcher is a single
// render thread, and a torn diagnostic counter would be harmless in any case.
template <unsigned Op>
void __cdecl EngineOpThunk() {
    ++g_engOpCount[Op];
    if constexpr (kExpectedDispatch[Op] == kEngineStubHandler) ++g_engStubDispatched;

    // `if constexpr` so a non-draw thunk compiles to the increment and the call, with none of
    // this present at all. Only the four draw opcodes carry the drop path.
    if constexpr (Op == 40 || Op == 41 || Op == 42 || Op == 43) {
        ++g_drawsSeen;
        NotePassDraw();
        if (g_settings.dropMode != kDropOff && ShouldDropDraw()) {
            if (KillChainOk()) {
                volatile unsigned char* sw =
                    reinterpret_cast<volatile unsigned char*>(kAddrDrawDisabled);
                const unsigned char prev = *sw;   // restore what the ENGINE had, not zero
                *sw = 1;
                g_engOrig[Op]();
                *sw = prev;
                ++g_dropsDone;
                return;
            }
            ++g_dropsRefused;   // chain not valid - pass the draw through untouched
        }
    } else if constexpr (Op == 9) {
        if (g_settings.passCensus) NotePassBind();
    }

    g_engOrig[Op]();
}

template <unsigned Op>
struct EngineThunkFiller {
    static void Fill() {
        g_engThunk[Op] = &EngineOpThunk<Op>;
        EngineThunkFiller<Op - 1>::Fill();
    }
};
template <>
struct EngineThunkFiller<0> {
    static void Fill() { g_engThunk[0] = &EngineOpThunk<0>; }
};

// The host must be SR3, at the base the static map describes, with every address committed.
bool EngineMapApplies() {
    HMODULE h = GetModuleHandleW(nullptr);
    wchar_t path[MAX_PATH]{};
    if (!h || !GetModuleFileNameW(h, path, MAX_PATH)) return false;
    const wchar_t* leaf = wcsrchr(path, L'\\');
    leaf = leaf ? leaf + 1 : path;
    if (_wcsicmp(leaf, L"SaintsRowTheThird.exe") != 0) {
        Log("engine map: host is not SaintsRowTheThird.exe - disabled");
        return false;
    }
    if (reinterpret_cast<uintptr_t>(h) != kExpectedBase) {
        Log("engine map: module base 0x%08X, expected 0x%08X - DISABLED, the static map does not "
            "apply", static_cast<unsigned>(reinterpret_cast<uintptr_t>(h)),
            static_cast<unsigned>(kExpectedBase));
        return false;
    }
    const uintptr_t probe[] = {kAddrDispatchTable, kAddrReadPtr, kAddrBlockStart,
                              kAddrDrawDisabled};
    for (unsigned pi = 0; pi < sizeof(probe) / sizeof(probe[0]); ++pi) {
        const uintptr_t a = probe[pi];
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof(mbi)) ||
            mbi.State != MEM_COMMIT) {
            Log("engine map: 0x%08X is not committed - disabled", static_cast<unsigned>(a));
            return false;
        }
    }
    return true;
}

// True if the live table still matches the shipped exe (or holds our own thunks once installed).
// Every difference is logged: "the table changed" and "ONE entry changed because another mod owns
// it" are different situations, and only the listing separates them.
bool VerifyDispatchTable(const char* when, bool quiet) {
    const volatile unsigned* table = reinterpret_cast<const volatile unsigned*>(kAddrDispatchTable);
    unsigned bad = 0;
    for (unsigned i = 0; i < kEngineOpCount; ++i) {
        const unsigned live = table[i];
        if (live == kExpectedDispatch[i]) continue;
        if (g_engHooked && live == reinterpret_cast<unsigned>(g_engThunk[i])) continue;
        if (!quiet && bad < 8)
            Log("engine hooks: %s - table[%u] is 0x%08X, expected 0x%08X",
                when, i, live, kExpectedDispatch[i]);
        ++bad;
    }
    if (bad && !quiet)
        Log("engine hooks: %s - %u of %u entries DIFFER from the shipped exe",
            when, bad, kEngineOpCount);
    g_engMismatches = bad;
    return bad == 0;
}

bool InstallEngineHooks() {
    if (g_engHooked) return true;
    if (!EngineMapApplies()) return false;
    if (!VerifyDispatchTable("before install", false)) {
        Log("engine hooks: REFUSING to write. The live table does not match the shipped exe - "
            "either the static map does not describe this process, or another mod hooked it "
            "first. Writing into it would corrupt whatever owns it.");
        return false;
    }

    EngineThunkFiller<kEngineOpCount - 1>::Fill();

    unsigned* table = reinterpret_cast<unsigned*>(kAddrDispatchTable);
    DWORD old = 0;
    if (!VirtualProtect(table, kEngineOpCount * sizeof(unsigned), PAGE_READWRITE, &old)) {
        Log("engine hooks: VirtualProtect failed (%lu) - NOT hooking",
            static_cast<unsigned long>(GetLastError()));
        return false;
    }
    for (unsigned i = 0; i < kEngineOpCount; ++i) {
        g_engOrig[i] = reinterpret_cast<EngineOpFn>(table[i]);
        table[i] = reinterpret_cast<unsigned>(g_engThunk[i]);
        ++g_engHookedCount;
    }
    VirtualProtect(table, kEngineOpCount * sizeof(unsigned), old, &old);

    g_engHooked = true;
    Log("engine hooks: INSTALLED over %u of %u dispatch entries at 0x%08X (58 live handlers, "
        "16 stubs). Stage 0 is INERT - every thunk counts and calls the original.",
        g_engHookedCount, kEngineOpCount, static_cast<unsigned>(kAddrDispatchTable));
    return true;
}

// ---------------------------------------------------------------------------------------------
// the census
//
// THE FALSIFIER. A hook that installed but never runs and a hook that never installed look
// identical from outside the process; only this listing separates them. There is no Present here
// to report from - this plugin does not hook D3D9 at all - so the report is on a timer and the
// rates are PER SECOND rather than per frame.

unsigned g_reports = 0;
unsigned g_prevTotal = 0;

void ReportCensus(double seconds) {
    unsigned total = 0;
    unsigned snapshot[kEngineOpCount];
    for (unsigned i = 0; i < kEngineOpCount; ++i) {
        snapshot[i] = g_engOpCount[i];
        total += snapshot[i];
    }

    Log("");
    Log("=== ENGINE HOOKS census #%u ===", ++g_reports);
    if (!g_engHooked) {
        Log("  not installed%s",
            g_engMismatches ? " - the dispatch table did not match the shipped exe" : "");
        return;
    }
    Log("  %u/%u dispatch entries hooked | %u commands dispatched in total",
        g_engHookedCount, kEngineOpCount, total);
    if (!total) {
        Log("  ZERO commands dispatched. The table was written but the render thread never "
            "reached our thunks - the hook is NOT live and nothing below is evidence.");
        return;
    }
    const double perSec = seconds > 0.0 ? (total - g_prevTotal) / seconds : 0.0;
    Log("  %.0f commands/second since the last census", perSec);
    g_prevTotal = total;

    // Every opcode the engine actually used, most frequent first.
    unsigned order[kEngineOpCount];
    for (unsigned i = 0; i < kEngineOpCount; ++i) order[i] = i;
    for (unsigned i = 1; i < kEngineOpCount; ++i) {
        const unsigned k = order[i];
        int j = static_cast<int>(i) - 1;
        while (j >= 0 && snapshot[order[j]] < snapshot[k]) { order[j + 1] = order[j]; --j; }
        order[j + 1] = k;
    }
    unsigned shown = 0;
    for (unsigned i = 0; i < kEngineOpCount && shown < 24; ++i) {
        const unsigned op = order[i];
        if (!snapshot[op]) break;
        const char* name = EngineOpName(op);
        Log("    op %2u %-30s %10u  %5.1f%%%s", op,
            name ? name : "(identity not established)", snapshot[op],
            100.0 * snapshot[op] / total, EngineOpIsDraw(op) ? "   <- DRAW" : "");
        ++shown;
    }
    // The stub is a bare `ret` that never advances the read pointer, so the engine dispatching one
    // would hang the render thread. 0 confirms the static reading; non-zero is a real finding.
    Log("  stub opcodes dispatched: %u (expected 0)", g_engStubDispatched);
    // The engine's own culling. Non-zero means SR3 decided an object was not visible and every
    // draw handler became a no-op.
    Log("  engine draw kill-switch (0x03395EA4) currently: %u",
        static_cast<unsigned>(*reinterpret_cast<const volatile unsigned char*>(kAddrDrawDisabled)));

    // STAGE 1. The falsifier for the drop primitive: drops COUNTED AGAINST draws seen, and
    // refusals counted separately. A mode that is on but drops nothing, and one that drops
    // everything, are not distinguishable from the image alone - a black frame could equally be
    // the drop working or the game having stopped. These three numbers separate them.
    const char* modeName = g_settings.dropMode == kDropAll   ? "ALL draws"
                         : g_settings.dropMode == kDropOneInN ? "one in N"
                                                              : "off";
    Log("  DROPS: mode=%u (%s) | %u draws seen, %u dropped (%.1f%%), %u refused",
        g_settings.dropMode, modeName,
        g_drawsSeen, g_dropsDone,
        g_drawsSeen ? 100.0 * g_dropsDone / g_drawsSeen : 0.0, g_dropsRefused);
    if (g_settings.dropMode != kDropOff && !g_dropsDone && g_drawsSeen)
        Log("    dropMode is ON but NOTHING was dropped - either the kill chain never validated "
            "(refused=%u) or the draw thunks are not being reached", g_dropsRefused);
    if (g_dropsRefused)
        Log("    refusals mean [0x351F8F8] -> +0x50 -> +4 did not validate as writable, so those "
            "draws were passed through untouched rather than risking a fault inside the engine");
    if (g_settings.dropMode == kDropOneInN)
        Log("    expected share for dropOneInN=%u is %.1f%%", g_settings.dropOneInN,
            100.0 / g_settings.dropOneInN);

    // PASS CENSUS. A pass boundary is a SetRenderTarget command (op 9); its surface is at [cmd+8].
    // This is the unit stage 2 will filter on, named by the engine rather than inferred.
    if (g_settings.passCensus) {
        Log("  PASSES (render target from the op 9 command: index at [cmd+4], surface at [cmd+8]): "
            "%u distinct (index,surface) pairs%s | %u NULL binds (slot unbinds, not passes)",
            g_passCount, g_passOverflow ? " - TABLE FULL, more exist" : "", g_nullBinds);
        Log("    draws are attributed to what is bound at INDEX 0, the colour target; slots 1+ are "
            "MRT companions and are listed only to show the shape");
        unsigned porder[kMaxPasses];
        for (unsigned i = 0; i < g_passCount; ++i) porder[i] = i;
        for (unsigned i = 1; i < g_passCount; ++i) {
            const unsigned k = porder[i];
            int j = static_cast<int>(i) - 1;
            while (j >= 0 && g_passes[porder[j]].draws < g_passes[k].draws) {
                porder[j + 1] = porder[j]; --j;
            }
            porder[j + 1] = k;
        }
        for (unsigned i = 0; i < g_passCount && i < 16; ++i) {
            const PassRow& r = g_passes[porder[i]];
            Log("    rt%u surface 0x%08X : %10u draws over %7u binds%s",
                r.index, r.surface, r.draws, r.binds,
                r.index ? "   (MRT companion)" : "");
        }
    }
    if (!VerifyDispatchTable("this census", false))
        Log("  OUR THUNKS HAVE BEEN DISPLACED - something rewrote the table after we installed");
}

// ---------------------------------------------------------------------------------------------
// entry point

DWORD WINAPI Worker(LPVOID) {
    // Let the engine finish its own initialisation before writing into its .data. The table's
    // values come straight from the PE image so they are valid at load, but a delay costs nothing
    // and removes the whole question.
    Sleep(g_settings.installDelayMs);

    if (!g_settings.engineHooks) {
        Log("engineHooks=0 - this plugin is inert and will not touch the game.");
        return 0;
    }

    // Retry: if the table does not verify yet, it may still be mid-initialisation.
    for (unsigned attempt = 0; attempt < 40 && !g_engHooked; ++attempt) {
        if (InstallEngineHooks()) break;
        if (g_engMismatches == 0) break;   // a hard refusal, not a timing problem
        Sleep(500);
    }
    if (!g_engHooked) {
        Log("engine hooks: not installed after retrying - stopping.");
        return 0;
    }

    for (;;) {
        Sleep(g_settings.reportSeconds * 1000);
        ReportCensus(static_cast<double>(g_settings.reportSeconds));
    }
}

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(self);
        LogInit(self);
        LoadSettings(self);
        Log("sr3-engine.asi - SR3 engine renderer control, standalone.");
        Log("  separate from sr3-rtx.asi: this hooks the ENGINE's command dispatch table at "
            "0x013509F8; the main shim hooks D3D9. Different tables, different logs, different ini.");
        Log("  engineHooks=%d reportSeconds=%u installDelayMs=%u",
            g_settings.engineHooks ? 1 : 0, g_settings.reportSeconds, g_settings.installDelayMs);
        CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    }
    return TRUE;
}

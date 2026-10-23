// parts/30_setup_device.cpp - setup, crash reporting, occlusion queries, CreateDevice hooks.
// NOT a standalone translation unit: #included, in order, by ../sr3rtx.cpp inside its anonymous
// namespace. Originally sr3rtx.cpp lines 25144-25617 (old file:line citations map here).
// ---------------------------------------------------------------- setup

// ---------------------------------------------------------------- crash reporting
//
// The user reports the game dying perhaps one time in three when moving fast or freefalling from
// the penthouse roof. The log flushes every line, so it ends mid-sentence on a crash and says
// nothing about why. This makes the next one produce evidence instead of a hypothesis.
//
// Two things are recorded: where the fault was (module + offset, and for an access violation the
// address that was touched), and what the shim was doing (frame, draw index, the albedo last
// bound and whether the mesh cache supplied it). If the faulting address is the cached albedo,
// the diagnosis is finished.
//
// dbghelp is loaded at crash time rather than imported, so a missing dbghelp.dll costs the dump
// and not the process.
LPTOP_LEVEL_EXCEPTION_FILTER g_prevFilter = nullptr;

// dbghelp.dll and MiniDumpWriteDump are resolved once, at device-hook time (see HookDevice),
// into these two - not inside CrashFilter, where LoadLibraryA would take the loader lock in a
// process that may already be crashed because of a corrupted heap or a deadlocked loader.
// WriteMinidump below still falls back to loading them itself if this cache is empty (a crash
// before HookDevice ever ran, or dbghelp genuinely was not present at hook time), because a
// dump that occasionally does not need the fallback is worse than a fallback that is not
// always needed.
typedef BOOL(WINAPI* WriteDump_t)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
                                  PMINIDUMP_EXCEPTION_INFORMATION, PVOID, PVOID);
HMODULE g_dbghelpModule = nullptr;
WriteDump_t g_writeDumpProc = nullptr;

// Shared by CrashFilter and the crashTestDumpAtStart self-test in HookDevice. mei may be
// null, which MiniDumpWriteDump accepts and simply writes a dump of the process as it
// currently stands with no exception record - that is how the self-test verifies the writer,
// the flags and the file handle without waiting for the game to actually crash.
static bool WriteMinidump(const char* dumpPath, MINIDUMP_EXCEPTION_INFORMATION* mei) {
    WriteDump_t write = g_writeDumpProc;
    HMODULE dbghelp = g_dbghelpModule;
    if (!write) {
        dbghelp = LoadLibraryA("dbghelp.dll");
        if (dbghelp)
            write = reinterpret_cast<WriteDump_t>(GetProcAddress(dbghelp, "MiniDumpWriteDump"));
    }
    if (!write) return false;

    // GENERIC_READ | GENERIC_WRITE: the documented dbghelp sample opens the dump file for
    // both - MiniDumpWriteDump seeks back and re-reads parts of what it has already written to
    // lay out the directory correctly. Write-only worked often enough that this went unnoticed
    // until it silently produced a dump some tools refused to parse.
    const HANDLE file = CreateFileA(dumpPath, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    // A dump written with only MiniDumpWithIndirectlyReferencedMemory had
    // MINIDUMP_THREAD.Stack.Memory.Rva == 0 for every one of 35 threads - no stack was
    // recoverable from the dump itself, and the crash that motivated this fix had to be
    // re-derived from the raw memory list instead. MiniDumpWithThreadInfo was added at the
    // same time and the empty RVAs went away, but thread stacks actually come from the thread
    // list stream, which every dump type already writes - so ThreadInfo was very likely never
    // what fixed it, and the comment that used to live here claimed it was without
    // re-deriving that. Left in below anyway (removing it has not been tested, and it costs
    // little); what more plausibly answers "why did the stack look unreadable" is
    // MiniDumpWithFullMemoryInfo, added here: it carries the VirtualQuery map as of the crash,
    // which is the direct answer to "was this address mapped" - the question an
    // apparently-empty stack is really asking.
    //   MiniDumpWithIndirectlyReferencedMemory - memory pointed TO FROM the stacks/globals
    //     (locals, this-pointers), so a walked stack does not hit unreadable gaps a debugger
    //     cannot fill in later.
    //   MiniDumpWithThreadInfo - emits the THREAD_INFO_LIST stream.
    //   MiniDumpWithFullMemoryInfo - the VirtualQuery map at crash time.
    //   MiniDumpWithDataSegs - the shim's own globals (g_pendingSurf, g_pendingLock,
    //     g_internal, g_internalThread among them), readable straight out of the dump without
    //     a matching build loaded to point a debugger at them.
    // Deliberately NOT MiniDumpWithFullMemory - that would dump the entire address space and
    // defeat the point of keeping this dump small.
    const MINIDUMP_TYPE dumpType = static_cast<MINIDUMP_TYPE>(
        MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo |
        MiniDumpWithFullMemoryInfo | MiniDumpWithDataSegs);
    const BOOL ok = write(GetCurrentProcess(), GetCurrentProcessId(), file, dumpType, mei,
                          nullptr, nullptr);
    CloseHandle(file);
    return ok != FALSE;
}

LONG WINAPI CrashFilter(EXCEPTION_POINTERS* info) {
    // Re-entrancy: a fault INSIDE this filter (the heap it walks is what is corrupted, a Log()
    // call itself faults, dbghelp misbehaves) must not recurse back into it - it must fall
    // straight through to whatever handled crashes before us.
    static volatile LONG g_crashEntered;
    if (InterlockedExchange(&g_crashEntered, 1)) return EXCEPTION_CONTINUE_SEARCH;
    if (!info || !info->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;
    const EXCEPTION_RECORD& er = *info->ExceptionRecord;

    // THE DUMP FIRST, before a single Log() call: Log heap-allocates (formatting into a
    // buffer, then a buffered file write), and if what got us here was heap corruption, that
    // allocation can itself be what finishes the process before the one piece of evidence that
    // would have explained the crash ever reaches disk.
    char path[MAX_PATH]{};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (char* slash = strrchr(path, '\\')) strcpy_s(slash + 1, 32, "sr3-rtx-crash.dmp");
    MINIDUMP_EXCEPTION_INFORMATION mei{};
    mei.ThreadId = GetCurrentThreadId();
    mei.ExceptionPointers = info;
    // FALSE: ExceptionPointers points into OUR OWN address space (we are writing the dump from
    // inside the faulting process's exception filter), not a debugger's foreign memory -
    // required or dbghelp misreads it.
    mei.ClientPointers = FALSE;
    const bool dumped = WriteMinidump(path, &mei);

    char module[MAX_PATH] = "?";
    uintptr_t offset = 0;
    HMODULE mod = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(er.ExceptionAddress), &mod) && mod) {
        GetModuleFileNameA(mod, module, MAX_PATH);
        offset = reinterpret_cast<uintptr_t>(er.ExceptionAddress) - reinterpret_cast<uintptr_t>(mod);
    }

    Log("*** CRASH: code 0x%08lX at %p (%s +0x%IX)", er.ExceptionCode,
        er.ExceptionAddress, module, offset);
    Log("    minidump %s: %s", dumped ? "written" : "FAILED", path);
    if (er.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er.NumberParameters >= 2) {
        Log("    access violation %s address %p",
            er.ExceptionInformation[0] ? "WRITING" : "reading",
            reinterpret_cast<void*>(er.ExceptionInformation[1]));
    }
    Log("    shim state: frame %u draw %u | last albedo %p (from mesh cache: %s) | "
        "%u meshes cached, %u RT textures | marker %p",
        g_frames, g_drawIndexThisFrame, static_cast<void*>(g_lastBoundAlbedo),
        g_lastAlbedoFromCache ? "YES" : "no",
        static_cast<unsigned>(g_meshAlbedo.size()), static_cast<unsigned>(g_rtTextures.size()),
        static_cast<void*>(g_marker));

    return g_prevFilter ? g_prevFilter(info) : EXCEPTION_CONTINUE_SEARCH;
}

// Re-installed periodically: whoever calls SetUnhandledExceptionFilter last wins, and the game
// and the Remix bridge both install their own well after we start. Chaining to the previous
// filter means nobody's handling is lost.
void InstallCrashFilter() {
    // g_prevFilter is captured on the FIRST install only. Re-running this every 600 frames used
    // to reassign it every time - if the game or the bridge installs its OWN filter after us at
    // some point in the process's life (this runs for the whole of it) and that filter chains
    // back to whatever was previously installed, a later re-install here could capture THAT
    // filter as "previous" and build a cycle: ours calls theirs, theirs eventually calls ours
    // again.
    static bool installedOnce = false;
    LPTOP_LEVEL_EXCEPTION_FILTER prev = SetUnhandledExceptionFilter(CrashFilter);
    if (!installedOnce) {
        if (prev != CrashFilter) g_prevFilter = prev;
        installedOnce = true;
    }
}

// ---------------------------------------------------------------- occlusion queries
//
// SR3 does its own GPU occlusion culling: it draws bounding volumes, wraps them in a
// D3DQUERYTYPE_OCCLUSION query, reads the visible-pixel count back, and skips submitting an
// object whose count came back zero. This is the readback the mode-2 comment above refers to -
// "a draw whose RESULT the engine reads can never be skipped, only hidden".
//
// `rtx.useVertexCapture = False` is a global skip of every shader draw, the depth prepass
// included, so the depth buffer those queries test against is never written. Measured
// 2026-08-23 from two frame dumps taken in the same area (camera y~147 both times):
//
//     capture ON   5330 draws/frame   (MARK 3930, CONVERT 1260, PASS 140)
//     capture OFF  1426 draws/frame   (MARK 1182, CONVERT  220, PASS  24)
//
// A 73% collapse in the game's OWN submission, matching the 77% collapse recorded for
// hiddenPassMode=2. That is the whole "objects enter the frustum and are dropped" symptom: the
// game asks whether an object is visible, is told no, and stops drawing it. Objects clipped by
// a frustum plane survive because an occlusion test on a clipped bounding box is unreliable and
// engines skip the query for those - which is exactly why only the periphery rendered.
//
// The fix does not need anything from Remix. The queries are D3D9 objects and we are the D3D9
// layer: answer them "fully visible" and the engine stops culling itself, whatever the depth
// buffer contains.
//
// ONLY D3DQUERYTYPE_OCCLUSION is touched. D3DQUERYTYPE_EVENT is a fence the game uses for frame
// pacing, and lying about one of those would break synchronisation rather than culling.
typedef HRESULT(WINAPI* CreateQuery_t)(IDirect3DDevice9*, D3DQUERYTYPE, IDirect3DQuery9**);
typedef HRESULT(WINAPI* QueryGetData_t)(IDirect3DQuery9*, void*, DWORD, DWORD);
typedef HRESULT(WINAPI* QueryIssue_t)(IDirect3DQuery9*, DWORD);
CreateQuery_t g_origCreateQuery = nullptr;
QueryGetData_t g_origQueryGetData = nullptr;
QueryIssue_t g_origQueryIssue = nullptr;
// Tracks whether a draw is being issued inside an occlusion query, and - when the answer is
// fabricated anyway - does not forward the Issue calls at all. Two bridge round trips per query
// per frame, for a query nobody will ever read.
HRESULT WINAPI Hook_QueryIssue(IDirect3DQuery9* self, DWORD flags) {
    if (self && self->GetType() == D3DQUERYTYPE_OCCLUSION) {
        if (flags & D3DISSUE_BEGIN) { g_inOcclusionQuery = true; g_curOcclusionQuery = self; }
        if (flags & D3DISSUE_END) { g_inOcclusionQuery = false; g_curOcclusionQuery = nullptr; }
        if (g_settings.forceOcclusionVisible && g_settings.skipOcclusionProxies) {
            ++g_occlIssueSkipped;
            return D3D_OK;
        }
    }
    return g_origQueryIssue(self, flags);
}

// A count large enough that no visibility threshold can read it as "occluded". Not 0xFFFFFFFF:
// an engine that scales a coverage fraction by the count would overflow on that.
constexpr DWORD kVisiblePixels = 1u << 20;

HRESULT WINAPI Hook_QueryGetData(IDirect3DQuery9* self, void* data, DWORD size, DWORD flags) {
    if (self) {
        // GetType is a plain accessor - no GPU work, no bridge round trip beyond the call
        // itself - so the type is asked per call rather than cached against a pointer that
        // could be recycled by a later query of a different type. Asked even when the override
        // is off, because "how many occlusion queries does SR3 read back per frame" is the
        // baseline that says whether this mechanism is worth anything at all.
        if (self->GetType() == D3DQUERYTYPE_OCCLUSION) {
            ++g_occlusionQueriesSeen;
            if (!g_settings.forceOcclusionVisible)
                return g_origQueryGetData(self, data, size, flags);
            ++g_occlusionForced;
            // Our own test, when it has an answer: a box entirely behind rasterised opaque
            // geometry reads "0 pixels" and the engine does not draw the object this frame.
            if (OcclusionSaysOccluded(self)) {
                ++g_occlAnswered0;
                if (!g_settings.occlusionDryRun) {
                    if (data && size >= sizeof(DWORD)) *static_cast<DWORD*>(data) = 0;
                    return D3D_OK;
                }
            }
            // The real query is deliberately NOT drained. Calling through with the game's flags
            // can carry D3DGETDATA_FLUSH, which stalls on the GPU across the 32-to-64-bit
            // bridge - the same mistake that made the instance-buffer read lock present as a
            // frame-rate collapse. Fabricating the answer costs nothing and never blocks.
            if (data && size >= sizeof(DWORD)) *static_cast<DWORD*>(data) = kVisiblePixels;
            return D3D_OK;   // always "ready", so the engine never spins waiting
        }
    }
    return g_origQueryGetData(self, data, size, flags);
}

HRESULT WINAPI Hook_CreateQuery(IDirect3DDevice9* dev, D3DQUERYTYPE type,
                                IDirect3DQuery9** returned) {
    const HRESULT hr = g_origCreateQuery(dev, type, returned);
    // D3D9 allows a null out-pointer as a "is this type supported" probe, which creates nothing.
    if (SUCCEEDED(hr) && returned && *returned) {
        ++g_queriesCreated;
        // Every IDirect3DQuery9 shares one vtable, so patching from the first one we are handed
        // covers all of them - the same pattern the vertex-buffer Lock hook uses.
        if (!g_origQueryGetData) {
            PatchVTable(*returned, kSlotQueryIssue, &Hook_QueryIssue,
                        reinterpret_cast<void**>(&g_origQueryIssue));
            PatchVTable(*returned, kSlotQueryGetData, &Hook_QueryGetData,
                        reinterpret_cast<void**>(&g_origQueryGetData));
            Log("query vtable patched from the first %s query (GetData slot %d), "
                "forceOcclusionVisible=%d",
                type == D3DQUERYTYPE_OCCLUSION ? "OCCLUSION" : "non-occlusion",
                kSlotQueryGetData, g_settings.forceOcclusionVisible ? 1 : 0);
        }
    }
    return hr;
}

void HookDevice(void* device, const char* origin, const D3DPRESENT_PARAMETERS* params) {
    static bool hooked = false;
    if (hooked || !device) return;
    hooked = true;
    g_createTick = GetTickCount();
    QueryPerformanceFrequency(&g_qpcFreq);
    SeedRenderStateDefaults();
    if (params && params->BackBufferHeight) {
        g_backAspect = static_cast<float>(params->BackBufferWidth) /
                       static_cast<float>(params->BackBufferHeight);
        g_backBufferW = params->BackBufferWidth;
        g_backBufferH = params->BackBufferHeight;
    }

    Log("%s -> hooking device (backbuffer %ux%u, aspect %.3f)", origin,
        params ? params->BackBufferWidth : 0, params ? params->BackBufferHeight : 0,
        g_backAspect);
    RemixApiInit(static_cast<IDirect3DDevice9*>(device));

    struct { int slot; void* hook; void** orig; } kHooks[] = {
        {kSlotPresent,                  &Hook_Present,                  reinterpret_cast<void**>(&g_origPresent)},
        {kSlotCreateTexture,            &Hook_CreateTexture,            reinterpret_cast<void**>(&g_origCreateTexture)},
        {kSlotSetRenderTarget,          &Hook_SetRenderTarget,          reinterpret_cast<void**>(&g_origSetRenderTarget)},
        // Candidate entry points for filling the character texture - see the fill probe.
        {kSlotUpdateSurface,            &Hook_UpdateSurface,            reinterpret_cast<void**>(&g_origUpdateSurface)},
        {kSlotUpdateTexture,            &Hook_UpdateTexture,            reinterpret_cast<void**>(&g_origUpdateTexture)},
        {kSlotStretchRect,              &Hook_StretchRect,              reinterpret_cast<void**>(&g_origStretchRect)},
        {kSlotColorFill,                &Hook_ColorFill,                reinterpret_cast<void**>(&g_origColorFill)},
        {kSlotSetTransform,             &Hook_SetTransform,             reinterpret_cast<void**>(&g_origSetTransform)},
        {kSlotSetRenderState,           &Hook_SetRenderState,           reinterpret_cast<void**>(&g_origSetRenderState)},
        {kSlotSetTexture,               &Hook_SetTexture,               reinterpret_cast<void**>(&g_origSetTexture)},
        {kSlotSetTextureStageState,     &Hook_SetTextureStageState,     reinterpret_cast<void**>(&g_origSetTextureStageState)},
        {kSlotSetSamplerState,          &Hook_SetSamplerState,          reinterpret_cast<void**>(&g_origSetSamplerState)},
        {kSlotDrawPrimitive,            &Hook_DrawPrimitive,            reinterpret_cast<void**>(&g_origDrawPrimitive)},
        {kSlotDrawIndexedPrimitive,     &Hook_DrawIndexedPrimitive,     reinterpret_cast<void**>(&g_origDrawIndexedPrimitive)},
        {kSlotDrawPrimitiveUP,          &Hook_DrawPrimitiveUP,          reinterpret_cast<void**>(&g_origDrawPrimitiveUP)},
        {kSlotDrawIndexedPrimitiveUP,   &Hook_DrawIndexedPrimitiveUP,   reinterpret_cast<void**>(&g_origDrawIndexedPrimitiveUP)},
        {kSlotSetVertexDeclaration,     &Hook_SetVertexDeclaration,     reinterpret_cast<void**>(&g_origSetVertexDeclaration)},
        {kSlotSetStreamSource,          &Hook_SetStreamSource,          reinterpret_cast<void**>(&g_origSetStreamSource)},
        {kSlotSetStreamSourceFreq,      &Hook_SetStreamSourceFreq,      reinterpret_cast<void**>(&g_origSetStreamSourceFreq)},
        {kSlotCreateVertexShader,       &Hook_CreateVertexShader,       reinterpret_cast<void**>(&g_origCreateVertexShader)},
        {kSlotSetVertexShader,          &Hook_SetVertexShader,          reinterpret_cast<void**>(&g_origSetVertexShader)},
        {kSlotSetVertexShaderConstantF, &Hook_SetVertexShaderConstantF, reinterpret_cast<void**>(&g_origSetVSConstF)},
        {kSlotCreatePixelShader,        &Hook_CreatePixelShader,        reinterpret_cast<void**>(&g_origCreatePixelShader)},
        {kSlotSetPixelShader,           &Hook_SetPixelShader,           reinterpret_cast<void**>(&g_origSetPixelShader)},
        {kSlotSetPixelShaderConstantF,  &Hook_SetPixelShaderConstantF,  reinterpret_cast<void**>(&g_origSetPSConstF)},
        {kSlotCreateQuery,              &Hook_CreateQuery,              reinterpret_cast<void**>(&g_origCreateQuery)},
    };
    for (const auto& h : kHooks) PatchVTable(device, h.slot, h.hook, h.orig);

    // After the vtable is patched, so the creation goes through the same path everything else
    // does and Hook_CreateTexture sees it (it only records render targets, which this is not).
    //
    // Gated on hiddenPassMode == 3 as of 2026-09-17: mode 3 ('bind the marker texture') is the
    // only consumer of g_marker (see HiddenDisp and BeginMark), and the deployed ini runs mode
    // 2 ('skip'). Building and uploading a 4x4 texture every launch for a mode nobody selects
    // was pure waste - every use of g_marker is already null-safe, so leaving it uncreated does
    // no harm and mode 2 now does no marker-related work at all.
    if (g_settings.hiddenPassMode == 3)
        CreateMarkerTexture(static_cast<IDirect3DDevice9*>(device));
    {
        IDirect3DDevice9* d = static_cast<IDirect3DDevice9*>(device);
        // How many lights may be lit at once. Asked rather than assumed, because assuming is what
        // broke the flashlight: lights past this limit are silently stored and never enabled.
        // D3D9 defines MaxActiveLights == 0 as "no limit".
        D3DCAPS9 caps{};
        if (SUCCEEDED(d->GetDeviceCaps(&caps))) {
            g_maxLightSlots = (caps.MaxActiveLights == 0) ? 64u
                                                          : min(64u, caps.MaxActiveLights);
            Log("device reports MaxActiveLights = %lu, using %u light slots",
                static_cast<unsigned long>(caps.MaxActiveLights), g_maxLightSlots);
            // The converted texture coordinates need a stream the game is not using. SR3 binds
            // 0-3; the highest the device allows is furthest from anything it might add.
            if (caps.MaxStreams > 1)
                g_uvStream = min(kUvStreamDefault, static_cast<DWORD>(caps.MaxStreams - 1));
            Log("device reports MaxStreams = %lu, converted texcoords go on stream %lu",
                static_cast<unsigned long>(caps.MaxStreams),
                static_cast<unsigned long>(g_uvStream));
            // Fixed-function vertex blending is the alternative to CPU skinning: put the c52
            // palette in D3DTS_WORLDMATRIX(0..63), enable indexed blending, and let the draw
            // keep its own BLENDINDICES/BLENDWEIGHT. That hands Remix real bones, which is what
            // dispatchSkinning needs before a skinned USD REPLACEMENT can attach to a character.
            // SR3 needs 64 matrices and 4 influences per vertex. Asked rather than assumed -
            // D3D9 permits a device to report 0 blend matrices and no indexed blending at all.
            Log("device reports MaxVertexBlendMatrices = %lu, MaxVertexBlendMatrixIndex = %lu, "
                "VertexProcessingCaps = 0x%08lX (SR3 needs 64 matrices, 4 influences)",
                static_cast<unsigned long>(caps.MaxVertexBlendMatrices),
                static_cast<unsigned long>(caps.MaxVertexBlendMatrixIndex),
                static_cast<unsigned long>(caps.VertexProcessingCaps));
            // The weight/index side stream needs ONE more slot beyond the uv conversion's
            // own, bound alongside stream 0 and the uv stream on the same hardware-skinned
            // draw. kSkinFFStreamDefault sits one below kUvStreamDefault so the two never
            // collide on a device with room for both; on one too constrained (MaxStreams <=
            // 7), clamping would hand them the SAME slot, which is worse than not offering
            // the feature at all - g_skinFFStreamsOk says whether there was room, and
            // SkinViaFixedFunction refuses outright (counted, like any other refusal reason)
            // rather than silently colliding with the uv stream when there was not.
            if (caps.MaxStreams > 1)
                g_skinFFStream = min(kSkinFFStreamDefault, static_cast<DWORD>(caps.MaxStreams - 1));
            g_skinFFStreamsOk = (g_skinFFStream != g_uvStream);
            Log("device reports MaxStreams = %lu, hardware-skin weights/indices go on stream "
                "%lu (%s)",
                static_cast<unsigned long>(caps.MaxStreams),
                static_cast<unsigned long>(g_skinFFStream),
                g_skinFFStreamsOk ? "distinct from the uv stream"
                                  : "COLLIDES with the uv stream - hardware skinning disabled");
        }
        if (SUCCEEDED(d->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &g_backBufferSurface)))
            Log("back buffer surface %p (cleared once a frame; the composite no longer writes it)",
                static_cast<void*>(g_backBufferSurface));   // reference deliberately kept
        else
            Log("GetBackBuffer failed - the back-buffer clear is disabled");
    }
    InstallCrashFilter();

    // Preloaded here, once, while the process is healthy - not inside CrashFilter, where
    // LoadLibraryA would take the loader lock in a process that might already be crashed. See
    // the note above g_dbghelpModule.
    if (!g_dbghelpModule) {
        g_dbghelpModule = LoadLibraryA("dbghelp.dll");
        if (g_dbghelpModule)
            g_writeDumpProc = reinterpret_cast<WriteDump_t>(
                GetProcAddress(g_dbghelpModule, "MiniDumpWriteDump"));
        Log("dbghelp.dll %s, MiniDumpWriteDump %s",
            g_dbghelpModule ? "preloaded" : "NOT FOUND (crash dumps will retry LoadLibraryA)",
            g_writeDumpProc ? "resolved" : "unresolved");
    }
    // crashTestDumpAtStart: writes ONE dump with a null exception pointer, so the writer, its
    // flags and the file handle can be verified on demand instead of waiting for the game to
    // actually crash. OFF by default - this is a diagnostic, not something to leave running.
    if (g_settings.crashTestDumpAtStart) {
        char testPath[MAX_PATH]{};
        GetModuleFileNameA(nullptr, testPath, MAX_PATH);
        if (char* slash = strrchr(testPath, '\\')) strcpy_s(slash + 1, 32, "sr3-rtx-test.dmp");
        const bool testOk = WriteMinidump(testPath, nullptr);
        Log("crashTestDumpAtStart: wrote %s: %s", testPath, testOk ? "OK" : "FAILED");
    }

    // Captured but not intercepted: called by us, never filtered.
    PatchVTable(device, kSlotGetRenderState, nullptr, reinterpret_cast<void**>(&g_origGetRenderState));
    PatchVTable(device, kSlotSetMaterial, nullptr, reinterpret_cast<void**>(&g_origSetMaterial));
    PatchVTable(device, kSlotSetLight, nullptr, reinterpret_cast<void**>(&g_origSetLight));
    PatchVTable(device, kSlotLightEnable, nullptr, reinterpret_cast<void**>(&g_origLightEnable));
    PatchVTable(device, kSlotSetIndices, &Hook_SetIndices, reinterpret_cast<void**>(&g_origSetIndices));
}

HRESULT WINAPI Hook_CreateDevice(IDirect3D9* self, UINT adapter, D3DDEVTYPE type, HWND focus,
                                 DWORD flags, D3DPRESENT_PARAMETERS* params,
                                 IDirect3DDevice9** returned) {
    const HRESULT hr = g_origCreateDevice(self, adapter, type, focus, flags, params, returned);
    if (SUCCEEDED(hr) && returned && *returned) HookDevice(*returned, "CreateDevice", params);
    else Log("CreateDevice FAILED hr=0x%08lX", hr);
    return hr;
}

HRESULT WINAPI Hook_CreateDeviceEx(IDirect3D9Ex* self, UINT adapter, D3DDEVTYPE type, HWND focus,
                                   DWORD flags, D3DPRESENT_PARAMETERS* params,
                                   D3DDISPLAYMODEEX* mode, IDirect3DDevice9Ex** returned) {
    const HRESULT hr = g_origCreateDeviceEx(self, adapter, type, focus, flags, params, mode,
                                            returned);
    if (SUCCEEDED(hr) && returned && *returned) HookDevice(*returned, "CreateDeviceEx", params);
    else Log("CreateDeviceEx FAILED hr=0x%08lX", hr);
    return hr;
}

DWORD WINAPI Init(LPVOID) {
    g_log = _fsopen("sr3-rtx.log", "w", _SH_DENYNO);   // shared so it can be tailed live
    LoadSettings();
    // The tuning window. Started through the py launcher's windowless pyw.exe with the game folder
    // and our pid; the tuner closes itself when this process exits. Once per process - Init runs
    // once, and a live reload re-reads settings without coming back here.
    if (g_settings.autoStartTuner) {
        char dir[MAX_PATH]{};
        GetModuleFileNameA(nullptr, dir, MAX_PATH);
        if (char* slash = strrchr(dir, '\\')) *slash = 0;
        char script[MAX_PATH + 32]{};
        sprintf_s(script, "%s\\sr3-tuner.pyw", dir);
        if (GetFileAttributesA(script) != INVALID_FILE_ATTRIBUTES) {
            char cmd[3 * MAX_PATH]{};
            sprintf_s(cmd, "pyw.exe \"%s\" \"%s\" %lu", script, dir, GetCurrentProcessId());
            STARTUPINFOA si{}; si.cb = sizeof(si);
            PROCESS_INFORMATION pi{};
            if (CreateProcessA(nullptr, cmd, nullptr, nullptr, FALSE, DETACHED_PROCESS, nullptr, dir,
                               &si, &pi)) {
                CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
                Log("tuner started (autoStartTuner=1): %s", script);
            } else {
                Log("tuner NOT started: CreateProcess(pyw.exe) failed, error %lu", GetLastError());
            }
        } else {
            Log("tuner NOT started: %s is missing", script);
        }
    }
    Log("sr3-rtx loaded (pid %lu) - FFP conversion, ported from sr2-rtx-remix-proxy",
        GetCurrentProcessId());
    Log("settings: ffp=%d convertSkinned=%d hiddenPassMode=%d skipUntextured=%d "
        "screenSpaceMode=%d hideLightVolumes=%d skipMirrored=%d dumpFrame=%d",
        g_settings.ffp, g_settings.convertSkinned, g_settings.hiddenPassMode,
        g_settings.skipUntextured, g_settings.screenSpaceMode, g_settings.hideLightVolumes,
        g_settings.skipMirrored, g_settings.dumpFrame);
    Log("          rankAlbedo=%d excludeRTAlbedo=%d cacheMeshAlbedo=%d injectLights=%d",
        g_settings.rankAlbedo, g_settings.excludeRTAlbedo, g_settings.cacheMeshAlbedo,
        g_settings.injectLights);
    Log("          blockEngineOutputToScreen=%d", g_settings.blockEngineOutputToScreen);

    // d3d9.dll here is the Remix bridge client. Wait for it if the game has not loaded it yet.
    HMODULE d3d9 = nullptr;
    for (int attempt = 0; attempt < 200 && !d3d9; ++attempt) {
        d3d9 = GetModuleHandleA("d3d9.dll");
        if (!d3d9) Sleep(50);
    }
    if (!d3d9) { Log("d3d9.dll never appeared - giving up"); return 0; }

    // Our probe object shares its vtable with whatever the game creates, so patching here
    // installs the hook for the game's instance too. Nothing is released: dropping the last
    // reference before the game creates its own could tear down the Remix bridge underneath us.
    auto create = reinterpret_cast<IDirect3D9*(WINAPI*)(UINT)>(
        GetProcAddress(d3d9, "Direct3DCreate9"));
    if (create) {
        if (IDirect3D9* probe = create(D3D_SDK_VERSION)) {
            const bool ok = PatchVTable(probe, kSlotCreateDevice, &Hook_CreateDevice,
                                        reinterpret_cast<void**>(&g_origCreateDevice));
            Log(ok ? "CreateDevice hook installed" : "failed to patch IDirect3D9 vtable");
        } else {
            Log("Direct3DCreate9 returned null");
        }
    }

    auto createEx = reinterpret_cast<HRESULT(WINAPI*)(UINT, IDirect3D9Ex**)>(
        GetProcAddress(d3d9, "Direct3DCreate9Ex"));
    if (createEx) {
        IDirect3D9Ex* probeEx = nullptr;
        if (SUCCEEDED(createEx(D3D_SDK_VERSION, &probeEx)) && probeEx) {
            const bool ok = PatchVTable(probeEx, kSlotCreateDeviceEx, &Hook_CreateDeviceEx,
                                        reinterpret_cast<void**>(&g_origCreateDeviceEx));
            Log(ok ? "CreateDeviceEx hook installed" : "failed to patch IDirect3D9Ex vtable");
        }
    }
    return 0;
}


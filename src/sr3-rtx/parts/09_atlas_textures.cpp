// parts/09_atlas_textures.cpp - FFP conversion intro, the atlas, texture/surface lock hooks, render-target copies.
// NOT a standalone translation unit: #included, in order, by ../sr3rtx.cpp inside its anonymous
// namespace. Originally sr3rtx.cpp lines 5934-6814 (old file:line citations map here).
// ---------------------------------------------------------------- FFP conversion

// Which texture should Remix see as this surface's base colour?

// ---------------------------------------------------------------------------------------------
// How does SR3 fill its character texture? Nothing else can be built until this is known.
//
// Settled by A/B on 2026-08-29: with `d3d9.dll` renamed away, so the game runs on Windows' own
// D3D9 with no DXVK and no Remix, THE CHARACTER IS TEXTURED CORRECTLY. Under the Remix stack the
// same texture measures all zeroes. The game, the save, the character mods and SR3's own display
// settings are all cleared, and so is every configuration knob: rtx.conf restored wholesale from
// 08-18, user.conf, `d3d9.allowDiscard`, `d3d9.apitraceMode` - each verified parsed, each inert.
//
// So SR3's writes to its 2048x1024 X8R8G8B8 D3DPOOL_DEFAULT + D3DUSAGE_DYNAMIC texture land on
// native D3D9 and do not land under DXVK. The only route left for this shim is to snoop those
// writes at the CPU boundary, BEFORE DXVK loses them, and re-upload through UpdateTexture - the
// path Remix hashes. That is the architecture already proven on the morph and instance streams.
//
// It cannot be written without knowing WHICH call the game uses. This probe answers exactly that
// and nothing else: it counts, per candidate entry point, how many touch a texture of the atlas's
// shape. Whichever is non-zero is the one to snoop; if all are zero the game fills it by a route
// none of these cover, and that is worth knowing before writing a subsystem for the wrong one.
//
// Thread safety: the game locks resources from more than one thread - proven by a crash dump and
// then measured at 272 buffer locks a frame off the render thread. So this keeps a FIXED array of
// candidate pointers, written only on the render thread and scanned without a lock. No map is
// mutated from a foreign thread, which is the bug that crashed this project once already.
constexpr unsigned kMaxAtlasPtrs = 32;
void* g_atlasPtrs[kMaxAtlasPtrs] = {};
unsigned g_atlasPtrCount = 0;

unsigned g_fillLockRect = 0, g_fillUpdateTexture = 0, g_fillUpdateSurface = 0;
unsigned g_fillStretchRect = 0, g_fillColorFill = 0;
unsigned g_fillStretchFailed = 0;   // the game's OWN StretchRect into it returning an error
unsigned g_atlasPtrOverflow = 0;    // candidates that did not fit the fixed array
unsigned g_atlasCreateReports = 0;
unsigned g_bigTexReports = 0;
unsigned g_fillLockLevels = 0;        // bitmask of mip levels seen locked
DWORD    g_fillLockFlags = 0;         // OR of every lock flag seen
unsigned g_fillReports = 0;

bool IsAtlasPtr(const void* p) {
    if (!p) return false;
    for (unsigned i = 0; i < g_atlasPtrCount; ++i)
        if (g_atlasPtrs[i] == p) return true;
    return false;
}

void NoteAtlasPtr(void* p) {
    if (!p || IsAtlasPtr(p) || g_atlasPtrCount >= kMaxAtlasPtrs) { 
        if (p && !IsAtlasPtr(p) && g_atlasPtrCount >= kMaxAtlasPtrs) ++g_atlasPtrOverflow;
        return;
    }
    g_atlasPtrs[g_atlasPtrCount++] = p;
}


// Does this SURFACE belong to the tracked atlas TEXTURE?
//
// CORRECTED 2026-08-29. The first version matched any uncompressed surface >= 256x256, which also
// matches the 2560x1440 back buffer and every HDR target - so its "StretchRect 860" counted the
// whole frame's blits, not writes to the character texture. A count that does not measure what
// its name says is worse than no count, and this project has been burned by that three times.
//
// GetContainer walks the surface back to its parent texture, which is exact. It AddRefs, so the
// result is released immediately - a probe that leaks a reference per call would pin every
// texture in the game.
bool IsAtlasSurface(IDirect3DSurface9* surf) {
    if (!surf || !g_atlasPtrCount) return false;
    IDirect3DBaseTexture9* parent = nullptr;
    if (FAILED(surf->GetContainer(__uuidof(IDirect3DTexture9), reinterpret_cast<void**>(&parent))) ||
        !parent)
        return false;
    const bool hit = IsAtlasPtr(parent);
    parent->Release();
    return hit;
}

typedef HRESULT(WINAPI* SurfLockRect_t)(IDirect3DSurface9*, D3DLOCKED_RECT*, const RECT*, DWORD);
typedef HRESULT(WINAPI* SurfUnlockRect_t)(IDirect3DSurface9*);
SurfLockRect_t g_origSurfLockRect = nullptr;
SurfUnlockRect_t g_origSurfUnlockRect = nullptr;
unsigned g_fillSurfLock = 0;

// ---------------------------------------------------------------------------------------------
// The character-atlas snoop
//
// Measured 2026-08-29, after four probes that each eliminated a route:
//
//     SURFACE LockRect 18 | texture LockRect 0 (on the atlas), UpdateTexture 0, UpdateSurface 0,
//                           StretchRect 0, ColorFill 0
//     TEXTURE FILL #3: SURFACE LockRect | flags=0x0 pitch=8192 subrect=whole   <- 2048 wide, mip 0
//     #4 pitch=4096  #5 2048  #6 1024  #7 512  #8 256                          <- the mip chain
//
// SR3 fills the character texture by taking each mip level's SURFACE with GetSurfaceLevel and
// locking THAT - `IDirect3DSurface9::LockRect`, a different vtable from the texture's own method,
// which is why the first three fill probes saw nothing. The lock is whole-surface with flags 0:
// a plain preserving write.
//
// Those writes reach the game's own D3D9 correctly - with `d3d9.dll` renamed away the character
// is textured - and do not survive into the image Remix samples, which measures all zeroes
// through a GPU read whose control returned 126.7 of an expected 126.7.
//
// So the shim takes the pixels at the CPU boundary, where they demonstrably exist, and re-uploads
// them through SYSTEMMEM + UpdateTexture - the path Remix hashes, as the cloth generator's note
// says. Same architecture as the morph and instance stream snoop, applied to a texture.
//
// Only MIP 0 is captured. Remix samples the top level for a material and 2048x1024x4 is 8 MB per
// character; capturing the whole chain would add a third for nothing.
struct AtlasSnoop {
    IDirect3DBaseTexture9* tex = nullptr;   // parent texture, the key
    UINT w = 0, h = 0;
    std::vector<unsigned char> pixels;      // BGRA, mip 0
    bool fresh = false;                     // written since the copy was last built
    IDirect3DTexture9* copy = nullptr;      // what gets bound instead
    double mean = -1.0;
    unsigned version = 0;                   // bumped on every capture; keys SkinAlbedo's bake
};
constexpr unsigned kMaxAtlasSnoop = 8;
AtlasSnoop g_atlasSnoop[kMaxAtlasSnoop];
unsigned g_atlasSnoopCount = 0;

// One pending lock per SURFACE. The game locks resources from more than one thread - proven by a
// crash dump and measured at 272 buffer locks a frame - so this is a small fixed array guarded by
// its own critical section, never a map resized from a foreign thread.
struct PendingSurfLock {
    IDirect3DSurface9* surf = nullptr;
    IDirect3DBaseTexture9* tex = nullptr;
    void* bits = nullptr;
    int pitch = 0;
    UINT w = 0, h = 0;
};
constexpr unsigned kMaxPendingSurf = 8;
PendingSurfLock g_pendingSurf[kMaxPendingSurf];
CRITICAL_SECTION g_atlasCs;
bool g_atlasCsReady = false;

unsigned g_atlasCaptured = 0;      // mip-0 captures completed
unsigned g_atlasCopyBuilt = 0;     // uploads handed to Remix
unsigned g_atlasBound = 0;         // draws/frame that bound a snooped copy
unsigned g_atlasCaptureFailed = 0;
unsigned g_atlasSrcUnmapped = 0;   // snoopValidateSource: source range failed the
                                    // VirtualQuery check, copy skipped
unsigned g_atlasSrcException = 0;  // copy loop raised a structured exception despite
                                    // passing validation
// A second LockRect on a surface that already had an unconsumed pending entry queued - see
// the note in Hook_SurfLockRect. This IS the September 14 defect: non-zero means an unlock
// was skipped somewhere and its entry would have been orphaned before the overwrite-in-place
// fix there made that merely wasteful instead of fatal.
unsigned g_atlasPendingOrphans = 0;
unsigned g_atlasDumps = 0;

// ---------------------------------------------------------------------------------------------
// Where the character pixels actually are
//
// The snoop proved the game writes ZEROES into its 2048x1024 dynamic texture: captured straight
// out of the game's own mapped memory at Unlock, mean 0.0, while a 1280x768 texture captured the
// same way read 14.3. So nothing downstream was losing the pixels - they were never written.
//
// But the character IS correct without Remix. So the compositor produces zeroes only here, and
// the obvious suspect is the step before it: the game creates a 2048x1024 RENDER TARGET of the
// same size (BIG TEXTURE #3: fmt=21 usage=0x1). A GPU composite into that, read back and written
// into the dynamic texture, would emit zeroes if the READBACK is what fails under DXVK.
//
// This reads every large uncompressed render target ONCE with the path whose control already
// passed (StretchRect into our own target, then GetRenderTargetData - SelfTestCopyPath returned
// 126.7 of an expected 126.7). If one of them holds a character, the pixels exist on the GPU and
// the shim can bind THAT instead. If they are all empty, the composite never ran.
constexpr unsigned kMaxRtProbe = 12;
IDirect3DTexture9* g_rtProbe[kMaxRtProbe] = {};
UINT g_rtProbeW[kMaxRtProbe] = {}, g_rtProbeH[kMaxRtProbe] = {};
double g_rtProbeMean[kMaxRtProbe];
bool g_rtProbeDone[kMaxRtProbe] = {};
unsigned g_rtProbeAttempts[kMaxRtProbe] = {};
unsigned g_rtProbeCount = 0;
unsigned g_rtProbeRead = 0;
unsigned g_rtProbeFailed = 0, g_rtProbeFailReports = 0;

void NoteRenderTargetCandidate(IDirect3DTexture9* tex, UINT w, UINT h) {
    if (!tex || g_rtProbeCount >= kMaxRtProbe) return;
    g_rtProbe[g_rtProbeCount] = tex;
    g_rtProbeW[g_rtProbeCount] = w;
    g_rtProbeH[g_rtProbeCount] = h;
    g_rtProbeMean[g_rtProbeCount] = -1.0;
    ++g_rtProbeCount;
}

void AtlasCsInit() {
    if (!g_atlasCsReady) { InitializeCriticalSection(&g_atlasCs); g_atlasCsReady = true; }
}

AtlasSnoop* FindSnoop(IDirect3DBaseTexture9* tex) {
    for (unsigned i = 0; i < g_atlasSnoopCount; ++i)
        if (g_atlasSnoop[i].tex == tex) return &g_atlasSnoop[i];
    return nullptr;
}

typedef HRESULT(WINAPI* TexLockRect_t)(IDirect3DTexture9*, UINT, D3DLOCKED_RECT*, const RECT*, DWORD);
typedef HRESULT(WINAPI* TexUnlockRect_t)(IDirect3DTexture9*, UINT);
typedef HRESULT(WINAPI* UpdateSurface_t)(IDirect3DDevice9*, IDirect3DSurface9*, const RECT*,
                                         IDirect3DSurface9*, const POINT*);
typedef HRESULT(WINAPI* UpdateTexture_t)(IDirect3DDevice9*, IDirect3DBaseTexture9*,
                                         IDirect3DBaseTexture9*);
typedef HRESULT(WINAPI* StretchRect_t)(IDirect3DDevice9*, IDirect3DSurface9*, const RECT*,
                                       IDirect3DSurface9*, const RECT*, D3DTEXTUREFILTERTYPE);
typedef HRESULT(WINAPI* ColorFill_t)(IDirect3DDevice9*, IDirect3DSurface9*, const RECT*, D3DCOLOR);
TexLockRect_t g_origTexLockRect = nullptr;
TexUnlockRect_t g_origTexUnlockRect = nullptr;
UpdateSurface_t g_origUpdateSurface = nullptr;
UpdateTexture_t g_origUpdateTexture = nullptr;
StretchRect_t g_origStretchRect = nullptr;
ColorFill_t g_origColorFill = nullptr;

// Defined below, with AtlasSourceRangeReadable and CopyAtlasRowsGuarded (the atlas capture's
// validated-and-guarded copy) - forward-declared here because Hook_TexUnlockRect, just below,
// needs to consume a pending atlas entry too, and both unlock paths share exactly the same
// consume-and-copy logic. See the note above Hook_TexUnlockRect for why a second entry point
// needs it at all.
void ConsumeAtlasPendingLock(const PendingSurfLock& p);

HRESULT WINAPI Hook_TexLockRect(IDirect3DTexture9* self, UINT level, D3DLOCKED_RECT* rect,
                                const RECT* r, DWORD flags) {
    const HRESULT hr = g_origTexLockRect(self, level, rect, r, flags);
    if (!InternalHere() && SUCCEEDED(hr) && IsAtlasPtr(self)) {
        ++g_fillLockRect;
        if (level < 32) g_fillLockLevels |= (1u << level);
        g_fillLockFlags |= flags;
        if (g_fillReports < 6) {
            ++g_fillReports;
            Log("TEXTURE FILL #%u: LockRect on the character texture | level=%u flags=0x%08lX "
                "pitch=%d subrect=%s", g_fillReports, level, flags,
                rect ? rect->Pitch : 0, r ? "yes" : "whole");
        }
    }
    return hr;
}

HRESULT WINAPI Hook_TexUnlockRect(IDirect3DTexture9* self, UINT level) {
    // A surface-locked / texture-unlocked pair - LockRect through the SURFACE vtable (which is
    // what queues the g_pendingSurf entry, see Hook_SurfLockRect) followed by UnlockRect through
    // the TEXTURE vtable instead of the surface's own - left that entry with nobody to ever
    // consume it. Before the overwrite-in-place fix in Hook_SurfLockRect that orphan just sat
    // until the NEXT lock of the same surface silently replaced it; now it is consumed HERE, the
    // moment we see the texture-side unlock, so the capture this lock actually did is not lost.
    if (level == 0 && IsAtlasPtr(self)) {
        IDirect3DSurface9* s = nullptr;
        if (SUCCEEDED(self->GetSurfaceLevel(0, &s)) && s) {
            PendingSurfLock p{};
            if (g_atlasCsReady) {
                EnterCriticalSection(&g_atlasCs);
                for (unsigned i = 0; i < kMaxPendingSurf; ++i) {
                    if (g_pendingSurf[i].surf == s) {
                        p = g_pendingSurf[i];
                        g_pendingSurf[i] = PendingSurfLock{};
                        break;
                    }
                }
                LeaveCriticalSection(&g_atlasCs);
            }
            ConsumeAtlasPendingLock(p);
            s->Release();
        }
    }
    return g_origTexUnlockRect(self, level);
}

// The surface-level lock. IsAtlasSurface walks the surface back to its parent texture, so this
// fires only for a surface belonging to a tracked atlas.
IDirect3DBaseTexture9* AtlasParent(IDirect3DSurface9* surf) {
    if (!surf || !g_atlasPtrCount) return nullptr;
    IDirect3DBaseTexture9* parent = nullptr;
    if (FAILED(surf->GetContainer(__uuidof(IDirect3DTexture9),
                                  reinterpret_cast<void**>(&parent))) || !parent)
        return nullptr;
    const bool hit = IsAtlasPtr(parent);
    parent->Release();          // borrowed: the texture outlives the lock by construction
    return hit ? parent : nullptr;
}

HRESULT WINAPI Hook_SurfLockRect(IDirect3DSurface9* self, D3DLOCKED_RECT* rect, const RECT* r,
                                 DWORD flags) {
    const HRESULT hr = g_origSurfLockRect(self, rect, r, flags);
    if (InternalHere() || FAILED(hr) || !rect || !rect->pBits) return hr;
    IDirect3DBaseTexture9* parent = AtlasParent(self);
    if (!parent) return hr;

    ++g_fillSurfLock;
    g_fillLockFlags |= flags;

    // Only the TOP level is captured, identified by matching the parent's level-0 size. A
    // sub-rect lock is refused rather than partially captured: a partial copy of a texture we
    // then hand to Remix as complete would be a worse bug than the one being fixed.
    D3DSURFACE_DESC sd{};
    if (FAILED(self->GetDesc(&sd))) return hr;
    D3DSURFACE_DESC td{};
    if (FAILED(static_cast<IDirect3DTexture9*>(parent)->GetLevelDesc(0, &td))) return hr;
    const bool topLevel = (sd.Width == td.Width && sd.Height == td.Height);

    if (g_fillReports < 8) {
        ++g_fillReports;
        Log("TEXTURE FILL #%u: SURFACE LockRect on the character texture | flags=0x%08lX "
            "pitch=%d %ux%u %s", g_fillReports, flags, rect->Pitch, sd.Width, sd.Height,
            topLevel ? "TOP LEVEL" : "mip");
    }
    if (!topLevel || r) return hr;

    AtlasCsInit();
    EnterCriticalSection(&g_atlasCs);
    // A second lock of the same surface before the first was ever unlocked means the matching
    // Hook_SurfUnlockRect never ran - the main-thread unlock that should have consumed the
    // first entry was skipped (the g_internal race InternalHere fixes), leaving it here with a
    // bits pointer the real Unlock has already invalidated. Appending a SECOND entry let a
    // later Hook_SurfUnlockRect find the STALE one FIRST and copy from a dead pointer - the
    // September 14 crash. Overwriting in place instead means there is only ever one entry per
    // surface, so there is nothing stale left to find.
    bool overwroteExisting = false;
    for (unsigned i = 0; i < kMaxPendingSurf; ++i) {
        if (g_pendingSurf[i].surf == self) {
            g_pendingSurf[i] = {self, parent, rect->pBits, rect->Pitch, sd.Width, sd.Height};
            ++g_atlasPendingOrphans;
            overwroteExisting = true;
            break;
        }
    }
    if (!overwroteExisting) {
        for (unsigned i = 0; i < kMaxPendingSurf; ++i) {
            if (g_pendingSurf[i].surf == nullptr) {
                g_pendingSurf[i] = {self, parent, rect->pBits, rect->Pitch, sd.Width, sd.Height};
                break;
            }
        }
    }
    LeaveCriticalSection(&g_atlasCs);
    return hr;
}

// snoopValidateSource: p.bits is a raw surface pointer captured back in Hook_SurfLockRect
// and held in g_pendingSurf across the lock/unlock gap. A crash dump measured 2026-09-14
// faulted a memmove reading exactly that pointer during world load - the surface had gone
// unmapped (a DISCARD rename, a lost device, another thread, the surface simply freed)
// between the lock and this unlock, and the old unconditional copy walked straight into
// the hole and took the whole process down. This subsystem only re-uploads a DIAGNOSTIC
// copy of the character atlas; it must never be able to do that. So the ENTIRE range the
// copy loop is about to read - src through the last byte of the last row - is checked in
// one pass, with VirtualQuery, before a single byte is touched. onFailAddr and onFailInfo,
// if given, receive the address and the full MEMORY_BASIC_INFORMATION of the first page that
// failed, so a false refusal can be told apart from a genuinely unmapped range.
//
// Shared with Hook_VBUnlock's vertex-buffer copy (the identical defect, just never guarded at
// all until then): the VirtualQuery walk itself lives in MemoryRangeReadable below, which
// knows nothing about rows or pixels, only a flat byte range. This function computes that
// range from the atlas's width/height/pitch and delegates.
static bool MemoryRangeReadable(const void* src, size_t len, const void** onFailAddr,
                                 MEMORY_BASIC_INFORMATION* onFailInfo) {
    if (len == 0) return true;
    const unsigned char* cur = static_cast<const unsigned char*>(src);
    const unsigned char* const rangeEnd = cur + len;
    while (cur < rangeEnd) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(cur, &mbi, sizeof(mbi)) != sizeof(mbi)) {
            if (onFailAddr) *onFailAddr = cur;
            if (onFailInfo) *onFailInfo = MEMORY_BASIC_INFORMATION{};
            return false;
        }
        // PAGE_NOCACHE (0x200) and PAGE_WRITECOMBINE (0x400) are how driver-mapped lock memory
        // often comes back, and the old mask (PAGE_GUARD only) made either of them compare
        // unequal to every protection constant below, so a page that was genuinely readable was
        // refused. All three are masked off before the compare; none of them changes whether
        // the page can be READ.
        const DWORD prot = mbi.Protect &
            ~static_cast<DWORD>(PAGE_GUARD | PAGE_NOCACHE | PAGE_WRITECOMBINE);
        const bool readable = mbi.State == MEM_COMMIT && !(mbi.Protect & PAGE_GUARD) &&
            (prot == PAGE_READONLY || prot == PAGE_READWRITE || prot == PAGE_WRITECOPY ||
             prot == PAGE_EXECUTE_READ || prot == PAGE_EXECUTE_READWRITE ||
             prot == PAGE_EXECUTE_WRITECOPY);
        if (!readable) {
            if (onFailAddr) *onFailAddr = cur;
            if (onFailInfo) *onFailInfo = mbi;
            return false;
        }
        cur = static_cast<unsigned char*>(mbi.BaseAddress) + mbi.RegionSize;
    }
    return true;
}

static bool AtlasSourceRangeReadable(const unsigned char* src, UINT w, UINT h, int pitch,
                                      const void** onFailAddr,
                                      MEMORY_BASIC_INFORMATION* onFailInfo) {
    if (w == 0 || h == 0) return true;
    const size_t len = static_cast<size_t>(h - 1) * static_cast<size_t>(pitch) +
                       static_cast<size_t>(w) * 4;
    return MemoryRangeReadable(src, len, onFailAddr, onFailInfo);
}

// The __try/__except itself, isolated in its own function with plain parameters and no C++
// objects in scope. MSVC forbids __try in a function that also requires object unwinding
// (error C2712): Hook_SurfUnlockRect (by way of ConsumeAtlasPendingLock, below) holds a
// std::vector and a critical-section lock/unlock pair, and Hook_VBUnlock holds an
// InstanceCache& and more vectors, so the __try cannot live in either of them directly. This
// is that one guard, SHARED rather than duplicated between the two copies - not a replacement
// for AtlasSourceRangeReadable/MemoryRangeReadable above: a range that passes that check can
// still be unmapped a microsecond later, and this catches exactly that race instead of
// crashing on it.
static bool CopyRangeGuarded(void* dst, const void* src, size_t len) {
    __try {
        memcpy(dst, src, len);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The atlas copy is not one flat range but h rows of w*4 bytes each, strided by pitch, so this
// calls the shared guard once per row rather than once for the whole surface.
static bool CopyAtlasRowsGuarded(unsigned char* dstPixels, const unsigned char* src,
                                  UINT w, UINT h, int pitch) {
    for (UINT y = 0; y < h; ++y) {
        if (!CopyRangeGuarded(dstPixels + static_cast<size_t>(y) * w * 4,
                              src + static_cast<size_t>(y) * pitch,
                              static_cast<size_t>(w) * 4))
            return false;
    }
    return true;
}

// Shared by Hook_SurfUnlockRect and Hook_TexUnlockRect - the game unlocks a mip-0 atlas
// surface through either vtable, and both need to consume whatever Hook_SurfLockRect queued
// for it. p is the already-popped entry (zeroed when nothing was pending); this does the
// actual validated copy into the snoop cache and the diagnostic dumps.
void ConsumeAtlasPendingLock(const PendingSurfLock& p) {
    if (!p.surf || !p.bits || p.pitch <= 0) return;

    // Copy BEFORE unlocking - after the unlock the pointer is no longer the game's to give.
    try {
        EnterCriticalSection(&g_atlasCs);
        AtlasSnoop* e = FindSnoop(p.tex);
        if (!e && g_atlasSnoopCount < kMaxAtlasSnoop) {
            e = &g_atlasSnoop[g_atlasSnoopCount++];
            e->tex = p.tex;
        }
        if (e) {
            e->w = p.w; e->h = p.h;
            const size_t need = static_cast<size_t>(p.w) * p.h * 4;
            if (e->pixels.size() != need) e->pixels.assign(need, 0);
            const unsigned char* src = static_cast<const unsigned char*>(p.bits);
            // snoopValidateSource (default on): p.bits can go stale between the matching
            // lock and this unlock (see AtlasSourceRangeReadable above) - validate the
            // whole range before trusting it, then guard the copy itself regardless,
            // since a range that just passed validation can still be unmapped a
            // microsecond later.
            bool copied = false;
            const void* failAddr = nullptr;
            MEMORY_BASIC_INFORMATION failInfo{};
            const bool rangeOk = !g_settings.snoopValidateSource ||
                AtlasSourceRangeReadable(src, p.w, p.h, p.pitch, &failAddr, &failInfo);
            if (!rangeOk) {
                ++g_atlasSrcUnmapped;
                if (g_atlasSrcUnmapped <= 4)
                    Log("ATLAS SNOOP: SKIPPED a stale/unmapped source range before "
                        "copying - surf=%p src=%p w=%u h=%u pitch=%d, failing page at %p "
                        "state=%#lx protect=%#lx allocBase=%p",
                        static_cast<void*>(p.surf), static_cast<const void*>(src), p.w,
                        p.h, p.pitch, failAddr, failInfo.State, failInfo.Protect,
                        failInfo.AllocationBase);
            } else {
                copied = CopyAtlasRowsGuarded(e->pixels.data(), src, p.w, p.h, p.pitch);
                if (!copied) ++g_atlasSrcException;
            }
            if (copied) {
                // The falsifier: what did the game actually write? A non-zero mean here proves
                // the pixels exist at this boundary, which every read further down the stack
                // denied.
                double acc = 0.0; unsigned n = 0;
                for (UINT y = 0; y < p.h; y += 8)
                    for (UINT x = 0; x < p.w; x += 8) {
                        const unsigned char* q = &e->pixels[(static_cast<size_t>(y) * p.w + x) * 4];
                        acc += q[0] + q[1] + q[2]; ++n;
                    }
                e->mean = n ? acc / (3.0 * n) : 0.0;
                e->fresh = true;
                ++e->version;
                ++g_atlasCaptured;
                if (g_atlasCaptured <= 4)
                    Log("ATLAS CAPTURED #%u: %ux%u from the game's own write | mean %.1f of 255",
                        g_atlasCaptured, p.w, p.h, e->mean);
                // Write the largest capture out so it can be LOOKED AT. The user reports the
                // atlas has "some stuff in it but the skin part is black", and a mean of 3.8
                // against a fully composited 182.7 says the same thing without saying WHICH
                // region is missing. A picture does. Bounded: the two biggest captures, once
                // each, ~6 MB apiece. The character atlas is the 2048x1024 one; 1280x768 is
                // the title screen (dumped and identified 2026-08-31), so require both
                // dimensions to be large.
                if (p.w >= 2048 && p.h >= 1024) WriteAtlasDdsOnce(p.w, p.h, e->pixels.data());
                if (p.w >= 1024 && g_atlasDumps < 2) {
                    ++g_atlasDumps;
                    std::vector<unsigned char> rgb;
                    try {
                        rgb.resize(static_cast<size_t>(p.w) * p.h * 3);
                        for (size_t i = 0, n = static_cast<size_t>(p.w) * p.h; i < n; ++i) {
                            rgb[i * 3 + 0] = e->pixels[i * 4 + 2];   // BGRA -> RGB
                            rgb[i * 3 + 1] = e->pixels[i * 4 + 1];
                            rgb[i * 3 + 2] = e->pixels[i * 4 + 0];
                        }
                        char tag[48];
                        sprintf_s(tag, "atlas%u", g_atlasDumps);
                        DumpRaw(tag, 0, p.w, p.h, rgb.data());
                    } catch (...) {
                    }
                }
            }
        }
        LeaveCriticalSection(&g_atlasCs);
    } catch (...) {
        ++g_atlasCaptureFailed;
        LeaveCriticalSection(&g_atlasCs);
    }
}

// The write has landed in the game's own mapped memory by now, so this is where it is copied.
HRESULT WINAPI Hook_SurfUnlockRect(IDirect3DSurface9* self) {
    PendingSurfLock p{};
    // The !g_internal guard used to be here too: a main-thread unlock landing while the render
    // thread held g_internal true (across the whole of BeginFFP) skipped this entirely,
    // orphaning the entry - see g_internalThread/InternalHere and the note in
    // Hook_SurfLockRect. Dropped, so a matching entry is ALWAYS consumed: our own internal
    // unlocks are on OUR OWN staging surfaces, never on one the game itself queued here, so
    // this cannot mis-fire.
    if (g_atlasCsReady) {
        EnterCriticalSection(&g_atlasCs);
        for (unsigned i = 0; i < kMaxPendingSurf; ++i) {
            if (g_pendingSurf[i].surf == self) {
                p = g_pendingSurf[i];
                g_pendingSurf[i] = PendingSurfLock{};
                break;
            }
        }
        LeaveCriticalSection(&g_atlasCs);
    }
    ConsumeAtlasPendingLock(p);
    return g_origSurfUnlockRect(self);
}

// Read one candidate render target per frame and record what it holds. Render thread only.
void ProbeRenderTargetContents(IDirect3DDevice9* dev) {
    if (!dev) return;
    // ROUND-ROBIN, FOREVER, keeping the MAXIMUM.
    //
    // The first version read each candidate ONCE and marked it done. It ran one per frame from
    // frame 1, so every read landed during startup and loading - before anything had been drawn -
    // and all ten reported 0.0, including the 2560x1440 scene targets that certainly hold the
    // rendered world. The measurement said nothing about the character and everything about when
    // it was taken.
    //
    // That is the third time on this bug that the right quantity was measured at the wrong TIME:
    // compositeToTexturePass counted a population in a frame that could not contain it, the fill
    // probe hooked LockRect after the fill had already happened, and now this. A one-shot read of
    // a surface whose contents change is worth nothing; only the maximum over time is.
    // BOUNDED, 2026-08-29. The first round-robin version FROZE THE GAME right after the intro
    // logos: it allocated a CreateOffscreenPlainSurface per read - 14.7 MB for each 2560x1440
    // candidate - and did a full GPU sync every four frames, forever, in a 32-bit address space
    // shared with the game, Remix's client and a 24 MB skinning ring. That is precisely the
    // failure already recorded in YOUR-INSTRUCTIONS.md, where pre-sizing the snoop buffers
    // "exhausted a 32-bit address space and killed the game".
    //
    // A shim must degrade, not abort its host, and a DIAGNOSTIC least of all. So:
    //   * a total budget of reads for the whole session, not a rate;
    //   * one read every 120 frames, roughly two seconds;
    //   * surfaces larger than the character atlas are read only a couple of times, purely as
    //     the control for whether GetRenderTargetData can see game-written surfaces at all.
    constexpr unsigned kRtProbeBudget = 60;
    constexpr unsigned kRtProbeInterval = 120;
    if (!g_rtProbeCount || g_rtProbeRead >= kRtProbeBudget) return;
    if (g_frames % kRtProbeInterval) return;
    const unsigned i = (g_frames / kRtProbeInterval) % g_rtProbeCount;
    {
        // The big scene targets cost 14.7 MB a read and are only the control; a few is plenty.
        if (g_rtProbeW[i] > 2048 && g_rtProbeAttempts[i] >= 3) return;
        ++g_rtProbeAttempts[i];
        IDirect3DTexture9* src = g_rtProbe[i];
        const UINT w = g_rtProbeW[i], h = g_rtProbeH[i];
        IDirect3DSurface9* srcSurf = nullptr;
        IDirect3DSurface9* sysSurf = nullptr;
        const bool wasInternal = g_internal;
        g_internal = true;
        g_internalThread = GetCurrentThreadId();
        // Each step reports its own failure. The previous version counted only SUCCESSFUL reads,
        // so a GetRenderTargetData that refused every surface would have printed "0 reads" -
        // indistinguishable from "the probe never ran", which is exactly the ambiguity that
        // wasted the run before this one.
        HRESULT hrSurf = E_FAIL, hrPlain = E_FAIL, hrGet = E_FAIL, hrLock = E_FAIL;
        try {
            hrSurf = src->GetSurfaceLevel(0, &srcSurf);
            if (SUCCEEDED(hrSurf) && srcSurf)
                hrPlain = dev->CreateOffscreenPlainSurface(w, h, D3DFMT_A8R8G8B8,
                                                           D3DPOOL_SYSTEMMEM, &sysSurf, nullptr);
            if (SUCCEEDED(hrPlain) && sysSurf)
                hrGet = dev->GetRenderTargetData(srcSurf, sysSurf);
            if (SUCCEEDED(hrGet)) {
                D3DLOCKED_RECT lr{};
                hrLock = sysSurf->LockRect(&lr, nullptr, D3DLOCK_READONLY);
                if (SUCCEEDED(hrLock) && lr.pBits) {
                    double acc = 0.0; unsigned n = 0;
                    for (UINT y = 0; y < h; y += 8) {
                        const unsigned char* row = static_cast<const unsigned char*>(lr.pBits) +
                                                   static_cast<size_t>(y) * lr.Pitch;
                        for (UINT x = 0; x < w; x += 8) {
                            acc += row[x * 4 + 0] + row[x * 4 + 1] + row[x * 4 + 2];
                            ++n;
                        }
                    }
                    const double mean = n ? acc / (3.0 * n) : 0.0;
                    ++g_rtProbeRead;
                    // Only an INCREASE is reported, so the log shows each surface's best content
                    // rather than thousands of repeats.
                    if (mean > g_rtProbeMean[i] + 0.5) {
                        g_rtProbeMean[i] = mean;
                        if (!g_rtProbeDone[i] || mean > 1.0) {
                            g_rtProbeDone[i] = true;
                            Log("RT CONTENT #%u: %ux%u render target | mean rose to %.1f of 255 "
                                "at frame %u  <- non-zero means the pixels exist on the GPU here",
                                i + 1, w, h, mean, g_frames);
                        }
                    }
                    sysSurf->UnlockRect();
                }
            }
        } catch (...) {
        }
        if (FAILED(hrGet) || FAILED(hrLock)) {
            ++g_rtProbeFailed;
            if (g_rtProbeFailReports < 6) {
                ++g_rtProbeFailReports;
                Log("RT CONTENT FAILED #%u: %ux%u | GetSurfaceLevel=0x%08lX "
                    "CreateOffscreenPlain=0x%08lX GetRenderTargetData=0x%08lX LockRect=0x%08lX "
                    "  <- the read tool cannot see this surface, so any zero it reports is "
                    "meaningless", g_rtProbeFailReports, w, h, hrSurf, hrPlain, hrGet, hrLock);
            }
        }
        if (sysSurf) sysSurf->Release();
        if (srcSurf) srcSurf->Release();
        g_internal = wasInternal;
    }
}

// Build (or refresh) the texture handed to Remix from the snooped pixels. Render thread only.
// The snooped CPU pixels of a composited texture (BGRA mip 0) and their capture version, for
// SkinAlbedo: NPC heads draw from these composited textures too, which a lock cannot read.
bool AtlasPixelsFor(IDirect3DBaseTexture9* tex, std::vector<unsigned char>& out, UINT& w, UINT& h,
                    unsigned& version) {
    if (!g_atlasCsReady) return false;
    EnterCriticalSection(&g_atlasCs);
    AtlasSnoop* e = FindSnoop(tex);
    bool ok = false;
    if (e && !e->pixels.empty()) {
        try { out = e->pixels; w = e->w; h = e->h; version = e->version; ok = true; } catch (...) {}
    }
    LeaveCriticalSection(&g_atlasCs);
    return ok;
}

IDirect3DTexture9* AtlasCopyFor(IDirect3DDevice9* dev, IDirect3DBaseTexture9* tex) {
    if (!g_atlasCsReady) return nullptr;
    EnterCriticalSection(&g_atlasCs);
    AtlasSnoop* e = FindSnoop(tex);
    if (!e || (!e->fresh && !e->copy) || e->pixels.empty()) {
        IDirect3DTexture9* existing = e ? e->copy : nullptr;
        LeaveCriticalSection(&g_atlasCs);
        return existing;
    }
    if (!e->fresh) { IDirect3DTexture9* c = e->copy; LeaveCriticalSection(&g_atlasCs); return c; }

    const UINT w = e->w, h = e->h;
    std::vector<unsigned char> local;
    try { local = e->pixels; } catch (...) { LeaveCriticalSection(&g_atlasCs); return e->copy; }
    e->fresh = false;
    IDirect3DTexture9* old = e->copy;
    LeaveCriticalSection(&g_atlasCs);

    IDirect3DTexture9* staging = nullptr;
    IDirect3DTexture9* made = nullptr;
    const bool wasInternal = g_internal;
    g_internal = true;
    g_internalThread = GetCurrentThreadId();
    try {
        if (SUCCEEDED(dev->CreateTexture(w, h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM,
                                         &staging, nullptr)) && staging) {
            D3DLOCKED_RECT dst{};
            if (SUCCEEDED(staging->LockRect(0, &dst, nullptr, 0)) && dst.pBits) {
                for (UINT y = 0; y < h; ++y) {
                    unsigned char* drow = static_cast<unsigned char*>(dst.pBits) +
                                          static_cast<size_t>(y) * dst.Pitch;
                    const unsigned char* srow = &local[static_cast<size_t>(y) * w * 4];
                    for (UINT x = 0; x < w; ++x) {
                        drow[x * 4 + 0] = srow[x * 4 + 0];
                        drow[x * 4 + 1] = srow[x * 4 + 1];
                        drow[x * 4 + 2] = srow[x * 4 + 2];
                        drow[x * 4 + 3] = 255;   // the source is X8R8G8B8; alpha is undefined
                    }
                }
                staging->UnlockRect(0);
            }
            // UpdateTexture is the upload Remix hashes - the whole point of the exercise.
            if (SUCCEEDED(dev->CreateTexture(w, h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                                             &made, nullptr)) && made) {
                if (FAILED(dev->UpdateTexture(staging, made))) { made->Release(); made = nullptr; }
            }
            staging->Release();
        }
    } catch (...) { made = nullptr; }
    g_internal = wasInternal;

    if (!made) return old;
    ++g_atlasCopyBuilt;
    EnterCriticalSection(&g_atlasCs);
    e->copy = made;
    LeaveCriticalSection(&g_atlasCs);
    if (old) old->Release();
    return made;
}
// Patch the shared IDirect3DTexture9 vtable ONCE. Every texture shares it, so a second patch
// would store our own hook as the "original" and the hook would call itself.
void InstallTextureHooks(IDirect3DTexture9* tex) {
    static bool done = false;
    if (done || !tex) return;
    done = true;
    PatchVTable(tex, kSlotTexLockRect, &Hook_TexLockRect,
                reinterpret_cast<void**>(&g_origTexLockRect));
    PatchVTable(tex, kSlotTexUnlockRect, &Hook_TexUnlockRect,
                reinterpret_cast<void**>(&g_origTexUnlockRect));
    // The surface vtable is shared too, and is reached through any texture's level 0.
    IDirect3DSurface9* surf = nullptr;
    if (SUCCEEDED(tex->GetSurfaceLevel(0, &surf)) && surf) {
        PatchVTable(surf, kSlotSurfLockRect, &Hook_SurfLockRect,
                    reinterpret_cast<void**>(&g_origSurfLockRect));
        PatchVTable(surf, kSlotSurfUnlockRect, &Hook_SurfUnlockRect,
                    reinterpret_cast<void**>(&g_origSurfUnlockRect));
        surf->Release();
    }
}

HRESULT WINAPI Hook_UpdateSurface(IDirect3DDevice9* dev, IDirect3DSurface9* src, const RECT* sr,
                                  IDirect3DSurface9* dst, const POINT* dp) {
    if (!InternalHere() && IsAtlasSurface(dst)) ++g_fillUpdateSurface;
    return g_origUpdateSurface(dev, src, sr, dst, dp);
}

HRESULT WINAPI Hook_UpdateTexture(IDirect3DDevice9* dev, IDirect3DBaseTexture9* src,
                                  IDirect3DBaseTexture9* dst) {
    if (!InternalHere() && IsAtlasPtr(dst)) ++g_fillUpdateTexture;
    return g_origUpdateTexture(dev, src, dst);
}

HRESULT WINAPI Hook_StretchRect(IDirect3DDevice9* dev, IDirect3DSurface9* src, const RECT* sr,
                                IDirect3DSurface9* dst, const RECT* dr,
                                D3DTEXTUREFILTERTYPE filter) {
    const HRESULT hr = g_origStretchRect(dev, src, sr, dst, dr, filter);
    if (!InternalHere() && IsAtlasSurface(dst)) {
        ++g_fillStretchRect;
        if (FAILED(hr)) ++g_fillStretchFailed;
        if (g_fillReports < 8) {
            ++g_fillReports;
            D3DSURFACE_DESC sd{};
            const bool haveSrc = src && SUCCEEDED(src->GetDesc(&sd));
            Log("TEXTURE FILL #%u: StretchRect INTO the character texture | hr=0x%08lX | "
                "src %ux%u fmt=%d pool=%d usage=0x%lX | dstRect %s | filter=%d",
                g_fillReports, hr,
                haveSrc ? sd.Width : 0, haveSrc ? sd.Height : 0,
                haveSrc ? static_cast<int>(sd.Format) : 0,
                haveSrc ? static_cast<int>(sd.Pool) : -1,
                haveSrc ? static_cast<unsigned long>(sd.Usage) : 0UL,
                dr ? "sub" : "whole", static_cast<int>(filter));
        }
    }
    return hr;
}

HRESULT WINAPI Hook_ColorFill(IDirect3DDevice9* dev, IDirect3DSurface9* surf, const RECT* r,
                              D3DCOLOR c) {
    if (!InternalHere() && IsAtlasSurface(surf)) ++g_fillColorFill;
    return g_origColorFill(dev, surf, r, c);
}

// ---------------------------------------------------------------------------------------------
// Self-test for the copy path, because "it returned S_OK" is not evidence it moved pixels
//
// The GPU copy reports 0 StretchRect refusals, 0 failures, and a mean of 0.0 for the character
// atlas. Read literally that says the atlas is empty. This project has now been wrong three
// times in a row by believing exactly that kind of reading:
//
//   * `usage=0x200` read as D3DUSAGE_RENDERTARGET, when it is D3DUSAGE_DYNAMIC;
//   * 240 blank LockRect readbacks read as "the game never wrote it", when a READONLY lock of a
//     DYNAMIC default-pool surface has UNDEFINED contents;
//   * a black 2048x1024 texture in two Remix captures read as a date correlation, when an
//     all-zero image simply always hashes the same.
//
// Each time the missing piece was a CONTROL: a case where the answer is known in advance. So
// before concluding anything from a zero mean, run the identical path over a source this shim
// FILLED ITSELF, matched to the atlas in the properties that matter - D3DPOOL_DEFAULT,
// D3DUSAGE_DYNAMIC, uncompressed, written by LockRect and never by the GPU.
//
//   control mean ~= the value written  -> the path works, and the atlas really is empty
//   control mean == 0                  -> StretchRect cannot read a DYNAMIC default texture and
//                                         every zero this path has reported is meaningless
//
// Runs ONCE, allocates 256x256, and releases everything.
bool g_copySelfTestDone = false;

void SelfTestCopyPath(IDirect3DDevice9* dev) {
    if (g_copySelfTestDone || !dev || !g_settings.rtAlbedoCopy) return;
    g_copySelfTestDone = true;

    const UINT W = 256, H = 256;
    const unsigned char kR = 200, kG = 120, kB = 60;      // mean should land near 127
    IDirect3DTexture9* srcDyn = nullptr;
    IDirect3DTexture9* scratch = nullptr;
    IDirect3DSurface9* srcSurf = nullptr;
    IDirect3DSurface9* scratchSurf = nullptr;
    IDirect3DSurface9* sysSurf = nullptr;
    const bool wasInternal = g_internal;
    g_internal = true;
    g_internalThread = GetCurrentThreadId();
    double fillMean = -1.0, resolveMean = -1.0, stretchMean = -1.0;
    HRESULT hrStretch = E_FAIL, hrResolve = E_FAIL;
    try {
        // A source matched to the atlas: DEFAULT pool, DYNAMIC, uncompressed, CPU-written.
        if (FAILED(dev->CreateTexture(W, H, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8,
                                      D3DPOOL_DEFAULT, &srcDyn, nullptr)) || !srcDyn) goto done;
        {
            D3DLOCKED_RECT lr{};
            if (SUCCEEDED(srcDyn->LockRect(0, &lr, nullptr, D3DLOCK_DISCARD)) && lr.pBits) {
                for (UINT y = 0; y < H; ++y) {
                    unsigned char* row = static_cast<unsigned char*>(lr.pBits) +
                                         static_cast<size_t>(y) * lr.Pitch;
                    for (UINT x = 0; x < W; ++x) {
                        row[x * 4 + 0] = kB; row[x * 4 + 1] = kG;
                        row[x * 4 + 2] = kR; row[x * 4 + 3] = 255;
                    }
                }
                srcDyn->UnlockRect(0);
                fillMean = (kR + kG + kB) / 3.0;
            }
        }
        if (FAILED(dev->CreateTexture(W, H, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                                      D3DPOOL_DEFAULT, &scratch, nullptr)) || !scratch) goto done;
        if (FAILED(scratch->GetSurfaceLevel(0, &scratchSurf)) || !scratchSurf) goto done;
        if (FAILED(dev->CreateOffscreenPlainSurface(W, H, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM,
                                                    &sysSurf, nullptr)) || !sysSurf) goto done;

        auto meanOf = [&]() -> double {
            D3DLOCKED_RECT lr{};
            if (FAILED(sysSurf->LockRect(&lr, nullptr, D3DLOCK_READONLY)) || !lr.pBits) return -1.0;
            double acc = 0.0; unsigned n = 0;
            for (UINT y = 0; y < H; y += 4) {
                const unsigned char* row = static_cast<const unsigned char*>(lr.pBits) +
                                           static_cast<size_t>(y) * lr.Pitch;
                for (UINT x = 0; x < W; x += 4) {
                    acc += row[x * 4 + 0] + row[x * 4 + 1] + row[x * 4 + 2];
                    ++n;
                }
            }
            sysSurf->UnlockRect();
            return n ? acc / (3.0 * n) : -1.0;
        };

        // STEP 1 - does the RESOLVE work at all? Fill the render target with a known colour
        // through the GPU and read it back. This tests GetRenderTargetData on its own.
        if (SUCCEEDED(dev->ColorFill(scratchSurf, nullptr, D3DCOLOR_ARGB(255, kR, kG, kB)))) {
            hrResolve = dev->GetRenderTargetData(scratchSurf, sysSurf);
            if (SUCCEEDED(hrResolve)) resolveMean = meanOf();
        }
        // STEP 2 - does StretchRect actually move pixels out of a DYNAMIC default texture?
        // Clear the target first so a no-op StretchRect cannot inherit step 1's colour.
        dev->ColorFill(scratchSurf, nullptr, D3DCOLOR_ARGB(255, 0, 0, 0));
        if (SUCCEEDED(srcDyn->GetSurfaceLevel(0, &srcSurf)) && srcSurf) {
            hrStretch = dev->StretchRect(srcSurf, nullptr, scratchSurf, nullptr, D3DTEXF_NONE);
            if (SUCCEEDED(hrStretch) && SUCCEEDED(dev->GetRenderTargetData(scratchSurf, sysSurf)))
                stretchMean = meanOf();
        }
    } catch (...) {
    }
done:
    Log("COPY PATH SELF-TEST (control for the COMPOSITED ALBEDO zero):");
    Log("    wrote a DEFAULT+DYNAMIC 256x256 source by LockRect, expected mean %.1f", fillMean);
    Log("    step 1  ColorFill -> GetRenderTargetData      mean %.1f  hr=0x%08lX   "
        "(tests the RESOLVE alone)", resolveMean, hrResolve);
    Log("    step 2  StretchRect(dynamic) -> resolve       mean %.1f  hr=0x%08lX   "
        "(tests reading a DYNAMIC default texture)", stretchMean, hrStretch);
    Log("    VERDICT: step 2 near %.1f means the path WORKS and the character atlas really is "
        "empty. step 2 at 0.0 with step 1 fine means StretchRect cannot read a DYNAMIC default "
        "texture, and every zero this path has reported is meaningless.", fillMean);
    if (sysSurf) sysSurf->Release();
    if (scratchSurf) scratchSurf->Release();
    if (scratch) scratch->Release();
    if (srcSurf) srcSurf->Release();
    if (srcDyn) srcDyn->Release();
    g_internal = wasInternal;
}


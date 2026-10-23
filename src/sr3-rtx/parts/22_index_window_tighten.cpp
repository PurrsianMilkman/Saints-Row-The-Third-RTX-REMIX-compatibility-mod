// parts/22_index_window_tighten.cpp - the skinned index-window probe and tighten-converted-window.
// NOT a standalone translation unit: #included, in order, by ../sr3rtx.cpp inside its anonymous
// namespace. Originally sr3rtx.cpp lines 16731-18120 (old file:line citations map here).
// ---------------------------------------------------------------- the index window probe
// A skinned draw is served from OUR copy of its vertex window [minIndex, minIndex+numVertices)
// in the skin ring. An index outside that window reads whatever else sits in the ring at that
// address - a vertex of another mesh, or stale bytes - and the triangle stretches to wherever
// that vertex is, for exactly as long as that ring content lasts. The game's own buffer holds
// the whole mesh, so a rigid draw with the same indices renders correctly, which is why nothing
// here ever saw it. Scanned once per index range, budgeted, reported in the frame report.
struct IdxWinKey {
    IDirect3DIndexBuffer9* ib; UINT start, prims, minIndex, count; int type;
    bool operator==(const IdxWinKey& o) const {
        return ib == o.ib && start == o.start && prims == o.prims && minIndex == o.minIndex && count == o.count && type == o.type;
    }
};
struct IdxWinKeyHash {
    size_t operator()(const IdxWinKey& k) const {
        size_t h = reinterpret_cast<size_t>(k.ib) * 0x9E3779B1u;
        h ^= (k.start * 0x85EBCA6Bu) + (k.prims * 0xC2B2AE35u) + (k.minIndex * 0x27D4EB2Fu) + (k.count * 0x165667B1u) + static_cast<size_t>(k.type);
        return h;
    }
};
struct IdxWinResult { bool scanned; unsigned below, above, lo, hi; };
std::unordered_map<IdxWinKey, IdxWinResult, IdxWinKeyHash> g_idxWin;
// NOT pinned by the 2026-09-18 g_tightWin fix below, deliberately: this cache never changes what
// gets sent to the device, only what the INDEX WINDOW report counts, so a stale hit from a
// recycled ib address costs a wrong number in a log line, not corrupted geometry - not a trade
// worth pinning every index buffer this probe ever looks at for.
unsigned g_idxWinBudgetFrame = 0, g_idxWinBudgetUsed = 0, g_idxWinReports = 0, g_idxWinWorstOver = 0;
unsigned long long g_idxWinDrawsOut = 0, g_idxWinDrawsOk = 0, g_idxWinScans = 0, g_idxWinScanFail = 0;
// A free measurement, already computed and previously thrown away: r.lo/r.hi is the tight
// range a SKINNED draw's own indices actually reference, but SkinAndBind blends and uploads
// the whole DECLARED numVertices regardless (skinned draws are deliberately excluded from
// TightenConvertedWindow - see SkinAndBind's "vertex 0 must land on index minIndex" note).
// Mirrors g_tightDeclaredSum/g_tightRefSum, so the two report lines read alike.
unsigned long long g_idxWinDeclaredSum = 0, g_idxWinRefSum = 0;

void ProbeIndexWindow(IDirect3DIndexBuffer9* ib, D3DPRIMITIVETYPE type, INT baseVertex, UINT minIndex,
                      UINT numVertices, UINT startIndex, UINT prims) {
    if (!g_settings.probeIndexWindow || !ib || !prims || !numVertices) return;
    if (type != D3DPT_TRIANGLELIST && type != D3DPT_TRIANGLESTRIP) return;
    const IdxWinKey key{ib, startIndex, prims, minIndex, numVertices, static_cast<int>(type)};
    auto it = g_idxWin.find(key);
    if (it == g_idxWin.end()) {
        if (g_idxWinBudgetFrame != g_frames) { g_idxWinBudgetFrame = g_frames; g_idxWinBudgetUsed = 0; }
        if (g_idxWinBudgetUsed >= 8 || g_idxWin.size() > 8192) return;
        ++g_idxWinBudgetUsed;
        IdxWinResult r{};
        const UINT nIdx = (type == D3DPT_TRIANGLESTRIP) ? prims + 2 : prims * 3;
        D3DINDEXBUFFER_DESC d{};
        void* m = nullptr;
        const bool wasInt = g_internal;
        g_internal = true;
        g_internalThread = GetCurrentThreadId();
        if (SUCCEEDED(ib->GetDesc(&d)) && d.Size) {
            const UINT isz = (d.Format == D3DFMT_INDEX32) ? 4 : 2;
            if (static_cast<unsigned long long>(startIndex) * isz + static_cast<unsigned long long>(nIdx) * isz <= d.Size &&
                SUCCEEDED(ib->Lock(startIndex * isz, nIdx * isz, &m, D3DLOCK_READONLY)) && m) {
                r.scanned = true; r.lo = 0xFFFFFFFFu; r.hi = 0;
                // indices are relative to baseVertex, as minIndex is
                const UINT winLo = minIndex, winHi = minIndex + numVertices;
                for (UINT i = 0; i < nIdx; ++i) {
                    const unsigned v = (isz == 4) ? static_cast<const unsigned*>(m)[i] : static_cast<const unsigned short*>(m)[i];
                    if (v < r.lo) r.lo = v;
                    if (v > r.hi) r.hi = v;
                    if (v < winLo) ++r.below; else if (v >= winHi) ++r.above;
                }
                ib->Unlock();
                ++g_idxWinScans;
            } else ++g_idxWinScanFail;
        } else ++g_idxWinScanFail;
        g_internal = wasInt;
        try { it = g_idxWin.emplace(key, r).first; } catch (...) { return; }
        if (r.scanned && (r.below || r.above) && g_idxWinReports < 10) {
            ++g_idxWinReports;
            Log("INDEX OUTSIDE THE VERTEX WINDOW #%u (skinned draw): window [%u, %u) but its %u indices span [%u, %u] - "
                "%u below, %u above; baseVertex %d, %u prims (%s). The ring copy holds only the window, so those "
                "triangles read another mesh's vertices.",
                g_idxWinReports, minIndex, minIndex + numVertices, nIdx, r.lo, r.hi, r.below, r.above, baseVertex, prims,
                type == D3DPT_TRIANGLESTRIP ? "strip" : "list");
        }
    }
    const IdxWinResult& r = it->second;
    if (!r.scanned) return;
    g_idxWinDeclaredSum += numVertices;
    g_idxWinRefSum += (r.hi - r.lo + 1);
    if (r.below || r.above) {
        ++g_idxWinDrawsOut;
        const unsigned over = (r.hi >= minIndex + numVertices) ? (r.hi - (minIndex + numVertices) + 1) : (minIndex - r.lo);
        if (over > g_idxWinWorstOver) g_idxWinWorstOver = over;
    } else {
        ++g_idxWinDrawsOk;
    }
}

// ---------------------------------------------------------------- tighten converted window
//
// A building decal is submitted as 2-4 triangles with NumVertices declaring the WHOLE mesh the
// index buffer belongs to (measured: NumVertices = 28336 for a 2-triangle draw). Remix's geometry
// generation hash, from its own startup log, is positions, indices, texcoords, geometrydescriptor,
// vertexlayout, vertexshader - positions and texcoords included - so that decal's identity to
// Remix is computed over a 28,336-vertex slice of a buffer the game's own streamer rewrites many
// times a frame. The decal's six vertices never move; its DECLARED WINDOW does, and Remix cannot
// tell those apart because the whole-mesh window is all this shim ever forwarded.
//
// TEAM A's cleanroom disassembly of the engine (spec-geometry-format.md, paragraph 8.1-8.2) found
// that SR3 itself stores a TIGHT per-range vertex bound with every draw range, validated on 8,899
// of 8,899 ranges, and reads it back through FUN_00e72d00. The tight range already exists as
// engine data; the D3D9 call this shim intercepts is simply never given it. This section recovers
// the same tight range from the one thing the shim can see directly - the index buffer content -
// rather than trying to reach into engine structures this shim does not otherwise touch.
//
// D3D9 SEMANTICS, worked out here so nobody has to re-derive them under a bug report: the vertex
// an index i addresses is VB[BaseVertexIndex + i]. MinVertexIndex is the SMALLEST INDEX VALUE a
// draw touches, NOT that value plus BaseVertexIndex, and NumVertices is (MaxVertexIndex -
// MinVertexIndex + 1). BaseVertexIndex is added to every index the same way whether the window is
// loose or tight, so tightening only ever changes MinVertexIndex and NumVertices; BaseVertexIndex
// and StartIndex are carried through to the tightened call completely unchanged.
//
// NO INDEX BUFFER LOCK/UNLOCK HOOK EXISTS IN THIS SHIM (checked: no kSlotIBLock, no PatchVTable
// call against an index buffer's vtable anywhere in this file - only vertex buffers are watched,
// via kSlotVBLock/Hook_VBLock, see PatchVTable(buffer, kSlotVBLock, ...) above). So there is no
// event that can invalidate a cached range when the game rewrites the bytes a cache entry
// described. The only safe way to run this without that hook is to refuse every DYNAMIC index
// buffer outright and cache ranges from non-dynamic buffers only, on the same assumption the rest
// of this file already makes for static vertex data (see IsDynamicVB and the fresh-buffer
// deferral above): a non-dynamic buffer is written once at load and never rewritten in place. If
// an index-buffer Lock/Unlock hook is ever added, this is the first place that should start
// trusting dynamic buffers too.
struct TightWinKey {
    IDirect3DIndexBuffer9* ib;
    UINT start, prims;
    int type;
    // size/format (2026-09-18 audit fix): folded in so a DIFFERENTLY SHAPED buffer allocated at
    // a released one's old address cannot be mistaken for it. Supplied by GetTightIbShape below,
    // which is what keeps this from costing a GetDesc bridge round trip on every draw.
    UINT size;
    DWORD format;
    bool operator==(const TightWinKey& o) const {
        return ib == o.ib && start == o.start && prims == o.prims && type == o.type &&
               size == o.size && format == o.format;
    }
};
struct TightWinKeyHash {
    size_t operator()(const TightWinKey& k) const {
        size_t h = reinterpret_cast<size_t>(k.ib) * 0x9E3779B1u;
        h ^= (k.start * 0x85EBCA6Bu) + (k.prims * 0xC2B2AE35u) + (static_cast<size_t>(k.type) * 0x27D4EB2Fu);
        h ^= (k.size * 0x165667B1u) + (static_cast<size_t>(k.format) * 0xD3A2646Cu);
        return h;
    }
};
// ok=false is cached too, exactly as g_idxWin caches a scan failure: an unscannable slice does not
// change shape draw to draw, so retrying it every frame would just pay the same refusal forever.
// lo/hi are raw index VALUES with no BaseVertexIndex folded in - MinVertexIndex/MaxVertexIndex
// exactly, per the semantics above, with nothing added by the caller.
struct TightWinResult { bool ok; UINT lo, hi; unsigned lastUsed = 0; };   // lastUsed: frame, see tightWinEvictByAge
std::unordered_map<TightWinKey, TightWinResult, TightWinKeyHash> g_tightWin;
// Bounded the way g_idxWin is bounded: a decal-heavy area can introduce many distinct (ib,
// start, prims, type) tuples in one loading burst, and letting the cache grow forever would
// hold every tuple a whole level has ever declared. kTightWinCap is the SWEEP-eligibility
// threshold (SweepTightWin only runs once the cache is past it, exactly like OcclMeshFor's
// 2000), not a hard ceiling - the cache is allowed to grow past it between sweeps, because the
// entries are cheap (four scalars each, unlike an occluder mesh's geometry) and refusing new
// ones the moment the soft cap is touched is what made every insert-time refusal at kTightWinCap
// pay the 32-lock budget on the SAME draws every frame. kTightWinHardCap is the actual refusal
// point, mirroring OcclMeshFor's 20000-against-2000 ratio, and should essentially never be hit -
// if it is, that is worth knowing (see g_tightRefuseCacheFull), not silently absorbing.
constexpr size_t kTightWinCap = 4096;
constexpr size_t kTightWinHardCap = kTightWinCap * 10;
unsigned long long g_tightWinEvictions = 0;
// New index ranges scanned this frame, budgeted like g_idxWinBudgetUsed above: first sight of a
// new area can offer many distinct decal ranges in one frame, and bounding the Locks spent on
// them keeps that frame from paying for the whole area's decals at once. The cache backfills over
// the following frames instead.
unsigned g_tightWinBudgetFrame = 0, g_tightWinBudgetUsed = 0;

// Refused, and counted apart, so a silent guard is never the reason this looks inert (the project
// has been bitten by that before: see idxWin, occlMesh). Every one of these falls back to the
// draw's ORIGINAL declared window - nothing here can make a draw's window WIDER.
unsigned long long g_tightRefuseNoIB = 0;        // no index buffer bound to the draw
unsigned long long g_tightRefuseType = 0;        // primitive type not scanned here, or an empty draw - its index count would be a guess
unsigned long long g_tightRefuseDescFail = 0;    // GetDesc on the index buffer failed
unsigned long long g_tightRefuseDynamic = 0;     // DYNAMIC index buffer: no Lock/Unlock hook watches it, see the comment above
unsigned long long g_tightRefuseOOB = 0;         // the index slice [startIndex, startIndex+nIdx) runs past the buffer's own byte size
unsigned long long g_tightRefuseLockFail = 0;    // the read lock itself failed
unsigned long long g_tightRefuseBudget = 0;      // per-frame Lock budget spent; retried next frame
unsigned long long g_tightRefuseNotSubset = 0;   // refused because the range fell outside [minIndex, minIndex+numVertices) AND widenUnderDeclaredWindow is OFF - see below; with it ON (the default) this population is WIDENED instead (g_tightWidened) unless that would run past the buffer (g_tightRefuseWidenBeyondBuffer)
unsigned long long g_tightRefuseCacheFull = 0;   // tightWinEvictByAge on, and the cache is past kTightWinHardCap even after sweeping - should not happen

// How often the safety net's referenced range falls outside what the game declared, measured
// every time regardless of widenUnderDeclaredWindow - this is the instrument the OUTSIDE WINDOW
// report line and probe (in TightenConvertedWindow, below) are built from. below/above are not
// mutually exclusive with "both": a draw counted in "both" is also counted in below and in above,
// so below + above - both is the total, exactly what g_tightRefuseNotSubset alone used to be
// before widening existed.
unsigned long long g_tightOutsideBelow = 0;   // referenced lo is below the declared minIndex
unsigned long long g_tightOutsideAbove = 0;   // referenced hi reaches at or past minIndex+numVertices
unsigned long long g_tightOutsideBoth = 0;    // both at once
// TASK 2's fix, counted apart from g_tightComputed (which stays the "genuine subset, narrowed or
// unchanged" population only, exactly as before). g_tightWidenedAddedSum is verts ADDED to the
// declared window, the mirror of g_tightRemoved above.
unsigned long long g_tightWidened = 0;
unsigned long long g_tightWidenedAddedSum = 0;
// The widened window would have declared a vertex past the end of the bound stream-0 buffer.
// Not papered over: this means the game itself is referencing vertices that do not exist, worse
// than the original defect this feature was written to fix, so it is refused and counted apart.
unsigned long long g_tightRefuseWidenBeyondBuffer = 0;

// Measured whether or not tightenConvertedWindow is ON: this is the instrument for "how big is
// the prize", meant to run a whole play session with the switch OFF and read the answer off the
// report line before ever changing a pixel. Totalled here per DRAW; the report line divides by
// frames or by g_tightComputed as appropriate. Repeated a second time for draws whose albedo
// sampler is Decal-named (StrStrIA(...,"Decal"), the same test ProbeDecal/the overlay-offset code
// already use) - that is the population this was written for, so its own figures must be visible
// apart from the general ones rather than diluted into them.
unsigned long long g_tightComputed = 0, g_tightDeclaredSum = 0, g_tightRefSum = 0, g_tightRemoved = 0;
unsigned long long g_tightDecalComputed = 0, g_tightDecalDeclaredSum = 0, g_tightDecalRefSum = 0, g_tightDecalRemoved = 0;

// TIGHT WINDOW's own per-buffer shape: ibd.Size, ibd.Format and whether it is DYNAMIC, for one
// distinct index buffer pointer, resolved once and shared by every (ib, start, prims, type)
// tuple that references it. GetDesc is a bridge round trip (see g_rtChannels far above -
// "measured once per target CHANGE, never per draw"), and TightWinKey now needs a buffer's shape
// on EVERY lookup, hit or miss, not only on first sight of a new tuple the way ibd used to be
// fetched - this is what keeps that from costing a round trip on every draw whose window is
// already cached.
struct TightIbShape { UINT size; DWORD format; bool dynamic; };
std::unordered_map<IDirect3DIndexBuffer9*, TightIbShape> g_tightIbShapes;

// Referenced on first sight exactly like g_shaders/g_layouts far above (see their comments) when
// tightenWindowPinIndexBuffers is on - nothing here is ever erased, so the reference also pins
// the buffer for the process lifetime. This is what actually closes the defect TightWinKey's
// size/format fields only defend against: while a buffer's shape is cached here, its address
// cannot be freed and handed to a new, differently-shaped buffer, so a stale g_tightWin entry can
// never be read back for the wrong object. With the switch off this cache still exists -
// TightWinKey needs SOME shape to hash - but takes no reference, which is what makes the switch a
// fair A/B against today's shipped behaviour rather than a weaker version of the fix.
//
// Returns null only when GetDesc itself fails. Not cached: that should never happen for a live
// resource pointer, and caching a permanent failure would mean caching a shape never actually
// read.
const TightIbShape* GetTightIbShape(IDirect3DIndexBuffer9* ib) {
    const auto it = g_tightIbShapes.find(ib);
    if (it != g_tightIbShapes.end()) return &it->second;
    const bool wasInt = g_internal;
    g_internal = true;
    g_internalThread = GetCurrentThreadId();
    D3DINDEXBUFFER_DESC ibd{};
    const HRESULT hr = ib->GetDesc(&ibd);
    g_internal = wasInt;
    if (FAILED(hr)) return nullptr;
    if (g_settings.tightenWindowPinIndexBuffers) ib->AddRef();
    return &g_tightIbShapes.emplace(
        ib, TightIbShape{ibd.Size, static_cast<DWORD>(ibd.Format),
                        (ibd.Usage & D3DUSAGE_DYNAMIC) != 0}).first->second;
}

// Computes the tight window a RIGID, CONVERTED, INDEXED draw's own indices actually reference.
// Always updates the counters above, regardless of g_settings.tightenConvertedWindow - the caller
// decides whether to act on *outMinIndex/*outNumVertices, but the measurement runs every time so
// a session with the switch off still reports the size of what tightening it would remove.
// Returns false, leaving the outputs untouched, on every refusal reason named above; the caller's
// own fallback is simply to keep using the draw's original minIndex/numVertices. Can also return
// true with a WIDENED *outMinIndex/*outNumVertices - wider than the game's own declared window -
// when the referenced range falls outside it and widenUnderDeclaredWindow is on; that widened
// window is always checked against the real vertex buffer first (see g_tightRefuseWidenBeyondBuffer
// in TightenConvertedWindow's body), so a true return never claims a vertex that does not exist.
bool TightenConvertedWindow(IDirect3DIndexBuffer9* ib, D3DPRIMITIVETYPE type, INT baseVertex,
                            UINT minIndex, UINT numVertices, UINT startIndex, UINT primitiveCount,
                            bool albedoIsDecal, UINT* outMinIndex, UINT* outNumVertices) {
    if (!ib) { ++g_tightRefuseNoIB; return false; }
    // Triangle list, strip AND fan: a strip's and a fan's index COUNT is the same formula
    // (VertsForPrimitives already gives prims+2 for both), and the referenced index VALUES for
    // either one are simply the whole contiguous slice [startIndex, startIndex+nIdx) - winding
    // decides how those indices group into triangles, not which vertices they touch, and a
    // min/max scan does not care about winding. Anything else (points, lines, or a type this
    // function does not name) is refused rather than guessed at.
    if (type != D3DPT_TRIANGLELIST && type != D3DPT_TRIANGLESTRIP && type != D3DPT_TRIANGLEFAN) {
        ++g_tightRefuseType;
        return false;
    }
    const UINT nIdx = VertsForPrimitives(type, primitiveCount);
    if (!nIdx) { ++g_tightRefuseType; return false; }

    // 2026-09-18 AUDIT FIX: the shape has to be known before the cache can even be LOOKED UP now
    // that it is part of the key (TightWinKey above), not only on a miss the way ibd used to be
    // fetched. GetTightIbShape is what keeps that from costing a GetDesc bridge round trip on
    // every draw.
    const TightIbShape* shape = GetTightIbShape(ib);
    if (!shape) { ++g_tightRefuseDescFail; return false; }

    const TightWinKey key{ib, startIndex, primitiveCount, static_cast<int>(type),
                          shape->size, shape->format};
    auto it = g_tightWin.find(key);
    if (it == g_tightWin.end()) {
        // Cache MISS only, bracketed by PH_TIGHTEN: a hit falls straight through to the return
        // below and must stay completely outside this timer, so tightening a warm frame keeps
        // costing nothing. This pays a synchronous D3DLOCK_READONLY Lock/Unlock round trip, up
        // to 32 times a frame, and until the 2026-09-18 PROFILE audit nothing timed it at all -
        // it was invisible inside PH_PROBES, which is where this cost used to be attributed.
        LONGLONG tT = Now();
        if (g_tightWinBudgetFrame != g_frames) { g_tightWinBudgetFrame = g_frames; g_tightWinBudgetUsed = 0; }
        if (g_tightWinBudgetUsed >= 32) { ++g_tightRefuseBudget; PhaseAdd(PH_TIGHTEN, tT); return false; }
        ++g_tightWinBudgetUsed;

        TightWinResult r{false, 0, 0};
        if (shape->dynamic) {
            ++g_tightRefuseDynamic;
        } else {
            const UINT isz = (shape->format == D3DFMT_INDEX32) ? 4 : 2;
            if (static_cast<unsigned long long>(startIndex) * isz + static_cast<unsigned long long>(nIdx) * isz > shape->size) {
                ++g_tightRefuseOOB;
            } else {
                const bool wasInt = g_internal;
                g_internal = true;
                g_internalThread = GetCurrentThreadId();
                void* m = nullptr;
                if (SUCCEEDED(ib->Lock(startIndex * isz, nIdx * isz, &m, D3DLOCK_READONLY)) && m) {
                    UINT lo = 0xFFFFFFFFu, hi = 0;
                    for (UINT i = 0; i < nIdx; ++i) {
                        const unsigned v = (isz == 4) ? static_cast<const unsigned*>(m)[i]
                                                       : static_cast<const unsigned short*>(m)[i];
                        if (v < lo) lo = v;
                        if (v > hi) hi = v;
                    }
                    ib->Unlock();
                    r.ok = true; r.lo = lo; r.hi = hi;
                } else {
                    ++g_tightRefuseLockFail;
                }
                g_internal = wasInt;
            }
        }
        if (g_settings.tightWinEvictByAge) {
            // No per-insert eviction: SweepTightWin (Hook_Present, every 300 frames, the same
            // cadence KickOcclusionJob ages out g_occlMeshes) reclaims whatever has gone unused
            // for a while instead. Refused past the hard ceiling only - see its comment above,
            // this should not happen in practice.
            if (g_tightWin.size() > kTightWinHardCap) { ++g_tightRefuseCacheFull; PhaseAdd(PH_TIGHTEN, tT); return false; }
        } else if (g_tightWin.size() >= kTightWinCap) {
            g_tightWin.erase(g_tightWin.begin());
            ++g_tightWinEvictions;
        }
        try { it = g_tightWin.emplace(key, r).first; } catch (...) { PhaseAdd(PH_TIGHTEN, tT); return false; }
        PhaseAdd(PH_TIGHTEN, tT);
    }
    // Stamped here rather than in each branch above, so both a HIT and a freshly inserted
    // MISS get exactly the same touch - this is the one line SweepTightWin relies on to know
    // an entry is still wanted.
    it->second.lastUsed = g_frames;
    const TightWinResult& r = it->second;
    if (!r.ok) return false;

    // MinVertexIndex is the index VALUE r.lo itself, never r.lo relative to anything else - see
    // the D3D9 semantics above. NumVertices covers up to and including the highest value read.
    const UINT tMin = r.lo;
    const UINT tNum = r.hi - r.lo + 1;

    // THE SAFETY NET. [tMin, tMin+tNum) is what the indices actually reference; when it is not
    // entirely inside [minIndex, minIndex+numVertices), what the game declared, something has to
    // give: either the window is corrected to cover both, or the draw is refused rather than
    // guessed at, because silent geometry corruption dressed up as an optimisation is the
    // alternative. below/above/both are measured every time this fires, whether or not
    // widenUnderDeclaredWindow goes on to act on it - see the OUTSIDE WINDOW report line, which is
    // what named this population before this function ever corrected anything.
    const unsigned long long declaredEnd = static_cast<unsigned long long>(minIndex) + numVertices;
    const unsigned long long refEnd = static_cast<unsigned long long>(tMin) + tNum;
    const bool below = tMin < minIndex;
    const bool above = refEnd > declaredEnd;
    if (below || above) {
        if (below) ++g_tightOutsideBelow;
        if (above) ++g_tightOutsideAbove;
        if (below && above) ++g_tightOutsideBoth;
        // Capped at 12 and re-armed by the capture key, exactly like ProbeDecalDraw/ProbeHudDraw
        // (RearmCaptures resets g_outsideWinReports to 0 alongside them). Everything printed here
        // already describes THIS draw: g_curPS, g_curDrawVertexCount/g_curDrawPrimCount and
        // g_lastWorld are all stamped for it earlier in Hook_DrawIndexedPrimitive, the same fields
        // ProbeDecalDraw reads. Fires for the whole population, not only the fraction still
        // refused below - naming what these draws ARE is the point, not just what happens to them.
        if (g_outsideWinReports < 12) {
            ++g_outsideWinReports;
            const unsigned belowBy = below ? (minIndex - tMin) : 0;
            const unsigned aboveBy = above ? static_cast<unsigned>(refEnd - declaredEnd) : 0;
            Log("    OUTSIDE WINDOW probe #%u: albedoSampler='%s' lowest='%s'@%d | verts=%u prims=%u "
                "at(%.2f %.2f %.2f) | declared=[%u,%u) referenced=[%u,%u] | outside: below=%u above=%u "
                "| stream0 dynamic=%s, index format=%s",
                g_outsideWinReports, g_curPS.albedoSampler, g_curPS.lowestSampler, g_curPS.lowestSamplerReg,
                g_curDrawVertexCount, g_curDrawPrimCount, g_lastWorld._41, g_lastWorld._42, g_lastWorld._43,
                minIndex, minIndex + numVertices, r.lo, r.hi, belowBy, aboveBy,
                IsDynamicVB(g_stream0) ? "yes" : "no",
                shape->format == D3DFMT_INDEX32 ? "INDEX32" : "INDEX16");
        }

        if (!g_settings.widenUnderDeclaredWindow) {
            ++g_tightRefuseNotSubset;
            return false;
        }

        // WIDEN instead of refuse: the corrected window covers both what the game declared and
        // what the indices actually reference. BaseVertexIndex and StartIndex are not touched
        // here - the caller keeps passing its own baseVertex/startIndex through unchanged, exactly
        // as the narrowing path above already does.
        const UINT wMin = min(minIndex, tMin);
        const unsigned long long wEnd = max(declaredEnd, refEnd);

        // CLAMP TO THE BUFFER. The vertex actually addressed by index value v is BaseVertexIndex +
        // v, so the highest legal index value depends on baseVertex too, not only on the buffer's
        // own size - VertexRangeFits already does exactly this arithmetic for every read-lock of a
        // game vertex buffer in this shim (see its own comment on the baseVertex/UINT overflow
        // trap). If the corrected window would still reach past the end of the bound stream-0
        // buffer, this is not a window chosen too small: it is the game referencing vertices that
        // DO NOT EXIST, worse than the defect this feature fixes, and it must be refused and
        // counted apart rather than papered over.
        if (wEnd - wMin > 0xFFFFFFFFull) { ++g_tightRefuseWidenBeyondBuffer; return false; }
        const UINT wNum = static_cast<UINT>(wEnd - wMin);
        const INT wFirstSigned = baseVertex + static_cast<INT>(wMin);
        if (wFirstSigned < 0 ||
            !VertexRangeFits(g_stream0, g_stream0Offset, static_cast<UINT>(wFirstSigned), wNum, g_stream0Stride)) {
            ++g_tightRefuseWidenBeyondBuffer;
            return false;
        }

        ++g_tightWidened;
        g_tightWidenedAddedSum += (wNum - numVertices);
        *outMinIndex = wMin;
        *outNumVertices = wNum;
        return true;
    }

    ++g_tightComputed;
    g_tightDeclaredSum += numVertices;
    g_tightRefSum += tNum;
    g_tightRemoved += (numVertices - tNum);
    if (albedoIsDecal) {
        ++g_tightDecalComputed;
        g_tightDecalDeclaredSum += numVertices;
        g_tightDecalRefSum += tNum;
        g_tightDecalRemoved += (numVertices - tNum);
    }
    *outMinIndex = tMin;
    *outNumVertices = tNum;
    return true;
}

// Age out g_tightWin entries the streamer has moved past, the same way KickOcclusionJob ages
// out g_occlMeshes: checked every 300 frames, only once the cache is past its soft cap, erasing
// whatever has not been looked up (hit OR miss - TightenConvertedWindow's single, unconditional
// "it->second.lastUsed = g_frames" after the cache lookup covers both) in the last 900. Called
// from Hook_Present,
// after KickOcclusionJob, for the same reason KickOcclusionJob itself runs there: end of frame,
// once per frame, nothing else needs to run between the sweep and the next draw.
void SweepTightWin() {
    if (!g_settings.tightWinEvictByAge) return;
    if ((g_frames % 300) != 0 || g_tightWin.size() <= kTightWinCap) return;
    for (auto it = g_tightWin.begin(); it != g_tightWin.end();)
        if (g_frames > it->second.lastUsed + 900) { it = g_tightWin.erase(it); ++g_tightWinEvictions; }
        else ++it;
}

bool SkinAndBind(IDirect3DDevice9* dev, INT baseVertex, UINT minIndex, UINT numVertices) {
    LONGLONG tS = Now();
    g_skinRefuseWhy = nullptr;
    if (!g_skinVB || numVertices == 0) { g_skinRefuseWhy = "no ring buffer"; return false; }
    // Negative baseVertex is legal in D3D9 and would address before the buffer.
    const INT first = baseVertex + static_cast<INT>(minIndex);
    if (first < 0) { g_skinRefuseWhy = "baseVertex + minIndex is negative"; return false; }
    const UINT firstVertex = static_cast<UINT>(first);
    if (baseVertex != 0) ++g_skinNonZeroBaseVertex;

    const BaseMesh* mesh = GetBaseMesh(firstVertex, numVertices);
    if (!mesh || mesh->verts.size() != numVertices) {
        if (!g_skinRefuseWhy) g_skinRefuseWhy = "cached mesh has the wrong vertex count";
        return false;
    }

    // Does the palette we are about to read belong to THIS object? Measured, not assumed - the
    // assumption in the code below is "the palette is written a few draws earlier under the same
    // objTM, so it belongs to the draw", and detached car parts posed onto NPC limbs is what it
    // looks like when that is false.
    // The draw index of the most recent palette write among the bones this mesh reads. That
    // burst is THIS object's palette; anything written materially earlier belongs to something
    // drawn before and must not pose these vertices. Zero means "no usable epoch", which leaves
    // the blend below unfiltered.
    unsigned paletteEpoch = 0;
    {
        const bool sameFrame = (g_lastBoneUploadFrame == g_frames);
        const unsigned drawsAgo =
            sameFrame && g_drawIndexThisFrame >= g_lastBoneUploadDraw
                ? g_drawIndexThisFrame - g_lastBoneUploadDraw : 0u;
        if (mesh->maxBone > g_skinMaxBoneSeen) g_skinMaxBoneSeen = mesh->maxBone;
        if (g_lastBoneUploadBones < g_skinMinReachSeen) g_skinMinReachSeen = g_lastBoneUploadBones;
        if (!sameFrame) ++g_skinPaletteStaleFrame;
        else if (mesh->maxBone >= g_lastBoneUploadBones) ++g_skinPaletteBeyondReach;
        else if (drawsAgo > 16) ++g_skinPaletteFarUpload;
        else ++g_skinPaletteOwn;

        // The measurement that does not depend on how many calls an upload took: were all the
        // bones this mesh reads written at the same time, for the same object?
        unsigned oldest = 0xFFFFFFFFu, newest = 0u;
        bool neverWritten = false;
        for (unsigned b = 0; b < static_cast<unsigned>(kBonesMax); ++b) {
            if (!(mesh->usedBones & (1ull << b))) continue;
            if (g_boneWrittenFrame[b] != g_frames) { neverWritten = true; break; }
            const unsigned w = g_boneWrittenDraw[b];
            if (w < oldest) oldest = w;
            if (w > newest) newest = w;
        }
        if (neverWritten) {
            ++g_skinBoneNeverWritten;          // reads a bone nothing wrote this frame at all
        } else if (oldest != 0xFFFFFFFFu) {
            paletteEpoch = newest;
            const unsigned spread = newest - oldest;
            if (spread > g_skinWorstSpread) g_skinWorstSpread = spread;
            // One object's palette arrives as a burst of consecutive calls, so its bones share a
            // draw index or sit within a draw or two. A wide spread means two objects' bones.
            if (spread > 4) {
                ++g_skinMixedPalette;
                // Counting these was not enough. Rejecting the older epoch stranded ~640
                // vertices per mixed draw in bind pose, which says the older bones are the
                // MAJORITY of the mesh and therefore almost certainly legitimate. So name the
                // draws instead: what they are, where they sit, and how the two epochs split.
                // If these turn out to be characters rather than vehicles, the whole palette
                // line of enquiry is finished and the car parts have another cause entirely.
                if (g_skinMixedReports < 16) {
                    ++g_skinMixedReports;
                    unsigned inNewest = 0, inOldest = 0;
                    for (unsigned b2 = 0; b2 < static_cast<unsigned>(kBonesMax); ++b2) {
                        if (!(mesh->usedBones & (1ull << b2))) continue;
                        if (newest - g_boneWrittenDraw[b2] <= kStaleBoneDraws) ++inNewest;
                        else ++inOldest;
                    }
                    const D3DMATRIX obj = FromRegisters(&g_vsConst[kRegObjTM][0], 3);
                    Log("MIXED-PALETTE #%u: ps='%s' verts=%u | objTM t=(%.1f %.1f %.1f) | "
                        "draw %u of frame %u | epochs: %u bones at draw %u (newest), "
                        "%u bones up to %u draws older | highest bone %u",
                        g_skinMixedReports, g_curPS.albedoSampler, numVertices,
                        obj._41, obj._42, obj._43, g_drawIndexThisFrame, g_frames,
                        inNewest, newest, inOldest, spread, mesh->maxBone);
                    // The uploads themselves, newest last. A contiguous block from c52 is an
                    // object publishing its whole palette; a short range starting elsewhere is
                    // a partial refresh - or another object's bones landing on top of ours.
                    for (unsigned h = 0; h < 24; ++h) {
                        const unsigned slot = (g_paletteHistoryPos + h) % 24;
                        const PaletteWrite& w = g_paletteHistory[slot];
                        if (!w.endReg || w.frame != g_frames) continue;
                        Log("        upload: draw %u regs c%u..c%u (bones %u..%u)%s",
                            w.draw, w.startReg, w.endReg,
                            w.startReg > kRegBonePalette
                                ? (w.startReg - kRegBonePalette) / kRegsPerBone : 0u,
                            (w.endReg - kRegBonePalette) / kRegsPerBone,
                            w.startReg == kRegBonePalette ? "  <- starts at c52" : "");
                    }
                }
            } else ++g_skinCleanPalette;
        }
    }

    const UINT stride = sizeof(SkinnedVertex);
    // Seam duplicates for a tiled cloth texture ride behind the draw's own vertices.
    const UINT dups = (g_clothRemap && g_clothRemap->dupSrc.size() == g_clothRemap->dupShiftRaw.size())
                          ? static_cast<UINT>(g_clothRemap->dupSrc.size()) : 0u;
    const UINT need = (numVertices + dups) * stride;
    // Index i reads at (streamOffset + (baseVertex + i) * stride), so our vertex 0 must land on
    // index minIndex: the write position has to be at least (baseVertex + minIndex) * stride.
    // Meeting that here is what lets the game's own index buffer be reused verbatim - no index
    // copy and no rebasing.
    const UINT base = firstVertex * stride;
    if (static_cast<unsigned long long>(base) + need > g_skinRingBytes) {
        g_skinRefuseWhy = "mesh does not fit the ring buffer";
        return false;
    }
    if (g_skinRingPos < base) g_skinRingPos = base;
    bool discard = g_skinRingFresh;
    if (g_skinRingPos + need > g_skinRingBytes) {
        g_skinRingPos = base; discard = true; ++g_skinRingWraps;
    }
    if (discard) ++g_skinRingDiscards;
    if (g_skinRingPos + need > g_skinRingHighWater) g_skinRingHighWater = g_skinRingPos + need;

    // D3DLOCK_DISCARD is defined only for a WHOLE-buffer lock - the contract is "I am about to
    // overwrite the entire buffer", and pairing it with a sub-range is not something D3D9
    // promises anything about. So a discard locks (0, 0) and we index into the returned pointer
    // ourselves; only NOOVERWRITE takes the sub-range form.
    void* mapped = nullptr;
    const LONGLONG tLock = Now();
    const HRESULT lockHr = discard
        ? g_skinVB->Lock(0, 0, &mapped, D3DLOCK_DISCARD)
        : g_skinVB->Lock(g_skinRingPos, need, &mapped, D3DLOCK_NOOVERWRITE);
    {
        const double lockMs = MsSince(tLock);
        if (discard) g_skinLockDiscardMs += lockMs; else g_skinLockKeepMs += lockMs;
        if (lockMs > g_skinLockWorstMs) g_skinLockWorstMs = lockMs;
    }
    if (FAILED(lockHr) || !mapped) {
        g_skinRefuseWhy = "lock of the ring buffer failed";
        return false;
    }
    g_skinRingFresh = false;

    SkinnedVertex* out = reinterpret_cast<SkinnedVertex*>(
        static_cast<unsigned char*>(mapped) + (discard ? g_skinRingPos : 0u));
    // The register this shader actually declares, not the one a character shader used.
    const UINT boneBase = (g_curVS.boneReg >= 0)
        ? static_cast<UINT>(g_curVS.boneReg) : kRegBonePalette;
    if (boneBase != kRegBonePalette) ++g_skinForeignBoneReg; else ++g_skinC52BoneReg;
    if (g_curVS.usesMorph) ++g_morphDraws;
    // Name the draws whose shader reads no BLENDINDICES. These are the ones that were being
    // posed from a stranger's palette; the sampler identifies which mesh so the finding can be
    // checked against what is visibly wrong on screen rather than taken on trust.
    if (!g_curVS.usesBlendIndices && g_skinNoBoneDeclReports < 10) {
        ++g_skinNoBoneDeclReports;
        Log("NO BONE DECL #%u: ps='%s' verts=%u - this vertex shader declares no "
            "BLENDINDICES input and places the mesh by objTM alone. boneReg=%d. %s",
            g_skinNoBoneDeclReports, g_curPS.albedoSampler, numVertices, g_curVS.boneReg,
            g_settings.skinRequireBoneDecl ? "Left in its bind pose (correct)."
                                           : "STILL reading c52 (skinRequireBoneDecl=0).");
    }
    if (g_skinBoneRegSeen < 8 && boneBase != kRegBonePalette) {
        ++g_skinBoneRegSeen;
        Log("BONE REGISTER: ps='%s' declares Bone_weights at c%u, NOT c52 - every draw of this "
            "shader has been skinned from the wrong constants", g_curPS.albedoSampler, boneBase);
    }
    // GROUND TRUTH: is our shadow of the constants what the DEVICE actually holds?
    //
    // Every hypothesis so far has assumed g_vsConst mirrors the device. It is built purely from
    // SetVertexShaderConstantF calls we intercept, so anything that sets constants by another
    // route - a state block apply, a redundant-call filter of our own, a bridge-side cache -
    // leaves it stale without any of the previous probes being able to tell.
    //
    // The game renders its own cars correctly from the same c52[v.x*3] the disassembly shows, so
    // at the moment of a car draw the DEVICE must hold the car's palette. If our shadow disagrees,
    // that is the bug, and no amount of ownership heuristics on a wrong shadow would have found it.
    //
    // Read back only for the first few draws: GetVertexShaderConstantF crosses the 32-to-64-bit
    // bridge and is far too expensive to do per draw.
    //
    // 2026-09-18 AUDIT FIX: the cap used to be tested against g_shadowCheckReports, which only
    // ever incremented in the MISMATCH branch below - so if the shadow ever agreed with the
    // device even once, the count would never reach 12 and this stayed armed forever, paying its
    // two round trips on every single-bone skinned draw for the rest of the process. Inert today
    // only because the answer has been 12 of 12 mismatched, every run - a trap set for the first
    // session where it agrees. g_shadowCheckRuns increments once per ACTUAL round trip, win or
    // lose, so the cap disarms it after 12 runs regardless of outcome. Also gated behind
    // shadowCheckProbe now, OFF by default: this has already produced its answer.
    if (g_settings.shadowCheckProbe && g_shadowCheckRuns < 12 && !g_curVS.usesBlendWeights && !mesh->verts.empty()) {
        const unsigned b0 = mesh->verts[0].idx[0];
        if (b0 < static_cast<unsigned>(kBonesMax)) {
            ++g_shadowCheckRuns;
            float dev4[12] = {}, obj4[12] = {};
            g_internal = true;
            g_internalThread = GetCurrentThreadId();
            const HRESULT h1 = dev->GetVertexShaderConstantF(boneBase + b0 * kRegsPerBone, dev4, 3);
            const HRESULT h2 = dev->GetVertexShaderConstantF(kRegObjTM, obj4, 3);
            g_internal = false;
            if (SUCCEEDED(h1) && SUCCEEDED(h2)) {
                const float* shadowBone = &g_vsConst[boneBase + b0 * kRegsPerBone][0];
                const float* shadowObj = &g_vsConst[kRegObjTM][0];
                float boneDiff = 0.0f, objDiff = 0.0f;
                for (int e = 0; e < 12; ++e) {
                    boneDiff = max(boneDiff, std::fabs(dev4[e] - shadowBone[e]));
                    objDiff = max(objDiff, std::fabs(obj4[e] - shadowObj[e]));
                }
                if (boneDiff > 1e-4f || objDiff > 1e-4f) {
                    ++g_shadowCheckReports;
                    ++g_shadowMismatch;
                    Log("SHADOW MISMATCH #%u: ps='%s' bone %u | bone rows differ by %.4f, objTM by "
                        "%.4f | device bone t=(%.2f %.2f %.2f) shadow t=(%.2f %.2f %.2f) | "
                        "device objTM t=(%.2f %.2f %.2f) shadow t=(%.2f %.2f %.2f)",
                        g_shadowCheckReports, g_curPS.albedoSampler, b0, boneDiff, objDiff,
                        dev4[3], dev4[7], dev4[11],
                        shadowBone[3], shadowBone[7], shadowBone[11],
                        obj4[3], obj4[7], obj4[11],
                        shadowObj[3], shadowObj[7], shadowObj[11]);
                } else {
                    ++g_shadowMatch;
                    if (g_shadowMatch == 1)
                        Log("SHADOW CHECK: device constants MATCH our shadow on the first vehicle "
                            "draw (bone %u) - the shadow is not the problem", b0);
                }
            }
        }
    }

    const MorphSource morph = MorphForThisDraw(firstVertex, numVertices);
    if (morph.active) ++g_morphAppliedDraws;
    const float* palette = &g_vsConst[boneBase][0];
    // How many bones can actually be READ from boneBase without running off g_vsConst.
    //
    // boneReg comes from the shader's own constant table as a raw 16-bit register number, and
    // nothing validated it. The palette is 64 bones x 3 registers, so anything above c64 makes
    // `palette + bone*12` index past a 1024-float array. Every shader in this game declares it
    // at c52 and the report has always said so - which is exactly why this was never noticed.
    // An out-of-bounds READ does not reliably crash; it reads whatever global follows, and poses
    // a character by it.
    const unsigned safeBones =
        (boneBase + kBonesMax * kRegsPerBone <= kMaxVsConst)
            ? static_cast<unsigned>(kBonesMax)
            : (kMaxVsConst > boneBase ? (kMaxVsConst - boneBase) / kRegsPerBone : 0u);
    if (safeBones < static_cast<unsigned>(kBonesMax) && g_boneRangeReports < 4) {
        ++g_boneRangeReports;
        Log("BONE RANGE: ps='%s' declares its palette at c%u, so only %u of %d bones fit in the "
            "%u-register shadow - influences beyond that are dropped rather than read out of "
            "bounds", g_curPS.albedoSampler, boneBase, safeBones, kBonesMax, kMaxVsConst);
    }
    double cSkin[3] = {}, cBind[3] = {};
    unsigned cN = 0;
    // THE FAST PATH. Identical output to the loop below whenever the four policy gates are off
    // (the report has shown each of them dropping 0 influences/frame): no per-influence checks,
    // no per-vertex counters, and safe to split across threads because it touches nothing but
    // its own range of `out`. The reference loop below stays for when a policy is on.
    PhaseAdd(PH_SKIN_PRE, tS);
    const bool fastPath = g_settings.skinFast && !g_settings.rejectStaleBones &&
                          !g_settings.paletteSetupScope && !g_settings.vehicleBonesOff &&
                          !g_settings.clampBonesToUpload;
    if (fastPath) {
        const bool fReads = g_curVS.usesBlendIndices || !g_settings.skinRequireBoneDecl;
        const int fInfl = !fReads ? 0 : (g_curVS.usesBlendWeights ? 4 : 1);
        if (!fReads) ++g_skinNoBoneDecl;
        else if (fInfl == 1) ++g_skinSingleBone;
        else ++g_skinFourBone;
        const BaseVertex* fv = mesh->verts.data();
        const float* fShift = (g_clothRemap && g_clothRemap->shiftRaw.size() >= numVertices)
                                  ? g_clothRemap->shiftRaw.data() : nullptr;
        auto fastRange = [&, fv, fShift, fInfl](UINT i0, UINT i1) {
            for (UINT i = i0; i < i1; ++i) {
                const BaseVertex& b = fv[i];
                float mpos[3] = {b.pos[0], b.pos[1], b.pos[2]};
                float mnrm[3] = {b.nrm[0], b.nrm[1], b.nrm[2]};
                if (morph.active) {
                    const size_t off = static_cast<size_t>(i) * morph.stride;
                    const unsigned char* md = morph.slice->data();
                    if (morph.fresh && off + morph.posOff + 8u <= morph.slice->size()) {
                        const short* d = reinterpret_cast<const short*>(md + off + morph.posOff);
                        for (int k = 0; k < 3; ++k) mpos[k] += static_cast<float>(d[k]) * kMorphScale;
                        if (morph.nrmOff >= 0 &&
                            off + static_cast<size_t>(morph.nrmOff) + 4u <= morph.slice->size()) {
                            const unsigned char* nn = md + off + morph.nrmOff;
                            float len = 0.0f;
                            for (int k = 0; k < 3; ++k) {
                                const float nm = (static_cast<float>(nn[k]) / 255.0f) * 2.0f - 1.0f;
                                mnrm[k] = b.nrm[k] + 2.0f * nm;
                                len += mnrm[k] * mnrm[k];
                            }
                            len = std::sqrt(len);
                            if (len > 1e-8f) for (int k = 0; k < 3; ++k) mnrm[k] /= len;
                        }
                    }
                }
                float m[12] = {};
                float sum = 0.0f;
                for (int k = 0; k < fInfl; ++k) {
                    const float wk = (fInfl == 1) ? 1.0f : b.w[k];
                    if (wk == 0.0f) continue;
                    const unsigned bone = b.idx[k];
                    if (bone >= safeBones) continue;
                    const float* r = palette + bone * kRegsPerBone * 4;
                    for (int e = 0; e < 12; ++e) m[e] += wk * r[e];
                    sum += wk;
                }
                SkinnedVertex& o = out[i];
                const float shift = fShift ? fShift[i] : 0.0f;
                if (sum <= 0.0f) {
                    o.pos[0] = mpos[0]; o.pos[1] = mpos[1]; o.pos[2] = mpos[2];
                    o.nrm[0] = mnrm[0]; o.nrm[1] = mnrm[1]; o.nrm[2] = mnrm[2];
                    o.uv[0] = b.uv[0] + shift;
                    o.uv[1] = b.uv[1];
                    continue;
                }
                const float inv = 1.0f / sum;
                for (int e = 0; e < 12; ++e) m[e] *= inv;
                for (int r = 0; r < 3; ++r)
                    o.pos[r] = m[r * 4 + 0] * mpos[0] + m[r * 4 + 1] * mpos[1] +
                               m[r * 4 + 2] * mpos[2] + m[r * 4 + 3];
                float n[3];
                for (int r = 0; r < 3; ++r)
                    n[r] = m[r * 4 + 0] * mnrm[0] + m[r * 4 + 1] * mnrm[1] + m[r * 4 + 2] * mnrm[2];
                const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                const float ninv = (len > 1e-8f) ? 1.0f / len : 0.0f;
                for (int r = 0; r < 3; ++r) o.nrm[r] = n[r] * ninv;
                o.uv[0] = b.uv[0] + shift;
                o.uv[1] = b.uv[1];
            }
        };
        SkinPoolRun(fastRange, numVertices);
        ++g_skinFastDraws;
        g_skinVertsTotal += numVertices;
    } else
    for (UINT i = 0; i < numVertices; ++i) {
        const BaseVertex& b = mesh->verts[i];
        // pos = base + delta/8192, and NORMAL1 replaces NORMAL0 - the order and the scale the
        // `_mc` shaders use: mad r1.xyz, v4, c0.x, v0, THEN the bone blend.
        float mpos[3] = {b.pos[0], b.pos[1], b.pos[2]};
        float mnrm[3] = {b.nrm[0], b.nrm[1], b.nrm[2]};
        if (morph.active) {
            // The slice is this draw's own copy, indexed from 0 - no cache pointer, no global
            // offset, nothing another thread can move under us.
            const size_t off = static_cast<size_t>(i) * morph.stride;
            const unsigned char* md = morph.slice->data();
            if (morph.fresh && off + morph.posOff + 8u <= morph.slice->size()) {
                const short* d =
                    reinterpret_cast<const short*>(md + off + morph.posOff);
                for (int k = 0; k < 3; ++k) mpos[k] += static_cast<float>(d[k]) * kMorphScale;
                // The normal is a DELTA too, not a replacement:
                //     mad r0.yzw, v5.xxyz, c0.y, c0.z    n_morph = v5*2 - 1
                //     mad r1,     v2,      c0.y, c0.z    n_base  = v2*2 - 1
                //     mad r0.yzw, r0,      c0.y, r1.xxyz n = n_base + 2*n_morph
                // Substituting n_morph for n_base lights the surface by a vector the game never
                // computes - the "something wrong with the normals" on heads and player skin.
                if (morph.nrmOff >= 0 &&
                    off + static_cast<size_t>(morph.nrmOff) + 4u <= morph.slice->size()) {
                    const unsigned char* n = md + off + morph.nrmOff;
                    float len = 0.0f;
                    for (int k = 0; k < 3; ++k) {
                        const float nm = (static_cast<float>(n[k]) / 255.0f) * 2.0f - 1.0f;
                        mnrm[k] = b.nrm[k] + 2.0f * nm;
                        len += mnrm[k] * mnrm[k];
                    }
                    len = std::sqrt(len);
                    if (len > 1e-8f) for (int k = 0; k < 3; ++k) mnrm[k] /= len;
                }
            } else {
                ++g_morphStaleVerts;
            }
        }
        float m[12] = {};
        float sum = 0.0f;
        // A shader that declares no blendweight input uses exactly ONE bone - index component 0,
        // implicit weight 1. Blending the other three would mix in bones the game never reads.
        // A shader with no BLENDINDICES input indexes no palette: 0 influences, so the vertex
        // falls through to the bind-pose path below and objTM places it - which is exactly and
        // only what the game's own static variant does.
        const bool reads = g_curVS.usesBlendIndices || !g_settings.skinRequireBoneDecl;
        const int influences = !reads ? 0 : (g_curVS.usesBlendWeights ? 4 : 1);
        if (i == 0) {
            if (!reads) ++g_skinNoBoneDecl;
            else if (influences == 1) ++g_skinSingleBone; else ++g_skinFourBone;
        }
        for (int k = 0; k < influences; ++k) {
            // Reject on WEIGHT, not index: slot 255 is the "unused" sentinel and would address
            // c817, far outside the 192-register palette.
            const float wk = (influences == 1) ? 1.0f : b.w[k];
            if (wk == 0.0f) continue;
            const unsigned bone = b.idx[k];
            if (bone >= safeBones) continue;
            // Reject an influence whose bone matrix belongs to a DIFFERENT object.
            //
            // The palette shadow persists across draws, so a bone this object never wrote still
            // holds the previous object's matrix. Measured 2026-08-25: 5 skinned draws a frame
            // read bones written up to 276 draws apart - two objects' palettes blended into one
            // mesh. That is the detached car part standing where an NPC's arm is.
            //
            // The surviving weights are renormalised below (the blend already divides by `sum`),
            // so dropping an influence redistributes it across this object's own bones rather
            // than shrinking the vertex toward the origin. A vertex that loses EVERY influence
            // falls through to the existing bind-pose path, which keeps it attached to its own
            // object instead of teleporting it onto someone else.
            if (g_settings.rejectStaleBones && paletteEpoch) {
                if (g_boneWrittenFrame[bone] != g_frames ||
                    paletteEpoch - g_boneWrittenDraw[bone] > kStaleBoneDraws) {
                    ++g_staleInfluencesRejected;
                    continue;
                }
            }
            // Is this bone part of THIS object's palette?
            //
            // Every upload in the game starts at c52 and runs as long as that object's skeleton
            // needs, so the palette shadow always holds a prefix belonging to the current object
            // and a tail left over from previous ones. A vertex bound to a bone in that tail is
            // posed by another object entirely.
            //
            // Multi-call uploads are handled by construction: consecutive constant writes with no
            // draw between them share a draw index, so the whole upload carries one stamp. This
            // is what the first "beyond the upload's reach" test got wrong - it compared against
            // the LAST call's length instead of the upload's stamp.
            //
            // Only some vertices of a mesh are affected, which is why the result is a car that
            // stretches as well as drifts: the bound vertices are dragged and the rest stay put.
            // The palette in the shadow belongs to a DIFFERENT draw setup, so it is not this
            // object's. Contribute nothing: the vertex keeps its bind pose and objTM places it,
            // which leaves the part attached to itself instead of posed onto another object.
            if (g_settings.paletteSetupScope && g_paletteSetupId != g_setupId) {
                ++g_paletteOutOfScope;
                continue;
            }
            // Diagnostic, and possibly an acceptable interim state: ignore the bone palette
            // entirely for single-bone (vehicle) draws and let objTM place the bind pose.
            //
            // The clamp above verifies a bone came from the MOST RECENT upload, not that the most
            // recent upload belongs to this draw's object. When a character's palette lands between
            // a car's upload and the car's draw, the character's bones carry the current stamp and
            // pass. Whether that happens depends on draw order, which depends on what is on
            // screen - which is precisely how the drift was described.
            //
            // If cars are stable and correctly placed with the palette ignored, the bones are the
            // whole problem and nothing else is involved. What is lost is whatever the bones
            // encode beyond rest position: door swing, damage deformation, wheel spin.
            if (g_settings.vehicleBonesOff && !g_curVS.usesBlendWeights) {
                ++g_vehicleBonesSkipped;
                continue;
            }
            if (g_settings.clampBonesToUpload &&
                (g_boneWrittenFrame[bone] != g_frames ||
                 g_boneWrittenDraw[bone] != g_lastBoneUploadDraw)) {
                ++g_staleBoneVerts;
                continue;   // no influence: falls through to bind pose, which objTM then places
            }
            const float* r = palette + bone * kRegsPerBone * 4;
            for (int e = 0; e < 12; ++e) m[e] += wk * r[e];
            sum += wk;
        }
        if (sum <= 0.0f) {   // no influence at all: leave the vertex in its bind pose
            ++g_skinVertsBindPose;
            memcpy(out[i].pos, mpos, sizeof(mpos));
            memcpy(out[i].nrm, mnrm, sizeof(mnrm));
            out[i].uv[0] = b.uv[0] + ((g_clothRemap && i < g_clothRemap->shiftRaw.size())
                                          ? g_clothRemap->shiftRaw[i] : 0.0f);
            out[i].uv[1] = b.uv[1];
            continue;
        }
        const float inv = 1.0f / sum;
        for (int e = 0; e < 12; ++e) m[e] *= inv;

        // Row-major float3x4: row r is m[r*4 .. r*4+3], and the shader takes dp4 against
        // (pos, 1), so the fourth component of each row is the translation.
        for (int r = 0; r < 3; ++r)
            out[i].pos[r] = m[r * 4 + 0] * mpos[0] + m[r * 4 + 1] * mpos[1] +
                            m[r * 4 + 2] * mpos[2] + m[r * 4 + 3];
        // Normals rotate only - the shader uses dp3, dropping the translation column.
        float n[3];
        for (int r = 0; r < 3; ++r)
            n[r] = m[r * 4 + 0] * mnrm[0] + m[r * 4 + 1] * mnrm[1] + m[r * 4 + 2] * mnrm[2];
        const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        const float ninv = (len > 1e-8f) ? 1.0f / len : 0.0f;
        for (int r = 0; r < 3; ++r) out[i].nrm[r] = n[r] * ninv;
        // The tile shift, when this draw's generated texture is wider than one tile. Raw units:
        // b.uv holds the short2 as a float and the texture matrix still divides by 1024.
        out[i].uv[0] = b.uv[0] + ((g_clothRemap && i < g_clothRemap->shiftRaw.size())
                                      ? g_clothRemap->shiftRaw[i] : 0.0f);
        out[i].uv[1] = b.uv[1];
        for (int r = 0; r < 3; ++r) { cSkin[r] += out[i].pos[r]; cBind[r] += mpos[r]; }
        ++cN;
        ++g_skinVertsTotal;
    }
    // STEP 3a capture. One draw, once: the first big skinned mesh of the run. Reading back
    // through the mapped pointer is what the centroid accumulation above already does, so it is
    // no new risk. Guarded on size so this takes a body, not an eyelash.
    // GATED ON THE ATLAS, exactly as the build is. Measured 2026-08-31: the build required
    // g_atlasDdsReady but the capture did not, so the vertices came from the FIRST big skinned
    // draw of the run - which happens during loading, long before the player character exists in
    // the world - and the mesh was then built from that stale pose at whatever position it held.
    // Submitted correctly, rendered correctly, and nowhere the player would ever look.
    // A capture and the build that consumes it must share their preconditions.
    // Capture one mesh at a time. After each is built the capture state resets, so the next
    // frame picks up another of the character's vertex buffers.
    //
    // The size floor is 64 rather than 4000 because accessories are small - 4000 was chosen to
    // find the BODY and would now exclude most of what is still missing. Membership is decided by
    // objTM translation instead: every part of one character shares it, which keeps NPCs out
    // without needing to know anything about them.
    // THE WINDOW OPENS ON THE FRAME AFTER THE BODY IS FOUND, NOT ON IT.
    //
    // The previous version opened the window at the moment the body was recognised, so every part
    // drawn EARLIER in that same frame had already gone past - and SR3 draws the body late, which
    // is why only the body was captured. A window that starts mid-frame can only ever collect the
    // tail of one.
    //
    // So the body's frame is a SCOUTING frame: it fixes the reference position and schedules the
    // real window for the next frame, which is then collected whole, body included.
    if (g_settings.remixApiCharacter && !g_apiCaptureClosed && !g_apiHaveRef &&
        g_atlasDdsReady && numVertices >= 4000) {
        g_apiRefPos[0] = g_vsConst[kRegObjTM + 0][3];
        g_apiRefPos[1] = g_vsConst[kRegObjTM + 1][3];
        g_apiRefPos[2] = g_vsConst[kRegObjTM + 2][3];
        g_apiHaveRef = true;
        g_apiCapFrame = g_frames + 1;
        Log("remix api: character found at (%.1f %.1f %.1f) - capturing every part of it on "
            "frame %u", g_apiRefPos[0], g_apiRefPos[1], g_apiRefPos[2], g_apiCapFrame);
    }

    bool wantThis = g_settings.remixApiCharacter && !g_apiCaptureClosed && g_apiHaveRef &&
                    g_atlasDdsReady && numVertices >= 64 &&
                    g_apiPendingCount < kMaxApiMeshes && g_frames == g_apiCapFrame;
    if (wantThis)
        for (unsigned d = 0; d < g_apiPendingCount; ++d)
            if (g_apiPending[d].vb == g_stream0) { wantThis = false; break; }  // already taken
    if (wantThis) {
        // Membership by objTM translation: every part of one character shares it. The tolerance is
        // wide enough to survive a frame of movement between the scouting frame and this one, and
        // the FIRST part taken re-anchors the reference so later parts match it exactly.
        const float px = g_vsConst[kRegObjTM + 0][3];
        const float py = g_vsConst[kRegObjTM + 1][3];
        const float pz = g_vsConst[kRegObjTM + 2][3];
        const float dx = px - g_apiRefPos[0], dy = py - g_apiRefPos[1], dz = pz - g_apiRefPos[2];
        if (dx * dx + dy * dy + dz * dz > 4.0f) {
            wantThis = false;                                       // a different actor
        } else if (g_apiPendingCount == 0) {
            g_apiRefPos[0] = px; g_apiRefPos[1] = py; g_apiRefPos[2] = pz;
        }
    }
    if (wantThis) {
        try {
            ApiPending& p = g_apiPending[g_apiPendingCount];
            // ------------------------------------------------------------------------------
            // UV SCALE, computed exactly as the fixed-function conversion computes it.
            //
            // The API path baked in kShortUVScale alone. The FFP path builds a texture matrix of
            // `raw * tiling / 1024` - the per-material tiling as well - and the API path never
            // learned the second half. So any part whose material tiles its albedo came out with
            // UVs wrong by exactly that factor.
            //
            // The 2026-09-04 vertex audit shows it plainly. Nine of the ten captured parts land
            // inside the unit square:
            //
            //     part 1  u[0.011 0.992]  v[0.010 0.987]
            //     part 4  u[0.005 0.992]  v[0.004 0.995]
            //     ...
            //     part 10 u[-1.072 2.787] v[-0.414 2.734]     <- the underwear
            //
            // A u span of 3.86 where every sibling spans 0.98. And the cloth bake for that same
            // part covered 3% of its texture and came out at mean 1.8 of 255 - because the baker
            // rasterises into UV space, so triangles that land outside 0..1 simply never write
            // any texels. One wrong scale, and both the geometry and the texture read as broken.
            //
            // This is not a new rule, it is the removal of an inconsistency: the FFP conversion
            // is what textures the world correctly today, and the API path now asks the same
            // question the same way. If no tiling constant matches, the answer is 1/1024 and
            // nothing changes - so a part that was right stays right.
            {
                float su = 1.0f, sv = 1.0f;
                if (g_curLayout.texcoordType == D3DDECLTYPE_SHORT2)
                    su = sv = CurrentShortUVScale();
                int tu = -1, tv = -1;
                TilingForAlbedo(tu, tv);
                if (tu >= 0 && tu < static_cast<int>(kMaxVsConst)) su *= g_vsConst[tu][0];
                if (tv >= 0 && tv < static_cast<int>(kMaxVsConst)) sv *= g_vsConst[tv][0];
                p.uvScale[0] = su;
                p.uvScale[1] = sv;
                p.uvTilingReg[0] = tu;
                p.uvTilingReg[1] = tv;
            }
            // With skinning ON the mesh must be the BIND POSE, because Remix will pose it.
            // With skinning OFF we hand over the already-skinned vertices and the result is
            // frozen - which is what step 3b shipped and what works today.
            if (!g_settings.remixApiSkinning) {
                p.verts.assign(out, out + numVertices);
                p.weights.clear();
                p.bones.clear();
                p.boneCount = 0;
                p.boneReg = 0;
            } else {
            p.verts.clear();
            p.weights.clear();
            p.bones.clear();
            p.uv2.clear();
            p.uv2.reserve(mesh->verts.size() * 2);
            p.verts.reserve(mesh->verts.size());
            p.weights.reserve(mesh->verts.size() * 4);
            p.bones.reserve(mesh->verts.size() * 4);
            for (size_t bi = 0; bi < mesh->verts.size(); ++bi) {
                const BaseVertex& bv = mesh->verts[bi];
                SkinnedVertex sv{};
                memcpy(sv.pos, bv.pos, sizeof(sv.pos));
                memcpy(sv.nrm, bv.nrm, sizeof(sv.nrm));
                sv.uv[0] = bv.uv[0];
                sv.uv[1] = bv.uv[1];
                p.verts.push_back(sv);
                p.uv2.push_back(bv.uv2[0]);
                p.uv2.push_back(bv.uv2[1]);
                // Normalised here: the game's shader divides by the sum, and Remix will not.
                float sum = bv.w[0] + bv.w[1] + bv.w[2] + bv.w[3];
                if (sum <= 0.0f) sum = 1.0f;
                for (int c = 0; c < 4; ++c) {
                    p.weights.push_back(bv.w[c] / sum);
                    const unsigned b = bv.idx[c];
                    p.bones.push_back(b < kMaxApiBones ? b : 0u);   // 255 is the unused sentinel
                }
            }
            // ALWAYS the full palette, and every bone initialised to identity at build time.
            // A blend index that exceeds boneTransforms_count would have Remix read past the
            // array, and maxBone is computed only over NON-ZERO weights - so a vertex carrying a
            // high index with a zero weight can legitimately exceed it. Sending the whole palette
            // removes the possibility rather than reasoning about it.
            p.boneCount = kMaxApiBones;
            p.boneReg = boneBase;
            }
            p.vb = g_stream0;
            p.firstVertex = static_cast<long long>(baseVertex) + minIndex;
            p.albedo = (g_curPS.albedoStage >= 0 && g_curPS.albedoStage < 8)
                           ? g_curTexture[g_curPS.albedoStage] : nullptr;
            p.slots.clear();
            p.ib.clear();
            p.ibSize = 0;
            p.ibHave = false;
            p.ib32 = false;
            // Taken at the SAME moment as the vertices, and every part is taken in the SAME
            // frame, so the whole character shares one pose and one placement.
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 4; ++c)
                    p.objTM[r][c] = g_vsConst[kRegObjTM + r][c];
            if (!g_apiHaveRef) {
                g_apiRefPos[0] = p.objTM[0][3];
                g_apiRefPos[1] = p.objTM[1][3];
                g_apiRefPos[2] = p.objTM[2][3];
                g_apiHaveRef = true;
                g_apiCapFrame = g_frames;    // the window is this frame only
            }
            g_apiCapMinIndex = minIndex;
            ++g_apiPendingCount;
        } catch (...) {
        }
    }
    // The duplicated seam vertices: copies of already-skinned vertices, with the OTHER tile's
    // shift. out[src] carries its home shift, so that is taken back out before the new one goes
    // in. The private index list refers to these as minIndex + numVertices + j.
    if (dups) {
        for (UINT j = 0; j < dups; ++j) {
            const unsigned srcV = g_clothRemap->dupSrc[j];
            if (srcV >= numVertices) continue;
            out[numVertices + j] = out[srcV];
            const float home = (srcV < g_clothRemap->shiftRaw.size()) ? g_clothRemap->shiftRaw[srcV] : 0.0f;
            out[numVertices + j].uv[0] = out[srcV].uv[0] - home + g_clothRemap->dupShiftRaw[j];
        }
    }
    PhaseAdd(PH_SKIN_LOOP, tS);
    g_skinVB->Unlock();

    // Only on the dump frame: measuring a bounding box over every skinned vertex of every draw
    // is exactly the per-vertex cost this path cannot afford every frame, and one frame answers
    // the question.
    g_dumpSkin = g_frameLog && !g_frameDumpDone && (g_frames + 1 == g_frameDumpTarget);
    if (g_dumpSkin) {
        g_dumpSkinFirst = firstVertex;
        g_dumpSkinVB = static_cast<void*>(g_stream0);
        for (int r = 0; r < 3; ++r) {
            g_dumpSkinBindMin[r] = g_dumpSkinBindMax[r] = mesh->verts.empty() ? 0.0f
                                                        : mesh->verts[0].pos[r];
            g_dumpSkinBindC[r]  = cN ? static_cast<float>(cBind[r] / cN) : 0.0f;
            g_dumpSkinPosedC[r] = cN ? static_cast<float>(cSkin[r] / cN) : 0.0f;
        }
        for (UINT i = 0; i < numVertices && i < mesh->verts.size(); ++i)
            for (int r = 0; r < 3; ++r) {
                const float v = mesh->verts[i].pos[r];
                if (v < g_dumpSkinBindMin[r]) g_dumpSkinBindMin[r] = v;
                if (v > g_dumpSkinBindMax[r]) g_dumpSkinBindMax[r] = v;
            }
        g_dumpSkinBones = BonesUsedCount(mesh->usedBones);
        g_dumpSkinHighBone = mesh->maxBone;
        g_dumpSkinLowBone = 0;
        for (unsigned b = 0; b < 64; ++b)
            if (mesh->usedBones & (1ull << b)) { g_dumpSkinLowBone = b; break; }
        // Newest and oldest upload behind this mesh's own bones, and how many distinct
        // objects wrote them. One object, one upload -> new == old and gens == 1.
        unsigned newest = 0, oldest = 0xFFFFFFFFu, gens = 0, seen[8] = {};
        g_dumpSkinPalNewBones = g_dumpSkinPalOldBones = 0;
        for (unsigned b = 0; b < 64; ++b) {
            if (!(mesh->usedBones & (1ull << b))) continue;
            const unsigned d = g_boneWrittenDraw[b];
            if (d >= newest) { newest = d; g_dumpSkinPalNewBones = g_boneWrittenUploadBones[b]; }
            if (d <= oldest) { oldest = d; g_dumpSkinPalOldBones = g_boneWrittenUploadBones[b]; }
            const unsigned g = g_boneWrittenObjGen[b];
            bool have = false;
            for (unsigned i = 0; i < gens; ++i) if (seen[i] == g) { have = true; break; }
            if (!have && gens < 8) seen[gens++] = g;
        }
        g_dumpSkinPalNewDraw = newest;
        g_dumpSkinPalOldDraw = (oldest == 0xFFFFFFFFu) ? 0u : oldest;
        g_dumpSkinPalGens = gens;
    }

    // THE measurement, after four runs of arguing about palettes: how far did skinning MOVE this
    // mesh from its rest position? Both centroids are in object space, so objTM cancels and this
    // is purely what the bone matrices did.
    //
    // A car part at rest sits on its own bind pose - a door swings, a wheel spins, nothing
    // travels far. A part posed by ANOTHER object's bones is displaced by that object's limb
    // positions, and the displacement changes as that object animates. That is the reported
    // "parts move around in sync with player or NPC animation", and it is a distance in units
    // rather than an interpretation of upload ordering.
    if (cN) {
        double dd = 0.0;
        for (int r = 0; r < 3; ++r) {
            const double e = (cSkin[r] - cBind[r]) / cN;
            dd += e * e;
        }
        const float d = static_cast<float>(std::sqrt(dd));
        if (d > g_skinWorstDisplace) g_skinWorstDisplace = d;
        if (d > 3.0f) {
            ++g_skinDisplaced;
            if (g_skinDisplaceReports < 16) {
                ++g_skinDisplaceReports;
                const D3DMATRIX o = FromRegisters(&g_vsConst[kRegObjTM][0], 3);
                Log("DISPLACED SKIN #%u: ps='%s' verts=%u moved %.1f units from its bind pose "
                    "| objTM t=(%.1f %.1f %.1f) | bones used %u, highest %u | draw %u frame %u",
                    g_skinDisplaceReports, g_curPS.albedoSampler, numVertices, d,
                    o._41, o._42, o._43,
                    BonesUsedCount(mesh->usedBones), mesh->maxBone,
                    g_drawIndexThisFrame, g_frames);
            }
        } else ++g_skinAtRest;

        // MEASURE THE DRIFT ITSELF, rather than assuming what "correct" looks like.
        //
        // The previous target - "a rigid part should end up centred on its origin" - was WRONG.
        // These bones legitimately place a part at its position ON the car: bone[13] is a 22
        // degree rotation plus (0, 1.121, -1.440), and the mesh is authored at the origin. An
        // offset result is normal.
        //
        // What is NOT normal is the reported symptom: the render moves while the object's real
        // position does not. So track, per mesh, the world-space centroid and the objTM it was
        // computed under. If objTM is UNCHANGED between two frames but the world centroid has
        // moved, that is the drift, in units, with no assumption about what the right answer is.
        if (!g_curVS.usesBlendWeights && cN) {
            const D3DMATRIX o = FromRegisters(&g_vsConst[kRegObjTM][0], 3);
            const float sx = static_cast<float>(cSkin[0]/cN), sy = static_cast<float>(cSkin[1]/cN),
                        sz = static_cast<float>(cSkin[2]/cN);
            const D3DVECTOR local = {sx, sy, sz};
            const D3DVECTOR w = TransformPoint(&local.x, o);
            // The cached BaseMesh pointer identifies the mesh; entries are stable until the
            // cache is invalidated, and an invalidation just restarts the comparison.
            auto& e = g_driftTrack[reinterpret_cast<unsigned long long>(mesh)];
            if (e.seen && e.frame != g_frames) {
                const bool objSame = std::fabs(e.obj[0]-o._41) < 1e-4f &&
                                     std::fabs(e.obj[1]-o._42) < 1e-4f &&
                                     std::fabs(e.obj[2]-o._43) < 1e-4f;
                const float dx = w.x-e.world[0], dy = w.y-e.world[1], dz = w.z-e.world[2];
                const float moved = std::sqrt(dx*dx + dy*dy + dz*dz);
                if (objSame) {
                    if (moved > 0.01f) {
                        ++g_driftMoved;
                        if (moved > g_driftWorst) g_driftWorst = moved;
                        if (g_driftReports < 16) {
                            ++g_driftReports;
                            const unsigned b0 = mesh->verts[0].idx[0];
                            const float* r = &g_vsConst[boneBase + b0*kRegsPerBone][0];
                            Log("DRIFT #%u: ps='%s' verts=%u bone0=%u | objTM UNCHANGED (%.2f %.2f "
                                "%.2f) but world centroid moved %.3f units",
                                g_driftReports, g_curPS.albedoSampler, numVertices, b0,
                                o._41, o._42, o._43, moved);
                            Log("    was (%8.3f %8.3f %8.3f)  now (%8.3f %8.3f %8.3f)",
                                e.world[0], e.world[1], e.world[2], w.x, w.y, w.z);
                            Log("    bone[%u] translate now (%7.3f %7.3f %7.3f), was (%7.3f %7.3f "
                                "%7.3f)  | palette upload %u bones at draw %u",
                                b0, r[3], r[7], r[11], e.bone[0], e.bone[1], e.bone[2],
                                g_lastBoneUploadBones, g_lastBoneUploadDraw);
                        }
                    } else ++g_driftStill;
                }
            }
            const unsigned b0 = mesh->verts.empty() ? 0u : mesh->verts[0].idx[0];
            const float* r = &g_vsConst[boneBase + b0*kRegsPerBone][0];
            e.seen = true; e.frame = g_frames;
            e.world[0]=w.x; e.world[1]=w.y; e.world[2]=w.z;
            e.obj[0]=o._41; e.obj[1]=o._42; e.obj[2]=o._43;
            e.bone[0]=r[3]; e.bone[1]=r[7]; e.bone[2]=r[11];
        }

        // Catch the exact failing case and dump every input to it.
        //
        // The capture gives a numeric target: a standalone rigid part should end up centred on
        // its own origin, because objTM already carries its real (physics) position. Measured
        // 2026-08-25 - a detached bumper sat at local (0, -0.085, 2.075) and a detached door at
        // (0.958, 0.515, -0.078), i.e. still at their positions ON THE CAR. The bone whose job is
        // to undo that offset is not doing it.
        //
        // Everything upstream is verified: the constants match the device (650k comparisons), the
        // register layout matches all 882 CTABs, and the transform matches the disassembly. So one
        // of the three inputs must differ for these draws specifically, and this prints all of
        // them for the draws that actually come out wrong.
        if (!g_curVS.usesBlendWeights && g_rigidOffsetReports < 20 && cN) {
            const double ox = cSkin[0]/cN, oy = cSkin[1]/cN, oz = cSkin[2]/cN;
            const double off = std::sqrt(ox*ox + oy*oy + oz*oz);
            const double bx = cBind[0]/cN, by = cBind[1]/cN, bz = cBind[2]/cN;
            if (off > 1.0) {                       // skinned result is NOT near its own origin
                ++g_rigidOffsetReports;
                const unsigned b0 = mesh->verts[0].idx[0];
                const float* r = &g_vsConst[boneBase + b0 * kRegsPerBone][0];
                const D3DMATRIX o = FromRegisters(&g_vsConst[kRegObjTM][0], 3);
                Log("OFFSET PART #%u: ps='%s' verts=%u bones=%u bone0=%u",
                    g_rigidOffsetReports, g_curPS.albedoSampler, numVertices,
                    BonesUsedCount(mesh->usedBones), b0);
                Log("    bind centroid   (%8.3f %8.3f %8.3f)", bx, by, bz);
                Log("    skinned centroid(%8.3f %8.3f %8.3f)   |offset| %.3f  <- should be ~0",
                    ox, oy, oz, off);
                Log("    bone[%u] rows    (%7.3f %7.3f %7.3f | %7.3f)", b0, r[0], r[1], r[2], r[3]);
                Log("                    (%7.3f %7.3f %7.3f | %7.3f)", r[4], r[5], r[6], r[7]);
                Log("                    (%7.3f %7.3f %7.3f | %7.3f)", r[8], r[9], r[10], r[11]);
                Log("    objTM translate (%8.3f %8.3f %8.3f) | palette upload was %u bones at "
                    "draw %u, this draw %u",
                    o._41, o._42, o._43, g_lastBoneUploadBones, g_lastBoneUploadDraw,
                    g_drawIndexThisFrame);
            }
        }

        // The population every previous test was blind to. A mesh using one or two bones is
        // posed RIGIDLY - it cannot deform, it can only translate, which is exactly "the mesh
        // matches the car, is not deformed, but moves". The spread test cannot see it because a
        // single bone has no spread to measure.
        // The single-bone population, tested against its own object rather than against a
        // skeleton-size guess.
        if (!g_curVS.usesBlendWeights) {
            const unsigned b0 = mesh->verts.empty() ? 0u : mesh->verts[0].idx[0];
            if (b0 < static_cast<unsigned>(kBonesMax)) {
                if (g_boneWrittenObjGen[b0] == g_objGeneration) {
                    ++g_rigidOwnObject;
                } else {
                    ++g_rigidForeignObject;
                    if (g_rigidForeignReports < 20) {
                        ++g_rigidForeignReports;
                        const D3DMATRIX o = FromRegisters(&g_vsConst[kRegObjTM][0], 3);
                        Log("FOREIGN BONE #%u: ps='%s' verts=%u bone %u was written under objTM "
                            "generation %u but this draw is generation %u (%u objects later) | "
                            "objTM t=(%.1f %.1f %.1f) | moved %.2f | draw %u frame %u",
                            g_rigidForeignReports, g_curPS.albedoSampler, numVertices, b0,
                            g_boneWrittenObjGen[b0], g_objGeneration,
                            g_objGeneration - g_boneWrittenObjGen[b0],
                            o._41, o._42, o._43, d, g_drawIndexThisFrame, g_frames);
                    }
                }
            }
        }

        const unsigned used = BonesUsedCount(mesh->usedBones);
        if (used <= 2) {
            ++g_skinFewBones;
            const unsigned b0 = mesh->maxBone;   // with <=2 bones this is the meaningful one
            const unsigned upl = g_boneWrittenUploadBones[b0];
            // A skeleton's size is a fingerprint. A 1-bone part reading from a 40-bone upload is
            // reading a character's palette, not its own.
            if (upl > used + 4) {
                ++g_skinFewBonesForeign;
                if (g_skinFewBoneReports < 20) {
                    ++g_skinFewBoneReports;
                    const D3DMATRIX o = FromRegisters(&g_vsConst[kRegObjTM][0], 3);
                    Log("RIGID PART #%u: ps='%s' verts=%u uses %u bone(s) (idx %u) but that bone "
                        "came from a %u-BONE upload at draw %u | moved %.2f units | "
                        "objTM t=(%.1f %.1f %.1f) | draw %u frame %u",
                        g_skinFewBoneReports, g_curPS.albedoSampler, numVertices, used, b0,
                        upl, g_boneWrittenDraw[b0], d, o._41, o._42, o._43,
                        g_drawIndexThisFrame, g_frames);
                }
            } else ++g_skinFewBonesOwn;
        }
    }

    g_internal = true;
    g_internalThread = GetCurrentThreadId();
    PhaseAdd(PH_SKIN_POST, tS);
    g_origSetStreamSource(dev, 0, g_skinVB, g_skinRingPos - base, stride);
    g_origSetVertexDeclaration(dev, nullptr);
    dev->SetFVF(kSkinnedFVF);
    g_internal = false;

    g_skinRingPos += need;
    ++g_skinnedConverted;

    return true;
}

void UnbindSkinned(IDirect3DDevice9* dev) {
    g_internal = true;
    g_internalThread = GetCurrentThreadId();
    g_origSetStreamSource(dev, 0, g_stream0, g_stream0Offset, g_stream0Stride);
    g_origSetVertexDeclaration(dev, g_curDecl);
    g_internal = false;
}

bool g_skinVBTried = false;
void CreateSkinBuffer(IDirect3DDevice9* dev) {
    // Tried ONCE. Without this flag a failed creation is retried on every draw - thousands of
    // CreateVertexBuffer calls a frame, each one a round trip across the 32->64 bit bridge, on
    // the exact path that must stay cheap.
    if (g_skinVB || g_skinVBTried || !g_settings.convertSkinned) return;
    g_skinVBTried = true;
    // Clamped: too small and every draw wraps, too large and a DEFAULT-pool allocation this big
    // may simply fail, which would refuse every skinned draw.
    {
        const int mb = (g_settings.skinRingMB < 4) ? 4
                     : (g_settings.skinRingMB > 256) ? 256 : g_settings.skinRingMB;
        g_skinRingBytes = static_cast<UINT>(mb) * 1024u * 1024u;
    }
    // DEFAULT + DYNAMIC + WRITEONLY: the pool D3D9Ex accepts, and the usage that lets
    // NOOVERWRITE/DISCARD locks avoid stalling on the GPU. Not DrawIndexedPrimitiveUP - that
    // path is recorded in sr2-fork.md as a null-pointer crash inside the Remix bridge server.
    g_internal = true;
    g_internalThread = GetCurrentThreadId();
    const HRESULT hr = dev->CreateVertexBuffer(g_skinRingBytes, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY,
                                               0, D3DPOOL_DEFAULT, &g_skinVB, nullptr);
    g_internal = false;
    Log("skinning: vertex buffer %s (%u KB, %u-byte vertices)",
        SUCCEEDED(hr) ? "created" : "FAILED", g_skinRingBytes / 1024,
        static_cast<unsigned>(sizeof(SkinnedVertex)));
}


// parts/28_orphan_sweep.cpp - the orphan sweep.
// NOT a standalone translation unit: #included, in order, by ../sr3rtx.cpp inside its anonymous
// namespace. Originally sr3rtx.cpp lines 23644-23952 (old file:line citations map here).
// ================================================================== orphan sweep
// See the Settings comment above orphanSweep for why this exists. Tally -> probe -> release, in
// that order, and within release: ERASE EVERY CACHE ENTRY FIRST, RELEASE THE REFERENCE LAST - the
// same rule every invalidation function in this file already follows, because it is what keeps
// the pointer-reuse guarantee every cache above depends on. If a released object's address were
// handed to a brand-new one before the stale cache entry naming it were gone, the new object
// would inherit the old one's cached data.
struct OrphanTally { unsigned refs = 0; char kind = 0; };   // kind: 'V' 'I' 'T' 'D' 'S'

// One pass over every cache that takes a reference (see the AddRef inventory each cache's own
// comment documents), building a per-pointer tally of how many of THOSE references target each
// object. Three declaration caches (g_layouts, g_uvDecls[0/1], g_skinFFDecls) and four texture
// caches (g_rtTextures, g_rtCopies, g_clothCache, g_meshAlbedo) can each independently hold a
// share of the SAME pointer, and this deliberately sums them all onto one entry - an object is
// only ever an orphan when NOTHING outside the shim holds it, which means every one of the
// shim's own shares has to be accounted for at once, not cache by cache.
std::unordered_map<IUnknown*, OrphanTally> TallyOrphans() {
    std::unordered_map<IUnknown*, OrphanTally> t;
    auto bump = [&](void* p, char kind) {
        if (!p) return;
        OrphanTally& e = t[reinterpret_cast<IUnknown*>(p)];
        ++e.refs;
        e.kind = kind;
    };
    // 'V' vertex buffers: UvBufferFor (ring and static both), SkinFFBufferFor (every entry, ok
    // or failed - the map key IS the source vb, so one bump per vector entry), GetBaseMesh,
    // RegisterSnoop.
    for (const auto& kv : g_uvBuffers) bump(kv.second.src, 'V');
    for (const auto& kv : g_skinFFBuffers)
        for (size_t i = 0; i < kv.second.size(); ++i) bump(kv.first, 'V');
    for (const auto& kv : g_baseMeshes) bump(kv.second.owner, 'V');
    // THE SHAPE BAKE: one bump per AddRef the bake cache holds - src AND morphVb, one each per
    // entry, exactly the two AddRef calls MorphBakeFor makes when it inserts a new entry. The
    // baked copy itself is shim-owned and never tallied - nothing outside this shim ever
    // references it, so it is never an orphan candidate the way its two inputs are.
    for (const auto& kv : g_morphBakes) { bump(kv.second.src, 'V'); bump(kv.second.morphVb, 'V'); }
    {
        // g_instCache is the one cache the game's own threads also touch (Hook_VBLock and
        // Hook_VBUnlock, both under SnoopGuard - Unlock can even insert), so it is walked
        // under the same lock, never bare.
        SnoopGuard guard;
        for (const auto& kv : g_instCache) bump(kv.first, 'V');
    }
    // 'I' index buffers: GetTightIbShape only takes a reference when the pin switch is on, and
    // that switch is fixed for the process lifetime (LoadSettings runs once) - so either every
    // entry here holds one, or none do.
    if (g_settings.tightenWindowPinIndexBuffers)
        for (const auto& kv : g_tightIbShapes) bump(kv.first, 'I');
    // 'T' textures: g_rtTextures (every entry), g_rtCopies (isRT entries only), g_clothCache
    // (pattern/dobTex, one ref per cache entry - several outfits can share one pattern),
    // g_meshAlbedo (inert today at cacheMeshAlbedo=0, tallied anyway in case it is ever turned on).
    for (IDirect3DBaseTexture9* p : g_rtTextures) bump(p, 'T');
    for (const auto& kv : g_rtCopies) if (kv.second.isRT) bump(kv.first, 'T');
    for (const auto& kv : g_clothCache) if (kv.second.pattern) bump(kv.second.pattern, 'T');
    for (const auto& kv : g_meshAlbedo) if (kv.second) bump(kv.second, 'T');
    // 'D' declarations: the SOURCE declaration is the key everywhere; the clone each cache also
    // stores is owned outright by the shim (fresh from CreateVertexDeclaration) and is never
    // itself an orphan candidate.
    for (const auto& kv : g_layouts) bump(kv.first, 'D');
    for (const auto& kv : g_uvDecls[0]) bump(kv.first, 'D');
    for (const auto& kv : g_uvDecls[1]) bump(kv.first, 'D');
    for (const auto& kv : g_skinFFDecls) bump(kv.first, 'D');
    // 'S' shaders: g_shaders holds both VS and PS under one void*-keyed map.
    for (const auto& kv : g_shaders) bump(kv.first, 'S');
    return t;
}

// Release everything this shim holds pinned to a VERTEX BUFFER that turned out to be an orphan.
// Reuses the existing per-cache invalidation functions - each already erases its own entries and
// releases its own share in one step - plus the one cache (g_instCache) that never had an
// invalidation path at all because nothing used to erase it.
void ForgetInstanceStream(IDirect3DVertexBuffer9* vb) {
    if (!vb) return;
    InvalidateUvBuffers(vb);
    InvalidateBaseMeshes(vb);
    InvalidateSkinFFBuffers(vb);
    InvalidateMorphBakes(vb);   // THE SHAPE BAKE (src OR morphVb role)
    InvalidateOccluders(vb);
    g_recentWrites.erase(vb);
    SnoopGuard guard;
    const auto it = g_instCache.find(vb);
    if (it != g_instCache.end()) {
        // Mirrors Hook_VBUnlock's own addition (g_snoopBytes += add * 2, data and fresh grown
        // together) exactly, using the sizes actually reached rather than re-deriving the growth.
        const size_t rel = it->second.data.size() + it->second.fresh.size();
        g_snoopBytes -= (rel <= g_snoopBytes) ? rel : g_snoopBytes;
        if (g_pendingLock.vb == vb) g_pendingLock = {};
        g_instCache.erase(it);
        vb->Release();
    }
}

// Release everything this shim holds pinned to a TEXTURE that turned out to be an orphan. Mirrors
// g_rtTextures/g_rtCopies/g_clothCache/g_meshAlbedo's own AddRef sites, plus the no-ref
// pointer-keyed caches (g_albedoFormatCache, g_albedoColourlessTextures) that must not outlive it.
void ForgetTexture(IDirect3DBaseTexture9* t) {
    if (!t) return;
    if (g_rtTextures.erase(t)) t->Release();
    {
        const auto it = g_rtCopies.find(t);
        if (it != g_rtCopies.end()) {
            if (it->second.isRT) t->Release();
            if (it->second.copy) it->second.copy->Release();
            g_rtCopies.erase(it);
        }
    }
    // Full teardown of one entry, exactly what ReleaseClothCache does for every entry at once.
    for (auto it = g_clothCache.begin(); it != g_clothCache.end();) {
        if (it->second.pattern == t) {
            if (it->second.generated) {
                g_clothCutoutTex.erase(it->second.generated);
                const auto lt = g_clothTileLayout.find(it->second.generated);
                if (lt != g_clothTileLayout.end()) {
                    if (lt->second.ib) lt->second.ib->Release();
                    if (g_clothRemap == &lt->second) g_clothRemap = nullptr;
                    g_clothTileLayout.erase(lt);
                }
                it->second.generated->Release();
            }
            it->second.pattern->Release();
            it = g_clothCache.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = g_meshAlbedo.begin(); it != g_meshAlbedo.end();) {
        if (it->second == t) { t->Release(); it = g_meshAlbedo.erase(it); }
        else ++it;
    }
    g_albedoFormatCache.erase(t);
    g_albedoColourlessTextures.erase(t);
}

// Release everything this shim holds pinned to a SOURCE DECLARATION that turned out to be an
// orphan. Up to three caches independently AddRef the same source pointer; each is released here
// exactly once per cache that actually holds it, mirroring their own AddRef sites.
void ForgetDeclaration(IDirect3DVertexDeclaration9* d) {
    if (!d) return;
    if (g_layouts.erase(static_cast<void*>(d))) d->Release();
    for (int s = 0; s < 2; ++s) {
        const auto it = g_uvDecls[s].find(d);
        if (it != g_uvDecls[s].end()) {
            if (it->second) it->second->Release();   // our own clone, owned outright
            d->Release();                             // the source's own share
            g_uvDecls[s].erase(it);
        }
    }
    const auto it = g_skinFFDecls.find(d);
    if (it != g_skinFFDecls.end()) {
        if (it->second) it->second->Release();
        d->Release();
        g_skinFFDecls.erase(it);
    }
}

// Release everything this shim holds pinned to a SHADER that turned out to be an orphan. `p` is
// void* because g_shaders holds both vertex and pixel shaders under one untyped key.
void ForgetShader(void* p) {
    if (!p) return;
    if (g_shaders.erase(p)) reinterpret_cast<IUnknown*>(p)->Release();
    g_vfxMaskShaders.erase(reinterpret_cast<IDirect3DPixelShader9*>(p));
    for (auto it = g_tilingForAlbedoCache.begin(); it != g_tilingForAlbedoCache.end();) {
        if (reinterpret_cast<void*>(it->first.vs) == p || reinterpret_cast<void*>(it->first.ps) == p)
            it = g_tilingForAlbedoCache.erase(it);
        else
            ++it;
    }
}

// Release every INDEX BUFFER in `orphans` in one pass over each cache that names one, rather than
// one pass per buffer - g_tightWin/g_idxWin/g_occlMeshes have no by-IB index the way the VB caches
// have g_uvByVB/g_baseMeshByVB/g_occlMeshByVB, so this is the batched equivalent.
void ForgetIndexBuffers(const std::unordered_set<IDirect3DIndexBuffer9*>& orphans) {
    if (orphans.empty()) return;
    for (auto it = g_tightWin.begin(); it != g_tightWin.end();)
        if (orphans.count(it->first.ib)) it = g_tightWin.erase(it); else ++it;
    for (auto it = g_idxWin.begin(); it != g_idxWin.end();)
        if (orphans.count(it->first.ib)) it = g_idxWin.erase(it); else ++it;
    bool touchedOccl = false;
    for (auto it = g_occlMeshes.begin(); it != g_occlMeshes.end();)
        if (orphans.count(it->first.ib)) { it = g_occlMeshes.erase(it); touchedOccl = true; }
        else ++it;
    if (touchedOccl) OcclMeshIndexRebuild();
    for (IDirect3DIndexBuffer9* ib : orphans)
        if (g_tightIbShapes.erase(ib)) ib->Release();
}

// Diagnostic-only: how many bytes an orphan texture holds, approximating the whole mip chain
// (4/3 of the top level). GetLevelDesc is answered LOCALLY on this bridge build (no IPC), and
// this only ever runs for a confirmed orphan, never for every live texture in the tally.
double OrphanTextureApproxMB(IDirect3DBaseTexture9* t) {
    if (!t || t->GetType() != D3DRTYPE_TEXTURE) return 0.0;
    D3DSURFACE_DESC d{};
    if (FAILED(static_cast<IDirect3DTexture9*>(t)->GetLevelDesc(0, &d))) return 0.0;
    return (static_cast<double>(d.Width) * d.Height * 4.0 * 4.0 / 3.0) / (1024.0 * 1024.0);
}

unsigned g_orphanSweeps = 0;
unsigned long long g_orphanReleasedObjects = 0;
double g_orphanReleasedMB = 0.0;
unsigned g_orphanLastReleased = 0;
double g_orphanLastReleasedMB = 0.0;
bool g_orphanBookkeepingBroken = false;

// Runs on the render thread, from Hook_Present, every orphanSweepFrames frames.
void SweepOrphans() {
    if (!g_settings.orphanSweep || g_orphanBookkeepingBroken) return;
    const unsigned cadence = static_cast<unsigned>(max(1, g_settings.orphanSweepFrames));
    if (g_frames % cadence != 0) return;

    const LONGLONG t0 = Now();
    std::unordered_map<IUnknown*, OrphanTally> tally = TallyOrphans();
    if (tally.empty()) return;

    // Currently bound is never an orphan, whatever the tally says - and skipping it here means
    // it is never even probed.
    std::unordered_set<IUnknown*> bound;
    auto mark = [&](void* p) { if (p) bound.insert(reinterpret_cast<IUnknown*>(p)); };
    mark(g_stream0);
    for (int i = 0; i < kMaxStreams; ++i) mark(g_streamVB[i]);
    mark(g_curIB);
    for (int i = 0; i < 8; ++i) mark(g_curTexture[i]);
    mark(g_curDecl);
    mark(g_lastVS);
    mark(g_lastPS);
    mark(g_lastBoundAlbedo);

    unsigned probed = 0;
    std::vector<IDirect3DVertexBuffer9*> vOrphans;
    std::unordered_set<IDirect3DIndexBuffer9*> iOrphans;
    std::vector<IDirect3DBaseTexture9*> tOrphans;
    std::vector<IDirect3DVertexDeclaration9*> dOrphans;
    std::vector<void*> sOrphans;

    for (auto& kv : tally) {
        IUnknown* p = kv.first;
        if (bound.count(p)) continue;
        ++probed;
        const unsigned pub = static_cast<unsigned>(p->AddRef() - 1);
        p->Release();
        if (pub < kv.second.refs) {
            // The bookkeeping safety valve: the shim believes it holds more references than the
            // bridge's own public count says exist, which means the tally itself is wrong - and
            // releasing on top of a wrong tally risks releasing a reference the shim does not
            // actually hold. Stop releasing for the rest of the process; log it once.
            g_orphanBookkeepingBroken = true;
            Log("ORPHAN SWEEP: BOOKKEEPING OVER-COUNT on %p (kind %c) - shim tally says %u, "
                "bridge public refcount says %u. Releasing NOTHING this sweep and NEVER AGAIN "
                "this process.",
                static_cast<void*>(p), kv.second.kind, kv.second.refs, pub);
            break;
        }
        if (pub > kv.second.refs) continue;   // the game still holds it
        switch (kv.second.kind) {
            case 'V': vOrphans.push_back(reinterpret_cast<IDirect3DVertexBuffer9*>(p)); break;
            case 'I': iOrphans.insert(reinterpret_cast<IDirect3DIndexBuffer9*>(p)); break;
            case 'T': tOrphans.push_back(reinterpret_cast<IDirect3DBaseTexture9*>(p)); break;
            // See orphanSweepShadersDecls: pinned unless explicitly switched on.
            case 'D': if (g_settings.orphanSweepShadersDecls) dOrphans.push_back(reinterpret_cast<IDirect3DVertexDeclaration9*>(p)); break;
            case 'S': if (g_settings.orphanSweepShadersDecls) sOrphans.push_back(p); break;
            default: break;
        }
    }
    if (g_orphanBookkeepingBroken) return;

    const unsigned total = static_cast<unsigned>(vOrphans.size() + iOrphans.size() +
                                                  tOrphans.size() + dOrphans.size() + sOrphans.size());
    if (!total) return;

    // Bytes, computed only for confirmed orphans - diagnostic only, never a release decision.
    double mbV = 0.0, mbI = 0.0, mbT = 0.0;
    for (IDirect3DVertexBuffer9* vb : vOrphans) {
        const auto d = g_vbDescCache.find(vb);
        if (d != g_vbDescCache.end()) mbV += d->second.Size / (1024.0 * 1024.0);
    }
    for (IDirect3DIndexBuffer9* ib : iOrphans) {
        const auto s = g_tightIbShapes.find(ib);
        if (s != g_tightIbShapes.end()) mbI += s->second.size / (1024.0 * 1024.0);
    }
    for (IDirect3DBaseTexture9* tex : tOrphans) mbT += OrphanTextureApproxMB(tex);
    const double totalMB = mbV + mbI + mbT;

    if (!g_settings.orphanSweepReportOnly) {
        for (IDirect3DVertexBuffer9* vb : vOrphans) ForgetInstanceStream(vb);
        ForgetIndexBuffers(iOrphans);
        for (IDirect3DBaseTexture9* tex : tOrphans) ForgetTexture(tex);
        for (IDirect3DVertexDeclaration9* d : dOrphans) ForgetDeclaration(d);
        for (void* s : sOrphans) ForgetShader(s);
        g_orphanReleasedObjects += total;
        g_orphanReleasedMB += totalMB;
    }
    g_orphanLastReleased = total;
    g_orphanLastReleasedMB = totalMB;
    ++g_orphanSweeps;

    const double ms = MsSince(t0);
    Log("ORPHAN SWEEP #%u (frame %u): %u resources / %.1f MB kept alive only by the shim, %s - "
        "VB %u/%.1f MB, IB %u/%.1f MB, tex %u/%.1f MB, decl %u, shader %u | probed %u pinned "
        "objects in %.2f ms | cumulative %llu objects / %.1f MB",
        g_orphanSweeps, g_frames, total, totalMB,
        g_settings.orphanSweepReportOnly ? "WOULD be released (report-only)" : "released",
        static_cast<unsigned>(vOrphans.size()), mbV,
        static_cast<unsigned>(iOrphans.size()), mbI,
        static_cast<unsigned>(tOrphans.size()), mbT,
        static_cast<unsigned>(dOrphans.size()), static_cast<unsigned>(sOrphans.size()),
        probed, ms, g_orphanReleasedObjects, g_orphanReleasedMB);
}



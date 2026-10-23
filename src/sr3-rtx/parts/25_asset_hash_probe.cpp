// parts/25_asset_hash_probe.cpp - the asset hash probe.
// NOT a standalone translation unit: #included, in order, by ../sr3rtx.cpp inside its anonymous
// namespace. Originally sr3rtx.cpp lines 21429-22214 (old file:line citations map here).
// ---------------------------------------------------------------- asset hash probe
//
// THE QUESTION: in Remix's "Geometry Hash" debug view, building decals flicker while the camera
// is static, and an NPC already on screen changes colour (hash) when a new character walks into
// view - the player's own character does not. Remix computes that hash (rtx.conf:
// geometryAssetHashRuleString = indices,texcoords,geometrydescriptor) from three things this
// shim controls the input to: the index range [StartIndex, StartIndex+indexCount) read from the
// bound index buffer and rebased by its own minimum; a geometry descriptor built from indexCount
// and vertexCount = (maxIndex-minIndex+1) computed FROM THE INDICES, not from the draw's own
// MinVertexIndex/NumVertices; and the TEXCOORD0 values at the UNIQUE vertices those indices
// reference. This probe replicates all three, per converted draw, across a short burst armed by
// the existing F9 capture key, and reports whether/what actually changes draw to draw - so the
// flicker can be attributed to a real per-frame hash change (and to WHICH component) rather than
// guessed at from a colour on screen.
//
// READ-ONLY: every buffer this touches is locked D3DLOCK_READONLY, or read from the existing
// snoop copy (SnoopCopy) for a dynamic source - never a direct Lock of a dynamic buffer, the same
// hard rule TightenConvertedWindow/UvBufferFor already honour. It binds nothing, issues no draw
// and changes no render state or pixel. With assetHashProbe off, ArmAssetHashProbe still runs
// (called unconditionally from RearmCaptures) but leaves g_hashProbeActive false, and the per-draw
// hook (ProbeAssetHash) returns on its first line - so the switch genuinely gates all of it.

// FNV-1a 64: same offset basis/prime this file already uses for UvKey/SkinMeshKey (see their own
// comments) - kept as a byte-wise primitive here because the index and texcoord components are
// naturally byte/float streams, not a handful of scalars to XOR together.
inline unsigned long long AssetHashFnv1a(const void* data, size_t len,
                                         unsigned long long h = 1469598103934665603ull) {
    const unsigned char* p = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < len; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

// A converted UV pair. Not std::array - this file has no <array> include, and a plain POD with
// no padding hashes exactly like the two floats it holds.
struct AssetHashUv2 { float u, v; };

// Identity: the same game draw across frames - deliberately NOT including minIndex/numVertices
// (TightenConvertedWindow can legitimately change those run to run for the same object) or
// anything about the pose. Exactly the tuple the task spec names.
struct AssetHashIdentity {
    IDirect3DIndexBuffer9* ib;
    UINT startIndex;
    UINT primitiveCount;
    IDirect3DVertexBuffer9* vb;
    UINT streamOffset;
    INT baseVertex;
    bool operator==(const AssetHashIdentity& o) const {
        return ib == o.ib && startIndex == o.startIndex && primitiveCount == o.primitiveCount &&
               vb == o.vb && streamOffset == o.streamOffset && baseVertex == o.baseVertex;
    }
};
struct AssetHashIdentityHash {
    size_t operator()(const AssetHashIdentity& k) const {
        size_t h = reinterpret_cast<size_t>(k.ib) * 0x9E3779B1u;
        h ^= reinterpret_cast<size_t>(k.vb) * 0xC2B2AE35u;
        h ^= (static_cast<size_t>(k.startIndex) * 0x85EBCA6Bu) ^
             (static_cast<size_t>(k.primitiveCount) * 0x27D4EB2Fu);
        h ^= (static_cast<size_t>(k.streamOffset) * 0x165667B1u) ^
             (static_cast<size_t>(static_cast<unsigned>(k.baseVertex)) * 0xD3A2646Cu);
        return h;
    }
};

// Secondary key: catches the game rebuilding a draw (a new IB/VB/StartIndex for what is visibly
// the same object), per the task spec - bound albedo pointer, objTM translation to the cm,
// primitiveCount.
struct AssetHashSecondaryKey {
    IDirect3DBaseTexture9* albedo;
    int cx, cy, cz;   // world translation, rounded to 0.01 (i.e. cm)
    UINT primitiveCount;
    bool operator==(const AssetHashSecondaryKey& o) const {
        return albedo == o.albedo && cx == o.cx && cy == o.cy && cz == o.cz &&
               primitiveCount == o.primitiveCount;
    }
};
struct AssetHashSecondaryHash {
    size_t operator()(const AssetHashSecondaryKey& k) const {
        size_t h = reinterpret_cast<size_t>(k.albedo) * 0x9E3779B1u;
        h ^= (static_cast<size_t>(k.cx) * 0x85EBCA6Bu) ^ (static_cast<size_t>(k.cy) * 0xC2B2AE35u) ^
             (static_cast<size_t>(k.cz) * 0x27D4EB2Fu);
        h ^= static_cast<size_t>(k.primitiveCount) * 0x165667B1u;
        return h;
    }
};

// One point sampled for the detailed flip report: which GLOBAL vertex (baseVertex + raw index
// value), and its converted uv. Capped per identity so a large skinned mesh cannot grow this
// without bound - the cap only limits how far into a big mesh the "first differing vertex" detail
// can reach; the HASH above always covers every unique vertex regardless.
struct AssetHashUvSample { UINT gv; float u, v; };
constexpr unsigned kHashProbeUvSampleCap = 64;
// TOP CHURNERS (assetHashProbeAlbedo): distinct albedo pointers remembered per identity across a
// burst, for LogAssetHashProbeSummary's top-churner report. Small on purpose - this is "which
// textures did we see", not a full history.
constexpr unsigned kHashProbeAlbedoPtrCap = 4;

struct AssetHashComponents {
    bool indexOk = false;   unsigned long long indexHash = 0; UINT indexLo = 0, indexHi = 0;
    bool descOk = false;    unsigned long long descHash = 0;
    bool convOk = false;    unsigned long long convHash = 0;   // texcoords the shim INTENDS Remix to read
    bool srcOk = false;     unsigned long long srcHash = 0;    // raw source texcoords, pre-conversion
    // THE MATERIAL HALF (assetHashProbeAlbedo): Remix's drawCall.getHash() XORs the geometry hash
    // above with materialData.getHash(), which is simply the albedo texture's image hash. This
    // shim cannot compute Remix's own image hash, but the bound texture's IDENTITY changing frame
    // to frame is a necessary condition for that hash to change - so the bound albedo POINTER
    // (reused from EffectiveAlbedo()'s own g_lastEffectiveAlbedo, not re-derived here) stands in
    // for it. "Hash" only to match the four fields above; it is the raw pointer value, and a null
    // pointer (no albedo bound) is itself a valid, comparable identity.
    bool albedoOk = false;  unsigned long long albedoHash = 0;
    // uvStatic/uvDynamic/native/skinRingCPU/skinFF/raw-short2-fallback/not-reproducible
    char path[24] = "";
    const char* refuseWhy = nullptr;   // set on the INDEX component only ("dynamic IB, not read", etc.)
};

struct AssetHashState {
    AssetHashComponents last;
    bool hasLast = false;
    bool everFlipped = false;
    unsigned lastDrawFrame = 0xFFFFFFFFu;
    int cls = 0;   // 0 decal-named, 1 skinned-CPU, 2 skinned-FF
    std::vector<AssetHashUvSample> uvSample;   // last draw's converted uv, ascending global vertex
    // TOP CHURNERS (assetHashProbeAlbedo) - see LogAssetHashProbeSummary's own comment. Updated
    // every draw of this identity, not only on a flip, so an identity that never flips still
    // reports correctly if it ever needs to.
    unsigned flipCount = 0;                 // cross-frame flip events this burst (any component)
    char firstSampler[28] = "";             // g_curPS.firstSampler as of this identity's last draw
    char albedoSamplerName[28] = "";        // g_curPS.albedoSampler (the winning colour map) likewise
    IDirect3DBaseTexture9* albedoPtrsSeen[kHashProbeAlbedoPtrCap] = {};
    unsigned albedoPtrCount = 0;            // distinct pointers recorded above, capped at kHashProbeAlbedoPtrCap
};

struct AssetHashDetailRecord {
    int cls;
    char ps[28];
    UINT numVerts, primCount;
    void* ib; UINT startIndex; UINT primitiveCount; void* vb; UINT streamOffset; INT baseVertex;
    float at[3];
    bool indexDiff, descDiff, convDiff, srcDiff, albedoDiff, pathDiff;
    unsigned long long oldIndexHash, newIndexHash, oldDescHash, newDescHash, oldConvHash, newConvHash,
        oldSrcHash, newSrcHash, oldAlbedoHash, newAlbedoHash;
    char oldPath[24], newPath[24];
    char uvDetail[160];   // "first differing vertex N: (u0,v0) -> (u1,v1)", or "" if not available
    bool sameFrame;       // true: a same-frame duplicate with a different hash, not a cross-frame flip
};

struct AssetHashExample {   // shared shape for churn examples and same-frame-duplicate examples
    int cls;
    char ps[28];
    float at[3];
    UINT primCount;
    char note[96];
};

constexpr size_t kHashProbeIdentityCap = 4096;
constexpr size_t kHashProbeSecondaryCap = 8192;
constexpr unsigned kHashProbeDetailCap = 30;
constexpr unsigned kHashProbeExampleCap = 10;

std::unordered_map<AssetHashIdentity, AssetHashState, AssetHashIdentityHash> g_hashProbeIdentities;
std::unordered_map<AssetHashSecondaryKey, AssetHashIdentity, AssetHashSecondaryHash> g_hashProbeSecondary;
std::vector<AssetHashDetailRecord> g_hashProbeDetail;
std::vector<AssetHashExample> g_hashProbeDupExamples;
std::vector<AssetHashExample> g_hashProbeChurnExamples;

unsigned g_hashProbeIdentitiesSeen[3] = {0, 0, 0};
unsigned g_hashProbeIdentitiesFlipped[3] = {0, 0, 0};
// component order: 0 index, 1 descriptor, 2 texcoord-conversion, 3 texcoord-source, 4 albedo
unsigned g_hashProbeFlipsByComponent[3][5] = {};
unsigned g_hashProbeFlipsByPath[3] = {0, 0, 0};
unsigned g_hashProbeChurnCount[3] = {0, 0, 0};
unsigned g_hashProbeDupSameFrame[3] = {0, 0, 0};
unsigned g_hashProbeIdentityOverflow = 0, g_hashProbeSecondaryOverflow = 0;

bool g_hashProbeActive = false;
unsigned g_hashProbeArmFrame = 0, g_hashProbeEndFrame = 0;
double g_hashProbeMs = 0.0;

void ResetAssetHashProbeBurst() {
    g_hashProbeIdentities.clear();
    g_hashProbeSecondary.clear();
    g_hashProbeDetail.clear();
    g_hashProbeDupExamples.clear();
    g_hashProbeChurnExamples.clear();
    for (int c = 0; c < 3; ++c) {
        g_hashProbeIdentitiesSeen[c] = 0;
        g_hashProbeIdentitiesFlipped[c] = 0;
        for (int k = 0; k < 5; ++k) g_hashProbeFlipsByComponent[c][k] = 0;
        g_hashProbeFlipsByPath[c] = 0;
        g_hashProbeChurnCount[c] = 0;
        g_hashProbeDupSameFrame[c] = 0;
    }
    g_hashProbeIdentityOverflow = 0;
    g_hashProbeSecondaryOverflow = 0;
    g_hashProbeMs = 0.0;
}

void LogAssetHashProbeSummary() {
    g_hashProbeActive = false;
    const unsigned framesCovered = (g_frames > g_hashProbeArmFrame)
                                       ? (g_frames - g_hashProbeArmFrame)
                                       : static_cast<unsigned>(g_settings.assetHashProbeFrames);
    // cls 0's label depends on assetHashProbeWorld: OFF, it only ever holds decal-named draws (the
    // original filter); ON, ProbeAssetHash's widened filter also drops plain textured world draws
    // into the SAME bucket (see worldCandidate there) - not "static" any more because the label
    // has to say which population is actually in it.
    const char* kClsName[3] = {g_settings.assetHashProbeWorld ? "decal/world" : "decal-named",
                               "skinned-CPU", "skinned-FF "};
    unsigned totalSeen = 0, totalFlipped = 0;
    for (int c = 0; c < 3; ++c) {
        totalSeen += g_hashProbeIdentitiesSeen[c];
        totalFlipped += g_hashProbeIdentitiesFlipped[c];
    }
    Log("ASSET HASH PROBE summary: burst armed at frame %u, %u frames covered | %.3f ms total, "
        "%.4f ms/frame | %u identities tracked (cap %u, %u overflow) | %u secondary keys (cap "
        "%u, %u overflow) | %u of %u identities flipped at least once",
        g_hashProbeArmFrame, framesCovered, g_hashProbeMs,
        framesCovered ? g_hashProbeMs / framesCovered : 0.0, totalSeen,
        static_cast<unsigned>(kHashProbeIdentityCap), g_hashProbeIdentityOverflow,
        static_cast<unsigned>(g_hashProbeSecondary.size()),
        static_cast<unsigned>(kHashProbeSecondaryCap), g_hashProbeSecondaryOverflow, totalFlipped,
        totalSeen);
    for (int c = 0; c < 3; ++c) {
        if (!g_hashProbeIdentitiesSeen[c] && !g_hashProbeChurnCount[c] && !g_hashProbeDupSameFrame[c])
            continue;
        Log("    class %s: identities=%u flipped=%u (%.1f%%) | flips by component: index=%u "
            "descriptor=%u texcoord-conversion=%u texcoord-source=%u albedo=%u | draws whose PATH "
            "changed=%u | identity churn (same secondary key, different identity)=%u | same "
            "identity drawn more than once this frame with a different hash=%u",
            kClsName[c], g_hashProbeIdentitiesSeen[c], g_hashProbeIdentitiesFlipped[c],
            g_hashProbeIdentitiesSeen[c]
                ? 100.0 * g_hashProbeIdentitiesFlipped[c] / g_hashProbeIdentitiesSeen[c]
                : 0.0,
            g_hashProbeFlipsByComponent[c][0], g_hashProbeFlipsByComponent[c][1],
            g_hashProbeFlipsByComponent[c][2], g_hashProbeFlipsByComponent[c][3],
            g_hashProbeFlipsByComponent[c][4],
            g_hashProbeFlipsByPath[c], g_hashProbeChurnCount[c], g_hashProbeDupSameFrame[c]);
    }
    unsigned shown = 0;
    for (const auto& d : g_hashProbeDetail) {
        if (shown >= kHashProbeDetailCap) break;
        ++shown;
        Log("    %s #%u [%s]: ps='%s' v=%u p=%u | ib=%p start=%u prims=%u vb=%p off=%u baseVtx=%d | "
            "at(%.2f %.2f %.2f) | index %s(%llx->%llx) desc %s(%llx->%llx) texConv %s(%llx->%llx) "
            "texSrc %s(%llx->%llx) albedo %s(%llx->%llx) | path '%s'->'%s'%s%s",
            d.sameFrame ? "SAME-FRAME DUP" : "FLIP", shown, kClsName[d.cls], d.ps,
            d.numVerts, d.primCount, d.ib, d.startIndex, d.primitiveCount, d.vb, d.streamOffset,
            d.baseVertex, d.at[0], d.at[1], d.at[2], d.indexDiff ? "CHANGED" : "same",
            d.oldIndexHash, d.newIndexHash, d.descDiff ? "CHANGED" : "same", d.oldDescHash,
            d.newDescHash, d.convDiff ? "CHANGED" : "same", d.oldConvHash, d.newConvHash,
            d.srcDiff ? "CHANGED" : "same", d.oldSrcHash, d.newSrcHash,
            d.albedoDiff ? "CHANGED" : "same", d.oldAlbedoHash, d.newAlbedoHash,
            d.oldPath, d.newPath, d.uvDetail[0] ? " | " : "", d.uvDetail);
    }
    unsigned shownDup = 0;
    for (const auto& e : g_hashProbeDupExamples) {
        if (shownDup >= kHashProbeExampleCap) break;
        ++shownDup;
        Log("    DUP EXAMPLE #%u [%s]: ps='%s' at(%.2f %.2f %.2f) prims=%u | %s", shownDup,
            kClsName[e.cls], e.ps, e.at[0], e.at[1], e.at[2], e.primCount, e.note);
    }
    unsigned shownChurn = 0;
    for (const auto& e : g_hashProbeChurnExamples) {
        if (shownChurn >= kHashProbeExampleCap) break;
        ++shownChurn;
        Log("    CHURN EXAMPLE #%u [%s]: ps='%s' at(%.2f %.2f %.2f) prims=%u | %s", shownChurn,
            kClsName[e.cls], e.ps, e.at[0], e.at[1], e.at[2], e.primCount, e.note);
    }
    // TOP CHURNERS (assetHashProbeAlbedo): everything above says HOW MANY draws changed and BY
    // WHICH component; this names WHICH identities changed most, with the two inputs Remix's own
    // drawCall-hash XOR is built from side by side - the pixel shader and albedo sampler that drew
    // them, and every distinct albedo texture pointer this identity was seen bound to across the
    // burst. Off (assetHashProbeAlbedo=0), st.flipCount/firstSampler/albedoSamplerName/
    // albedoPtrsSeen are never populated (see ProbeAssetHash), so this block is skipped rather
    // than print an all-zero table.
    if (g_settings.assetHashProbeAlbedo) {
        std::vector<const AssetHashState*> churners;
        try {
            churners.reserve(g_hashProbeIdentities.size());
            for (const auto& kv : g_hashProbeIdentities)
                if (kv.second.flipCount) churners.push_back(&kv.second);
            std::sort(churners.begin(), churners.end(),
                      [](const AssetHashState* a, const AssetHashState* b) {
                          return a->flipCount > b->flipCount;
                      });
        } catch (...) { churners.clear(); }
        unsigned shownTop = 0;
        for (const AssetHashState* st : churners) {
            if (shownTop >= kHashProbeExampleCap) break;
            ++shownTop;
            char ptrs[4 * 20] = "";
            size_t off = 0;
            for (unsigned i = 0; i < st->albedoPtrCount && off + 20 < sizeof(ptrs); ++i) {
                const int n = _snprintf_s(ptrs + off, sizeof(ptrs) - off, _TRUNCATE, "%s%p",
                                          i ? ", " : "", static_cast<void*>(st->albedoPtrsSeen[i]));
                if (n > 0) off += static_cast<size_t>(n);
            }
            Log("    TOP CHURNER #%u [%s]: flips=%u | pixel shader='%s' albedo sampler='%s' | "
                "albedo pointers seen: %s",
                shownTop, kClsName[st->cls], st->flipCount, st->firstSampler, st->albedoSamplerName,
                ptrs[0] ? ptrs : "(none recorded)");
        }
    }
}

void ArmAssetHashProbe() {
    if (!g_settings.assetHashProbe || g_settings.assetHashProbeFrames <= 0) {
        g_hashProbeActive = false;
        return;
    }
    ResetAssetHashProbeBurst();
    g_hashProbeArmFrame = g_frames;
    g_hashProbeEndFrame = g_frames + static_cast<unsigned>(g_settings.assetHashProbeFrames) - 1u;
    g_hashProbeActive = true;
    Log("ASSET HASH PROBE armed: next %d frames, every CONVERTED draw whose albedo names a Decal "
        "map or that is skinned%s - see the ASSET HASH PROBE summary at the end of the burst",
        g_settings.assetHashProbeFrames,
        g_settings.assetHashProbeWorld
            ? " (assetHashProbeWorld ON: plus any other non-skinned draw with a bound albedo)"
            : "");
}

void TickAssetHashProbeBurst() {
    if (g_hashProbeActive && g_frames > g_hashProbeEndFrame) LogAssetHashProbeSummary();
}

// The index component: [startIndex, startIndex+indexCount) from `ib`, rebased by its own
// minimum, FNV-1a64 over the rebased values as raw bytes in the buffer's own index width. Reuses
// GetTightIbShape (TightenConvertedWindow's own helper) for the dynamic/size/format check, so
// this costs no extra GetDesc bridge round trip beyond what TightenConvertedWindow already paid
// for the very same index buffer a few lines earlier in the caller, for a rigid draw. Never locks
// a DYNAMIC index buffer - refuses and says so, exactly the hard rule.
struct AssetHashIndexResult {
    bool ok = false;
    unsigned long long hash = 0;
    UINT lo = 0, hi = 0, nIdx = 0;
    int indexWidth = 0;
    const char* refuseWhy = nullptr;
    std::vector<UINT> uniqueRaw;   // sorted, de-duplicated raw index VALUES (relative to baseVertex)
};
AssetHashIndexResult ComputeAssetHashIndexComponent(IDirect3DIndexBuffer9* ib, D3DPRIMITIVETYPE type,
                                                    UINT startIndex, UINT primitiveCount) {
    AssetHashIndexResult r;
    if (!ib) { r.refuseWhy = "no index buffer"; return r; }
    if (type != D3DPT_TRIANGLELIST && type != D3DPT_TRIANGLESTRIP && type != D3DPT_TRIANGLEFAN) {
        r.refuseWhy = "primitive type not an indexed triangle list/strip/fan";
        return r;
    }
    const UINT nIdx = VertsForPrimitives(type, primitiveCount);
    if (!nIdx) { r.refuseWhy = "empty draw"; return r; }
    const TightIbShape* shape = GetTightIbShape(ib);
    if (!shape) { r.refuseWhy = "GetDesc failed"; return r; }
    if (shape->dynamic) { r.refuseWhy = "dynamic IB, not read"; return r; }
    const UINT isz = (shape->format == D3DFMT_INDEX32) ? 4 : 2;
    if (static_cast<unsigned long long>(startIndex) * isz + static_cast<unsigned long long>(nIdx) * isz >
        shape->size) {
        r.refuseWhy = "index slice runs past the buffer";
        return r;
    }
    const bool wasInt = g_internal;
    g_internal = true;
    g_internalThread = GetCurrentThreadId();
    void* m = nullptr;
    const HRESULT hr = ib->Lock(startIndex * isz, nIdx * isz, &m, D3DLOCK_READONLY);
    if (FAILED(hr) || !m) {
        g_internal = wasInt;
        r.refuseWhy = "index buffer lock failed";
        return r;
    }
    std::vector<UINT> vals;
    try { vals.resize(nIdx); }
    catch (...) { ib->Unlock(); g_internal = wasInt; r.refuseWhy = "OOM"; return r; }
    UINT lo = 0xFFFFFFFFu, hi = 0;
    for (UINT i = 0; i < nIdx; ++i) {
        const UINT v = (isz == 4) ? static_cast<const unsigned*>(m)[i]
                                  : static_cast<const unsigned short*>(m)[i];
        vals[i] = v;
        if (v < lo) lo = v;
        if (v > hi) hi = v;
    }
    ib->Unlock();
    g_internal = wasInt;

    r.lo = lo; r.hi = hi; r.nIdx = nIdx; r.indexWidth = static_cast<int>(isz);
    if (isz == 4) {
        std::vector<unsigned> rebased(nIdx);
        for (UINT i = 0; i < nIdx; ++i) rebased[i] = vals[i] - lo;
        r.hash = AssetHashFnv1a(rebased.data(), rebased.size() * sizeof(unsigned));
    } else {
        std::vector<unsigned short> rebased(nIdx);
        for (UINT i = 0; i < nIdx; ++i) rebased[i] = static_cast<unsigned short>(vals[i] - lo);
        r.hash = AssetHashFnv1a(rebased.data(), rebased.size() * sizeof(unsigned short));
    }
    // unique, sorted raw values - shared with the texcoord component so the index buffer is
    // locked exactly once per draw for this whole probe.
    try {
        r.uniqueRaw = vals;
        std::sort(r.uniqueRaw.begin(), r.uniqueRaw.end());
        r.uniqueRaw.erase(std::unique(r.uniqueRaw.begin(), r.uniqueRaw.end()), r.uniqueRaw.end());
    } catch (...) { r.uniqueRaw.clear(); }
    r.ok = true;
    return r;
}

// The texcoord component. `uniqueRaw` holds the draw's unique index VALUES, relative to
// baseVertex (per the D3D9 semantics TightenConvertedWindow's own comment works out) - so the
// GLOBAL vertex a value `v` addresses is baseVertex + v. Determines and reproduces the path the
// shim actually takes for THIS draw's texcoords, tags it, and fills both the conversion hash
// (what the shim intends Remix to read) and the source hash (the raw bytes before any
// conversion) - identical for every path except skinRingCPU, where the cloth tile shift is the
// only transform this shim ever applies to a texcoord.
struct AssetHashUvResult {
    bool convOk = false; unsigned long long convHash = 0;
    bool srcOk = false;  unsigned long long srcHash = 0;
    char path[24] = "";
    std::vector<AssetHashUvSample> sample;   // first kHashProbeUvSampleCap unique verts, ascending
};

// Static source: read-lock stream 0 for exactly the [lo,hi] byte span the unique vertices need
// (never the whole buffer) - same D3DLOCK_READONLY | D3DLOCK_NOSYSLOCK flags and g_internal guard
// UvBufferFor's own static branch and ProbeDecalDraw both use. Dynamic source: SnoopCopy the same
// span from the game's own writes, exactly as UvBufferFor's ring path does - never a direct Lock.
bool ReadRigidTexcoordsRaw(const std::vector<UINT>& globalVerts, int srcType, int srcOffset,
                           UINT stride, bool dynamicSrc, std::vector<AssetHashUv2>* out) {
    if (globalVerts.empty() || !g_stream0 || !stride) return false;
    const UINT lo = globalVerts.front(), hi = globalVerts.back();
    const UINT span = hi - lo + 1;
    if (span == 0 || span > 2000000) return false;
    out->resize(globalVerts.size());
    if (dynamicSrc) {
        std::vector<unsigned char> staging;
        try { staging.resize(static_cast<size_t>(span) * stride); } catch (...) { return false; }
        bool fresh = false;
        if (!SnoopCopy(g_stream0, g_stream0Offset + lo * stride, staging.data(),
                       static_cast<UINT>(staging.size()), &fresh) ||
            !fresh)
            return false;
        for (size_t i = 0; i < globalVerts.size(); ++i) {
            const UINT rel = globalVerts[i] - lo;
            const unsigned char* p = staging.data() + static_cast<size_t>(rel) * stride + srcOffset;
            if (srcType == D3DDECLTYPE_FLOAT2) {
                const float* f = reinterpret_cast<const float*>(p);
                (*out)[i] = AssetHashUv2{f[0], f[1]};
            } else {
                const short* s = reinterpret_cast<const short*>(p);
                (*out)[i] = AssetHashUv2{static_cast<float>(s[0]), static_cast<float>(s[1])};
            }
        }
        return true;
    }
    if (!VertexRangeFits(g_stream0, g_stream0Offset, lo, span, stride)) return false;
    const bool wasInt = g_internal;
    g_internal = true;
    g_internalThread = GetCurrentThreadId();
    void* m = nullptr;
    const HRESULT hr = g_stream0->Lock(g_stream0Offset + lo * stride, span * stride, &m,
                                       D3DLOCK_READONLY | D3DLOCK_NOSYSLOCK);
    if (FAILED(hr) || !m) { g_internal = wasInt; return false; }
    const unsigned char* base = static_cast<const unsigned char*>(m);
    for (size_t i = 0; i < globalVerts.size(); ++i) {
        const UINT rel = globalVerts[i] - lo;
        const unsigned char* p = base + static_cast<size_t>(rel) * stride + srcOffset;
        if (srcType == D3DDECLTYPE_FLOAT2) {
            const float* f = reinterpret_cast<const float*>(p);
            (*out)[i] = AssetHashUv2{f[0], f[1]};
        } else {
            const short* s = reinterpret_cast<const short*>(p);
            (*out)[i] = AssetHashUv2{static_cast<float>(s[0]), static_cast<float>(s[1])};
        }
    }
    g_stream0->Unlock();
    g_internal = wasInt;
    return true;
}

AssetHashUvResult ComputeAssetHashUvComponent(const std::vector<UINT>& uniqueRaw, INT baseVertex,
                                              UINT minIndex, UINT numVertices, bool skinnedLocal,
                                              bool skinnedFFLocal, const FFPScope& scope) {
    AssetHashUvResult r;
    if (uniqueRaw.empty()) { strcpy_s(r.path, "not-reproducible"); return r; }

    auto fillSample = [&](const std::vector<AssetHashUv2>& uv, const std::vector<UINT>& gvList) {
        const unsigned n =
            static_cast<unsigned>(min(gvList.size(), static_cast<size_t>(kHashProbeUvSampleCap)));
        try {
            r.sample.resize(n);
            for (unsigned i = 0; i < n; ++i) r.sample[i] = AssetHashUvSample{gvList[i], uv[i].u, uv[i].v};
        } catch (...) { r.sample.clear(); }
    };

    if (skinnedLocal) {
        // THE CPU RING (SkinAndBind): out[i].uv = mesh->verts[i].uv + (cloth shift, if any). `i`
        // there is the LOCAL index into the mesh - v - minIndex, per SkinAndBind's own indices-
        // relative-to-baseVertex arithmetic (ProbeIndexWindow's own comment states it too:
        // "indices are relative to baseVertex, as minIndex is").
        const long long fv = static_cast<long long>(baseVertex) + static_cast<long long>(minIndex);
        strcpy_s(r.path, "skinRingCPU");
        if (fv < 0) return r;
        const BaseMesh* mesh = GetBaseMesh(static_cast<UINT>(fv), numVertices);
        if (!mesh || mesh->verts.size() != numVertices) return r;
        std::vector<AssetHashUv2> src(uniqueRaw.size()), conv(uniqueRaw.size());
        std::vector<UINT> gv;
        try { gv.reserve(uniqueRaw.size()); } catch (...) {}
        size_t k = 0;
        for (const UINT v : uniqueRaw) {
            if (v < minIndex || v >= minIndex + numVertices) continue;   // outside this mesh's window
            const UINT local = v - minIndex;
            const BaseVertex& b = mesh->verts[local];
            const float shift = (g_clothRemap && local < g_clothRemap->shiftRaw.size())
                                    ? g_clothRemap->shiftRaw[local]
                                    : 0.0f;
            src[k] = AssetHashUv2{b.uv[0], b.uv[1]};
            conv[k] = AssetHashUv2{b.uv[0] + shift, b.uv[1]};
            try { gv.push_back(static_cast<UINT>(baseVertex) + v); } catch (...) {}
            ++k;
        }
        if (!k) return r;
        src.resize(k); conv.resize(k);
        r.srcHash = AssetHashFnv1a(src.data(), src.size() * sizeof(AssetHashUv2));
        r.convHash = AssetHashFnv1a(conv.data(), conv.size() * sizeof(AssetHashUv2));
        r.srcOk = r.convOk = true;
        fillSample(conv, gv);
        return r;
    }

    // Global vertex numbers, ascending (uniqueRaw is already sorted) - baseVertex can be
    // negative, so this is signed arithmetic clamped to >=0 exactly like every other read site in
    // this file (SkinAndBind's own "first < 0" guard, etc.).
    std::vector<UINT> gv;
    try { gv.reserve(uniqueRaw.size()); } catch (...) {}
    for (const UINT v : uniqueRaw) {
        const long long g = static_cast<long long>(baseVertex) + static_cast<long long>(v);
        if (g < 0 || g > 0xFFFFFFFFll) continue;
        try { gv.push_back(static_cast<UINT>(g)); } catch (...) {}
    }
    if (gv.empty()) { strcpy_s(r.path, "not-reproducible"); return r; }

    const UINT stride = g_stream0Stride;
    if (skinnedFFLocal) {
        // Hardware skin (SkinViaFixedFunction): stream 0 is left exactly as the game bound it -
        // InstallFloatUV always refuses a skinned draw (g_curLayout.skinned true), so whatever
        // type TEXCOORD0 declares is what reaches the device unconverted.
        if (g_curLayout.texcoordStream != 0 || g_curLayout.texcoordOffset < 0 || !stride) {
            strcpy_s(r.path, "not-reproducible");
            return r;
        }
        const bool isFloat2 = (g_curLayout.texcoordType == D3DDECLTYPE_FLOAT2);
        const bool isShort2 = (g_curLayout.texcoordType == D3DDECLTYPE_SHORT2);
        if (!isFloat2 && !isShort2) { strcpy_s(r.path, "not-reproducible"); return r; }
        // A SHORT2 TEXCOORD0 IS converted here, just not by InstallFloatUV: SkinViaFixedFunction
        // binds UvBufferFor's float2 stream itself and SkinFFDeclarationFor retargets TEXCOORD0
        // onto it - the same unscaled widening, so the values hashed below are what Remix reads.
        // Tagged apart from native FLOAT2 so the two populations can still be told apart.
        strcpy_s(r.path, isFloat2 ? "skinFF" : "skinFFuv");
        const bool dynamicSrc = IsDynamicVB(g_stream0);
        std::vector<AssetHashUv2> uv;
        if (!ReadRigidTexcoordsRaw(gv, g_curLayout.texcoordType, g_curLayout.texcoordOffset, stride,
                                   dynamicSrc, &uv))
            return r;
        r.srcHash = r.convHash = AssetHashFnv1a(uv.data(), uv.size() * sizeof(AssetHashUv2));
        r.srcOk = r.convOk = true;
        fillSample(uv, gv);
        return r;
    }

    // Rigid, non-skinned: the same set/type decision InstallFloatUV/UvBufferFor make for this
    // exact draw (DecalUvSourceSet, texcoord vs texcoord1).
    const int uvSet = DecalUvSourceSet();
    const int srcType = (uvSet == 1) ? g_curLayout.texcoord1Type : g_curLayout.texcoordType;
    const int srcOffset = (uvSet == 1) ? g_curLayout.texcoord1Offset : g_curLayout.texcoordOffset;
    const int srcStream = (uvSet == 1) ? g_curLayout.texcoord1Stream : g_curLayout.texcoordStream;
    if (srcStream != 0 || srcOffset < 0 || !stride || srcOffset + 4 > static_cast<int>(stride)) {
        strcpy_s(r.path, "not-reproducible");
        return r;
    }
    const bool dynamicSrc = IsDynamicVB(g_stream0);
    if (srcType == D3DDECLTYPE_SHORT2) {
        // scope.uvStream: did InstallFloatUV actually convert THIS draw? If so the shim's own
        // conversion is exactly "widen unscaled" (UvBufferFor's own comment: "RAW short values
        // widened to float, NOT scaled") - numerically identical to reading the raw source, so
        // conv==src for every rigid draw; only skinRingCPU's cloth shift ever makes them differ.
        // When the shim did NOT convert, Remix receives the original SHORT2 declaration on stream
        // 0 and discards it (see InstallFloatUV's own comment) - hashed anyway for comparison,
        // tagged apart so that is visible.
        strcpy_s(r.path, scope.uvStream ? (dynamicSrc ? "uvDynamic" : "uvStatic")
                                        : "raw-short2-fallback");
    } else if (srcType == D3DDECLTYPE_FLOAT2) {
        strcpy_s(r.path, "native");
    } else {
        strcpy_s(r.path, "not-reproducible");
        return r;
    }
    std::vector<AssetHashUv2> uv;
    if (!ReadRigidTexcoordsRaw(gv, srcType, srcOffset, stride, dynamicSrc, &uv)) return r;
    r.srcHash = r.convHash = AssetHashFnv1a(uv.data(), uv.size() * sizeof(AssetHashUv2));
    r.srcOk = r.convOk = true;
    fillSample(uv, gv);
    return r;
}

// Round a world-space coordinate to the centimetre, for the secondary key - matches the task
// spec's "objTM translation rounded to 0.01".
inline int AssetHashRoundCm(float v) { return static_cast<int>(std::lround(v * 100.0f)); }

void ProbeAssetHash(IDirect3DDevice9* dev, D3DPRIMITIVETYPE type, INT baseVertex, UINT minIndex,
                     UINT numVertices, UINT startIndex, UINT primitiveCount, bool skinnedLocal,
                     bool skinnedFFLocal, const FFPScope& scope) {
    if (!g_settings.assetHashProbe || !g_hashProbeActive) return;
    const bool decalNamed = g_curPS.isPixelShader && g_curPS.albedoSampler[0] &&
                            StrStrIA(g_curPS.albedoSampler, "Decal") != nullptr;
    const bool isSkinned = g_curLayout.skinned;
    // assetHashProbeWorld: with it OFF this adds nothing - worldCandidate is always false, so the
    // line below reduces to exactly the original "if (!decalNamed && !isSkinned) return;". With it
    // ON, a non-skinned draw that binds SOME albedo (g_lastEffectiveAlbedo, already fresh for THIS
    // draw - EffectiveAlbedo() runs earlier in BeginFFP, before this hook) is admitted too, even
    // when its shader names nothing "Decal": a building's wall/window material never matched
    // decalNamed and was never skinned, so it was never sampled at all until now.
    const bool worldCandidate = g_settings.assetHashProbeWorld && !isSkinned && g_lastEffectiveAlbedo;
    if (!decalNamed && !isSkinned && !worldCandidate) return;

    const LONGLONG t0 = Now();
    // A skinned draw that took neither skin path never reaches here as Disp::Convert (the caller
    // falls back to Disp::PassThrough before this hook runs) - so isSkinned true implies exactly
    // one of skinnedLocal/skinnedFFLocal is true. Folding an impossible combination into class 0
    // rather than asserting costs nothing.
    const int cls = isSkinned ? (skinnedLocal ? 1 : (skinnedFFLocal ? 2 : 0)) : 0;

    AssetHashIdentity key{g_curIB, startIndex, primitiveCount, g_stream0, g_stream0Offset, baseVertex};
    auto it = g_hashProbeIdentities.find(key);
    if (it == g_hashProbeIdentities.end()) {
        if (g_hashProbeIdentities.size() >= kHashProbeIdentityCap) {
            ++g_hashProbeIdentityOverflow;
            g_hashProbeMs += MsSince(t0);
            return;
        }
        try { it = g_hashProbeIdentities.emplace(key, AssetHashState{}).first; }
        catch (...) { g_hashProbeMs += MsSince(t0); return; }
        it->second.cls = cls;
        ++g_hashProbeIdentitiesSeen[cls];
    }
    AssetHashState& st = it->second;

    const AssetHashIndexResult idx =
        ComputeAssetHashIndexComponent(g_curIB, type, startIndex, primitiveCount);
    AssetHashComponents cur;
    cur.indexOk = idx.ok; cur.indexHash = idx.hash; cur.indexLo = idx.lo; cur.indexHi = idx.hi;
    cur.refuseWhy = idx.refuseWhy;
    if (idx.ok) {
        struct DescPacked { UINT indexCount, vertexCount; int primType, indexWidth; };
        const DescPacked dp{idx.nIdx, idx.hi - idx.lo + 1u, static_cast<int>(type), idx.indexWidth};
        cur.descHash = AssetHashFnv1a(&dp, sizeof(dp));
        cur.descOk = true;
    }
    const AssetHashUvResult uv = ComputeAssetHashUvComponent(
        idx.uniqueRaw, baseVertex, minIndex, numVertices, skinnedLocal, skinnedFFLocal, scope);
    cur.convOk = uv.convOk; cur.convHash = uv.convHash;
    cur.srcOk = uv.srcOk; cur.srcHash = uv.srcHash;
    strcpy_s(cur.path, uv.path);

    // THE MATERIAL HALF (assetHashProbeAlbedo): reuses EffectiveAlbedo()'s own pick rather than
    // re-deriving which sampler wins - g_lastEffectiveAlbedo is already fresh for THIS draw, set
    // earlier in BeginFFP before this hook ever runs. Recorded even when null: "no albedo bound"
    // is itself a stable identity, and a flip into or out of null is exactly the kind of churn
    // this exists to catch.
    cur.albedoOk = g_settings.assetHashProbeAlbedo;
    cur.albedoHash = cur.albedoOk ? static_cast<unsigned long long>(
                                        reinterpret_cast<uintptr_t>(g_lastEffectiveAlbedo))
                                  : 0;

    const bool sameFrame = (st.lastDrawFrame == g_frames);
    if (st.hasLast) {
        const bool indexDiff = cur.indexOk && st.last.indexOk && cur.indexHash != st.last.indexHash;
        const bool descDiff = cur.descOk && st.last.descOk && cur.descHash != st.last.descHash;
        const bool convDiff = cur.convOk && st.last.convOk && cur.convHash != st.last.convHash;
        const bool srcDiff = cur.srcOk && st.last.srcOk && cur.srcHash != st.last.srcHash;
        const bool albedoDiff = cur.albedoOk && st.last.albedoOk && cur.albedoHash != st.last.albedoHash;
        const bool pathDiff = strcmp(cur.path, st.last.path) != 0;
        if (indexDiff || descDiff || convDiff || srcDiff || albedoDiff || pathDiff) {
            if (sameFrame) {
                ++g_hashProbeDupSameFrame[cls];
                if (g_hashProbeDupExamples.size() < kHashProbeExampleCap) {
                    AssetHashExample e{};
                    e.cls = cls;
                    strncpy_s(e.ps, g_curPS.albedoSampler, _TRUNCATE);
                    e.at[0] = g_lastWorld._41; e.at[1] = g_lastWorld._42; e.at[2] = g_lastWorld._43;
                    e.primCount = primitiveCount;
                    _snprintf_s(e.note, _TRUNCATE, "%s%s%s%s%schanged within the same frame",
                                indexDiff ? "index " : "", descDiff ? "descriptor " : "",
                                (convDiff || srcDiff) ? "texcoord " : "", albedoDiff ? "albedo " : "",
                                pathDiff ? "path " : "");
                    try { g_hashProbeDupExamples.push_back(e); } catch (...) {}
                }
            } else {
                if (!st.everFlipped) { st.everFlipped = true; ++g_hashProbeIdentitiesFlipped[cls]; }
                ++st.flipCount;   // TOP CHURNERS: every cross-frame flip, any component, this burst
                if (indexDiff) ++g_hashProbeFlipsByComponent[cls][0];
                if (descDiff) ++g_hashProbeFlipsByComponent[cls][1];
                if (convDiff) ++g_hashProbeFlipsByComponent[cls][2];
                if (srcDiff) ++g_hashProbeFlipsByComponent[cls][3];
                if (albedoDiff) ++g_hashProbeFlipsByComponent[cls][4];
                if (pathDiff) ++g_hashProbeFlipsByPath[cls];
                if (g_hashProbeDetail.size() < kHashProbeDetailCap) {
                    AssetHashDetailRecord d{};
                    d.cls = cls;
                    strncpy_s(d.ps, g_curPS.albedoSampler, _TRUNCATE);
                    d.numVerts = numVertices; d.primCount = primitiveCount;
                    d.ib = g_curIB; d.startIndex = startIndex; d.primitiveCount = primitiveCount;
                    d.vb = g_stream0; d.streamOffset = g_stream0Offset; d.baseVertex = baseVertex;
                    d.at[0] = g_lastWorld._41; d.at[1] = g_lastWorld._42; d.at[2] = g_lastWorld._43;
                    d.indexDiff = indexDiff; d.descDiff = descDiff; d.convDiff = convDiff;
                    d.srcDiff = srcDiff; d.albedoDiff = albedoDiff; d.pathDiff = pathDiff;
                    d.oldIndexHash = st.last.indexHash; d.newIndexHash = cur.indexHash;
                    d.oldDescHash = st.last.descHash; d.newDescHash = cur.descHash;
                    d.oldConvHash = st.last.convHash; d.newConvHash = cur.convHash;
                    d.oldSrcHash = st.last.srcHash; d.newSrcHash = cur.srcHash;
                    d.oldAlbedoHash = st.last.albedoHash; d.newAlbedoHash = cur.albedoHash;
                    strncpy_s(d.oldPath, st.last.path, _TRUNCATE);
                    strncpy_s(d.newPath, cur.path, _TRUNCATE);
                    d.uvDetail[0] = '\0';
                    d.sameFrame = false;
                    if (convDiff || srcDiff) {
                        // First differing unique vertex between the retained sample (last draw)
                        // and this one - both capped at kHashProbeUvSampleCap entries in the same
                        // ascending order, so a mesh larger than that can miss a difference that
                        // lands past the sampled window; the HASH above already caught the flip
                        // regardless of whether this detail can locate it.
                        const size_t n = min(st.uvSample.size(), uv.sample.size());
                        for (size_t i = 0; i < n; ++i) {
                            if (st.uvSample[i].gv != uv.sample[i].gv) break;   // sample sets diverged
                            if (st.uvSample[i].u != uv.sample[i].u || st.uvSample[i].v != uv.sample[i].v) {
                                _snprintf_s(d.uvDetail, _TRUNCATE,
                                            "first differing vertex %u: (%.4f,%.4f) -> (%.4f,%.4f)",
                                            uv.sample[i].gv, st.uvSample[i].u, st.uvSample[i].v,
                                            uv.sample[i].u, uv.sample[i].v);
                                break;
                            }
                        }
                    }
                    try { g_hashProbeDetail.push_back(d); } catch (...) {}
                }
            }
        }
    }
    st.last = cur;
    st.hasLast = true;
    st.lastDrawFrame = g_frames;
    st.uvSample = uv.sample;
    // TOP CHURNERS bookkeeping (assetHashProbeAlbedo): which shader/sampler last drew this
    // identity, and every distinct albedo pointer it has been bound to this burst.
    if (g_settings.assetHashProbeAlbedo) {
        strncpy_s(st.firstSampler, g_curPS.firstSampler, _TRUNCATE);
        strncpy_s(st.albedoSamplerName, g_curPS.albedoSampler, _TRUNCATE);
        bool seenPtr = false;
        for (unsigned i = 0; i < st.albedoPtrCount; ++i)
            if (st.albedoPtrsSeen[i] == g_lastEffectiveAlbedo) { seenPtr = true; break; }
        if (!seenPtr && st.albedoPtrCount < kHashProbeAlbedoPtrCap)
            st.albedoPtrsSeen[st.albedoPtrCount++] = g_lastEffectiveAlbedo;
    }

    // Secondary key: same visible object, different identity = the game rebuilt the draw.
    AssetHashSecondaryKey sk{g_lastEffectiveAlbedo, AssetHashRoundCm(g_lastWorld._41),
                             AssetHashRoundCm(g_lastWorld._42), AssetHashRoundCm(g_lastWorld._43),
                             primitiveCount};
    auto sit = g_hashProbeSecondary.find(sk);
    if (sit == g_hashProbeSecondary.end()) {
        if (g_hashProbeSecondary.size() < kHashProbeSecondaryCap) {
            try { g_hashProbeSecondary.emplace(sk, key); } catch (...) {}
        } else {
            ++g_hashProbeSecondaryOverflow;
        }
    } else if (!(sit->second == key)) {
        ++g_hashProbeChurnCount[cls];
        if (g_hashProbeChurnExamples.size() < kHashProbeExampleCap) {
            AssetHashExample e{};
            e.cls = cls;
            strncpy_s(e.ps, g_curPS.albedoSampler, _TRUNCATE);
            e.at[0] = g_lastWorld._41; e.at[1] = g_lastWorld._42; e.at[2] = g_lastWorld._43;
            e.primCount = primitiveCount;
            strcpy_s(e.note, "same albedo/position/prim-count, different (ib/vb/start/baseVertex) identity");
            try { g_hashProbeChurnExamples.push_back(e); } catch (...) {}
        }
        sit->second = key;
    }

    g_hashProbeMs += MsSince(t0);
}


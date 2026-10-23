// parts/18_occlusion_cpu.cpp - the shim's CPU occlusion culler.
// NOT a standalone translation unit: #included, in order, by ../sr3rtx.cpp inside its anonymous
// namespace. Originally sr3rtx.cpp lines 12705-13578 (old file:line citations map here).
// ================================================================== CPU occlusion culling
// See the settings comment. Everything below runs on the main thread except OcclWorker, which
// only ever reads the copies handed to it and writes the result map under g_occlMu.
struct OcclMesh {
    std::vector<float> pos;        // object space, xyz per vertex
    std::vector<unsigned> idx;     // triangle list
    float bmin[3] = {0, 0, 0}, bmax[3] = {0, 0, 0};
    IDirect3DVertexBuffer9* vb = nullptr;
    UINT byteBegin = 0, byteEnd = 0;   // the slice of vb this mesh was read from
    bool bad = false;
    unsigned lastUsed = 0;         // frame; the cache evicts what the city has streamed away
};
struct OcclMeshKey {
    IDirect3DVertexBuffer9* vb; IDirect3DIndexBuffer9* ib; UINT first, count, start, prims; int type;
    bool operator==(const OcclMeshKey& o) const {
        return vb == o.vb && ib == o.ib && first == o.first && count == o.count && start == o.start &&
               prims == o.prims && type == o.type;
    }
};
struct OcclMeshKeyHash {
    size_t operator()(const OcclMeshKey& k) const {
        size_t h = reinterpret_cast<size_t>(k.vb) * 0x9E3779B1u;
        h ^= reinterpret_cast<size_t>(k.ib) + 0x7F4A7C15u + (h << 6) + (h >> 2);
        h ^= (k.first * 0x85EBCA6Bu) + (k.count * 0xC2B2AE35u) + (k.start * 0x27D4EB2Fu) + (k.prims * 0x165667B1u) + k.type;
        return h;
    }
};
std::unordered_map<OcclMeshKey, std::shared_ptr<OcclMesh>, OcclMeshKeyHash> g_occlMeshes;
std::unordered_map<IDirect3DVertexBuffer9*, std::vector<OcclMeshKey>> g_occlMeshByVB;   // for invalidation
void OcclMeshIndexRebuild() {
    g_occlMeshByVB.clear();
    for (const auto& kv : g_occlMeshes) { try { g_occlMeshByVB[kv.first.vb].push_back(kv.first); } catch (...) {} }
}
struct OcclOccluder {
    std::shared_ptr<OcclMesh> mesh; D3DMATRIX toClip; float area;
    D3DMATRIX world;         // object -> world, for the containment check against its box
    float t[3];              // world translation, to tie the geometry to the box it belongs to
    unsigned long long ident;   // mesh + translation: is this the same occluder as last job
    DWORD cull;              // D3DRS_CULLMODE at the draw: a face the game culls cannot occlude
    bool mirrored;           // negative-determinant world: the winding flips
    unsigned short id;       // object id, assigned per job from the quantised translation
};
struct OcclBox { IDirect3DQuery9* query; float c[8][3]; float t[3]; float size[3]; unsigned long long objKey; };
// Translation quantised to half a unit: every part of one object, and its box, share a key.
inline unsigned long long OcclKeyOf(const float* t) {
    const long long qx = static_cast<long long>(std::floor(t[0] * 2.0f)) & 0x1FFFFF;
    const long long qy = static_cast<long long>(std::floor(t[1] * 2.0f)) & 0x1FFFFF;
    const long long qz = static_cast<long long>(std::floor(t[2] * 2.0f)) & 0x1FFFFF;
    return (static_cast<unsigned long long>(qx) << 42) | (static_cast<unsigned long long>(qy) << 21) | static_cast<unsigned long long>(qz);
}
// The object a query is about: translation plus box size, so two boxes at one origin differ.
inline unsigned long long OcclObjKey(const float* t, const float* size) {
    unsigned long long k = OcclKeyOf(t) * 0x9E3779B97F4A7C15ull;
    for (int i = 0; i < 3; ++i) {
        const unsigned long long q = static_cast<unsigned long long>(static_cast<long long>(std::floor(size[i] * 4.0f)) & 0xFFFF);
        k = (k ^ q) * 0x100000001B3ull;
    }
    return k;
}
struct OcclFrame {
    std::vector<OcclOccluder> occluders;
    std::vector<OcclBox> boxes;
    D3DMATRIX viewProj = {};
    bool haveViewProj = false;
    unsigned frame = 0;
};
struct OcclResult { bool occluded; unsigned frame; unsigned streak; };
OcclFrame g_occlBuilding;                 // this frame's, main thread only
std::mutex g_occlMu;
std::condition_variable g_occlCv;
OcclFrame g_occlJob;
bool g_occlJobReady = false, g_occlWorkerBusy = false, g_occlWorkerStarted = false;
std::unordered_map<unsigned long long, OcclResult> g_occlResults;   // under g_occlMu; keyed by object, or by query pointer
unsigned g_occlMeshBudgetUsed = 0, g_occlMeshBudgetFrame = 0;
unsigned long long g_occlSkippedBudget = 0, g_occlNearDistVisible = 0;
unsigned g_occlJobReports = 0;
unsigned long long g_occlRecOffered = 0, g_occlRecNoMesh = 0, g_occlRecOffscreen = 0, g_occlRecTaken = 0,
                   g_occlJobOccluders = 0;
unsigned long long g_occlBoxes = 0, g_occlOccluders = 0, g_occlTrisRaster = 0, g_occlTested = 0,
                   g_occlCulled = 0, g_occlAnswered0 = 0, g_occlJobsRun = 0, g_occlJobsDropped = 0,
                   g_occlMeshBad = 0, g_occlNearVisible = 0, g_occlOffscreenVisible = 0;
double g_occlWorkerMs = 0.0;
// The object each query was last issued for (render thread only, like RecordProxyBox), and how
// often a query pointer changed object - the measurement behind occlusionKeyByObject.
std::unordered_map<IDirect3DQuery9*, unsigned long long> g_occlQueryObj;
unsigned long long g_occlQueryRemaps = 0;
float g_occlRenderOffsetMax = 0.0f;
// Flicker instrumentation (worker, under g_occlMu when written): verdict flips while the camera
// is still, attributed by what the failing pixel held; pool churn; self-id hits; containment.
unsigned long long g_occlStillJobs = 0, g_occlFlipVis = 0, g_occlFlipCull = 0, g_occlFlipSelf = 0,
                   g_occlFlipUnwritten = 0, g_occlFlipMargin = 0, g_occlFlipFarther = 0,
                   g_occlChurnUsedAdd = 0, g_occlChurnUsedRem = 0, g_occlChurnOffAdd = 0, g_occlChurnOffRem = 0,
                   g_occlSelfFound = 0, g_occlPokeBoxes = 0;
double g_occlPokeMax = 0.0;
unsigned g_occlFlipExplained = 0, g_occlPokeExplained = 0;
unsigned long long g_occlCapSkipped = 0, g_occlStickyKept = 0,
                   g_occlFlipNotOffered = 0, g_occlFlipNotUsed = 0, g_occlFlipUsedNotCov = 0;

inline void OcclTransform(const float* p, const D3DMATRIX& m, float* out4) {
    out4[0] = p[0] * m._11 + p[1] * m._21 + p[2] * m._31 + m._41;
    out4[1] = p[0] * m._12 + p[1] * m._22 + p[2] * m._32 + m._42;
    out4[2] = p[0] * m._13 + p[1] * m._23 + p[2] * m._33 + m._43;
    out4[3] = p[0] * m._14 + p[1] * m._24 + p[2] * m._34 + m._44;
}

unsigned long long g_occlMeshInvWhole = 0, g_occlMeshInvRange = 0, g_occlMeshInvSpared = 0;
void InvalidateOccluders(IDirect3DVertexBuffer9* vb, UINT offset, UINT size) {
    const bool whole = (size == 0) || !g_settings.occlusionRangeInvalidation;
    auto hit = [&](const OcclMesh& m) { return whole || (m.byteBegin < offset + size && offset < m.byteEnd); };
    if (g_settings.indexedInvalidation) {
        const auto list = g_occlMeshByVB.find(vb);
        if (list == g_occlMeshByVB.end()) return;
        if (whole) {
            for (const OcclMeshKey& k : list->second) g_occlMeshes.erase(k);
            g_occlMeshInvWhole += list->second.size();
            g_occlMeshByVB.erase(list);
            return;
        }
        std::vector<OcclMeshKey> kept;
        for (const OcclMeshKey& k : list->second) {
            const auto it = g_occlMeshes.find(k);
            if (it == g_occlMeshes.end()) continue;
            if (hit(*it->second)) { g_occlMeshes.erase(it); ++g_occlMeshInvRange; }
            else { try { kept.push_back(k); } catch (...) {} ++g_occlMeshInvSpared; }
        }
        if (kept.empty()) g_occlMeshByVB.erase(list); else list->second.swap(kept);
        return;
    }
    for (auto it = g_occlMeshes.begin(); it != g_occlMeshes.end();)
        if (it->first.vb == vb && hit(*it->second)) it = g_occlMeshes.erase(it); else ++it;
}

// Reads a rigid draw's geometry once. Two bridge locks per mesh, budgeted per frame.
std::shared_ptr<OcclMesh> OcclMeshFor(IDirect3DDevice9* dev, D3DPRIMITIVETYPE type, INT baseVertex,
                                      UINT minIndex, UINT numVertices, UINT startIndex, UINT prims) {
    if (!g_stream0 || !g_curIB || !g_stream0Stride || numVertices < 3 || !prims) return nullptr;
    if (type != D3DPT_TRIANGLELIST && type != D3DPT_TRIANGLESTRIP) return nullptr;
    const UINT first = static_cast<UINT>(max(0, baseVertex + static_cast<INT>(minIndex)));
    OcclMeshKey key{g_stream0, g_curIB, first, numVertices, startIndex, prims, static_cast<int>(type)};
    const auto it = g_occlMeshes.find(key);
    if (it != g_occlMeshes.end()) { it->second->lastUsed = g_frames; return it->second->bad ? nullptr : it->second; }
    if (g_occlMeshBudgetFrame != g_frames) { g_occlMeshBudgetFrame = g_frames; g_occlMeshBudgetUsed = 0; }
    if (g_occlMeshBudgetUsed >= static_cast<unsigned>(g_settings.occlusionMeshBudget)) return nullptr;
    // The cache filled at 6000 in the last run and stayed full, so no new mesh could enter and
    // every new block was invisible to the culler. Evicted by age now (KickOcclusionJob).
    if (g_occlMeshes.size() > 20000) return nullptr;
    ++g_occlMeshBudgetUsed;
    auto mesh = std::make_shared<OcclMesh>();
    mesh->vb = g_stream0;
    mesh->bad = true;
    mesh->lastUsed = g_frames;
    mesh->byteBegin = g_stream0Offset + first * g_stream0Stride;
    mesh->byteEnd = mesh->byteBegin + numVertices * g_stream0Stride;
    g_occlMeshes[key] = mesh;      // recorded even when bad, so it is not retried every frame
    try { g_occlMeshByVB[g_stream0].push_back(key); } catch (...) {}
    if (g_curLayout.posOffset < 0 ||
        (g_curLayout.posType != D3DDECLTYPE_FLOAT3 && g_curLayout.posType != D3DDECLTYPE_FLOAT4) ||
        !VertexRangeFits(g_stream0, g_stream0Offset, first, numVertices, g_stream0Stride)) {
        ++g_occlMeshBad;
        return nullptr;
    }
    const bool wasInt = g_internal;
    g_internal = true;
    g_internalThread = GetCurrentThreadId();
    // positions
    void* m = nullptr;
    if (SUCCEEDED(g_stream0->Lock(g_stream0Offset + first * g_stream0Stride, numVertices * g_stream0Stride,
                                  &m, D3DLOCK_READONLY | D3DLOCK_NOSYSLOCK)) && m) {
        try {
            mesh->pos.resize(static_cast<size_t>(numVertices) * 3);
            const unsigned char* b = static_cast<const unsigned char*>(m);
            float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
            for (UINT i = 0; i < numVertices; ++i) {
                const float* p = reinterpret_cast<const float*>(b + i * g_stream0Stride + g_curLayout.posOffset);
                for (int k = 0; k < 3; ++k) {
                    mesh->pos[static_cast<size_t>(i) * 3 + k] = p[k];
                    if (p[k] < mn[k]) mn[k] = p[k];
                    if (p[k] > mx[k]) mx[k] = p[k];
                }
            }
            memcpy(mesh->bmin, mn, sizeof(mn));
            memcpy(mesh->bmax, mx, sizeof(mx));
        } catch (...) { mesh->pos.clear(); }
        g_stream0->Unlock();
    }
    // indices
    if (!mesh->pos.empty()) {
        const UINT nIdx = (type == D3DPT_TRIANGLESTRIP) ? prims + 2 : prims * 3;
        D3DINDEXBUFFER_DESC ibd{};
        void* im = nullptr;
        if (SUCCEEDED(g_curIB->GetDesc(&ibd)) && ibd.Size) {
            const UINT isz = (ibd.Format == D3DFMT_INDEX32) ? 4 : 2;
            if (startIndex * isz + nIdx * isz <= ibd.Size &&
                SUCCEEDED(g_curIB->Lock(startIndex * isz, nIdx * isz, &im, D3DLOCK_READONLY)) && im) {
                try {
                    std::vector<unsigned> raw(nIdx);
                    for (UINT i = 0; i < nIdx; ++i)
                        raw[i] = (isz == 4) ? static_cast<const unsigned*>(im)[i]
                                            : static_cast<const unsigned short*>(im)[i];
                    const UINT tris = (type == D3DPT_TRIANGLESTRIP) ? (nIdx >= 2 ? nIdx - 2 : 0) : nIdx / 3;
                    mesh->idx.reserve(static_cast<size_t>(tris) * 3);
                    for (UINT t = 0; t < tris; ++t) {
                        unsigned a, b2, c;
                        if (type == D3DPT_TRIANGLESTRIP) { a = raw[t]; b2 = raw[t + 1]; c = raw[t + 2]; if (t & 1) { const unsigned tmp = b2; b2 = c; c = tmp; } }
                        else { a = raw[t * 3]; b2 = raw[t * 3 + 1]; c = raw[t * 3 + 2]; }
                        // ring-relative: index + baseVertex - first
                        const long long ra = static_cast<long long>(baseVertex) + a - first;
                        const long long rb = static_cast<long long>(baseVertex) + b2 - first;
                        const long long rc = static_cast<long long>(baseVertex) + c - first;
                        if (ra < 0 || rb < 0 || rc < 0 || ra >= numVertices || rb >= numVertices || rc >= numVertices) continue;
                        if (ra == rb || rb == rc || ra == rc) continue;
                        mesh->idx.push_back(static_cast<unsigned>(ra));
                        mesh->idx.push_back(static_cast<unsigned>(rb));
                        mesh->idx.push_back(static_cast<unsigned>(rc));
                    }
                } catch (...) { mesh->idx.clear(); }
                g_curIB->Unlock();
            }
        }
    }
    g_internal = wasInt;
    if (mesh->pos.empty() || mesh->idx.empty()) { ++g_occlMeshBad; return nullptr; }
    mesh->bad = false;
    return mesh;
}

// A rigid opaque converted draw this frame: an occluder candidate, ranked by screen size.
void RecordOccluder(IDirect3DDevice9* dev, D3DPRIMITIVETYPE type, INT baseVertex, UINT minIndex,
                    UINT numVertices, UINT startIndex, UINT prims) {
    if (!g_settings.occlusionCull) return;
    ++g_occlRecOffered;
    auto mesh = OcclMeshFor(dev, type, baseVertex, minIndex, numVertices, startIndex, prims);
    if (!mesh) { ++g_occlRecNoMesh; return; }
    OcclOccluder o;
    o.mesh = mesh;
    o.toClip = Multiply(g_lastWorld, g_lastViewProj);
    o.world = g_lastWorld;
    o.t[0] = g_lastWorld._41; o.t[1] = g_lastWorld._42; o.t[2] = g_lastWorld._43;
    o.ident = (static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(mesh.get())) * 0x9E3779B97F4A7C15ull) ^ OcclKeyOf(o.t);
    o.cull = g_rsShadow[D3DRS_CULLMODE];
    {
        const D3DMATRIX& w = g_lastWorld;
        const float det = w._11 * (w._22 * w._33 - w._23 * w._32) - w._12 * (w._21 * w._33 - w._23 * w._31) +
                          w._13 * (w._21 * w._32 - w._22 * w._31);
        o.mirrored = det < 0.0f;
    }
    o.id = 0xFFFF;
    // screen-space size of the object-space box, for ranking; a box that touches the near
    // plane counts as huge
    float minx = 1e30f, maxx = -1e30f, miny = 1e30f, maxy = -1e30f;
    bool nearHit = false;
    for (int i = 0; i < 8; ++i) {
        const float p[3] = {(i & 1) ? mesh->bmax[0] : mesh->bmin[0], (i & 2) ? mesh->bmax[1] : mesh->bmin[1],
                            (i & 4) ? mesh->bmax[2] : mesh->bmin[2]};
        float c4[4];
        OcclTransform(p, o.toClip, c4);
        if (c4[3] <= 1e-3f) { nearHit = true; break; }
        const float x = c4[0] / c4[3], y = c4[1] / c4[3];
        if (x < minx) minx = x; if (x > maxx) maxx = x; if (y < miny) miny = y; if (y > maxy) maxy = y;
    }
    o.area = nearHit ? 1e9f : (maxx - minx) * (maxy - miny);
    if (!nearHit && (maxx < -1.0f || minx > 1.0f || maxy < -1.0f || miny > 1.0f)) { ++g_occlRecOffscreen; return; }
    ++g_occlRecTaken;
    if (!g_occlBuilding.haveViewProj) { g_occlBuilding.viewProj = g_lastViewProj; g_occlBuilding.haveViewProj = true; }
    try { g_occlBuilding.occluders.push_back(o); } catch (...) {}
}

// The proxy inside a query: the object's box, camera-relative, from the proxy shader's constants.
void RecordProxyBox(IDirect3DQuery9* query) {
    if (!g_settings.occlusionCull || !query) return;
    const float* sc = &g_vsConst[0][0];    // Scale
    const float* of = &g_vsConst[1][0];    // Offset
    const float* f = &g_vsConst[5][0];     // World_xform: m00 m10 m20 m01 m11 m21 m02 m12 m22 tx ty tz
    const float* ro = &g_vsConst[38][0];   // Render_offset
    D3DMATRIX W = kIdentity;
    W._11 = f[0]; W._12 = f[1]; W._13 = f[2];
    W._21 = f[3]; W._22 = f[4]; W._23 = f[5];
    W._31 = f[6]; W._32 = f[7]; W._33 = f[8];
    W._41 = f[9] - ro[0]; W._42 = f[10] - ro[1]; W._43 = f[11] - ro[2];
    OcclBox b;
    b.query = query;
    b.t[0] = W._41; b.t[1] = W._42; b.t[2] = W._43;
    b.size[0] = std::fabs(sc[0]); b.size[1] = std::fabs(sc[1]); b.size[2] = std::fabs(sc[2]);
    b.objKey = OcclObjKey(b.t, b.size);
    for (int i = 0; i < 3; ++i) if (std::fabs(ro[i]) > g_occlRenderOffsetMax) g_occlRenderOffsetMax = std::fabs(ro[i]);
    {
        const auto qi = g_occlQueryObj.find(query);
        if (qi == g_occlQueryObj.end()) { try { g_occlQueryObj[query] = b.objKey; } catch (...) {} }
        else { if (qi->second != b.objKey) ++g_occlQueryRemaps; qi->second = b.objKey; }
    }
    for (int i = 0; i < 8; ++i) {
        const float v[3] = {((i & 1) ? 0.5f : -0.5f) * sc[0] + of[0], ((i & 2) ? 0.5f : -0.5f) * sc[1] + of[1],
                            ((i & 4) ? 0.5f : -0.5f) * sc[2] + of[2]};
        float w4[4];
        OcclTransform(v, W, w4);
        b.c[i][0] = w4[0]; b.c[i][1] = w4[1]; b.c[i][2] = w4[2];
    }
    if (!g_occlBuilding.haveViewProj) {
        g_occlBuilding.viewProj = FromRegisters(&g_vsConst[kRegProjTM][0], 4);
        g_occlBuilding.haveViewProj = true;
    }
    try { g_occlBuilding.boxes.push_back(b); } catch (...) {}
}

void OcclWorker() {
    std::vector<float> depth;
    // last job's state, for the flicker attribution
    D3DMATRIX prevViewProj = {};
    bool prevHave = false;
    std::vector<unsigned short> prevOwner, prevOwnerIdx;
    std::vector<float> prevIdT;
    // last job's occluders (sorted order, as ownerIdx indexes them) and its culled boxes
    struct OcclPrevInfo { unsigned long long ident; float t[3]; unsigned tris; float amin[3], amax[3]; bool used; };
    std::vector<OcclPrevInfo> prevOccl;
    struct OcclCulledBox { float mn[3], mx[3], t[3], size[3]; };
    std::vector<OcclCulledBox> prevCulledBoxes;
    std::unordered_map<unsigned long long, bool> prevCulled;     // objKey -> culled after hysteresis
    std::unordered_set<unsigned long long> prevCulledT;          // OcclKeyOf(t) of culled boxes
    std::unordered_set<unsigned long long> prevUsed, prevOffered;
    for (;;) {
        OcclFrame job;
        {
            std::unique_lock<std::mutex> lk(g_occlMu);
            g_occlCv.wait(lk, [] { return g_occlJobReady; });
            job = std::move(g_occlJob);
            g_occlJobReady = false;
            g_occlWorkerBusy = true;
        }
        const LONGLONG t0 = Now();
        const int W = g_settings.occlusionWidth > 64 ? g_settings.occlusionWidth : 320;
        const int H = (g_backBufferW && g_backBufferH) ? static_cast<int>(static_cast<long long>(W) * g_backBufferH / g_backBufferW) : W * 9 / 16;
        // depth is LINEAR view depth (clip w), so a margin can be stated in game units; owner is
        // the object that wrote the nearest surface at that pixel, so a box is never judged by
        // its own geometry - the self-occlusion that made objects flicker in and out.
        std::vector<unsigned short> owner, ownerIdx;
        try {
            depth.assign(static_cast<size_t>(W) * H, 1e30f);
            owner.assign(static_cast<size_t>(W) * H, 0xFFFF);
            ownerIdx.assign(static_cast<size_t>(W) * H, 0xFFFF);
        } catch (...) { continue; }
        // object ids from the translation, quantised to half a unit: every part of one object
        // (and its box) shares one
        std::unordered_map<unsigned long long, unsigned short> ids;
        auto keyOf = OcclKeyOf;
        std::vector<float> idT;          // id -> translation of the first occluder that got it
        std::unordered_set<unsigned long long> usedNow, offeredNow;
        const bool still = prevHave && job.haveViewProj && !job.boxes.empty() &&
                           memcmp(&job.viewProj, &prevViewProj, sizeof(D3DMATRIX)) == 0;
        for (OcclOccluder& o : job.occluders) {
            const unsigned long long k = keyOf(o.t);
            auto f = ids.find(k);
            if (f == ids.end()) {
                const unsigned short nid = static_cast<unsigned short>(ids.size() < 0xFFFE ? ids.size() : 0xFFFE);
                ids[k] = nid; o.id = nid;
                if (static_cast<size_t>(nid) * 3 + 3 > idT.size()) idT.resize(static_cast<size_t>(nid) * 3 + 3, 0.0f);
                idT[nid * 3] = o.t[0]; idT[nid * 3 + 1] = o.t[1]; idT[nid * 3 + 2] = o.t[2];
            }
            else o.id = f->second;
            offeredNow.insert(o.ident);
        }
        const unsigned idCount = static_cast<unsigned>(idT.size() / 3);
        // occluders, largest first, within the triangle budget
        std::sort(job.occluders.begin(), job.occluders.end(),
                  [](const OcclOccluder& a, const OcclOccluder& b) { return a.area > b.area; });
        unsigned long long tris = 0;
        unsigned used = 0, skippedBudget = 0;
        const bool explain = (g_occlJobReports < 2) && !job.occluders.empty();
        if (explain) {
            ++g_occlJobReports;
            Log("OCCLUSION JOB #%u: %u occluders offered, %u boxes, buffer %dx%d, budget %d occluders / %d tris. "
                "Ranked candidates (area in NDC^2, tris):",
                g_occlJobReports, static_cast<unsigned>(job.occluders.size()),
                static_cast<unsigned>(job.boxes.size()), W, H, g_settings.occlusionMaxOccluders,
                g_settings.occlusionMaxTris);
        }
        unsigned explained = 0;
        unsigned capSkipped = 0, stickyKept = 0;
        for (const OcclOccluder& o : job.occluders) {
            // An occluder used last job stays in past the cap (sticky): the boundary at rank
            // <cap> otherwise moves as the offered set churns, and every pixel it uncovers is a
            // verdict flip on whatever sat behind it.
            const bool sticky = g_settings.occlusionStickyOccluders && prevUsed.count(o.ident) != 0;
            if (used >= static_cast<unsigned>(g_settings.occlusionMaxOccluders)) {
                if (!sticky) { ++capSkipped; continue; }
                ++stickyKept;
            }
            const unsigned long long mtris = o.mesh->idx.size() / 3;
            if (tris + mtris > static_cast<unsigned long long>(g_settings.occlusionMaxTris)) {
                ++skippedBudget;
                if (explain && explained < 60) { ++explained; Log("    skip (budget): area %.3g, %llu tris, %llu rasterised so far", o.area, mtris, tris); }
                continue;
            }
            ++used;
            usedNow.insert(o.ident);
            const size_t oiRaw = static_cast<size_t>(&o - job.occluders.data());
            const unsigned short oi = static_cast<unsigned short>(oiRaw < 0xFFFE ? oiRaw : 0xFFFE);
            if (explain && explained < 60) {
                ++explained;
                float bx0 = 1e30f, bx1 = -1e30f, by0 = 1e30f, by1 = -1e30f; bool nh = false;
                for (int i = 0; i < 8; ++i) {
                    const float p[3] = {(i & 1) ? o.mesh->bmax[0] : o.mesh->bmin[0], (i & 2) ? o.mesh->bmax[1] : o.mesh->bmin[1], (i & 4) ? o.mesh->bmax[2] : o.mesh->bmin[2]};
                    float c4[4]; OcclTransform(p, o.toClip, c4);
                    if (c4[3] <= 1e-3f) { nh = true; break; }
                    const float x = (c4[0] / c4[3] * 0.5f + 0.5f) * W, y = (0.5f - c4[1] / c4[3] * 0.5f) * H;
                    if (x < bx0) bx0 = x; if (x > bx1) bx1 = x; if (y < by0) by0 = y; if (y > by1) by1 = y;
                }
                Log("    use #%u: area %.3g, %llu tris, %u verts, screen rect %s x %.0f..%.0f y %.0f..%.0f",
                    used, o.area, mtris, static_cast<unsigned>(o.mesh->pos.size() / 3),
                    nh ? "(touches near plane)" : "", bx0, bx1, by0, by1);
            }
            const OcclMesh& mesh = *o.mesh;
            const size_t nv = mesh.pos.size() / 3;
            std::vector<float> sv(nv * 3);      // screen x, y, depth (w<=near -> depth = -1)
            for (size_t i = 0; i < nv; ++i) {
                float c4[4];
                OcclTransform(&mesh.pos[i * 3], o.toClip, c4);
                if (c4[3] <= 1e-3f) { sv[i * 3 + 2] = -1.0f; continue; }
                sv[i * 3 + 0] = (c4[0] / c4[3] * 0.5f + 0.5f) * W;
                sv[i * 3 + 1] = (0.5f - c4[1] / c4[3] * 0.5f) * H;
                sv[i * 3 + 2] = c4[3];               // linear view depth
            }
            for (size_t t = 0; t + 2 < mesh.idx.size(); t += 3) {
                const float* A = &sv[static_cast<size_t>(mesh.idx[t]) * 3];
                const float* B = &sv[static_cast<size_t>(mesh.idx[t + 1]) * 3];
                const float* C = &sv[static_cast<size_t>(mesh.idx[t + 2]) * 3];
                if (A[2] < 0.0f || B[2] < 0.0f || C[2] < 0.0f) continue;   // crosses the near plane: skip
                // A face the game culls does not occlude. Screen y is down, so a clockwise
                // triangle has positive signed area and is the FRONT face under D3DCULL_CCW; a
                // mirrored world flips that.
                const float sarea = (B[0] - A[0]) * (C[1] - A[1]) - (B[1] - A[1]) * (C[0] - A[0]);
                if (std::fabs(sarea) < 1e-6f) continue;
                const bool front = o.mirrored ? (sarea < 0.0f) : (sarea > 0.0f);
                if (o.cull == D3DCULL_CCW && !front) continue;
                if (o.cull == D3DCULL_CW && front) continue;
                ++tris;
                // the triangle's FARTHEST depth is what it writes: conservative for a LESS test
                const float zmax = max(A[2], max(B[2], C[2]));
                float minx = std::floor(min(A[0], min(B[0], C[0]))), maxx = std::ceil(max(A[0], max(B[0], C[0])));
                float miny = std::floor(min(A[1], min(B[1], C[1]))), maxy = std::ceil(max(A[1], max(B[1], C[1])));
                if (maxx < 0 || maxy < 0 || minx >= W || miny >= H) continue;
                if (minx < 0) minx = 0; if (miny < 0) miny = 0;
                if (maxx > W - 1) maxx = static_cast<float>(W - 1); if (maxy > H - 1) maxy = static_cast<float>(H - 1);
                const float inv = 1.0f / sarea;
                for (int y = static_cast<int>(miny); y <= static_cast<int>(maxy); ++y)
                    for (int x = static_cast<int>(minx); x <= static_cast<int>(maxx); ++x) {
                        const float px = x + 0.5f, py = y + 0.5f;
                        const float w0 = ((B[0] - px) * (C[1] - py) - (B[1] - py) * (C[0] - px)) * inv;
                        const float w1 = ((C[0] - px) * (A[1] - py) - (C[1] - py) * (A[0] - px)) * inv;
                        const float w2 = 1.0f - w0 - w1;
                        if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) continue;
                        const size_t pi = static_cast<size_t>(y) * W + x;
                        if (zmax < depth[pi]) { depth[pi] = zmax; owner[pi] = o.id; ownerIdx[pi] = oi; }
                    }
            }
        }
        if ((g_occlJobsRun % 300) == 150)
            Log("OCCLUSION JOB SUMMARY (job %llu): %u occluders in the job, %u used, %u skipped on the "
                "triangle budget, %llu triangles rasterised, %u distinct objects",
                g_occlJobsRun, static_cast<unsigned>(job.occluders.size()), used, skippedBudget, tris,
                static_cast<unsigned>(ids.size()));
        // the boxes
        unsigned culled = 0, tested = 0, nearVis = 0, offVis = 0, nearDist = 0, selfFound = 0, pokeBoxes = 0;
        double pokeMax = 0.0;
        struct OcclOut {
            unsigned long long key, objKey; const OcclBox* box; bool occluded, tested;
            int x0, y0, x1, y1; float minW, limit;
            size_t failPi; unsigned short failOwner; float failDepth; bool failSelf;
        };
        std::vector<OcclOut> out;
        out.reserve(job.boxes.size());
        const float selfTol = g_settings.occlusionSelfToleranceCm > 0 ? g_settings.occlusionSelfToleranceCm / 100.0f : 0.0f;
        for (const OcclBox& b : job.boxes) {
            ++tested;
            OcclOut ob{};
            ob.key = g_settings.occlusionKeyByObject ? b.objKey : static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(b.query));
            ob.objKey = b.objKey;
            ob.box = &b;
            ob.failOwner = 0xFFFF;
            float minx = 1e30f, maxx = -1e30f, miny = 1e30f, maxy = -1e30f, zmin = 1e30f, minW = 1e30f;
            bool nearHit = false;
            for (int i = 0; i < 8; ++i) {
                float c4[4];
                OcclTransform(b.c[i], job.viewProj, c4);
                if (c4[3] <= 1e-3f) { nearHit = true; break; }
                if (c4[3] < minW) minW = c4[3];   // clip w = view depth for a perspective projection
                const float x = (c4[0] / c4[3] * 0.5f + 0.5f) * W, y = (0.5f - c4[1] / c4[3] * 0.5f) * H, z = c4[2] / c4[3];
                if (x < minx) minx = x; if (x > maxx) maxx = x; if (y < miny) miny = y; if (y > maxy) maxy = y;
                if (z < zmin) zmin = z;
            }
            bool occluded = false;
            if (nearHit) { ++nearVis; }
            else if (minW < static_cast<float>(g_settings.occlusionMinDistance)) { ++nearDist; }   // the near field is the engine's
            else if (minx < 0 || miny < 0 || maxx > W || maxy > H) { ++offVis; }   // partly off screen: unknown
            else {
                occluded = true;
                ob.tested = true;
                // The box's own object never occludes it: every id whose translation is within
                // occlusionSelfToleranceCm of the box's (0 = the exact quantised key). Everything
                // else must be nearer by a margin - 3 units or 3% of the distance, whichever is
                // larger - so a surface at the box's own depth cannot decide it.
                unsigned short selfIds[16]; unsigned nSelf = 0;
                if (selfTol > 0.0f) {
                    for (unsigned i = 0; i < idCount && nSelf < 16; ++i)
                        if (std::fabs(idT[i * 3] - b.t[0]) <= selfTol && std::fabs(idT[i * 3 + 1] - b.t[1]) <= selfTol &&
                            std::fabs(idT[i * 3 + 2] - b.t[2]) <= selfTol)
                            selfIds[nSelf++] = static_cast<unsigned short>(i);
                } else {
                    const auto selfIt = ids.find(keyOf(b.t));
                    if (selfIt != ids.end()) selfIds[nSelf++] = selfIt->second;
                }
                if (nSelf) {
                    ++selfFound;
                    // Does the box CONTAIN its own geometry? Self-occlusion is only possible when
                    // it does not, and the whole design assumes it does. World AABB of the box
                    // against the world AABB of every occluder that shares its id.
                    float bmin[3] = {1e30f, 1e30f, 1e30f}, bmax[3] = {-1e30f, -1e30f, -1e30f};
                    for (int i = 0; i < 8; ++i) for (int k = 0; k < 3; ++k) { if (b.c[i][k] < bmin[k]) bmin[k] = b.c[i][k]; if (b.c[i][k] > bmax[k]) bmax[k] = b.c[i][k]; }
                    float worstPoke = 0.0f; const OcclOccluder* worst = nullptr;
                    for (const OcclOccluder& o : job.occluders) {
                        bool mine = false;
                        for (unsigned s = 0; s < nSelf; ++s) if (selfIds[s] == o.id) { mine = true; break; }
                        if (!mine) continue;
                        float poke = 0.0f;
                        for (int i = 0; i < 8; ++i) {
                            const float p[3] = {(i & 1) ? o.mesh->bmax[0] : o.mesh->bmin[0], (i & 2) ? o.mesh->bmax[1] : o.mesh->bmin[1], (i & 4) ? o.mesh->bmax[2] : o.mesh->bmin[2]};
                            float w4[4]; OcclTransform(p, o.world, w4);
                            for (int k = 0; k < 3; ++k) { if (bmin[k] - w4[k] > poke) poke = bmin[k] - w4[k]; if (w4[k] - bmax[k] > poke) poke = w4[k] - bmax[k]; }
                        }
                        if (poke > worstPoke) { worstPoke = poke; worst = &o; }
                    }
                    if (worstPoke > 0.25f) {
                        ++pokeBoxes;
                        if (worstPoke > pokeMax) pokeMax = worstPoke;
                        if (g_occlPokeExplained < 5) {
                            ++g_occlPokeExplained;
                            Log("OCCL BOX DOES NOT CONTAIN ITS GEOMETRY: box t=(%.1f %.1f %.1f) size (%.1f %.1f %.1f) world aabb (%.1f %.1f %.1f)..(%.1f %.1f %.1f); "
                                "own occluder t=(%.1f %.1f %.1f) %u tris local bbox (%.1f %.1f %.1f)..(%.1f %.1f %.1f) pokes %.2f units outside",
                                b.t[0], b.t[1], b.t[2], b.size[0], b.size[1], b.size[2], bmin[0], bmin[1], bmin[2], bmax[0], bmax[1], bmax[2],
                                worst->t[0], worst->t[1], worst->t[2], static_cast<unsigned>(worst->mesh->idx.size() / 3),
                                worst->mesh->bmin[0], worst->mesh->bmin[1], worst->mesh->bmin[2], worst->mesh->bmax[0], worst->mesh->bmax[1], worst->mesh->bmax[2], worstPoke);
                        }
                    }
                }
                const float margin = max(3.0f, 0.03f * minW);
                const float limit = minW - margin;
                const int x0 = max(0, static_cast<int>(std::floor(minx)) - 1), x1 = min(W - 1, static_cast<int>(std::ceil(maxx)) + 1);
                const int y0 = max(0, static_cast<int>(std::floor(miny)) - 1), y1 = min(H - 1, static_cast<int>(std::ceil(maxy)) + 1);
                ob.x0 = x0; ob.y0 = y0; ob.x1 = x1; ob.y1 = y1; ob.minW = minW; ob.limit = limit;
                for (int y = y0; y <= y1 && occluded; ++y)
                    for (int x = x0; x <= x1; ++x) {
                        const size_t pi = static_cast<size_t>(y) * W + x;
                        const unsigned short ow = owner[pi];
                        bool self = false;
                        for (unsigned s = 0; s < nSelf; ++s) if (selfIds[s] == ow) { self = true; break; }
                        if (self || !(depth[pi] < limit)) {
                            occluded = false;
                            ob.failPi = pi; ob.failOwner = ow; ob.failDepth = depth[pi]; ob.failSelf = self;
                            break;
                        }
                    }
            }
            ob.occluded = occluded;
            if (occluded) ++culled;
            if (explain && &b == &job.boxes[0])
                Log("    first box: screen rect x %.0f..%.0f y %.0f..%.0f, nearest view depth %.1f, zmin %.4f -> %s",
                    minx, maxx, miny, maxy, minW, zmin,
                    nearHit ? "near plane" : (minW < g_settings.occlusionMinDistance ? "within min distance" :
                    ((minx < 0 || miny < 0 || maxx > W || maxy > H) ? "partly off screen" : (occluded ? "OCCLUDED" : "visible"))));
            out.push_back(ob);
        }
        // pool churn while the camera is still: which occluders came and went
        unsigned churnUsedAdd = 0, churnUsedRem = 0, churnOffAdd = 0, churnOffRem = 0;
        if (still) {
            for (const auto& e : usedNow) if (!prevUsed.count(e)) ++churnUsedAdd;
            for (const auto& e : prevUsed) if (!usedNow.count(e)) ++churnUsedRem;
            for (const auto& e : offeredNow) if (!prevOffered.count(e)) ++churnOffAdd;
            for (const auto& e : prevOffered) if (!offeredNow.count(e)) ++churnOffRem;
        }
        unsigned flipVis = 0, flipCull = 0, flipSelf = 0, flipUnwritten = 0, flipMargin = 0, flipFarther = 0,
                 flipNotOffered = 0, flipNotUsed = 0, flipUsedNotCov = 0;
        std::unordered_map<unsigned long long, bool> culledNow;
        std::unordered_set<unsigned long long> culledNowT;
        std::vector<OcclCulledBox> culledBoxesNow;
        {
            std::lock_guard<std::mutex> lk(g_occlMu);
            for (const OcclOut& r : out) {
                auto& res = g_occlResults[r.key];
                if (res.frame == job.frame) continue;    // a second box of the same object this job
                res.streak = r.occluded ? res.streak + 1 : 0;
                res.occluded = r.occluded;
                res.frame = job.frame;
                const bool cn = res.streak >= 4;
                culledNow[r.objKey] = cn;
                if (cn) {
                    culledNowT.insert(keyOf(r.box->t));
                    OcclCulledBox cb{};
                    cb.mn[0] = cb.mn[1] = cb.mn[2] = 1e30f; cb.mx[0] = cb.mx[1] = cb.mx[2] = -1e30f;
                    for (int i = 0; i < 8; ++i) for (int k = 0; k < 3; ++k) { if (r.box->c[i][k] < cb.mn[k]) cb.mn[k] = r.box->c[i][k]; if (r.box->c[i][k] > cb.mx[k]) cb.mx[k] = r.box->c[i][k]; }
                    for (int k = 0; k < 3; ++k) { cb.t[k] = r.box->t[k]; cb.size[k] = r.box->size[k]; }
                    try { culledBoxesNow.push_back(cb); } catch (...) {}
                }
                if (!still) continue;
                const auto pc = prevCulled.find(r.objKey);
                if (pc == prevCulled.end() || pc->second == cn) continue;
                if (cn) { ++flipCull; continue; }
                // culled last job, visible this one, camera still: THE flicker. Attribute it.
                ++flipVis;
                const char* why = "not tested (near plane / distance / off screen)";
                char detail[700] = "";
                if (r.tested) {
                    const int fx = static_cast<int>(r.failPi % W), fy = static_cast<int>(r.failPi / W);
                    const unsigned short po = (r.failPi < prevOwner.size()) ? prevOwner[r.failPi] : 0xFFFF;
                    const float* pt = (po != 0xFFFF && static_cast<size_t>(po) * 3 + 3 <= prevIdT.size()) ? &prevIdT[po * 3] : nullptr;
                    if (r.failSelf) {
                        ++flipSelf; why = "own object";
                        const float* st = (r.failOwner * 3u + 3u <= idT.size()) ? &idT[r.failOwner * 3] : nullptr;
                        snprintf(detail, sizeof detail, "pixel (%d,%d) is owned by the box's own object id %u t=(%.1f %.1f %.1f), depth %.1f",
                                 fx, fy, r.failOwner, st ? st[0] : 0.f, st ? st[1] : 0.f, st ? st[2] : 0.f, r.failDepth);
                    } else if (r.failOwner == 0xFFFF) {
                        ++flipUnwritten; why = "pixel unwritten";
                        // the MESH that wrote the pixel last job, and what became of it
                        const unsigned short poi = (r.failPi < prevOwnerIdx.size()) ? prevOwnerIdx[r.failPi] : 0xFFFF;
                        const OcclPrevInfo* pin = (poi < prevOccl.size()) ? &prevOccl[poi] : nullptr;
                        if (!pin) {
                            snprintf(detail, sizeof detail, "pixel (%d,%d) has no occluder now; last job's writer is unrecorded (owner id %u)", fx, fy, po);
                        } else {
                            const bool usd = usedNow.count(pin->ident) != 0, off = offeredNow.count(pin->ident) != 0;
                            const char* now;
                            if (usd) { ++flipUsedNotCov; now = "USED this job but no longer covering the pixel"; }
                            else if (off) { ++flipNotUsed; now = "offered this job but NOT USED (cap or triangle budget)"; }
                            else { ++flipNotOffered; now = "NOT OFFERED this job (the game did not draw it, or its mesh was dropped from the cache)"; }
                            char inbox[220] = "";
                            const float c[3] = {(pin->amin[0] + pin->amax[0]) * 0.5f, (pin->amin[1] + pin->amax[1]) * 0.5f, (pin->amin[2] + pin->amax[2]) * 0.5f};
                            for (const OcclCulledBox& cb : prevCulledBoxes)
                                if (c[0] >= cb.mn[0] && c[0] <= cb.mx[0] && c[1] >= cb.mn[1] && c[1] <= cb.mx[1] && c[2] >= cb.mn[2] && c[2] <= cb.mx[2]) {
                                    snprintf(inbox, sizeof inbox, "; its centre lies inside the box of object t=(%.1f %.1f %.1f) size (%.1f %.1f %.1f) which WE CULLED last job",
                                             cb.t[0], cb.t[1], cb.t[2], cb.size[0], cb.size[1], cb.size[2]);
                                    break;
                                }
                            snprintf(detail, sizeof detail, "pixel (%d,%d) has no occluder now; last job it was written by occluder #%u (ident %016llx, t=(%.1f %.1f %.1f), %u tris, world aabb (%.1f %.1f %.1f)..(%.1f %.1f %.1f)) - %s%s",
                                     fx, fy, poi, pin->ident, pin->t[0], pin->t[1], pin->t[2], pin->tris, pin->amin[0], pin->amin[1], pin->amin[2],
                                     pin->amax[0], pin->amax[1], pin->amax[2], now, inbox);
                        }
                    } else {
                        const bool inMargin = r.failDepth < r.minW;
                        if (inMargin) { ++flipMargin; why = "inside margin"; } else { ++flipFarther; why = "farther"; }
                        const float* ot = (r.failOwner * 3u + 3u <= idT.size()) ? &idT[r.failOwner * 3] : nullptr;
                        snprintf(detail, sizeof detail, "pixel (%d,%d) owner id %u t=(%.1f %.1f %.1f) depth %.1f vs limit %.1f (box minW %.1f); last job this pixel was id %u t=(%.1f %.1f %.1f)%s",
                                 fx, fy, r.failOwner, ot ? ot[0] : 0.f, ot ? ot[1] : 0.f, ot ? ot[2] : 0.f, r.failDepth, r.limit, r.minW,
                                 po, pt ? pt[0] : 0.f, pt ? pt[1] : 0.f, pt ? pt[2] : 0.f,
                                 (pt && prevCulledT.count(keyOf(pt))) ? " - CULLED BY US last job" : "");
                    }
                }
                if (g_occlFlipExplained < static_cast<unsigned>(g_settings.occlusionExplainFlips)) {
                    ++g_occlFlipExplained;
                    Log("OCCL FLIP job %llu (camera still): object t=(%.1f %.1f %.1f) size (%.1f %.1f %.1f) rect x%d..%d y%d..%d -> VISIBLE after being culled: %s - %s",
                        g_occlJobsRun, r.box->t[0], r.box->t[1], r.box->t[2], r.box->size[0], r.box->size[1], r.box->size[2],
                        r.x0, r.x1, r.y0, r.y1, why, detail);
                }
            }
            // results age out (a wholesale clear would reset every streak at once: a mass pop-in)
            if ((g_occlJobsRun % 600) == 599)
                for (auto it = g_occlResults.begin(); it != g_occlResults.end();)
                    if (job.frame > it->second.frame + 600) it = g_occlResults.erase(it); else ++it;
            if (still) {
                ++g_occlStillJobs;
                g_occlFlipVis += flipVis; g_occlFlipCull += flipCull; g_occlFlipSelf += flipSelf;
                g_occlFlipUnwritten += flipUnwritten; g_occlFlipMargin += flipMargin; g_occlFlipFarther += flipFarther;
                g_occlChurnUsedAdd += churnUsedAdd; g_occlChurnUsedRem += churnUsedRem;
                g_occlChurnOffAdd += churnOffAdd; g_occlChurnOffRem += churnOffRem;
                g_occlFlipNotOffered += flipNotOffered; g_occlFlipNotUsed += flipNotUsed; g_occlFlipUsedNotCov += flipUsedNotCov;
            }
            g_occlCapSkipped += capSkipped;
            g_occlStickyKept += stickyKept;
            g_occlSelfFound += selfFound;
            g_occlPokeBoxes += pokeBoxes;
            if (pokeMax > g_occlPokeMax) g_occlPokeMax = pokeMax;
            g_occlTrisRaster += tris;
            g_occlOccluders += used;
            g_occlJobOccluders += job.occluders.size();
            g_occlSkippedBudget += skippedBudget;
            g_occlTested += tested;
            g_occlCulled += culled;
            g_occlNearVisible += nearVis;
            g_occlNearDistVisible += nearDist;
            g_occlOffscreenVisible += offVis;
            g_occlWorkerMs += MsSince(t0);
            ++g_occlJobsRun;
            g_occlWorkerBusy = false;
        }
        // carry this job's state for the next attribution
        // this job's occluders summarised (sorted order, as ownerIdx indexes them)
        std::vector<OcclPrevInfo> occlNow;
        try {
            occlNow.reserve(job.occluders.size());
            for (const OcclOccluder& o : job.occluders) {
                OcclPrevInfo info{};
                info.ident = o.ident; info.t[0] = o.t[0]; info.t[1] = o.t[1]; info.t[2] = o.t[2];
                info.tris = static_cast<unsigned>(o.mesh->idx.size() / 3);
                info.used = usedNow.count(o.ident) != 0;
                info.amin[0] = info.amin[1] = info.amin[2] = 1e30f; info.amax[0] = info.amax[1] = info.amax[2] = -1e30f;
                for (int i = 0; i < 8; ++i) {
                    const float p[3] = {(i & 1) ? o.mesh->bmax[0] : o.mesh->bmin[0], (i & 2) ? o.mesh->bmax[1] : o.mesh->bmin[1], (i & 4) ? o.mesh->bmax[2] : o.mesh->bmin[2]};
                    float w4[4]; OcclTransform(p, o.world, w4);
                    for (int k = 0; k < 3; ++k) { if (w4[k] < info.amin[k]) info.amin[k] = w4[k]; if (w4[k] > info.amax[k]) info.amax[k] = w4[k]; }
                }
                occlNow.push_back(info);
            }
        } catch (...) { occlNow.clear(); }
        prevOccl.swap(occlNow);
        prevCulledBoxes.swap(culledBoxesNow);
        prevViewProj = job.viewProj;
        prevHave = job.haveViewProj;
        prevOwner.swap(owner);
        prevOwnerIdx.swap(ownerIdx);
        prevIdT.swap(idT);
        prevCulled.swap(culledNow);
        prevCulledT.swap(culledNowT);
        prevUsed.swap(usedNow);
        prevOffered.swap(offeredNow);
    }
}

// End of frame: hand this frame's data to the worker, or drop it if the worker is still busy.
void KickOcclusionJob() {
    if (!g_settings.occlusionCull) { g_occlBuilding = OcclFrame{}; return; }
    // Age out meshes the city has streamed away: not used for 900 frames, checked every 300.
    if ((g_frames % 300) == 0 && g_occlMeshes.size() > 2000) {
        for (auto it = g_occlMeshes.begin(); it != g_occlMeshes.end();)
            if (g_frames > it->second->lastUsed + 900) it = g_occlMeshes.erase(it); else ++it;
        OcclMeshIndexRebuild();
    }
    // The culler REFINES the fabricated "visible" answer; it has nothing to refine when the real
    // query is passed through, and the real query, against a depth buffer Remix never writes,
    // culls the whole world. Said once, loudly, rather than silently doing nothing.
    static bool warned = false;
    if (!g_settings.forceOcclusionVisible && !warned) {
        warned = true;
        Log("OCCLUSION CULL: occlusionCull=1 but forceOcclusionVisible=0 - the culler only acts on "
            "the fabricated answer, so it is INERT, and the engine is culling against an unwritten "
            "depth buffer. Set forceOcclusionVisible=1.");
    }
    g_occlBoxes += g_occlBuilding.boxes.size();
    if (!g_occlWorkerStarted) {
        g_occlWorkerStarted = true;
        try { std::thread(OcclWorker).detach(); } catch (...) { g_settings.occlusionCull = false; return; }
    }
    {
        std::lock_guard<std::mutex> lk(g_occlMu);
        if (g_occlJobReady || g_occlWorkerBusy) { ++g_occlJobsDropped; g_occlBuilding = OcclFrame{}; return; }
        g_occlBuilding.frame = g_frames;
        g_occlJob = std::move(g_occlBuilding);
        g_occlJobReady = true;
    }
    g_occlBuilding = OcclFrame{};
    g_occlCv.notify_one();
}

// The answer for a query, if the worker has one that is recent enough. True = occluded.
bool OcclusionSaysOccluded(IDirect3DQuery9* q) {
    if (!g_settings.occlusionCull || !q) return false;
    unsigned long long key = static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(q));
    if (g_settings.occlusionKeyByObject) {
        const auto qi = g_occlQueryObj.find(q);   // the object this query was last issued for
        if (qi == g_occlQueryObj.end()) return false;
        key = qi->second;
    }
    std::lock_guard<std::mutex> lk(g_occlMu);
    const auto it = g_occlResults.find(key);
    if (it == g_occlResults.end()) return false;
    if (g_frames > it->second.frame + 3) return false;   // stale: visible
    return it->second.streak >= 4;   // four consecutive jobs must agree before a cull; one visible resets
}

IDirect3DQuery9* g_curOcclusionQuery = nullptr;
unsigned g_occlProxyGeomReports = 0;
unsigned long long g_occlProxyObjTM = 0, g_occlProxyNoObjTM = 0;
unsigned long long g_occlCandDraws = 0, g_occlCandOpaque = 0, g_occlCandTris = 0,
                   g_occlCandBig = 0, g_occlCandMid = 0, g_occlCandInstanced = 0,
                   g_occlCandBlend = 0, g_occlCandATest = 0, g_occlCandTexkill = 0;
IDirect3DVertexBuffer9* g_occlProxyVBs[8] = {};
unsigned g_occlProxyVBCount = 0;
// Counts the draw and decides whether it is a proxy to drop. Called first thing in the hooks.
inline bool OcclusionProxyDraw(IDirect3DDevice9* dev, UINT primitiveCount, UINT numVertices,
                               UINT firstVertex) {
    if (!g_inOcclusionQuery) return false;
    ++g_occlDrawsSeen;
    if (g_curVS.usesObjTM) ++g_occlProxyObjTM; else ++g_occlProxyNoObjTM;
    if (numVertices == 8 && primitiveCount == 12) RecordProxyBox(g_curOcclusionQuery);
    if (g_stream0) {
        bool known = false;
        for (unsigned i = 0; i < g_occlProxyVBCount; ++i) if (g_occlProxyVBs[i] == g_stream0) known = true;
        if (!known && g_occlProxyVBCount < 8) g_occlProxyVBs[g_occlProxyVBCount++] = g_stream0;
    }
    // The first few proxies, in full: the 8 positions as stored, objTM, and the query.
    if (g_occlProxyGeomReports < 4 && g_stream0 && g_stream0Stride && numVertices == 8 &&
        g_curLayout.posOffset >= 0 &&
        (g_curLayout.posType == D3DDECLTYPE_FLOAT3 || g_curLayout.posType == D3DDECLTYPE_FLOAT4) &&
        VertexRangeFits(g_stream0, g_stream0Offset, firstVertex, numVertices, g_stream0Stride)) {
        void* m = nullptr;
        if (SUCCEEDED(g_stream0->Lock(g_stream0Offset + firstVertex * g_stream0Stride,
                                      numVertices * g_stream0Stride, &m,
                                      D3DLOCK_READONLY | D3DLOCK_NOSYSLOCK)) && m) {
            ++g_occlProxyGeomReports;
            const unsigned char* b = static_cast<const unsigned char*>(m);
            char buf[400];
            int n = 0;
            for (UINT i = 0; i < 8 && n < 360; ++i) {
                const float* p = reinterpret_cast<const float*>(b + i * g_stream0Stride + g_curLayout.posOffset);
                n += _snprintf_s(buf + n, sizeof(buf) - n, _TRUNCATE, "(%.1f %.1f %.1f) ", p[0], p[1], p[2]);
            }
            g_stream0->Unlock();
            if (g_occlProxyGeomReports == 1 && g_lastVS) {
                // The shader that places the cube, and every constant it could be reading.
                UINT sz = 0;
                if (SUCCEEDED(g_lastVS->GetFunction(nullptr, &sz)) && sz && sz < (1u << 20)) {
                    std::vector<unsigned char> code(sz);
                    if (SUCCEEDED(g_lastVS->GetFunction(code.data(), &sz))) {
                        char vdir[MAX_PATH] = {0};
                        GetModuleFileNameA(GetModuleHandleW(nullptr), vdir, MAX_PATH);
                        char* vsl = strrchr(vdir, 0x5C);
                        if (vsl) *(vsl + 1) = 0;
                        char vpath[MAX_PATH];
                        sprintf_s(vpath, "%ssr3-remix-proxy-vs.bin", vdir);
                        FILE* vf = nullptr;
                        if (fopen_s(&vf, vpath, "wb") == 0 && vf) { fwrite(code.data(), 1, sz, vf); fclose(vf); }
                        Log("OCCLUSION PROXY vertex shader: %u bytes -> %s", sz, vpath);
                    }
                }
                for (UINT r = 0; r < 40; r += 4)
                    Log("    proxy vs c%u..c%u: (%.3f %.3f %.3f %.3f) (%.3f %.3f %.3f %.3f) (%.3f %.3f %.3f %.3f) (%.3f %.3f %.3f %.3f)",
                        r, r + 3,
                        g_vsConst[r][0], g_vsConst[r][1], g_vsConst[r][2], g_vsConst[r][3],
                        g_vsConst[r+1][0], g_vsConst[r+1][1], g_vsConst[r+1][2], g_vsConst[r+1][3],
                        g_vsConst[r+2][0], g_vsConst[r+2][1], g_vsConst[r+2][2], g_vsConst[r+2][3],
                        g_vsConst[r+3][0], g_vsConst[r+3][1], g_vsConst[r+3][2], g_vsConst[r+3][3]);
            }
            const float* o = &g_vsConst[kRegObjTM][0];
            Log("OCCLUSION PROXY #%u: query %p, vb %p first %u, posType %d stride %u, usesObjTM %d | "
                "verts %s| objTM rows (%.2f %.2f %.2f %.1f) (%.2f %.2f %.2f %.1f) (%.2f %.2f %.2f %.1f)",
                g_occlProxyGeomReports, static_cast<void*>(g_curOcclusionQuery),
                static_cast<void*>(g_stream0), firstVertex, g_curLayout.posType, g_stream0Stride,
                g_curVS.usesObjTM ? 1 : 0, buf,
                o[0], o[1], o[2], o[3], o[4], o[5], o[6], o[7], o[8], o[9], o[10], o[11]);
        }
    }
    const bool proxySized = primitiveCount <= static_cast<UINT>(g_settings.occlusionProxyMaxPrims);
    if (proxySized) ++g_occlDrawsSmall; else ++g_occlDrawsLarge;
    if (g_occlDrawReports < 8) {
        ++g_occlDrawReports;
        Log("INSIDE OCCLUSION QUERY #%u: %u prims, %u verts, target %ux%u, vs %s, ps %s, "
            "zwrite=%lu zenable=%lu blend=%lu -> %s",
            g_occlDrawReports, primitiveCount, numVertices, g_rt0Width, g_rt0Height,
            g_lastVS ? "bound" : "none", g_lastPS ? "bound" : "none",
            static_cast<unsigned long>(g_rsShadow[D3DRS_ZWRITEENABLE]),
            static_cast<unsigned long>(g_rsShadow[D3DRS_ZENABLE]),
            static_cast<unsigned long>(g_rsShadow[D3DRS_ALPHABLENDENABLE]),
            (proxySized && g_settings.skipOcclusionProxies && g_settings.forceOcclusionVisible)
                ? "SKIPPED as a proxy"
                : "forwarded (too many prims to be a box)");
    }
    // 2026-09-18 AUDIT FIX: dropping the proxy draw is only safe when forceOcclusionVisible is
    // ALSO on. With it off, Hook_QueryGetData passes the query straight through instead of
    // fabricating an answer (see there), so a proxy skipped here leaves a REAL occlusion query
    // with nothing drawn inside it - an honest, and wrong, "0 pixels" that reads as "occluded"
    // and stops the engine submitting the object. Hook_QueryIssue already requires both switches
    // before it will drop the begin/end pair; this site was the one pairing bug left. No new ini
    // key - this is a pairing bug, and the live configuration (both 1) is unchanged.
    if (proxySized && g_settings.skipOcclusionProxies && g_settings.forceOcclusionVisible) {
        ++g_occlDrawsSkipped;
        return true;
    }
    return false;
}

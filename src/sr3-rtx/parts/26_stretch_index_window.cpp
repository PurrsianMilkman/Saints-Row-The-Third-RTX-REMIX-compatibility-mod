// parts/26_stretch_index_window.cpp - stretch detect and the index-window probe/clamp.
// NOT a standalone translation unit: #included, in order, by ../sr3rtx.cpp inside its anonymous
// namespace. Originally sr3rtx.cpp lines 22215-23118 (old file:line citations map here).
// ================================================================== STRETCH DETECT
// THE BUG: world decals "stretch across the screen" for exactly one frame when they load,
// unload or reload. Confirmed real geometry - the stretched shape shows in Remix's own
// Geometry Hash debug view, which is written per-pixel from the primary ray hit with no
// temporal accumulation to blame - and confirmed NOT the three suspects already disproven:
// Remix's DrawCallCache reusing an entry, Remix's instance-history/previous-position buffer,
// and this shim's own WORLDMATRIX(0)/D3DTS_WORLD aliasing (UnbindSkinFF restores it
// unconditionally and SkinViaFixedFunction has no refuse path after its staging loop). So for
// one frame, some draw hands Remix a vertex position or a transform that places a triangle
// across a huge swathe of the world. The reported population: batched world decals, the
// 'uvDynamic' path, world space, identity-ish transform, 12-234 triangles, a shared dynamic
// vertex buffer.
//
// This is the detector, not the fix - find the draw ITSELF, on the frame it happens, and dump
// everything about it, so no human reflex has to catch a one-frame event. stretchDetect gates
// all of it; OFF (the default) costs one bool test at the call site, above ProbeAssetHash's
// own call, and this function is never entered.
//
// READ-ONLY, the same rule ProbeAssetHash's own comment states above it: every buffer here is
// either locked D3DLOCK_READONLY (a STATIC source) or read from the existing snoop copy (a
// DYNAMIC source, via SnoopCopy) - never a direct Lock of a dynamic buffer. Binds nothing,
// issues no draw, changes no render state.
//
// WHY A NEW READ: UvBufferFor and ReadRigidTexcoordsRaw already walk this draw's vertices for
// TEXCOORD; nothing in this file already walks them for POSITION the same safe way, so this
// adds ONE new pass (ReadRigidPositionsRaw, below) - modelled line for line on
// ReadRigidTexcoordsRaw's own dynamic-snoop/static-lock split, just at g_curLayout.posOffset.
// Everything else is reused rather than re-derived: ComputeAssetHashIndexComponent supplies
// the unique referenced-vertex list (the very same index-buffer read ProbeAssetHash performs
// for this population when assetHashProbe is on - the two probes are gated independently and
// cost each other nothing), ComputeAssetHashUvComponent supplies the PATH tag unchanged,
// GetBaseMesh supplies the CPU-skin bind pose from its existing decoded cache (not a device
// read at all), and IsFinite/TransformPoint are the same helpers ApplyTransforms and
// MeasureWorldExtent already use.
//
// MeasureWorldExtent (above, near g_extentReports) already tried to answer this exact question
// and is why this is not built the same way: it read-locks g_stream0 UNCONDITIONALLY, without
// checking IsDynamicVB first - one of "roughly a dozen probes" the g_settings comment above
// vbUnlockOwnerThread names as a known hazard class (a DYNAMIC buffer's pending game-thread
// Lock consumed by this shim's own internal Unlock, the proximate cause of the September 14
// crash on a different buffer). It also only SAMPLES (a stride of numVertices/32) rather than
// scanning every vertex the draw references, so a single-vertex corruption can be stepped
// clean over, and it reports one scalar with no vertex identity - nothing to grep for. This
// fixes all three: dynamic-safe, exhaustive over the draw's own referenced vertices, and names
// the vertex.

// Raw bytes for exactly one vertex's POSITION field, captured at the moment it is decoded - no
// second read. 12 bytes is the float3 every converted draw is guaranteed to have: BeginFFP's
// own "position not float3/float4 in stream 0" gate (see its comment, above) refuses anything
// else before a draw can ever reach Disp::Convert, so nothing that reaches this probe needs a
// short/compressed branch the way ReadRigidTexcoordsRaw needs one for texcoords.
struct StretchVertexRaw { unsigned char b[12]; };

// Mirrors ReadRigidTexcoordsRaw exactly - same dynamic-snoop/static-lock split, same
// D3DLOCK_READONLY | D3DLOCK_NOSYSLOCK flags and g_internal guard - just reading POSITION
// instead of a texcoord. rawOut is filled in the same pass so the worst vertex's raw bytes
// cost nothing extra to keep.
bool ReadRigidPositionsRaw(const std::vector<UINT>& globalVerts, int posOffset, UINT stride,
                           bool dynamicSrc, std::vector<D3DVECTOR>* out,
                           std::vector<StretchVertexRaw>* rawOut) {
    if (globalVerts.empty() || !g_stream0 || !stride) return false;
    const UINT lo = globalVerts.front(), hi = globalVerts.back();
    const UINT span = hi - lo + 1;
    if (span == 0 || span > 2000000) return false;
    try { out->resize(globalVerts.size()); rawOut->resize(globalVerts.size()); }
    catch (...) { return false; }
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
            const unsigned char* p = staging.data() + static_cast<size_t>(rel) * stride + posOffset;
            const float* pf = reinterpret_cast<const float*>(p);
            (*out)[i] = D3DVECTOR{pf[0], pf[1], pf[2]};
            memcpy((*rawOut)[i].b, p, sizeof(StretchVertexRaw::b));
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
        const unsigned char* p = base + static_cast<size_t>(rel) * stride + posOffset;
        const float* pf = reinterpret_cast<const float*>(p);
        (*out)[i] = D3DVECTOR{pf[0], pf[1], pf[2]};
        memcpy((*rawOut)[i].b, p, sizeof(StretchVertexRaw::b));
    }
    g_stream0->Unlock();
    g_internal = wasInt;
    return true;
}

unsigned long long g_stretchDetectChecked = 0;     // converted draws actually scanned
unsigned long long g_stretchDetectUnreadable = 0;  // no position data AND a finite matrix - no verdict possible
unsigned long long g_stretchDetectFlagged = 0;     // flagged, all-time - NOT capped by stretchDetectDumps
unsigned g_stretchDetectDumped = 0;                // Log() blocks actually written, capped at stretchDetectDumps

// THE DISTRIBUTION: stretchDetectUnits (the flag threshold) is a guess dressed as a default -
// nothing before this measured the LEGITIMATE world-AABB diagonal spread of a batched world
// decal, so there was no evidence to set it from. This answers that independently of the
// threshold, in the same run that hunts the bug: every scanned draw with a computable diagonal
// (nFinite>0 - the ones with no position data at all are already counted apart, in
// g_stretchDetectUnreadable, and cannot contribute a diagonal) feeds both a histogram and a
// top-5 table, flagged or not. Both are O(1) per draw and allocate nothing on the draw path -
// see StretchHistBucket and StretchTopInsert, called from ProbeStretchDetect right after diag
// is computed.
constexpr int kStretchHistBuckets = 8;
// Upper edge of every bucket but the last: [0-10) [10-100) [100-500) [500-1000) [1000-2000)
// [2000-5000) [5000-20000) [20000+) - the task's own bucket list, verbatim.
constexpr float kStretchHistEdges[kStretchHistBuckets - 1] = {
    10.0f, 100.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 20000.0f};
unsigned long long g_stretchHist[kStretchHistBuckets] = {};

int StretchHistBucket(float diag) {
    for (int i = 0; i < kStretchHistBuckets - 1; ++i)
        if (diag < kStretchHistEdges[i]) return i;
    return kStretchHistBuckets - 1;
}

// THE TOP 5 largest world-AABB diagonals ever scanned, flagged or not - a fixed array updated
// in place (insertion sort over 5 elements), never a heap allocation. diag<0 marks an empty
// slot, which sorts below every real draw (a real diagonal is never negative) so the array
// fills left-to-right before anything is ever evicted.
struct StretchTopEntry {
    float diag = -1.0f;
    char ps[28] = "";
    UINT v = 0, p = 0;
    char path[24] = "";
    D3DVECTOR worldMin{}, worldMax{};
};
StretchTopEntry g_stretchTop5[5];

void StretchTopInsert(float diag, const char* ps, UINT v, UINT p, const char* path,
                      const D3DVECTOR& wMin, const D3DVECTOR& wMax) {
    if (diag <= g_stretchTop5[4].diag) return;   // does not beat the current 5th place
    int pos = 4;
    while (pos > 0 && g_stretchTop5[pos - 1].diag < diag) {
        g_stretchTop5[pos] = g_stretchTop5[pos - 1];
        --pos;
    }
    StretchTopEntry& e = g_stretchTop5[pos];
    e.diag = diag;
    strncpy_s(e.ps, ps, _TRUNCATE);
    e.v = v; e.p = p;
    strncpy_s(e.path, path, _TRUNCATE);
    e.worldMin = wMin; e.worldMax = wMax;
}

void ProbeStretchDetect(IDirect3DDevice9* dev, D3DPRIMITIVETYPE type, INT baseVertex, UINT minIndex,
                        UINT numVertices, UINT startIndex, UINT primitiveCount, bool skinnedLocal,
                        bool skinnedFFLocal, const FFPScope& scope) {
    ++g_stretchDetectChecked;
    const D3DMATRIX& world = g_lastWorld;
    const bool worldFinite = IsFinite(world);

    // Reused, not re-derived: the same IB read ProbeAssetHash performs for this population
    // when assetHashProbe is on - gated independently, so the two probes pay for this once
    // each.
    const AssetHashIndexResult idx =
        ComputeAssetHashIndexComponent(g_curIB, type, startIndex, primitiveCount);
    std::vector<UINT> gv;
    if (idx.ok && !idx.uniqueRaw.empty()) {
        try {
            gv.reserve(idx.uniqueRaw.size());
            for (const UINT v : idx.uniqueRaw) {
                const long long g = static_cast<long long>(baseVertex) + static_cast<long long>(v);
                if (g < 0 || g > 0xFFFFFFFFll) continue;
                gv.push_back(static_cast<UINT>(g));
            }
        } catch (...) { gv.clear(); }
    }

    // THE PATH TAG: exactly what ProbeAssetHash's own UV component computes for this identical
    // draw, reused verbatim so a stretch dump and an asset-hash dump name the same draw the
    // same way instead of inventing a second taxonomy for one bug hunt.
    const AssetHashUvResult uvPath = ComputeAssetHashUvComponent(
        idx.uniqueRaw, baseVertex, minIndex, numVertices, skinnedLocal, skinnedFFLocal, scope);

    std::vector<D3DVECTOR> obj, worldPos;
    std::vector<StretchVertexRaw> raw;
    std::vector<UINT> gvFinal;   // kept in lockstep with obj[]/raw[] - NOT always == gv (the
                                 // skinRingCPU branch below can skip an out-of-window index)
    bool havePositions = false;
    const char* posSource = "unavailable";

    if (!gv.empty()) {
        if (skinnedLocal) {
            // THE CPU RING (SkinAndBind) writes into g_skinVB, DYNAMIC|WRITEONLY - never
            // safely readable: the hard rule is no direct Lock of a dynamic buffer, and
            // WRITEONLY makes a read meaningless even where the driver tolerates it. The
            // nearest safe substitute is the BIND POSE GetBaseMesh already decoded and cached
            // for SkinAndBind's own use - an already-in-memory array, not a device Lock at
            // all - which is this mesh BEFORE this draw's bone transform. Good enough to catch
            // a corrupt SOURCE mesh or a corrupt WORLD matrix; BLIND to a corruption introduced
            // by the skin arithmetic itself. Said here rather than guessed past: inventing a
            // read of the ring would break the hard rule, and pretending this proxy sees
            // everything would be worse than saying so.
            posSource = "bind pose (pre-skin proxy - the CPU ring itself is not readable)";
            const long long fv = static_cast<long long>(baseVertex) + static_cast<long long>(minIndex);
            const BaseMesh* mesh = (fv >= 0) ? GetBaseMesh(static_cast<UINT>(fv), numVertices) : nullptr;
            if (mesh && mesh->verts.size() == numVertices) {
                try {
                    obj.reserve(gv.size()); raw.reserve(gv.size()); gvFinal.reserve(gv.size());
                    for (const UINT g : gv) {
                        if (g < minIndex || g >= minIndex + numVertices) continue;
                        const BaseVertex& b = mesh->verts[g - minIndex];
                        obj.push_back(D3DVECTOR{b.pos[0], b.pos[1], b.pos[2]});
                        StretchVertexRaw r{}; memcpy(r.b, b.pos, sizeof(r.b));
                        raw.push_back(r);
                        gvFinal.push_back(g);
                    }
                } catch (...) { obj.clear(); raw.clear(); gvFinal.clear(); }
                havePositions = !obj.empty();
            }
        } else if (g_curLayout.posStream == 0 && g_curLayout.posOffset >= 0 && g_stream0Stride) {
            // Rigid and hardware-skinned (skinFF) both leave stream 0's POSITION exactly as
            // the game bound it - skinFF's bone palette is applied by the fixed-function
            // pipeline at draw time, the same reason ComputeAssetHashUvComponent's own skinFF
            // branch reads stream 0 directly for texcoords instead of the ring.
            const bool dynamicSrc = IsDynamicVB(g_stream0);
            havePositions = ReadRigidPositionsRaw(gv, g_curLayout.posOffset, g_stream0Stride,
                                                  dynamicSrc, &obj, &raw);
            if (havePositions) gvFinal = gv;   // ReadRigidPositionsRaw fills 1:1 with gv, in order
            posSource = dynamicSrc ? "stream0 (dynamic, snooped)" : "stream0 (static, locked)";
        }
    }

    bool anyNonFinite = !worldFinite;
    D3DVECTOR objMin{1e30f, 1e30f, 1e30f}, objMax{-1e30f, -1e30f, -1e30f};
    D3DVECTOR worldMin{1e30f, 1e30f, 1e30f}, worldMax{-1e30f, -1e30f, -1e30f};
    UINT worstIdx = 0; D3DVECTOR worstObj{}, worstWorld{}; StretchVertexRaw worstRaw{};
    bool haveWorst = false;
    double sumX = 0.0, sumY = 0.0, sumZ = 0.0;
    unsigned nFinite = 0;

    if (havePositions) {
        try { worldPos.reserve(obj.size()); } catch (...) {}
        for (size_t i = 0; i < obj.size(); ++i) {
            const D3DVECTOR w = TransformPoint(&obj[i].x, world);
            try { worldPos.push_back(w); } catch (...) {}
            const bool vFinite = std::isfinite(obj[i].x) && std::isfinite(obj[i].y) &&
                                 std::isfinite(obj[i].z) && std::isfinite(w.x) &&
                                 std::isfinite(w.y) && std::isfinite(w.z);
            if (!vFinite) {
                anyNonFinite = true;
                if (!haveWorst) {   // the FIRST non-finite vertex names the bug on its own -
                                    // no "worse" one to keep looking for
                    haveWorst = true;
                    worstIdx = gvFinal[i]; worstObj = obj[i]; worstWorld = w; worstRaw = raw[i];
                }
                continue;
            }
            objMin.x = min(objMin.x, obj[i].x); objMin.y = min(objMin.y, obj[i].y);
            objMin.z = min(objMin.z, obj[i].z);
            objMax.x = max(objMax.x, obj[i].x); objMax.y = max(objMax.y, obj[i].y);
            objMax.z = max(objMax.z, obj[i].z);
            worldMin.x = min(worldMin.x, w.x); worldMin.y = min(worldMin.y, w.y);
            worldMin.z = min(worldMin.z, w.z);
            worldMax.x = max(worldMax.x, w.x); worldMax.y = max(worldMax.y, w.y);
            worldMax.z = max(worldMax.z, w.z);
            sumX += w.x; sumY += w.y; sumZ += w.z;
            ++nFinite;
            if (!anyNonFinite) {   // magnitude-worst only matters while nothing NaN has won yet
                const float m = max(max(std::fabs(w.x), std::fabs(w.y)), std::fabs(w.z));
                const float worstM = haveWorst ? max(max(std::fabs(worstWorld.x),
                                                         std::fabs(worstWorld.y)),
                                                     std::fabs(worstWorld.z))
                                               : -1.0f;
                if (m > worstM) {
                    haveWorst = true;
                    worstIdx = gvFinal[i]; worstObj = obj[i]; worstWorld = w; worstRaw = raw[i];
                }
            }
        }
    }

    if (!havePositions && !anyNonFinite) { ++g_stretchDetectUnreadable; return; }

    const float diag = (nFinite > 0)
        ? std::sqrt((worldMax.x - worldMin.x) * (worldMax.x - worldMin.x) +
                    (worldMax.y - worldMin.y) * (worldMax.y - worldMin.y) +
                    (worldMax.z - worldMin.z) * (worldMax.z - worldMin.z))
        : 0.0f;

    // THE DISTRIBUTION, updated for every scanned draw with a computable diagonal, BEFORE
    // the flag threshold below decides whether this one is "interesting" - this is what
    // makes the histogram and top-5 independent of stretchDetectUnits, per their own comment
    // above.
    if (nFinite > 0) {
        ++g_stretchHist[StretchHistBucket(diag)];
        StretchTopInsert(diag, g_curPS.albedoSampler, numVertices, primitiveCount,
                         uvPath.path[0] ? uvPath.path : "not-reproducible", worldMin, worldMax);
    }

    const bool flagged = anyNonFinite ||
                         (nFinite > 0 && diag > static_cast<float>(g_settings.stretchDetectUnits));
    if (!flagged) return;

    ++g_stretchDetectFlagged;
    if (g_stretchDetectDumped >= static_cast<unsigned>(max(0, g_settings.stretchDetectDumps))) return;
    ++g_stretchDetectDumped;

    // THE TYPICAL VERTEX: the finite vertex closest to the mean of every finite vertex in this
    // draw - contrast for the worst one, read straight out of the arrays already in memory, no
    // second device read.
    UINT typicalIdx = 0; D3DVECTOR typicalObj{}, typicalWorld{}; bool haveTypical = false;
    if (nFinite > 0) {
        const D3DVECTOR mean{static_cast<float>(sumX / nFinite), static_cast<float>(sumY / nFinite),
                             static_cast<float>(sumZ / nFinite)};
        float best = -1.0f;
        for (size_t i = 0; i < worldPos.size(); ++i) {
            const D3DVECTOR& w = worldPos[i];
            if (!std::isfinite(w.x) || !std::isfinite(w.y) || !std::isfinite(w.z)) continue;
            const float dx = w.x - mean.x, dy = w.y - mean.y, dz = w.z - mean.z;
            const float d = dx * dx + dy * dy + dz * dz;
            if (best < 0.0f || d < best) {
                best = d; haveTypical = true;
                typicalIdx = gvFinal[i]; typicalObj = obj[i]; typicalWorld = w;
            }
        }
    }

    const float* wf = &world._11;
    Log("STRETCH DETECT #%u: frame %u draw %u | ps(albedo sampler)='%s' shader=%p alb=%p | v=%u "
        "p=%u | ib=%p start=%u vb=%p off=%u baseVtx=%d stride=%u | path '%s' | positions: %s",
        g_stretchDetectDumped, g_frames, g_drawIndexThisFrame, g_curPS.albedoSampler,
        static_cast<void*>(g_lastPS), static_cast<void*>(g_lastEffectiveAlbedo), numVertices,
        primitiveCount, static_cast<void*>(g_curIB), startIndex, static_cast<void*>(g_stream0),
        g_stream0Offset, baseVertex, g_stream0Stride,
        uvPath.path[0] ? uvPath.path : "not-reproducible", posSource);
    Log("    world matrix (%s): [%.3f %.3f %.3f %.3f] [%.3f %.3f %.3f %.3f] "
        "[%.3f %.3f %.3f %.3f] [%.3f %.3f %.3f %.3f]",
        worldFinite ? "finite" : "NON-FINITE", wf[0], wf[1], wf[2], wf[3], wf[4], wf[5], wf[6],
        wf[7], wf[8], wf[9], wf[10], wf[11], wf[12], wf[13], wf[14], wf[15]);
    if (nFinite > 0)
        Log("    object AABB (%u finite verts) min(%.3f %.3f %.3f) max(%.3f %.3f %.3f) | world "
            "AABB min(%.1f %.1f %.1f) max(%.1f %.1f %.1f) diag=%.1f (limit %d)",
            nFinite, objMin.x, objMin.y, objMin.z, objMax.x, objMax.y, objMax.z, worldMin.x,
            worldMin.y, worldMin.z, worldMax.x, worldMax.y, worldMax.z, diag,
            g_settings.stretchDetectUnits);
    else
        Log("    object/world AABB: no finite vertex in this draw's referenced set (%u sampled)",
            static_cast<unsigned>(obj.size()));
    if (haveWorst)
        Log("    WORST vertex gv=%u raw(%02X %02X %02X %02X  %02X %02X %02X %02X  %02X %02X "
            "%02X %02X) obj(%.3f %.3f %.3f) world(%.1f %.1f %.1f)",
            worstIdx, worstRaw.b[0], worstRaw.b[1], worstRaw.b[2], worstRaw.b[3], worstRaw.b[4],
            worstRaw.b[5], worstRaw.b[6], worstRaw.b[7], worstRaw.b[8], worstRaw.b[9],
            worstRaw.b[10], worstRaw.b[11], worstObj.x, worstObj.y, worstObj.z, worstWorld.x,
            worstWorld.y, worstWorld.z);
    else
        Log("    WORST vertex: none available - positions %s", posSource);
    if (haveTypical)
        Log("    TYPICAL vertex gv=%u obj(%.3f %.3f %.3f) world(%.1f %.1f %.1f) - for contrast "
            "against the worst vertex above",
            typicalIdx, typicalObj.x, typicalObj.y, typicalObj.z, typicalWorld.x, typicalWorld.y,
            typicalWorld.z);
}

// ---------------------------------------------------------------- index window probe / clamp
//
// THE 1e10 SLIVER (measured, not re-derived - see the settings comment on indexWindowProbe,
// above, for the full citation): SR3 hides an unused world-decal quad by parking its vertices at
// exactly 1e10 so the rasteriser clips it away. Path tracing does not clip, so for one frame a
// triangle addressing that vertex stretches across the screen - confirmed real geometry in
// Remix's Geometry Hash debug view. Two independent samples found the offending vertex INDEX
// equal to the draw's own declared NumVertices, i.e. exactly one past the end of the window
// [MinVertexIndex, MinVertexIndex+NumVertices) the game declared for the draw. Remix never reads
// that window: dxvk-remix/src/d3d9/d3d9_rtx.cpp:666 sets geoData.vertexCount from the bound index
// buffer's own min/max, computed from the indices themselves, and vertexIndexOffset from that
// same min - MinVertexIndex and NumVertices (present on the DrawContext, d3d9_rtx.h:53-54) are
// simply never referenced in that file. So this probe/fix operates on the game's ORIGINAL,
// UNTIGHTENED minIndex/numVertices (the Hook_DrawIndexedPrimitive parameters), never
// tightMinIndex/tightNumVertices - TightenConvertedWindow's narrow/widen only changes what this
// shim TELLS the device the window is, which Remix does not consult for this bug either way.
//
// MEASUREMENT COST: for a RIGID (non-skinned) converted draw, TightenConvertedWindow (the block
// immediately above this one in Hook_DrawIndexedPrimitive) has ALREADY Locked and scanned this
// exact (ib, startIndex, primitiveCount, type, size, format) identity THIS SAME CALL - see
// g_tightWin - so CheckIndexWindow below costs one hash-map lookup and nothing else: no Lock, no
// allocation. A SKINNED draw is excluded from TightenConvertedWindow by design (see its own "the
// bind pose" note) so there is no cache entry to reuse there; ScanIndexMinMaxCheap pays for a
// fresh Lock + min/max scan instead - still no allocation, no hashing, no dedup, exactly the
// "single cheap scan of indices" the cost budget allows for a population no existing cache
// covers. If TightenConvertedWindow itself refused this identity (no IB, bad type, GetDesc
// failure, its own 32-lock-a-frame budget spent) there is no cache entry either, and this probe
// correctly reports the same draw as "not measured" rather than paying for a second Lock.
//
// READ-ONLY for the probe: every Lock here is D3DLOCK_READONLY on a buffer already known
// non-dynamic (GetTightIbShape's own dynamic flag), the same hard rule TightenConvertedWindow/
// ComputeAssetHashIndexComponent/ProbeAssetHash already honour above - a DYNAMIC index buffer is
// refused outright, never Locked, because (as TightenConvertedWindow's own comment states) no
// Lock/Unlock hook exists on index buffers in this shim, so there is no snoop copy to substitute
// and no way to know a cached scan of one has gone stale.

struct IndexWindowVerdict {
    bool measured = false;   // false: refused (no IB, bad type, dynamic, GetDesc/lock failed) - no verdict, not counted as examined
    bool outside = false;    // true: the referenced index range [lo,hi] is not entirely inside [minIndex, minIndex+numVertices)
    bool below = false;      // lo < minIndex (a SEPARATE, rarer population from the sentinel-at-the-end case)
    UINT lo = 0, hi = 0;     // the draw's own referenced index range (raw values, relative to baseVertex - same frame minIndex is in)
    UINT overshoot = 0;      // maxIndex - (MinVertexIndex+NumVertices-1), 0 unless hi is at or past the end of the window
};

unsigned long long g_iwpExamined = 0;        // converted indexed draws measured (rigid reuse + skinned fresh scan, combined)
unsigned long long g_iwpOutside = 0;         // ...of those, had ANY index outside the declared window
unsigned long long g_iwpBelow = 0;           // ...of the outside population, had an index BELOW MinVertexIndex (subset, reported apart)
unsigned g_iwpWorstOvershoot = 0;            // worst (maxIndex - (MinVertexIndex+NumVertices-1)) ever seen, all-time
unsigned g_iwpReports = 0;                   // INDEX WINDOW PROBE example lines written, capped at 10 for the whole session (not re-armed - see g_albedoChurnReports for the same style)
unsigned long long g_iwpRigidReuse = 0;      // measured via the g_tightWin cache TightenConvertedWindow already populated - zero extra Locks
unsigned long long g_iwpSkinnedScan = 0;     // measured via a fresh ScanIndexMinMaxCheap Lock
unsigned long long g_iwpRefuseDynamic = 0;   // skinned draw, DYNAMIC index buffer - not measured (no snoop copy for index buffers)
unsigned long long g_iwpRefuseOther = 0;     // no IB / unhandled or empty primitive type / GetDesc failed / index slice past buffer end / lock failed

unsigned long long g_iwcDrawsCorrected = 0;      // indexWindowClamp/indexWindowClampStrips: draws actually re-issued from the corrected ring buffer (native TRIANGLELIST or an expanded strip/fan - one shared counter, see IssueClampedTriangleListFromRing)
unsigned long long g_iwcTrianglesCollapsed = 0;  // triangles replaced with (MinVertexIndex,MinVertexIndex,MinVertexIndex) across those draws
unsigned long long g_iwcAllocFail = 0;           // read/expand/ring-create/ring-lock failed - fell back to the ORIGINAL draw, unchanged
unsigned long long g_iwcTopologyRefused = 0;     // flagged by the probe, not a TRIANGLELIST, and NOT corrected because it is not a strip/fan OR indexWindowClampStrips is off - fell back unchanged. A strip/fan corrected under indexWindowClampStrips=1 counts in g_iwcDrawsCorrected instead, never here.
unsigned g_iwcSentinelProofLogged = 0;           // INDEX WINDOW CLAMP SENTINEL PROOF blocks written this session - hard-capped at 10, see LogIndexWindowClampSentinelProof's own comment

// The cheap min/max-only scan used for a SKINNED draw only (see the file comment above). Modelled
// line for line on TightenConvertedWindow's own cache-miss scan body: D3DLOCK_READONLY, refuse
// DYNAMIC outright, two locals accumulate lo/hi over the raw index slice - no vector, no hash.
bool ScanIndexMinMaxCheap(IDirect3DIndexBuffer9* ib, D3DPRIMITIVETYPE type, UINT startIndex,
                          UINT primitiveCount, UINT* outLo, UINT* outHi) {
    if (!ib) { ++g_iwpRefuseOther; return false; }
    if (type != D3DPT_TRIANGLELIST && type != D3DPT_TRIANGLESTRIP && type != D3DPT_TRIANGLEFAN) {
        ++g_iwpRefuseOther;
        return false;
    }
    const UINT nIdx = VertsForPrimitives(type, primitiveCount);
    if (!nIdx) { ++g_iwpRefuseOther; return false; }
    const TightIbShape* shape = GetTightIbShape(ib);
    if (!shape) { ++g_iwpRefuseOther; return false; }
    if (shape->dynamic) { ++g_iwpRefuseDynamic; return false; }
    const UINT isz = (shape->format == D3DFMT_INDEX32) ? 4 : 2;
    if (static_cast<unsigned long long>(startIndex) * isz + static_cast<unsigned long long>(nIdx) * isz >
        shape->size) {
        ++g_iwpRefuseOther;
        return false;
    }
    const bool wasInt = g_internal;
    g_internal = true;
    g_internalThread = GetCurrentThreadId();
    void* m = nullptr;
    if (FAILED(ib->Lock(startIndex * isz, nIdx * isz, &m, D3DLOCK_READONLY)) || !m) {
        g_internal = wasInt;
        ++g_iwpRefuseOther;
        return false;
    }
    UINT lo = 0xFFFFFFFFu, hi = 0;
    for (UINT i = 0; i < nIdx; ++i) {
        const unsigned v = (isz == 4) ? static_cast<const unsigned*>(m)[i]
                                      : static_cast<const unsigned short*>(m)[i];
        if (v < lo) lo = v;
        if (v > hi) hi = v;
    }
    ib->Unlock();
    g_internal = wasInt;
    *outLo = lo; *outHi = hi;
    return true;
}

// THE MEASUREMENT. See the file comment above for the population, the cost, and why this uses
// the game's ORIGINAL minIndex/numVertices rather than any tightened/widened substitute.
IndexWindowVerdict CheckIndexWindow(IDirect3DIndexBuffer9* ib, D3DPRIMITIVETYPE type, UINT minIndex,
                                    UINT numVertices, UINT startIndex, UINT primitiveCount,
                                    bool skinnedAny) {
    IndexWindowVerdict v;
    if (!g_settings.indexWindowProbe || !ib || !numVertices) return v;
    UINT lo = 0, hi = 0;
    bool have = false;
    if (!skinnedAny) {
        // Reuse, not re-derive: TightenConvertedWindow (immediately above, in
        // Hook_DrawIndexedPrimitive) always runs first for a rigid converted draw and always
        // caches its scan in g_tightWin under this exact key, regardless of tightenConvertedWindow
        // - so this lookup is a guaranteed hit costing one hash computation, never a Lock.
        const TightIbShape* shape = GetTightIbShape(ib);
        if (shape) {
            const TightWinKey key{ib, startIndex, primitiveCount, static_cast<int>(type), shape->size,
                                  shape->format};
            const auto it = g_tightWin.find(key);
            if (it != g_tightWin.end() && it->second.ok) {
                lo = it->second.lo; hi = it->second.hi;
                have = true;
                ++g_iwpRigidReuse;
            }
        }
    } else if (ScanIndexMinMaxCheap(ib, type, startIndex, primitiveCount, &lo, &hi)) {
        have = true;
        ++g_iwpSkinnedScan;
    }
    if (!have) return v;

    v.measured = true;
    v.lo = lo; v.hi = hi;
    ++g_iwpExamined;
    // One past the last legal index value - matches TightenConvertedWindow's own declaredEnd.
    const unsigned long long declaredEnd = static_cast<unsigned long long>(minIndex) + numVertices;
    if (lo < minIndex) v.below = true;
    if (static_cast<unsigned long long>(hi) + 1ull > declaredEnd) {
        v.overshoot = static_cast<unsigned>((static_cast<unsigned long long>(hi) + 1ull) - declaredEnd);
    }
    v.outside = v.below || (v.overshoot > 0);
    if (v.outside) {
        ++g_iwpOutside;
        if (v.below) ++g_iwpBelow;
        if (v.overshoot > g_iwpWorstOvershoot) g_iwpWorstOvershoot = v.overshoot;
    }
    return v;
}

// ---- the fix: a shim-owned index buffer, degenerate-collapsed
//
// THE RING. Same NOOVERWRITE-append/DISCARD-on-wrap scheme g_uvRing (UvBufferFor's "THE UV
// RING") and g_skinVB already use, sized for this population instead of copied from theirs: the
// draws this ever touches are 2-4 triangles for a building decal up to a couple hundred for the
// stretch-detect population, never a whole mesh, so 1 MB (262,144 INDEX32 entries) is generous
// headroom rather than the 8 MB the UV ring needs for whole converted vertex streams. Always
// INDEX32 regardless of the game's own index width - the source is widened once, here, rather
// than making every later reader branch on width the way the game's own buffers require.
//
// indexWindowClampStrips (below) reuses this SAME ring for its EXPANDED buffer rather than a
// second one of its own: a strip/fan's own raw index count is only P+2, but the explicit
// triangle list ExpandStripOrFanToTriangleList builds from it needs 3*P - up to roughly 1.5x more
// for the small P this population actually has (a couple hundred triangles at most, the same
// population this ring was already sized generously for), so the existing 1 MB headroom already
// covers it. IssueClampedTriangleListFromRing's own `need > kIwcRingBytes` check is what makes an
// expanded draw that still would not fit degrade safely - falling back to the ORIGINAL, unmodified
// draw exactly like every other allocation failure here, never a skipped one.
IDirect3DIndexBuffer9* g_iwcRing = nullptr;
bool g_iwcRingTried = false;
constexpr UINT kIwcRingBytes = 1u * 1024u * 1024u;
UINT g_iwcRingPos = 0;
unsigned g_iwcRingWraps = 0, g_iwcRingAppends = 0;

// A FULL read of this draw's own index slice - unlike the probe above, the fix needs every
// individual index value (to decide, per TRIANGLE, whether to collapse it), not just the
// range's min/max, so this is a genuinely new Lock. Only reached for a draw the cheap probe
// above already flagged (rare - see g_iwpOutside), never for the general population, so it
// does not touch the cost budget that gates the probe itself. Refuses a DYNAMIC index buffer
// outright, the same hard rule as ScanIndexMinMaxCheap/TightenConvertedWindow - no snoop copy
// exists for an index buffer in this shim.
//
// Serves TWO callers now: IssueIndexWindowClampedDraw reads a TRIANGLELIST's own P*3 indices
// unchanged; IssueIndexWindowClampedStripFanDraw (indexWindowClampStrips) reads a
// TRIANGLESTRIP/TRIANGLEFAN's P+2 indices before expanding them into a triangle list -
// VertsForPrimitives already returns the right count for either shape, so nothing else here
// needed to change to admit the second caller.
bool ReadIndexRawForClamp(IDirect3DIndexBuffer9* ib, D3DPRIMITIVETYPE type, UINT startIndex,
                          UINT primitiveCount, std::vector<UINT>* out) {
    if (!ib ||
        (type != D3DPT_TRIANGLELIST && type != D3DPT_TRIANGLESTRIP && type != D3DPT_TRIANGLEFAN))
        return false;
    const UINT nIdx = VertsForPrimitives(type, primitiveCount);
    if (!nIdx) return false;
    const TightIbShape* shape = GetTightIbShape(ib);
    if (!shape || shape->dynamic) return false;
    const UINT isz = (shape->format == D3DFMT_INDEX32) ? 4 : 2;
    if (static_cast<unsigned long long>(startIndex) * isz + static_cast<unsigned long long>(nIdx) * isz >
        shape->size)
        return false;
    const bool wasInt = g_internal;
    g_internal = true;
    g_internalThread = GetCurrentThreadId();
    void* m = nullptr;
    if (FAILED(ib->Lock(startIndex * isz, nIdx * isz, &m, D3DLOCK_READONLY)) || !m) {
        g_internal = wasInt;
        return false;
    }
    try { out->resize(nIdx); }
    catch (...) { ib->Unlock(); g_internal = wasInt; return false; }
    for (UINT i = 0; i < nIdx; ++i)
        (*out)[i] = (isz == 4) ? static_cast<const unsigned*>(m)[i]
                               : static_cast<const unsigned short*>(m)[i];
    ib->Unlock();
    g_internal = wasInt;
    return true;
}

// THE FIX ITSELF. Called only for a converted, non-skinned draw the probe above found
// referencing an index outside its declared window (this draw's iwVerdict.outside in
// Hook_DrawIndexedPrimitive). Two callers below share everything except how the corrected
// triangle list is OBTAINED:
//   - IssueIndexWindowClampedDraw: the draw is already a D3DPT_TRIANGLELIST, so its own raw
//     indices ARE the triangle list - nothing to expand.
//   - IssueIndexWindowClampedStripFanDraw (indexWindowClampStrips, default OFF): the draw is a
//     D3DPT_TRIANGLESTRIP or D3DPT_TRIANGLEFAN, whose indices are SHARED between adjacent
//     triangles (VertsForPrimitives gives prims+2 indices for prims triangles) - collapsing one
//     triangle in place would corrupt its neighbours, so ExpandStripOrFanToTriangleList first
//     turns it into an explicit, non-sharing triangle list (see its own comment for the winding
//     fix a strip needs), and everything past that point is identical to the TRIANGLELIST path.
// Both then call CollapseOutOfWindowTriangles (the shared per-triangle judgement) and
// IssueClampedTriangleListFromRing (the shared ring write + draw + restore) - there is exactly
// one collapse implementation and one ring implementation, not two.
//
// Every triangle that touches an out-of-window index gets all three of ITS OWN indices replaced
// with MinVertexIndex - a zero-area triangle, never hit by a ray, harmless under rasterisation -
// and every other triangle in the draw is copied through unchanged (INCLUDING a strip's own
// naturally-degenerate stitching triangles, which repeat an index on purpose to join two strips
// and are left exactly as they are unless one of their indices is ALSO out of window). Primitive
// count, vertex window (minIndex/numVertices, passed straight through) and every other draw
// parameter are identical to the original call; only the index VALUES the corrected buffer holds
// differ.
//
// Both callers return false on ANY failure - the read lock, the expand, the ring's own
// create/lock, or finding that no individual triangle actually crosses the window when read one
// by one (belt and suspenders: the probe's lo/hi cannot lie about the range, but a defensive
// false here still falls back cleanly rather than issuing a no-op buffer swap). The caller's own
// fallback then issues the UNCHANGED original draw - the same disposition as every other refusal
// path in this file, never a skipped draw.

// THE PER-TRIANGLE JUDGEMENT, shared by both callers below: given an explicit triangle list
// (already 3 indices per triangle - a native TRIANGLELIST's own raw, OR a strip/fan already
// expanded by ExpandStripOrFanToTriangleList), collapse every triangle that touches an
// out-of-window index to (MinVertexIndex, MinVertexIndex, MinVertexIndex) in place, and leave
// every other triangle untouched - see the file comment above for why a strip's own stitching
// degenerates are never specially detected or protected: they simply never trip the range test
// unless one of their indices is genuinely out of window, in which case collapsing them further
// is harmless (still degenerate). *firstBadIndex receives whichever of the FIRST collapsed
// triangle's own three indices was actually out of window (left untouched if nothing is
// collapsed) - proof-logging fodder for LogIndexWindowClampSentinelProof, not used for the
// collapse decision itself.
unsigned CollapseOutOfWindowTriangles(std::vector<UINT>& raw, UINT minIndex, UINT numVertices,
                                      UINT* firstBadIndex) {
    const UINT nIdx = static_cast<UINT>(raw.size());
    const UINT lastLegal = minIndex + numVertices - 1;
    unsigned trisCollapsed = 0;
    bool haveFirst = false;
    for (UINT t = 0; t + 3 <= nIdx; t += 3) {
        const UINT a = raw[t], b = raw[t + 1], c = raw[t + 2];
        const bool badA = a < minIndex || a > lastLegal;
        const bool badB = b < minIndex || b > lastLegal;
        const bool badC = c < minIndex || c > lastLegal;
        if (badA || badB || badC) {
            if (!haveFirst && firstBadIndex) {
                *firstBadIndex = badA ? a : (badB ? b : c);
                haveFirst = true;
            }
            raw[t] = raw[t + 1] = raw[t + 2] = minIndex;
            ++trisCollapsed;
        }
    }
    return trisCollapsed;
}

// THE SENTINEL-PROOF DUMP (2026-09-24) - direct evidence that the out-of-window index and THE
// 1e10 SLIVER (see the file comment above CheckIndexWindow) are the same phenomenon, not two
// coincidental findings: this reads and logs the actual object-space POSITION the out-of-window
// index resolves to, for the first 10 draws indexWindowClamp/indexWindowClampStrips ever corrects
// this session. Hard-capped at 10 (g_iwcSentinelProofLogged, checked first, before any read) -
// this is proof, not telemetry, and must never become a per-frame cost the way indexWindowProbe's
// own measurement is designed to be.
//
// Same population and the same dynamic-snoop/static-lock split as ReadRigidPositionsRaw already
// uses for stretchDetect (see its own comment, above): a RIGID, CONVERTED draw leaves stream 0's
// POSITION exactly as the game bound it, which is guaranteed readable this way only when
// posStream==0 and posOffset is known - BeginFFP's own gate already refuses anything else before
// a draw can reach Disp::Convert. Both callers of this function already require !skinned &&
// !skinnedFF (see clampEligible in Hook_DrawIndexedPrimitive), so there is no bind-pose/CPU-ring
// branch to consider the way ProbeStretchDetect needs one for a skinned draw. Two separate
// single-vertex reads (the bad index, then a typical in-window index for contrast) rather than
// one combined read: ReadRigidPositionsRaw assumes its globalVerts arg arrives front-to-back
// sorted (it takes lo=front()/hi=back()), and the bad and typical vertices are not reliably
// ordered relative to each other, so keeping the two reads apart is simpler than sorting and
// unscrambling the result afterwards.
void LogIndexWindowClampSentinelProof(INT baseVertex, UINT minIndex, UINT numVertices,
                                      UINT badIndex) {
    if (g_iwcSentinelProofLogged >= 10) return;
    if (g_curLayout.posStream != 0 || g_curLayout.posOffset < 0 || !g_stream0Stride || !g_stream0)
        return;

    const long long badG = static_cast<long long>(baseVertex) + static_cast<long long>(badIndex);
    // The middle of the declared window - NOT MinVertexIndex (the value every bad triangle gets
    // collapsed TO), so this is a genuinely independent in-window vertex for contrast.
    const UINT typicalIndex = minIndex + numVertices / 2;
    const long long typicalG =
        static_cast<long long>(baseVertex) + static_cast<long long>(typicalIndex);
    if (badG < 0 || badG > 0xFFFFFFFFll) return;
    const bool dynamicSrc = IsDynamicVB(g_stream0);

    std::vector<UINT> gvBad{static_cast<UINT>(badG)};
    std::vector<D3DVECTOR> objBad;
    std::vector<StretchVertexRaw> rawBad;
    if (!ReadRigidPositionsRaw(gvBad, g_curLayout.posOffset, g_stream0Stride, dynamicSrc, &objBad,
                               &rawBad) ||
        objBad.empty())
        return;

    bool haveTypical = false;
    std::vector<D3DVECTOR> objTyp;
    std::vector<StretchVertexRaw> rawTyp;
    if (typicalG >= 0 && typicalG <= 0xFFFFFFFFll) {
        std::vector<UINT> gvTyp{static_cast<UINT>(typicalG)};
        haveTypical = ReadRigidPositionsRaw(gvTyp, g_curLayout.posOffset, g_stream0Stride,
                                            dynamicSrc, &objTyp, &rawTyp) &&
                     !objTyp.empty();
    }

    ++g_iwcSentinelProofLogged;
    Log("INDEX WINDOW CLAMP SENTINEL PROOF #%u: frame %u draw %u | ps(albedo sampler)='%s' | "
        "corrected draw v=%u minIndex=%u baseVtx=%d | BAD index=%u (global vtx=%u, one past "
        "declared window end=%u) raw(%02X %02X %02X %02X  %02X %02X %02X %02X  %02X %02X %02X "
        "%02X) obj(%.3f %.3f %.3f)",
        g_iwcSentinelProofLogged, g_frames, g_drawIndexThisFrame, g_curPS.albedoSampler,
        numVertices, minIndex, baseVertex, badIndex, static_cast<UINT>(badG),
        minIndex + numVertices, rawBad[0].b[0], rawBad[0].b[1], rawBad[0].b[2], rawBad[0].b[3],
        rawBad[0].b[4], rawBad[0].b[5], rawBad[0].b[6], rawBad[0].b[7], rawBad[0].b[8],
        rawBad[0].b[9], rawBad[0].b[10], rawBad[0].b[11], objBad[0].x, objBad[0].y, objBad[0].z);
    if (haveTypical)
        Log("    TYPICAL in-window vertex idx=%u (global vtx=%u) raw(%02X %02X %02X %02X  %02X "
            "%02X %02X %02X  %02X %02X %02X %02X) obj(%.3f %.3f %.3f) - for contrast against the "
            "BAD vertex above",
            typicalIndex, static_cast<UINT>(typicalG), rawTyp[0].b[0], rawTyp[0].b[1],
            rawTyp[0].b[2], rawTyp[0].b[3], rawTyp[0].b[4], rawTyp[0].b[5], rawTyp[0].b[6],
            rawTyp[0].b[7], rawTyp[0].b[8], rawTyp[0].b[9], rawTyp[0].b[10], rawTyp[0].b[11],
            objTyp[0].x, objTyp[0].y, objTyp[0].z);
    else
        Log("    TYPICAL in-window vertex: unavailable (global vtx %lld out of range or read "
            "failed)",
            typicalG);
}

// THE RING TAIL, shared by both callers below: given an explicit triangle list already collapsed
// by CollapseOutOfWindowTriangles (trisCollapsed of its triangles rewritten to MinVertexIndex),
// write it into the shim-owned ring (see THE RING, above), issue it as D3DPT_TRIANGLELIST with
// baseVertex/minIndex/numVertices passed straight through, and restore the game's own index
// buffer binding before returning - unconditionally, on every exit path, exactly as before this
// was split out of IssueIndexWindowClampedDraw. trisCollapsed==0 is refused here (nothing to
// correct - the caller's own probe-driven eligibility should make this unreachable, but a
// defensive false costs nothing and avoids a no-op buffer swap). raw.size() is always a multiple
// of 3 by construction in both callers (a native TRIANGLELIST's own P*3, or the P*3
// ExpandStripOrFanToTriangleList builds), so primitiveCount is recovered as raw.size()/3 rather
// than threaded through as a separate parameter.
bool IssueClampedTriangleListFromRing(IDirect3DDevice9* dev, IDirect3DIndexBuffer9* gameIB,
                                      INT baseVertex, UINT minIndex, UINT numVertices,
                                      const std::vector<UINT>& raw, unsigned trisCollapsed,
                                      UINT firstBadIndex, HRESULT* outHr) {
    if (!trisCollapsed) { ++g_iwcAllocFail; return false; }
    const UINT nIdx = static_cast<UINT>(raw.size());

    if (!g_iwcRing && !g_iwcRingTried) {
        g_iwcRingTried = true;
        const bool wasInt = g_internal;
        g_internal = true;
        g_internalThread = GetCurrentThreadId();
        if (FAILED(dev->CreateIndexBuffer(kIwcRingBytes, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY,
                                          D3DFMT_INDEX32, D3DPOOL_DEFAULT, &g_iwcRing, nullptr)))
            g_iwcRing = nullptr;
        g_internal = wasInt;
        Log("index window clamp ring: %s (%u KB)",
            g_iwcRing ? "created" : "CREATE FAILED - indexWindowClamp falls back to the original draw",
            kIwcRingBytes >> 10);
    }
    if (!g_iwcRing) { ++g_iwcAllocFail; return false; }

    // Expanded lists need 3*P indices where the strip/fan itself only carried P+2 (see THE RING's
    // own comment, above) - a draw large enough to exceed the ring even in its expanded form
    // still degrades safely here, the same fall-back-to-the-original-draw path an oversized
    // TRIANGLELIST read would already take.
    const UINT need = nIdx * 4u;
    if (need > kIwcRingBytes) { ++g_iwcAllocFail; return false; }
    bool discard = false;
    if (g_iwcRingPos + need > kIwcRingBytes) { discard = true; g_iwcRingPos = 0; ++g_iwcRingWraps; }
    const bool wasInt = g_internal;
    g_internal = true;
    g_internalThread = GetCurrentThreadId();
    void* m = nullptr;
    const HRESULT lockHr = discard ? g_iwcRing->Lock(0, 0, &m, D3DLOCK_DISCARD)
                                   : g_iwcRing->Lock(g_iwcRingPos, need, &m, D3DLOCK_NOOVERWRITE);
    if (FAILED(lockHr) || !m) {
        g_internal = wasInt;
        ++g_iwcAllocFail;
        return false;
    }
    memcpy(m, raw.data(), need);
    g_iwcRing->Unlock();
    g_internal = wasInt;
    const UINT ringStartIndex = g_iwcRingPos / 4u;
    g_iwcRingPos += need;
    ++g_iwcRingAppends;

    // THE SWAP - same pattern as the cloth-remap swap a few lines below in
    // Hook_DrawIndexedPrimitive (see its own comment): g_curIB already holds what the game last
    // bound, so no GetIndices bridge round trip is needed to save it, and it is restored
    // unconditionally the instant this draw returns - the same discipline UnbindSkinFF uses for
    // render state.
    g_internal = true;
    g_internalThread = GetCurrentThreadId();
    g_origSetIndices(dev, g_iwcRing);
    g_internal = false;
    *outHr = g_origDrawIndexedPrimitive(dev, D3DPT_TRIANGLELIST, baseVertex, minIndex, numVertices,
                                        ringStartIndex, nIdx / 3u);
    g_internal = true;
    g_internalThread = GetCurrentThreadId();
    g_origSetIndices(dev, gameIB);
    g_internal = false;

    ++g_iwcDrawsCorrected;
    g_iwcTrianglesCollapsed += trisCollapsed;
    LogIndexWindowClampSentinelProof(baseVertex, minIndex, numVertices, firstBadIndex);
    return true;
}

bool IssueIndexWindowClampedDraw(IDirect3DDevice9* dev, IDirect3DIndexBuffer9* ib, INT baseVertex,
                                 UINT minIndex, UINT numVertices, UINT startIndex,
                                 UINT primitiveCount, HRESULT* outHr) {
    std::vector<UINT> raw;
    if (!ReadIndexRawForClamp(ib, D3DPT_TRIANGLELIST, startIndex, primitiveCount, &raw)) {
        ++g_iwcAllocFail;
        return false;
    }
    UINT firstBadIndex = 0;
    const unsigned trisCollapsed =
        CollapseOutOfWindowTriangles(raw, minIndex, numVertices, &firstBadIndex);
    return IssueClampedTriangleListFromRing(dev, ib, baseVertex, minIndex, numVertices, raw,
                                            trisCollapsed, firstBadIndex, outHr);
}

// THE STRIP/FAN EXPANSION (indexWindowClampStrips) - turns a D3DPT_TRIANGLESTRIP's or
// D3DPT_TRIANGLEFAN's P+2 indices into an explicit P*3 TRIANGLELIST, so
// CollapseOutOfWindowTriangles can then judge each triangle independently the way it already
// does for a native TRIANGLELIST. src is the draw's own raw index slice, unmodified, exactly as
// ReadIndexRawForClamp read it.
//
// WINDING: neither topology repeats vertices between triangles, so the device derives each
// triangle from a 3-wide sliding window over the same index list. For a FAN the shared vertex is
// always index 0, so triangle i is simply (idx[0], idx[i+1], idx[i+2]) for every i - the D3D9
// SDK's own worked example (v0..v4 -> (v0,v1,v2), (v0,v2,v3), (v0,v3,v4)) is exactly this, no
// rearrangement needed. A STRIP has no fixed shared vertex, so instead the runtime ALTERNATES
// winding every other triangle to keep the whole strip's front face consistent without the app
// repeating any vertex - the SDK's own strip example (v0..v4 -> (v0,v1,v2), (v2,v1,v3),
// (v2,v3,v4)) shows this plainly: triangle 1 (odd) is (v2,v1,v3), NOT the raw sliding window
// (v1,v2,v3) - its first two indices are swapped. An explicit TRIANGLELIST has no such implicit
// alternation built in, so skipping that swap here would silently reverse the winding - and
// therefore the backface-culled side and the geometric normal - of every other triangle in the
// strip. Swapping any two of a triangle's three indices reverses its winding regardless of WHICH
// two (all three "reversed" orderings are cyclic rotations of each other), so this swaps the
// first two to match the SDK's own documented example exactly, rather than an
// arbitrary-but-equivalent pair.
bool ExpandStripOrFanToTriangleList(const std::vector<UINT>& src, D3DPRIMITIVETYPE type,
                                    UINT primitiveCount, std::vector<UINT>* out) {
    if (src.size() != static_cast<size_t>(primitiveCount) + 2) return false;
    try { out->resize(static_cast<size_t>(primitiveCount) * 3); } catch (...) { return false; }
    for (UINT i = 0; i < primitiveCount; ++i) {
        UINT a = 0, b = src[i + 1], c = src[i + 2];
        if (type == D3DPT_TRIANGLEFAN) {
            a = src[0];
        } else {   // D3DPT_TRIANGLESTRIP
            a = src[i];
            if (i & 1) std::swap(a, b);   // odd triangle: undo D3D9's implicit alternation (see
                                          // the comment above) so this explicit list renders the
                                          // same facing the strip itself would have
        }
        (*out)[i * 3 + 0] = a;
        (*out)[i * 3 + 1] = b;
        (*out)[i * 3 + 2] = c;
    }
    return true;
}

// THE FIX FOR A STRIP/FAN (indexWindowClampStrips, default OFF - see the setting's own comment
// for the 266342-flagged-draws-never-corrected measurement that motivated this). Called only for
// a converted, non-skinned D3DPT_TRIANGLESTRIP/D3DPT_TRIANGLEFAN draw the probe found referencing
// an out-of-window index - the same clampEligible gate IssueIndexWindowClampedDraw uses, with the
// topology check inverted (see Hook_DrawIndexedPrimitive's own call site). Expands to an explicit
// triangle list first (ExpandStripOrFanToTriangleList, above), then reuses
// CollapseOutOfWindowTriangles and IssueClampedTriangleListFromRing exactly as the TRIANGLELIST
// path does - the fix itself is identical from that point on, only how the triangle list is
// OBTAINED differs. Returns false (falling back to the ORIGINAL, unmodified strip/fan draw) on a
// read failure, a malformed index count, or anything IssueClampedTriangleListFromRing itself
// refuses - never a skipped draw.
bool IssueIndexWindowClampedStripFanDraw(IDirect3DDevice9* dev, IDirect3DIndexBuffer9* ib,
                                         D3DPRIMITIVETYPE type, INT baseVertex, UINT minIndex,
                                         UINT numVertices, UINT startIndex, UINT primitiveCount,
                                         HRESULT* outHr) {
    std::vector<UINT> src;
    if (!ReadIndexRawForClamp(ib, type, startIndex, primitiveCount, &src)) {
        ++g_iwcAllocFail;
        return false;
    }
    std::vector<UINT> raw;
    if (!ExpandStripOrFanToTriangleList(src, type, primitiveCount, &raw)) {
        ++g_iwcAllocFail;
        return false;
    }
    UINT firstBadIndex = 0;
    const unsigned trisCollapsed =
        CollapseOutOfWindowTriangles(raw, minIndex, numVertices, &firstBadIndex);
    return IssueClampedTriangleListFromRing(dev, ib, baseVertex, minIndex, numVertices, raw,
                                            trisCollapsed, firstBadIndex, outHr);
}


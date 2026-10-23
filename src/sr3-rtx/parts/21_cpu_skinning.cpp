// parts/21_cpu_skinning.cpp - CPU skinning and its worker pool.
// NOT a standalone translation unit: #included, in order, by ../sr3rtx.cpp inside its anonymous
// namespace. Originally sr3rtx.cpp lines 15369-16730 (old file:line citations map here).
// ---------------------------------------------------------------- CPU skinning
//
// Why this exists: the skinned vertex buffer is STATIC and holds the bind pose (measured
// 2026-08-18, usage 0x8 = WRITEONLY, no DYNAMIC bit). Every frame of animation lives in the
// bone palette at c52 - vertex shader constants. So the geometry Remix can see never changes,
// and characters freeze in place while the converted world, which we hand over already
// transformed, moves correctly. That asymmetry is the whole bug, and it is the user's own
// observation: "the world does not freeze but the character animations and characters do".
//
// The algorithm, from the disassembly of debug_diffuse_only_c.fxo_pc:
//
//     pos' = ( SUM_i  w_i * Bone[idx_i] ) * float4(pos, 1)  /  SUM_i w_i
//
// Bone[] is row-major float3x4 at c52, 3 registers per bone. objTM (c32) is applied AFTER
// skinning, so we skin into OBJECT space and the existing WORLD/VIEW/PROJECTION path places
// the result unchanged - no transform work is duplicated here.
//
// Two measured details that a reasonable guess would have got wrong:
//   - BLENDWEIGHT is UBYTE4, not UBYTE4N. The bytes sum to 254-255, not to 1.0, which is why
//     the shader divides by their sum. Treating them as normalised would shrink every mesh.
//   - Index 255 is a sentinel for "no influence", always paired with weight 0. 255*3 = 765
//     addresses far past the 192-register palette, so influences must be rejected on WEIGHT,
//     never trusted by index.

struct SkinnedVertex {   // what we submit: plain, uncompressed, fixed-function readable
    float pos[3];
    float nrm[3];
    float uv[2];
};
constexpr DWORD kSkinnedFVF = D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX1;

// The decoded bind pose, cached per mesh. Decoding means read-locking the source buffer, which
// is a bridge round trip; doing that for ~157 skinned draws every frame is precisely the
// "state-call catastrophe" of sr2-fork.md section 6. Decode once, then only the arithmetic runs
// per frame.
struct BaseVertex {
    float pos[3];
    float nrm[3];
    float uv[2];
    // Weights kept as floats rather than raw bytes so one skinning loop serves every storage
    // format the game uses. The shader normalises by their sum, so the SCALE does not matter -
    // only that the four values are in the right order.
    float w[4];
    unsigned char idx[4];
    // TEXCOORD1, raw like uv. The pattern map is sampled with this set, not with uv - measured
    // 2026-09-03: SHORT2 at offset 32, alongside TEXCOORD0's SHORT2 at 28, same stream.
    float uv2[2];
};
struct BaseMesh {
    std::vector<BaseVertex> verts;
    // Pinned for the same reason the mesh albedo cache pins its textures, and it is the same
    // bug that caused the 2026-08-18 use-after-free: a released buffer's address can be handed
    // straight back to a newly created one, and this cache would then answer with a completely
    // different mesh's bind pose. The key contains the pointer, so without a reference the key
    // itself is unstable.
    IDirect3DVertexBuffer9* owner = nullptr;
    // Whether uv2 in these vertices is real, or just the zeroes a reduced declaration left.
    bool hasUv2 = false;
    // Highest bone index this mesh actually references, counting only components with a
    // non-zero weight - slot 255 is the "unused" sentinel and would otherwise dominate.
    // Computed once at decode so the per-draw check costs one comparison.
    unsigned maxBone = 0;
    // Which bones this mesh actually reads, one bit each. 64 bones fits exactly, and it lets the
    // per-draw check walk only the bones that matter instead of all 64.
    unsigned long long usedBones = 0;
    // FIX 5 CACHE (2026-09-22): SkinViaFixedFunction used to re-derive these two facts on EVERY
    // draw with a per-vertex scan - 140 hardware-skinned draws/frame over meshes of thousands of
    // vertices, measured as a real part of the 9.5 ms skin phase. Both are pure functions of the
    // decoded vertices above, computed once here instead. safeBones itself is NOT cached - it
    // depends on the SHADER's bone register (kRegBonePalette or wherever this shader's CTAB put
    // it), not the mesh, so only the raw per-vertex facts are cached and the shader-dependent
    // comparison still runs per draw, cheaply, against these.
    //
    // Single-bone mode (shader reads no BLENDWEIGHT): every vertex follows idx[0] alone, so the
    // bones to stage are a mask over idx[0] across the mesh, not the four-weight usedBones above.
    unsigned long long singleBoneMask = 0;   // OR of (1ull<<idx[0]) for every vertex with idx[0] < kBonesMax
    unsigned singleBoneMaxIdx0 = 0;          // true max idx[0] across every vertex, UNfiltered by kBonesMax
    bool singleBoneIdx0Overflow = false;     // true if some vertex's idx[0] >= kBonesMax (unstageable on any shader)
    // Four-bone mode: true if some vertex has no usable influence at all (every weight 0, or
    // every weighted index >= kBonesMax) - SkinAndBind's CPU path leaves such a vertex at its
    // bind pose, which the GPU path cannot express, so the whole draw must refuse.
    bool noUsableInfluence = false;
};
std::unordered_map<unsigned long long, BaseMesh> g_baseMeshes;
// Which cached meshes came out of which buffer, so a write-lock can drop exactly those.
std::unordered_map<IDirect3DVertexBuffer9*, std::vector<unsigned long long>> g_baseMeshByVB;
unsigned g_baseMeshInvalidations = 0;
constexpr size_t kMaxBaseMeshes = 1024;

// Drop every cached bind pose decoded out of this buffer. Indexed by owner so a lock costs a

// hash lookup rather than a walk of the whole cache - the game locks buffers many times a frame

// and this runs inside that path.

void InvalidateBaseMeshes(IDirect3DVertexBuffer9* vb) {

    const auto list = g_baseMeshByVB.find(vb);

    if (list == g_baseMeshByVB.end()) return;

    for (const unsigned long long key : list->second) {

        const auto it = g_baseMeshes.find(key);

        if (it == g_baseMeshes.end()) continue;

        if (it->second.owner) it->second.owner->Release();

        g_baseMeshes.erase(it);

        ++g_baseMeshInvalidations;

    }

    g_baseMeshByVB.erase(list);

}

IDirect3DVertexBuffer9* g_skinVB = nullptr;
// The ring the CPU-skinned vertices are written into, and the one piece of shared mutable memory
// in the whole skinned path. Its write position is forced to firstVertex*stride so the game's own
// index buffer can be reused verbatim - which means the ring is consumed far faster than the
// vertex count alone suggests, because each draw skips forward to its own index base.
//
// When it fills, the wrap issues D3DLOCK_DISCARD over the WHOLE buffer, which orphans the
// allocation. Anything already submitted this frame that still refers to the old storage is fine
// for rasterisation, but it is exactly the kind of aliasing a path tracer reading the buffer later
// would not survive. How often that happens scales with how much skinned geometry is on screen -
// the one property of the reported drift that nothing else explains.
//
// Sized from the ini so the hypothesis can be tested by making wraps impossible rather than by
// argument. 8 MB was the original fixed size.
UINT g_skinRingBytes = 8u * 1024u * 1024u;
unsigned g_skinRingWraps = 0, g_skinRingDiscards = 0;
// How long the ring's own Lock takes, split by kind. A whole-buffer DISCARD asks the driver to
// hand back a FRESH allocation of the entire buffer; if its pool has none free it blocks until
// the GPU releases one. That stall is charged to whichever draw happened to be first in the
// frame, so it lands in the shim's per-draw time indistinguishable from skinning arithmetic.
// Timed separately, "is the ring's SIZE costing us frames?" stops being an inference from the
// shape of a sawtooth and becomes a number.
double g_skinLockDiscardMs = 0.0, g_skinLockKeepMs = 0.0, g_skinLockWorstMs = 0.0;
// Draws whose vertex shader reads no BLENDINDICES: not skinned by the game, whatever the
// vertex declaration says. Counted apart from g_skinC52BoneReg, which lumps "declared at c52"
// together with "declared nowhere, defaulted to c52" and so reported this bug as clean.
unsigned g_skinNoBoneDecl = 0, g_skinNoBoneDeclReports = 0;
UINT g_skinRingHighWater = 0;
UINT g_skinRingPos = 0;
bool g_skinRingFresh = true;   // next lock must DISCARD

unsigned g_skinnedConverted = 0, g_skinnedRefused = 0;
unsigned g_baseMeshDecodes = 0;
unsigned g_baseMeshCacheFlushes = 0;

// Does [firstVertex, firstVertex+count) lie inside this buffer?
//
// Every read-lock of a game vertex buffer in this shim computes its byte offset as
// `streamOffset + firstVertex * stride`, in UINT. Since baseVertex joined firstVertex on
// 2026-08-20 that product is much larger than it used to be, and an overflow would WRAP to a
// small offset that D3D accepts and that reads entirely the wrong vertices - silently, and
// looking exactly like the wrong-window bug that omitting baseVertex caused in the first place.
//
// Relying on Lock to reject an out-of-range request is not enough either: it is the caller's
// arithmetic that is unsound, and by the time Lock sees it the number is already wrong.
bool VertexRangeFits(IDirect3DVertexBuffer9* vb, UINT streamOffset, UINT firstVertex,
                     UINT count, UINT stride) {
    if (!vb || stride == 0 || count == 0) return false;
    D3DVERTEXBUFFER_DESC d{};
    if (FAILED(vb->GetDesc(&d))) return false;
    const unsigned long long begin =
        static_cast<unsigned long long>(streamOffset) +
        static_cast<unsigned long long>(firstVertex) * stride;
    const unsigned long long end = begin + static_cast<unsigned long long>(count) * stride;
    return end <= d.Size;
}

unsigned long long SkinMeshKey(void* vb, UINT offset, UINT stride, UINT minIndex, UINT count) {
    unsigned long long h = 1469598103934665603ull;
    const unsigned long long parts[5] = {
        reinterpret_cast<unsigned long long>(vb), offset, stride, minIndex, count};
    for (int i = 0; i < 5; ++i) {
        h ^= parts[i];
        h *= 1099511628211ull;
    }
    return h;
}

// Decode the bind pose once. Returns null if this mesh must not be skinned.
// A refused skinned draw falls back to PASS-THROUGH, so Remix reconstructs it as a SECOND copy
// alongside the skinned one. That makes every refusal a visible defect, not just a missed
// optimisation, and the reason has to be nameable rather than guessed at.
const char* g_skinRefuseWhy = nullptr;
#define SKIN_REFUSE(why) do { g_skinRefuseWhy = (why); return nullptr; } while (0)

// How many bytes a declaration type occupies, or 0 if this shim cannot read it.
int DeclTypeSize(int t) {
    switch (t) {
        case D3DDECLTYPE_FLOAT1:   return 4;
        case D3DDECLTYPE_FLOAT2:   return 8;
        case D3DDECLTYPE_FLOAT3:   return 12;
        case D3DDECLTYPE_FLOAT4:   return 16;
        case D3DDECLTYPE_D3DCOLOR: return 4;
        case D3DDECLTYPE_UBYTE4:   return 4;
        case D3DDECLTYPE_UBYTE4N:  return 4;
        case D3DDECLTYPE_SHORT2:   return 4;
        case D3DDECLTYPE_SHORT4:   return 8;
        case D3DDECLTYPE_SHORT2N:  return 4;
        case D3DDECLTYPE_SHORT4N:  return 8;
        default:                   return 0;
    }
}

// `firstVertex` is baseVertex + minIndex, NOT minIndex.
//
// D3D9 fetches the vertex for index i from (streamOffset + (BaseVertexIndex + i) * stride), so
// the vertices a draw actually uses start at baseVertex + MinVertexIndex. This decode ignored
// baseVertex entirely until 2026-08-20, and so did the stream bind and the cache key - all three
// consistently. Consistent is not harmless: it decodes and draws a DIFFERENT WINDOW of the shared
// character buffer, self-consistently, which renders one character's mesh with another's vertices
// and therefore another's UVs. That is the reported "each character had the wrong head", and the
// matching key collision is the "random head texture" that survived turning the mesh albedo cache
// off entirely.
//
// It is also the best remaining explanation for skinRigidSingleBone making clothing VANISH: a
// bind pose decoded from the wrong window is arbitrary geometry, and posing it by a real bone
// matrix puts it somewhere the camera never looks.
const BaseMesh* GetBaseMesh(UINT firstVertex, UINT numVertices) {
    if (!g_stream0) SKIN_REFUSE("no stream 0");
    const VertexLayout& L = g_curLayout;

    // Bounds by arithmetic, not by a magic minimum stride. The first version demanded
    // stride >= 28 - the size of the ONE layout that had been measured - and refused a perfectly
    // decodable 24-byte layout because of it, sending 23 draws a frame back to pass-through
    // where each became a second copy of a character. What actually matters is that every field
    // this decoder reads lies inside the stride.
    const int stride = static_cast<int>(g_stream0Stride);
    auto fits = [&](int off, int type) {
        const int size = DeclTypeSize(type);
        return size > 0 && off >= 0 && off + size <= stride;
    };

    if (L.posType != D3DDECLTYPE_FLOAT3 || !fits(L.posOffset, L.posType))
        SKIN_REFUSE("position is not a float3 inside the stride");
    // UBYTE4 and UBYTE4N hold the same four bytes and differ only in how the GPU scales them.
    // Since the skinning divides by the sum of the weights, that scale cancels and both decode
    // identically - so accepting UBYTE4N here costs nothing and is not a guess.
    // A declaration may legitimately carry BLENDINDICES and NO BLENDWEIGHT: that is rigid
    // single-bone attachment, where each vertex follows exactly one bone with an implicit weight
    // of 1. Measured 2026-08-19 - the refusal log printed `weights=?(-1)@-1` beside a perfectly
    // ordinary `indices=ubyte4(5)@20`, and demanding a weight element that the format never had
    // sent 55 draws a frame back to pass-through, each becoming a second copy of a character.
    // OFF by default, 2026-08-19. Enabling it took refusals from 55/frame to 0 and, in the same
    // run, "some character clothes items have vanished" - a correlation with nothing else to
    // explain it. The reasoning still looks right (BLENDINDICES with no BLENDWEIGHT is rigid
    // single-bone attachment), but "looks right" is exactly what the tint modulate and the dedup
    // also had. Something about these meshes is not what the declaration suggests - most likely
    // they are not bone-driven at all, in which case skinning them moves them somewhere the
    // camera never sees, which reads as vanishing.
    //
    // Off restores the previous behaviour: these draws are refused, pass through, and Remix
    // reconstructs them. That is the double-draw back for those items, and it is the better of
    // the two - a garment drawn twice is visible and wrong, a garment moved out of the world is
    // just gone.
    const bool rigidSingleBone = g_settings.skinRigidSingleBone && (L.blendWeightType < 0);
    if (!rigidSingleBone &&
        ((L.blendWeightType != D3DDECLTYPE_UBYTE4 && L.blendWeightType != D3DDECLTYPE_UBYTE4N &&
          L.blendWeightType != D3DDECLTYPE_D3DCOLOR && L.blendWeightType != D3DDECLTYPE_FLOAT4) ||
         !fits(L.blendWeightOffset, L.blendWeightType)))
        SKIN_REFUSE("blend weights are a type this decoder does not read");
    if ((L.blendIndexType != D3DDECLTYPE_UBYTE4 && L.blendIndexType != D3DDECLTYPE_D3DCOLOR) ||
        !fits(L.blendIndexOffset, L.blendIndexType))
        SKIN_REFUSE("blend indices are a type this decoder does not read");
    if ((L.normalType != D3DDECLTYPE_UBYTE4N && L.normalType != D3DDECLTYPE_FLOAT3) ||
        !fits(L.normalOffset, L.normalType))
        SKIN_REFUSE("normal is a type this decoder does not read");
    if ((L.texcoordType != D3DDECLTYPE_SHORT2 && L.texcoordType != D3DDECLTYPE_FLOAT2) ||
        !fits(L.texcoordOffset, L.texcoordType))
        SKIN_REFUSE("texcoord is a type this decoder does not read");

    const unsigned long long key =
        SkinMeshKey(g_stream0, g_stream0Offset, g_stream0Stride, firstVertex, numVertices);
    const auto it = g_baseMeshes.find(key);
    g_dumpSkinCacheHit = (it != g_baseMeshes.end());
    if (it != g_baseMeshes.end()) {
        // RE-DECODE IF THIS DRAW OFFERS THE SECOND UV SET AND THE CACHED COPY LACKS IT.
        //
        // The bind pose is cached from whichever draw touched the buffer FIRST, and that is
        // usually a PREPASS whose declaration is reduced - no TEXCOORD1. Every later material
        // draw then reused a cached mesh with no pattern coordinates, which is why every cloth
        // bake reported "uv2 0" even though the declaration probe had clearly seen
        // texcoord1 at offset 32.
        if (!(L.texcoord1Offset >= 0 && !it->second.hasUv2)) return &it->second;
        g_baseMeshes.erase(it);
    }
    // A full cache used to refuse every new mesh FOREVER, and a refusal falls back to
    // pass-through - so once the cap was reached, every newly seen character would silently stop
    // animating and gain a second copy, permanently, with only a counter to show for it. Clearing
    // turns that cliff into a rebuild: the meshes still on screen are re-decoded within a frame
    // or two. Releasing the pinned buffers is part of the clear, not optional.
    if (g_baseMeshes.size() >= kMaxBaseMeshes) {
        g_baseMeshByVB.clear();
        for (auto& kv : g_baseMeshes)
            if (kv.second.owner) kv.second.owner->Release();
        g_baseMeshes.clear();
        ++g_baseMeshCacheFlushes;
    }

    // The buffer is WRITEONLY. Reading it is outside what D3D9 promises, but this is DXVK
    // behind the bridge and the probe read it back correctly - and the alternative, snooping
    // every Lock the game makes, would mean holding a copy of every character mesh in the game
    // whether it is on screen or not. Verified by the values: positions land at human scale
    // (~1.6 units tall) and weight bytes sum to 255.
    D3DVERTEXBUFFER_DESC vbd{};
    if (FAILED(g_stream0->GetDesc(&vbd))) SKIN_REFUSE("GetDesc on the source buffer failed");
    if (vbd.Usage & D3DUSAGE_DYNAMIC) SKIN_REFUSE("source buffer is DYNAMIC - must not read-lock");
    if (!VertexRangeFits(g_stream0, g_stream0Offset, firstVertex, numVertices, g_stream0Stride))
        SKIN_REFUSE("vertex range lies outside the source buffer");

    void* mapped = nullptr;
    if (FAILED(g_stream0->Lock(g_stream0Offset + firstVertex * g_stream0Stride,
                               numVertices * g_stream0Stride, &mapped,
                               D3DLOCK_READONLY | D3DLOCK_NOSYSLOCK)) || !mapped)
        SKIN_REFUSE("read-lock of the source buffer failed");

    BaseMesh mesh;
    // A shim must degrade, not abort the process it is a guest in. An uncaught bad_alloc here
    // reaches the CRT as "This application has requested the Runtime to terminate it in an
    // unusual way" - which is what the user saw on 2026-08-28. Refusing the mesh costs a
    // pass-through draw; throwing costs the session.
    try {
        mesh.verts.resize(numVertices);
        // Recorded so a later draw with a fuller declaration can tell this decode was made
        // without the pattern's coordinates and ask for it again.
        mesh.hasUv2 = (L.texcoord1Offset >= 0 && fits(L.texcoord1Offset, L.texcoord1Type));
    } catch (const std::bad_alloc&) {
        g_stream0->Unlock();
        SKIN_REFUSE("out of memory decoding the bind pose");
    }
    const unsigned char* base = static_cast<const unsigned char*>(mapped);
    bool ok = true;
    for (UINT i = 0; i < numVertices && ok; ++i) {
        const unsigned char* v = base + i * g_stream0Stride;
        BaseVertex& out = mesh.verts[i];
        const float* p = reinterpret_cast<const float*>(v + L.posOffset);
        for (int k = 0; k < 3; ++k) {
            if (!std::isfinite(p[k])) { ok = false; break; }
            out.pos[k] = p[k];
        }
        if (L.normalType == D3DDECLTYPE_FLOAT3) {
            const float* n = reinterpret_cast<const float*>(v + L.normalOffset);
            for (int k = 0; k < 3; ++k) out.nrm[k] = n[k];
        } else {
            // ubyte4n arrives as 0..1; the shader expands it with v*2-1.
            const unsigned char* n = v + L.normalOffset;
            for (int k = 0; k < 3; ++k)
                out.nrm[k] = (static_cast<float>(n[k]) / 255.0f) * 2.0f - 1.0f;
        }

        out.uv2[0] = out.uv2[1] = 0.0f;
        if (L.texcoord1Offset >= 0 && fits(L.texcoord1Offset, L.texcoord1Type)) {
            if (L.texcoord1Type == D3DDECLTYPE_FLOAT2) {
                const float* t2 = reinterpret_cast<const float*>(v + L.texcoord1Offset);
                out.uv2[0] = t2[0];
                out.uv2[1] = t2[1];
            } else {
                const short* t2 = reinterpret_cast<const short*>(v + L.texcoord1Offset);
                out.uv2[0] = static_cast<float>(t2[0]);
                out.uv2[1] = static_cast<float>(t2[1]);
            }
        }

        if (L.texcoordType == D3DDECLTYPE_FLOAT2) {
            const float* t = reinterpret_cast<const float*>(v + L.texcoordOffset);
            out.uv[0] = t[0];
            out.uv[1] = t[1];
        } else {
            // Raw short2, NOT scaled. The existing conversion already installs a texture matrix
            // for short UVs, so emitting the raw values keeps one owner for that scale instead
            // of re-deriving it here and having the two disagree.
            const short* t = reinterpret_cast<const short*>(v + L.texcoordOffset);
            out.uv[0] = static_cast<float>(t[0]);
            out.uv[1] = static_cast<float>(t[1]);
        }

        // D3DCOLOR is stored BGRA, so its bytes must be reordered to match the shader's
        // .xyzw. UBYTE4 and UBYTE4N are already in order, and FLOAT4 needs no scaling because
        // the skinning divides by the sum.
        if (rigidSingleBone) {
            // One bone, full weight. The remaining three slots stay at zero and the skinning
            // loop skips them on weight, so the unused indices are never dereferenced.
            out.w[0] = 1.0f;
            out.w[1] = out.w[2] = out.w[3] = 0.0f;
        } else {
        const unsigned char* wb = v + L.blendWeightOffset;
        if (L.blendWeightType == D3DDECLTYPE_FLOAT4) {
            const float* wf = reinterpret_cast<const float*>(wb);
            for (int k = 0; k < 4; ++k) out.w[k] = std::isfinite(wf[k]) ? wf[k] : 0.0f;
        } else if (L.blendWeightType == D3DDECLTYPE_D3DCOLOR) {
            out.w[0] = static_cast<float>(wb[2]);   // R
            out.w[1] = static_cast<float>(wb[1]);   // G
            out.w[2] = static_cast<float>(wb[0]);   // B
            out.w[3] = static_cast<float>(wb[3]);   // A
        } else {
            for (int k = 0; k < 4; ++k) out.w[k] = static_cast<float>(wb[k]);
        }
        }

        const unsigned char* ib = v + L.blendIndexOffset;
        if (L.blendIndexType == D3DDECLTYPE_D3DCOLOR) {
            out.idx[0] = ib[2]; out.idx[1] = ib[1]; out.idx[2] = ib[0]; out.idx[3] = ib[3];
        } else {
            memcpy(out.idx, ib, 4);
        }
        // Only weighted components count: an unused slot carries index 255 with weight 0 and
        // would make every mesh look like it reads bone 255.
        bool vertUsable = false;
        for (int k = 0; k < 4; ++k) {
            if (out.w[k] == 0.0f || out.idx[k] >= static_cast<unsigned>(kBonesMax)) continue;
            if (out.idx[k] > mesh.maxBone) mesh.maxBone = out.idx[k];
            mesh.usedBones |= 1ull << out.idx[k];
            vertUsable = true;
        }
        if (!vertUsable) mesh.noUsableInfluence = true;
        // FIX 5 CACHE: the single-bone-mode facts SkinViaFixedFunction used to re-derive with its
        // own per-draw scan of idx[0] alone - see the BaseMesh comment above.
        if (out.idx[0] > mesh.singleBoneMaxIdx0) mesh.singleBoneMaxIdx0 = out.idx[0];
        if (out.idx[0] >= static_cast<unsigned>(kBonesMax)) mesh.singleBoneIdx0Overflow = true;
        else mesh.singleBoneMask |= 1ull << out.idx[0];
    }
    g_stream0->Unlock();
    if (!ok) return nullptr;

    ++g_baseMeshDecodes;
    mesh.owner = g_stream0;
    mesh.owner->AddRef();
    g_baseMeshByVB[g_stream0].push_back(key);
    return &(g_baseMeshes[key] = std::move(mesh));
}

// Skin into the ring buffer and point stream 0 at it. Returns false if the caller must fall
// back to passing the draw through untouched.
// The per-character morph delta, read from the game's own writes rather than by locking.
//
// Measured 2026-08-27: SR3 keeps every character's delta in ONE 5 MB D3DUSAGE_DYNAMIC buffer,
// stride 12, one contiguous block per character - the stream offset is what selects the
// character (0, 17112, 34224, ... and 17112 = 1426 verts x 12). Read-locking that is exactly
// what GetBaseMesh refuses to do and should refuse: it is write-combined memory behind a
// 32->64-bit bridge.
//
// So it is snooped, through the machinery the instance streams already use: registering the
// buffer with InstanceBufferData makes Hook_VBLock/Hook_VBUnlock copy the game's own writes,
// and the `fresh` map says which bytes have actually been written since the last discard.
//
// Applied per DRAW rather than baked into the bind pose, which is what the first attempt did.
// The bind pose is cached and the base mesh is static; the morph lives in a dynamic buffer the
// game rewrites. Baking it would have needed the cache invalidated on every write to a
// per-frame buffer - and would have served one NPC's face to the next the moment it went stale.
// THE SHAPE BAKE: forward-declared so the probe below (and MorphBakeFor, further down) can hash
// a delta slice. AssetHashFnv1a's own definition sits far below, beside the asset-hash probe it
// was written for; moving a 300-line-separated inline function is more invasive than a forward
// declaration. No default argument here - the real definition supplies it, and a default may be
// specified only once per TU - so every call below passes the FNV offset basis explicitly.
inline unsigned long long AssetHashFnv1a(const void* data, size_t len, unsigned long long h);

// THE SHAPE BAKE: find-only accessor into g_instCache's writeSeq/discardSeq, under SnoopGuard -
// never inserts (RegisterSnoop is the only thing allowed to create an entry). Forward-declared
// for the same reason as AssetHashFnv1a above; defined beside the rest of the bake machinery,
// after MorphForThisDraw.
bool SnoopWriteSeq(IDirect3DVertexBuffer9* vb, unsigned* writeSeq, unsigned* discardSeq = nullptr);

struct MorphSource {
    // A COPY of this draw's slice, not a pointer into the snoop cache - the locking thread can
    // resize that vector while this loop is running. Reused across draws so the allocation
    // happens once; a 7,977-vertex body slice is 96 KB and a head 17 KB.
    std::vector<unsigned char>* slice = nullptr;
    UINT stride = 0;
    int posOff = -1, nrmOff = -1;
    bool fresh = false;      // every byte of the slice was written since the last discard
    bool active = false;
    // THE SHAPE BAKE: which buffer this slice came from and at what byte offset.
    // MorphForThisDraw already computes both (vb, base64) to call SnoopCopy, but never kept them
    // past that call. MorphBakeFor and the morph probe both need to name the buffer this delta is
    // READ FROM (distinct from g_stream0, the mesh it is applied TO).
    IDirect3DVertexBuffer9* vb = nullptr;
    UINT base = 0;
};
std::vector<unsigned char> g_morphSlice;

// THE SHAPE BAKE - always-on bookkeeping and the morph probe (morphProbe).
//
// "0 vertices/frame fell back (not written yet)" was a LYING counter: g_morphStaleVerts only
// ever incremented in SkinAndBind's reference loop, so with skinFast=1 (the default) a non-fresh
// morph slice silently rendered the base mesh and nothing said so. g_morphStaleDraws is the
// truthful replacement - one bump per DRAW whose morph slice was not fully fresh, counted here
// regardless of which SkinAndBind loop runs, and regardless of morphProbe.
unsigned g_morphStaleDraws = 0;
// One character's morph delta, tracked across frames: does its hash actually change, and how
// often? Keyed by (vb, morphBase, bytes) - the morph buffer identity alone, not the mesh it is
// applied to, since two draws through different meshes (body, head, accessory) of the SAME
// character read DIFFERENT slices of the SAME morph buffer and are different blocks.
struct MorphProbeBlock {
    unsigned firstFrame = 0, lastFrame = 0, framesSeen = 0;
    unsigned long long hash = 0;
    bool haveHash = false;
    unsigned hashChanges = 0;
    unsigned lastChangeFrame = 0;
    unsigned discardSeqAtLastChange = 0;
    UINT verts = 0;
    IDirect3DVertexBuffer9* morphVb = nullptr;
    UINT morphBase = 0;
    IDirect3DVertexBuffer9* s0vb = nullptr;
    UINT s0stride = 0, s0size = 0;
    UINT firstVertex = 0;
    // Dedupe stamp: MorphForThisDraw runs TWICE for one logical draw when skinViaFixedFunction=1
    // refuses a morph-active draw for some OTHER reason after already checking morph.active here
    // (its own call), and SkinAndBind's CPU fallback then calls this again for the same draw.
    unsigned lastProbedFrame = 0xFFFFFFFFu;
    unsigned lastProbedDraw = 0xFFFFFFFFu;
};
std::unordered_map<unsigned long long, MorphProbeBlock> g_morphProbeBlocks;
constexpr size_t kMaxMorphProbeBlocks = 128;
unsigned g_morphProbeChangesAfterDiscard = 0, g_morphProbeChangesInPlace = 0;
// Gaps (in frames) between consecutive hash changes of ANY block, capped - the report's median/
// min/max gap is computed over this at report time, not maintained incrementally.
std::vector<unsigned> g_morphProbeGaps;
constexpr size_t kMaxMorphProbeGaps = 4096;

unsigned long long MorphProbeKey(IDirect3DVertexBuffer9* vb, UINT base, UINT bytes) {
    unsigned long long h = 1469598103934665603ull;
    const unsigned long long parts[3] = {
        reinterpret_cast<unsigned long long>(vb), base, bytes};
    for (int i = 0; i < 3; ++i) { h ^= parts[i]; h *= 1099511628211ull; }
    return h;
}

MorphSource MorphForThisDraw(UINT firstVertex, UINT numVertices) {
    MorphSource m;
    const VertexLayout& L = g_curLayout;
    if (!g_settings.applyMorph || !g_curVS.usesMorph) return m;
    if (L.morphStream < 0 || L.morphStream >= static_cast<int>(kMaxStreams)) return m;
    if (L.morphPosType != D3DDECLTYPE_SHORT4 || L.morphPosOffset < 0) return m;
    IDirect3DVertexBuffer9* vb = g_streamVB[L.morphStream];
    if (!vb) return m;
    m.stride = g_streamStride[L.morphStream];
    if (m.stride < 8) { ++g_morphNoStride; return m; }
    RegisterSnoop(vb);
    // THE SHAPE BAKE: mark this buffer as a MORPH stream, so Hook_VBLock/Hook_VBUnlock can bump
    // the morph-specific counters beside the generic ones they already maintain for every snooped
    // buffer. RegisterSnoop inserts+AddRefs; this only marks an entry that must already exist (the
    // call above either just created it or found it), so find - never operator[] - is used here,
    // which cannot itself create an un-AddRef'd entry.
    {
        SnoopGuard g;
        const auto mit = g_instCache.find(vb);
        if (mit != g_instCache.end()) mit->second.isMorph = true;
    }
    const unsigned long long base64 =
        static_cast<unsigned long long>(g_streamOffset[L.morphStream]) +
        static_cast<unsigned long long>(firstVertex) * m.stride;
    const unsigned long long bytes64 =
        static_cast<unsigned long long>(numVertices) * m.stride;
    // A slice this draw could not possibly need is a sign the stream is not what we think it is.
    if (base64 > 0xFFFFFFFFull || bytes64 > (64ull << 20)) { ++g_morphNotSnooped; return MorphSource{}; }
    const UINT bytes = static_cast<UINT>(bytes64);
    if (g_morphSlice.size() < bytes) {
        // Growth is bounded by the cap above, and this vector is reused for every draw - so it
        // settles at the largest slice seen and then never allocates again.
        try { g_morphSlice.resize(bytes); }
        catch (const std::bad_alloc&) { ++g_morphNotSnooped; return MorphSource{}; }
    }
    if (!SnoopCopy(vb, static_cast<UINT>(base64), g_morphSlice.data(), bytes, &m.fresh)) {
        ++g_morphNotSnooped;
        return MorphSource{};
    }
    m.slice = &g_morphSlice;
    m.posOff = L.morphPosOffset;
    m.nrmOff = (L.morphNrmType == D3DDECLTYPE_UBYTE4N &&
                L.morphNrmOffset >= 0 &&
                L.morphNrmOffset + 4 <= static_cast<int>(m.stride)) ? L.morphNrmOffset : -1;
    m.active = true;
    m.vb = vb;
    m.base = static_cast<UINT>(base64);

    // Truthful replacement for the dead g_morphStaleVerts-only signal: one bump per DRAW whose
    // morph slice was not fully fresh. Deduped per draw: with skinViaFixedFunction=1 a refused
    // morph draw reaches this twice (SkinViaFixedFunction, then SkinAndBind's fallback).
    {
        static unsigned s_staleFrame = 0xFFFFFFFFu, s_staleDraw = 0xFFFFFFFFu;
        if (!m.fresh && (s_staleFrame != g_frames || s_staleDraw != g_drawIndexThisFrame)) {
            s_staleFrame = g_frames;
            s_staleDraw = g_drawIndexThisFrame;
            ++g_morphStaleDraws;
        }
    }

    // THE MORPH PROBE: has this character's delta actually CHANGED since we last saw it? Nothing
    // before this measured that - the bake below is only safe if the answer is usually "no".
    if (g_settings.morphProbe) {
        const unsigned long long pkey = MorphProbeKey(vb, m.base, bytes);
        auto pit = g_morphProbeBlocks.find(pkey);
        if (pit == g_morphProbeBlocks.end() && g_morphProbeBlocks.size() < kMaxMorphProbeBlocks) {
            MorphProbeBlock nb;
            nb.firstFrame = g_frames;
            nb.morphVb = vb;
            nb.morphBase = m.base;
            nb.verts = numVertices;
            nb.s0vb = g_stream0;
            nb.s0stride = g_stream0Stride;
            nb.firstVertex = firstVertex;
            if (g_stream0) {
                const auto dit = g_vbDescCache.find(g_stream0);
                if (dit != g_vbDescCache.end()) nb.s0size = dit->second.Size;
            }
            pit = g_morphProbeBlocks.emplace(pkey, nb).first;
        }
        if (pit != g_morphProbeBlocks.end()) {
            MorphProbeBlock& blk = pit->second;
            if (blk.lastProbedFrame != g_frames || blk.lastProbedDraw != g_drawIndexThisFrame) {
                blk.lastProbedFrame = g_frames;
                blk.lastProbedDraw = g_drawIndexThisFrame;
                ++blk.framesSeen;
                blk.lastFrame = g_frames;
                const unsigned long long h = AssetHashFnv1a(m.slice->data(), bytes, 1469598103934665603ull);
                if (!blk.haveHash) {
                    blk.hash = h;
                    blk.haveHash = true;
                } else if (h != blk.hash) {
                    unsigned curDiscardSeq = 0;
                    SnoopWriteSeq(vb, nullptr, &curDiscardSeq);
                    if (blk.hashChanges > 0) {
                        const unsigned gap = g_frames - blk.lastChangeFrame;
                        if (g_morphProbeGaps.size() < kMaxMorphProbeGaps) g_morphProbeGaps.push_back(gap);
                    }
                    if (curDiscardSeq != blk.discardSeqAtLastChange) ++g_morphProbeChangesAfterDiscard;
                    else ++g_morphProbeChangesInPlace;
                    blk.discardSeqAtLastChange = curDiscardSeq;
                    blk.hash = h;
                    ++blk.hashChanges;
                    blk.lastChangeFrame = g_frames;
                }
            }
        }
    }
    return m;
}

// ---------------------------------------------------------------------------------------------
// THE SHAPE BAKE (morphBake) - apply the morph delta ONCE into a whole-buffer copy of the source
// mesh, and hand THAT to SkinViaFixedFunction instead of refusing the draw outright. Safe only
// when the delta itself is close to static frame to frame - see the morph probe above, which
// exists to answer that before this is trusted. Math is exactly section 2.1 of the plan and
// SkinAndBind's own fast-path morph block: the same fresh/offset guards, the same 1/8192 position
// delta, the same normal delta-and-renormalise - reproduced here (not shared) because SkinAndBind
// writes into a fixed-format SkinnedVertex and this writes back into the SOURCE layout's own byte
// formats, in place, across the whole buffer.
// ---------------------------------------------------------------------------------------------
struct MorphBake {
    IDirect3DVertexBuffer9* src = nullptr;      // the WHOLE source VB this was copied from (AddRef'd)
    IDirect3DVertexBuffer9* baked = nullptr;    // shim-owned copy, morph applied to its window
    UINT srcSize = 0;                            // src's desc.Size at build time - baked's own size too
    UINT stride = 0;
    UINT phase = 0;
    UINT firstVertex = 0;
    UINT numVertices = 0;
    // Content-addressed 2026-09-22: morphVb/morphBase/morphBytes are NO LONGER part of the cache
    // identity (see MorphBakeKey) - deltaHash is. morphVb is kept AddRef'd purely to hold the
    // buffer alive for TallyOrphans' bookkeeping; morphBase/morphBytes are informational only.
    IDirect3DVertexBuffer9* morphVb = nullptr;  // AddRef'd - kept alive only, not part of the key
    UINT morphBase = 0;
    UINT morphBytes = 0;
    unsigned long long deltaHash = 0;   // THE CACHE IDENTITY now (folded into MorphBakeKey), not just a staleness check
    unsigned snoopWriteSeq = 0;         // diagnostic only now - hit/miss no longer depends on it
    unsigned lastUsedFrame = 0;
};
std::unordered_map<unsigned long long, MorphBake> g_morphBakes;
size_t g_morphBakeBytes = 0;
unsigned g_morphBakeBuilt = 0, g_morphBakeRebuilt = 0, g_morphBakeHits = 0,
        g_morphBakeSeqRehash = 0, g_morphBakeEvicted = 0, g_morphBakeDraws = 0;
unsigned g_morphBakeRefuseNotFresh = 0, g_morphBakeRefuseLock = 0, g_morphBakeRefuseCreate = 0,
        g_morphBakeRefuseBudget = 0, g_morphBakeRefusePooled = 0, g_morphBakeRefusePerFrame = 0;
double g_morphBakeMsTotal = 0.0, g_morphBakeMsWorst = 0.0;
// MEASURED 2026-09-22: the game writes the morph buffer ~190 times a frame and repacks its shared
// pool on nearly every DISCARD, so a per-draw hash of the delta is now unconditional (the content
// hash IS the cache key below, not just a staleness check) - 34.7 morph draws a frame over only a
// HANDFUL of distinct character blocks (the probe saw 2), i.e. ~3 MB of hashing a frame to
// re-derive the same few answers if it were not memoised. One hash per (block, frame): every draw
// of the SAME block in the SAME frame reuses it instead of re-hashing.
struct MorphDeltaHashMemo { unsigned frame = 0xFFFFFFFFu; unsigned seq = 0; unsigned long long hash = 0; };
std::unordered_map<unsigned long long, MorphDeltaHashMemo> g_morphDeltaHashMemo;
unsigned g_morphBakeHashMemoHits = 0;
bool g_skinFFBakeBound = false;
const char* g_morphBakeRefuseWhy = nullptr;
// Per-frame budget bookkeeping, reset in Hook_Present beside the other per-frame resets.
unsigned g_morphBakeBuildsThisFrame = 0;      // new (first-time) builds this frame
unsigned g_morphBakeRebuildsThisFrame = 0;    // rebuilds (delta actually changed) this frame
bool g_morphBakeChurnLoggedThisFrame = false;
// 0 none, 1 hit, 2 built, 3 rebuilt - read by SkinViaFixedFunction right after calling
// MorphBakeFor, to fill g_dumpSkinFFBake on the dump frame. A plain global rather than an
// out-param: MorphBakeFor's signature is exactly the plan's (dev, firstVertex, numVertices,
// const MorphSource&), and this is dump-only bookkeeping the same way g_dumpSkinFF* already is.
int g_morphBakeLastOutcome = 0;

// THE SHAPE BAKE (content-addressed 2026-09-22): keyed on the delta's own CONTENT HASH instead
// of (morphVb, morphBase, morphBytes). MEASURED: the game repacks its shared morph pool 163-190
// writes/frame with 0.88 DISCARD/frame, and every content change (8,735 of 8,735 probed) landed
// after a DISCARD - never in place - which means a character's block routinely MOVES to a new
// offset (sometimes a new VB) between frames even when its shape bytes are byte-for-byte
// unchanged. Keying on (morphVb, morphBase) made every such move look like a brand-new block and
// forced a full rebuild - 7,699 of them in one measured run. Keying on deltaHash instead means an
// unchanged shape hits the SAME entry no matter where the game put it.
unsigned long long MorphBakeKey(IDirect3DVertexBuffer9* src, UINT stride, UINT phase,
                                UINT firstVertex, UINT numVertices,
                                unsigned long long deltaHash) {
    unsigned long long h = 1469598103934665603ull;
    const unsigned long long parts[6] = {
        reinterpret_cast<unsigned long long>(src), stride, phase, firstVertex, numVertices,
        deltaHash};
    for (int i = 0; i < 6; ++i) { h ^= parts[i]; h *= 1099511628211ull; }
    return h;
}

bool SnoopWriteSeq(IDirect3DVertexBuffer9* vb, unsigned* writeSeq, unsigned* discardSeq) {
    if (!vb) return false;
    SnoopGuard guard;
    const auto it = g_instCache.find(vb);
    if (it == g_instCache.end()) return false;
    if (writeSeq) *writeSeq = it->second.writeSeq;
    if (discardSeq) *discardSeq = it->second.discardSeq;
    return true;
}

// A write to the SOURCE mesh kills every bake taken from it - NOT keyed on the morph VB, because
// the key names src explicitly and a write to src invalidates every entry regardless of which
// morph buffer it paired with.
void InvalidateMorphBakesBySrc(IDirect3DVertexBuffer9* vb) {
    if (!vb) return;
    for (auto it = g_morphBakes.begin(); it != g_morphBakes.end();) {
        if (it->second.src == vb) {
            g_morphBakeBytes -= (it->second.srcSize <= g_morphBakeBytes) ? it->second.srcSize : g_morphBakeBytes;
            if (it->second.baked) it->second.baked->Release();
            if (it->second.src) it->second.src->Release();
            if (it->second.morphVb) it->second.morphVb->Release();
            it = g_morphBakes.erase(it);
        } else {
            ++it;
        }
    }
}

// Same, but by either role - src OR morphVb - for ForgetInstanceStream, which does not know which
// role an orphaned buffer played.
void InvalidateMorphBakes(IDirect3DVertexBuffer9* vb) {
    if (!vb) return;
    for (auto it = g_morphBakes.begin(); it != g_morphBakes.end();) {
        if (it->second.src == vb || it->second.morphVb == vb) {
            g_morphBakeBytes -= (it->second.srcSize <= g_morphBakeBytes) ? it->second.srcSize : g_morphBakeBytes;
            if (it->second.baked) it->second.baked->Release();
            if (it->second.src) it->second.src->Release();
            if (it->second.morphVb) it->second.morphVb->Release();
            it = g_morphBakes.erase(it);
        } else {
            ++it;
        }
    }
}

// Evict least-recently-used bakes (by lastUsedFrame, never one used THIS frame) until `needed`
// more bytes fit under morphBakeMaxMB, or nothing more can be evicted.
void MorphBakeEvictFor(size_t needed) {
    const size_t capBytes = static_cast<size_t>(max(1, g_settings.morphBakeMaxMB)) * 1024ull * 1024ull;
    while (g_morphBakeBytes + needed > capBytes) {
        auto oldest = g_morphBakes.end();
        for (auto it = g_morphBakes.begin(); it != g_morphBakes.end(); ++it) {
            if (it->second.lastUsedFrame == g_frames) continue;   // never evict something used this frame
            if (oldest == g_morphBakes.end() || it->second.lastUsedFrame < oldest->second.lastUsedFrame)
                oldest = it;
        }
        if (oldest == g_morphBakes.end()) return;   // nothing evictable this frame
        g_morphBakeBytes -= (oldest->second.srcSize <= g_morphBakeBytes) ? oldest->second.srcSize : g_morphBakeBytes;
        if (oldest->second.baked) oldest->second.baked->Release();
        if (oldest->second.src) oldest->second.src->Release();
        if (oldest->second.morphVb) oldest->second.morphVb->Release();
        g_morphBakes.erase(oldest);
        ++g_morphBakeEvicted;
    }
}

// Apply section 2.1's math into the whole-buffer copy already sitting in `dstMapped`, at
// [firstVertex, firstVertex+numVertices) - exactly SkinAndBind's fast-path morph block, byte for
// byte, except position/normal are written back into the SOURCE layout's own types (float3
// position always - GetBaseMesh already refused anything else before this mesh could reach here;
// UBYTE4N or float3 normal, matching g_curLayout.normalType) instead of into a fixed SkinnedVertex.
static void ApplyMorphBakeDelta(unsigned char* dstMapped, UINT stride, UINT firstVertex,
                                UINT numVertices, const MorphSource& morph) {
    unsigned char* dst8 = dstMapped;
    for (UINT i = 0; i < numVertices; ++i) {
        const size_t off = static_cast<size_t>(i) * morph.stride;
        if (off + static_cast<size_t>(morph.posOff) + 8u > morph.slice->size()) continue;
        const unsigned char* md = morph.slice->data();
        unsigned char* v = dst8 + (static_cast<size_t>(firstVertex) + i) * stride;

        // pos += short.xyz * kMorphScale - written straight back as the float3 GetBaseMesh
        // already required this layout's position to be.
        const short* d = reinterpret_cast<const short*>(md + off + morph.posOff);
        float* posOut = reinterpret_cast<float*>(v + g_curLayout.posOffset);
        for (int k = 0; k < 3; ++k) posOut[k] += static_cast<float>(d[k]) * kMorphScale;

        if (morph.nrmOff < 0 || off + static_cast<size_t>(morph.nrmOff) + 4u > morph.slice->size())
            continue;
        // n = normalize(n_base + 2*(ubyte/255*2-1)) - n_base decoded straight from the vertex we
        // just copied, exactly GetBaseMesh's own UBYTE4N/FLOAT3 decode.
        float nBase[3];
        if (g_curLayout.normalType == D3DDECLTYPE_FLOAT3) {
            const float* nf = reinterpret_cast<const float*>(v + g_curLayout.normalOffset);
            nBase[0] = nf[0]; nBase[1] = nf[1]; nBase[2] = nf[2];
        } else {
            const unsigned char* nb = v + g_curLayout.normalOffset;
            for (int k = 0; k < 3; ++k) nBase[k] = (static_cast<float>(nb[k]) / 255.0f) * 2.0f - 1.0f;
        }
        const unsigned char* nn = md + off + morph.nrmOff;
        float mnrm[3]; float len = 0.0f;
        for (int k = 0; k < 3; ++k) {
            const float nm = (static_cast<float>(nn[k]) / 255.0f) * 2.0f - 1.0f;
            mnrm[k] = nBase[k] + 2.0f * nm;
            len += mnrm[k] * mnrm[k];
        }
        len = std::sqrt(len);
        if (len > 1e-8f) for (int k = 0; k < 3; ++k) mnrm[k] /= len;
        if (g_curLayout.normalType == D3DDECLTYPE_FLOAT3) {
            float* nOut = reinterpret_cast<float*>(v + g_curLayout.normalOffset);
            nOut[0] = mnrm[0]; nOut[1] = mnrm[1]; nOut[2] = mnrm[2];
        } else {
            unsigned char* nOut = v + g_curLayout.normalOffset;   // 4th byte (index 3) untouched
            for (int k = 0; k < 3; ++k) {
                int enc = static_cast<int>(std::round((mnrm[k] * 0.5f + 0.5f) * 255.0f));
                if (enc < 0) enc = 0; else if (enc > 255) enc = 255;
                nOut[k] = static_cast<unsigned char>(enc);
            }
        }
    }
}

// Build (or return the cached) whole-source-VB copy with this draw's morph delta baked in.
// Refuses - with g_morphBakeRefuseWhy set and the matching counter already bumped - for anything
// not safe, exactly SkinViaFixedFunction's own refusal contract: the caller's CPU fallback runs
// unchanged.
IDirect3DVertexBuffer9* MorphBakeFor(IDirect3DDevice9* dev, UINT firstVertex, UINT numVertices,
                                     const MorphSource& morph) {
    g_morphBakeRefuseWhy = nullptr;
    g_morphBakeLastOutcome = 0;
    if (!morph.fresh) {
        g_morphBakeRefuseWhy = "morph block not fresh";
        ++g_morphBakeRefuseNotFresh;
        return nullptr;
    }
    if (!g_stream0 || g_stream0Stride == 0) {
        g_morphBakeRefuseWhy = "no stream 0";
        ++g_morphBakeRefuseLock;
        return nullptr;
    }
    const UINT stride = g_stream0Stride;
    const UINT phase = g_stream0Offset % stride;

    D3DVERTEXBUFFER_DESC desc{};
    const auto dit = g_vbDescCache.find(g_stream0);
    if (dit != g_vbDescCache.end()) {
        desc = dit->second;
    } else if (SUCCEEDED(g_stream0->GetDesc(&desc))) {
        try { g_vbDescCache[g_stream0] = desc; } catch (...) {}
    } else {
        g_morphBakeRefuseWhy = "GetDesc on the source buffer failed";
        ++g_morphBakeRefuseLock;
        return nullptr;
    }
    if (desc.Usage & D3DUSAGE_DYNAMIC) {
        g_morphBakeRefuseWhy = "source buffer is DYNAMIC";
        ++g_morphBakeRefuseLock;
        return nullptr;
    }
    if (!VertexRangeFits(g_stream0, g_stream0Offset, firstVertex, numVertices, stride)) {
        g_morphBakeRefuseWhy = "vertex range lies outside the source buffer";
        ++g_morphBakeRefuseLock;
        return nullptr;
    }
    // A pooled buffer holding many meshes end to end is exactly the wrong shape for a
    // WHOLE-buffer bake: one draw's morph would be baked across every OTHER mesh sharing it too.
    if (static_cast<unsigned long long>(desc.Size) > 4ull * numVertices * stride &&
        desc.Size > (1u << 20)) {
        g_morphBakeRefuseWhy = "source buffer pooled across many meshes";
        ++g_morphBakeRefusePooled;
        return nullptr;
    }

    const UINT morphBytes = numVertices * morph.stride;

    // THE SHAPE BAKE (content-addressed 2026-09-22): hash the delta BEFORE building the key - the
    // hash IS one of the key's own inputs now, so a lookup needs it up front, not just to validate
    // a stale entry. Still at most one real hash per (block, frame) thanks to the memo below: its
    // own key stays (vb, base, bytes) exactly as it was, since that is what identifies WHICH slice
    // is being read this draw, regardless of what the resulting bake key looks like.
    unsigned seq = 0;
    const bool haveSeq = SnoopWriteSeq(morph.vb, &seq, nullptr);
    const unsigned long long memoKey = MorphProbeKey(morph.vb, morph.base, morphBytes);
    unsigned long long h = 0ull;
    {
        auto& memo = g_morphDeltaHashMemo[memoKey];
        if (haveSeq && memo.frame == g_frames && memo.seq == seq) {
            h = memo.hash;
            ++g_morphBakeHashMemoHits;
        } else {
            ++g_morphBakeSeqRehash;
            h = morph.slice
                ? AssetHashFnv1a(morph.slice->data(), morphBytes, 1469598103934665603ull) : 0ull;
            memo.frame = g_frames;
            memo.seq = seq;
            memo.hash = h;
        }
    }

    const unsigned long long key = MorphBakeKey(g_stream0, stride, phase, firstVertex, numVertices, h);

    bool isRebuild = false;
    auto it = g_morphBakes.find(key);
    if (it != g_morphBakes.end()) {
        MorphBake& e = it->second;
        if (e.deltaHash == h) {
            // Content-addressed HIT: the map key already proves the bytes match (h fed the key
            // formula itself) - this equality only guards the combined 64-bit MorphBakeKey against
            // the astronomically unlikely case of two different inputs folding to the same value.
            // No snoopWriteSeq comparison needed any more: a block that moved to a new offset but
            // kept its bytes now simply hashes to the SAME key it always did.
            e.lastUsedFrame = g_frames;
            if (haveSeq) e.snoopWriteSeq = seq;
            ++g_morphBakeHits;
            g_morphBakeLastOutcome = 1;
            return e.baked;
        }
        // A genuine MorphBakeKey collision - different content folded to the same 64-bit key.
        // g_morphBakeRebuilt now means exactly this and should sit at ~0; nonzero here is a real
        // collision, not routine churn (the old offset-move churn is gone - a moved-but-unchanged
        // block just HITS, above).
        isRebuild = true;
    }

    // Per-frame budget: new builds and (now vanishingly rare) collision rebuilds are counted and
    // capped separately against morphBakeMaxPerFrame.
    const unsigned perFrameCap = static_cast<unsigned>(max(0, g_settings.morphBakeMaxPerFrame));
    if (!isRebuild) {
        if (g_morphBakeBuildsThisFrame >= perFrameCap) {
            g_morphBakeRefuseWhy = "per-frame new-build budget spent";
            ++g_morphBakeRefusePerFrame;
            return nullptr;
        }
    } else {
        ++g_morphBakeRebuildsThisFrame;
        if (g_morphBakeRebuildsThisFrame > perFrameCap) {
            if (!g_morphBakeChurnLoggedThisFrame) {
                g_morphBakeChurnLoggedThisFrame = true;
                Log("MORPH BAKE: %u MorphBakeKey collisions in frame %u - different delta content "
                    "folded to the same 64-bit key, which should not happen; set morphBake=0",
                    g_morphBakeRebuildsThisFrame, g_frames);
            }
            g_morphBakeRefuseWhy = "MorphBakeKey collision - per-frame rebuild budget spent";
            ++g_morphBakeRefusePerFrame;
            return nullptr;
        }
    }

    // An entry always holds a valid baked buffer or does not exist at all - so a rebuild that
    // fails anywhere below drops the whole entry (releasing src/morphVb too) rather than leaving
    // a half-updated one with a stale byte count.
    auto dropFailedRebuild = [&]() {
        if (!isRebuild) return;
        MorphBake& e2 = it->second;
        if (e2.src) e2.src->Release();
        if (e2.morphVb) e2.morphVb->Release();
        g_morphBakes.erase(it);
    };

    if (isRebuild) {
        MorphBake& e = it->second;
        e.lastUsedFrame = g_frames;   // protect this entry from self-eviction below
        g_morphBakeBytes -= (e.srcSize <= g_morphBakeBytes) ? e.srcSize : g_morphBakeBytes;
        if (e.baked) { e.baked->Release(); e.baked = nullptr; }
        // A true collision could in principle pair this key with a DIFFERENT src or morph VB than
        // last time - keep the entry's AddRef'd pointers in sync so exactly one AddRef of whichever
        // buffer is actually held is what TallyOrphans ever counts.
        if (e.src != g_stream0) {
            if (e.src) e.src->Release();
            e.src = g_stream0;
            if (e.src) e.src->AddRef();
        }
        if (e.morphVb != morph.vb) {
            if (e.morphVb) e.morphVb->Release();
            e.morphVb = morph.vb;
            if (e.morphVb) e.morphVb->AddRef();
        }
    }
    MorphBakeEvictFor(desc.Size);
    const size_t capBytes = static_cast<size_t>(max(1, g_settings.morphBakeMaxMB)) * 1024ull * 1024ull;
    if (g_morphBakeBytes + desc.Size > capBytes) {
        g_morphBakeRefuseWhy = "bake budget";
        ++g_morphBakeRefuseBudget;
        dropFailedRebuild();
        return nullptr;
    }

    const LONGLONG t0 = Now();
    g_internal = true;
    g_internalThread = GetCurrentThreadId();
    void* srcMapped = nullptr;
    if (FAILED(g_stream0->Lock(0, 0, &srcMapped, D3DLOCK_READONLY | D3DLOCK_NOSYSLOCK)) || !srcMapped) {
        g_internal = false;
        g_morphBakeRefuseWhy = "read-lock of the source buffer failed";
        ++g_morphBakeRefuseLock;
        dropFailedRebuild();
        return nullptr;
    }
    IDirect3DVertexBuffer9* baked = nullptr;
    const HRESULT hrCreate = dev->CreateVertexBuffer(desc.Size, D3DUSAGE_WRITEONLY, 0,
                                                     D3DPOOL_DEFAULT, &baked, nullptr);
    if (FAILED(hrCreate) || !baked) {
        g_stream0->Unlock();
        g_internal = false;
        g_morphBakeRefuseWhy = "CreateVertexBuffer failed for the baked copy";
        ++g_morphBakeRefuseCreate;
        dropFailedRebuild();
        return nullptr;
    }
    void* dstMapped = nullptr;
    if (FAILED(baked->Lock(0, 0, &dstMapped, D3DLOCK_DISCARD)) || !dstMapped) {
        baked->Release();
        g_stream0->Unlock();
        g_internal = false;
        g_morphBakeRefuseWhy = "lock of the baked copy failed";
        ++g_morphBakeRefuseLock;
        dropFailedRebuild();
        return nullptr;
    }
    memcpy(dstMapped, srcMapped, desc.Size);
    ApplyMorphBakeDelta(static_cast<unsigned char*>(dstMapped), stride, firstVertex, numVertices, morph);
    baked->Unlock();
    g_stream0->Unlock();
    g_internal = false;

    const double ms = MsSince(t0);
    g_morphBakeMsTotal += ms;
    if (ms > g_morphBakeMsWorst) g_morphBakeMsWorst = ms;

    // deltaHash is just `h`, already computed above from the same bytes - no need to hash again.
    if (isRebuild) {
        MorphBake& e = it->second;
        e.baked = baked;
        e.srcSize = desc.Size;
        e.stride = stride;
        e.phase = phase;
        e.firstVertex = firstVertex;
        e.numVertices = numVertices;
        e.morphBase = morph.base;
        e.morphBytes = morphBytes;
        e.deltaHash = h;
        if (haveSeq) e.snoopWriteSeq = seq;
        e.lastUsedFrame = g_frames;
        g_morphBakeBytes += desc.Size;
        ++g_morphBakeRebuilt;
        g_morphBakeLastOutcome = 3;
    } else {
        MorphBake e;
        e.src = g_stream0; e.src->AddRef();
        e.baked = baked;
        e.srcSize = desc.Size;
        e.stride = stride;
        e.phase = phase;
        e.firstVertex = firstVertex;
        e.numVertices = numVertices;
        e.morphVb = morph.vb; if (e.morphVb) e.morphVb->AddRef();
        e.morphBase = morph.base;
        e.morphBytes = morphBytes;
        e.deltaHash = h;
        if (haveSeq) e.snoopWriteSeq = seq;
        e.lastUsedFrame = g_frames;
        g_morphBakes.emplace(key, e);
        g_morphBakeBytes += desc.Size;
        ++g_morphBakeBuilt;
        ++g_morphBakeBuildsThisFrame;
        g_morphBakeLastOutcome = 2;
    }
    return baked;
}

// ---------------------------------------------------------------------------------------------
// STEP 3a - take ONE character mesh and submit it through the Remix API as well.
//
// Deliberately "as well", not "instead". A DOUBLED character is the proof that API-submitted
// character geometry landed; replacing the existing path in the same change would make a missing
// character mean either "the API failed" or "the old path was removed correctly", and that
// ambiguity has cost this project whole sessions before.
//
// The skinned vertices are already exactly the layout Remix wants - SkinnedVertex is
// {pos[3], nrm[3], uv[2]} and remixapi_HardcodedVertex is that plus a colour and padding - and
// they are already in WORLD space, so the instance transform is identity.
//
// Index rebasing: out[i] holds the skinned vertex for original index (baseVertex + minIndex + i),
// and the game's index buffer holds values `idx` that address (baseVertex + idx). So the index
// into our captured array is (baseVertex + idx) - (baseVertex + minIndex) = idx - minIndex.
// ---------------------------------------------------------------------------------------------
// STEP 3 TYPES, ALL IN ONE PLACE.
//
// These were spread across three regions and the declaration order caught me out FOUR times -
// each fix moved one more global earlier. The cause is structural: the capture runs in
// SkinAndBind, the build runs near the draw hook, and the per-frame DrawInstance tick sits in
// the Remix API block far above BOTH - so a type used by all three has exactly one legal home,
// which is here, after SkinnedVertex and before SkinAndBind.
//
// Only raw handles (ApiDoneMesh, the skip list, the reference position) may live earlier,
// because the draw tick needs them and they depend on no type declared below.
// ---------------------------------------------------------------------------------------------
// PRIMITIVE TYPE. SR3 draws its character slots as TRIANGLE STRIPS, and the whole of step 3b
// was built assuming lists. Proof, from the slot dump of 2026-08-31 - the ranges tile the index
// buffer exactly when a slot consumes (prims + 2) indices rather than (prims * 3):
//     slot  0: start   741, 3408 tris ->  741 + 3408 + 2 = 4151 = slot  8's start
//     slot  8: start  4151,  250 tris -> 4151 +  250 + 2 = 4403 = slot  9's start
//     slot  9: start  4403,  628 tris -> 4403 +  628 + 2 = 5033 = slot 10's start
// Reading prims*3 indices per slot therefore ran three times too far, into the NEXT slots' data,
// and then formed triangles from consecutive strip vertices as if they were independent triples.
// That is the holes, and it is why every aggregate looked healthy: the indices were all real,
// all in range, and all distinct.
//
// Hook_DrawIndexedPrimitive is handed the type as its first parameter and it was ignored.
// TINT. SR3 multiplies its diffuse result by Tint_color (c37 in the cloth shaders) - the
// shader model is documented above ShaderInfo: three Diffuse_Color_a/b/c weighted by the
// Pattern_Map channels at gamma 2.2, and then `mul oC0, r1, c37`. The API material was built with
// albedoConstant white, so that multiply was dropped and the copy rendered untinted.
//
// Captured PER SLOT, because the tint is what customisation actually varies - two slots of one
// body legitimately carry different colours, which is why remixapi_MeshInfoSurfaceTriangles has a
// material field per SURFACE rather than one per mesh.
// ALPHA STATE, read from the draw rather than assumed.
//
// Every part was given one hardcoded material - fully opaque, alphaTestType 7 (ALWAYS), one
// roughness - which flattens anything that relies on alpha. The eyes are the visible casualty:
// they use an alpha-tested or alpha-blended overlay, and forcing "always pass" makes the whole
// quad opaque.
//
// SR3's D3DCMPFUNC and Remix's AlphaTestType differ by exactly one, because Remix mirrors
// VkCompareOp: D3DCMP_NEVER=1..D3DCMP_ALWAYS=8 against NEVER=0..ALWAYS=7. So the mapping is
// (func - 1), and alpha test DISABLED means ALWAYS (7) - which is what the old hardcoded value
// happened to be, correct for the body and wrong for everything with a cutout.
struct ApiSlot {
    UINT start, prims;
    INT base;
    D3DPRIMITIVETYPE type;
    float tint[4];
    DWORD alphaTestEnable, alphaFunc, alphaRef;
    DWORD alphaBlendEnable, srcBlend, destBlend;
    // THE TEXTURE BELONGS HERE, not to the part.
    //
    // One albedo was stored per PART, taken from g_curPS.albedoStage on its first draw. Two
    // consequences, and the user saw the second: a shader with no ranked albedo gave null and the
    // part silently fell back to the body's atlas - "its just got the body albedo" - and slots
    // within one part that legitimately use DIFFERENT textures could never both be right.
    // 9 textures dumped for 10 parts was that fallback, visible in the log and not chased.
    //
    // The material is already per SURFACE, so the texture can be too.
    IDirect3DBaseTexture9* albedo;
    // CUSTOMISATION. SR3's clothing and accessories do not have a diffuse texture holding their
    // colour: the colour is three CONSTANTS weighted by the channels of a Pattern_Map, at gamma
    // 2.2, with a selector that sends some texels to a desaturated branch. The full recipe is
    // documented above ShaderInfo, read out of ir_sr3npcclothfull_c.
    //
    // Binding the raw Pattern_Map as albedo is what makes underwear, bracelets and piercings
    // WHITE - a pattern is a mask, not a colour. Three draws on the player rank a Pattern_Map at
    // 80 for exactly this reason.
    bool cloth, clothGenerated;
    float dcMul[4];             // Diffuse_Color (c11)
    int albedoRank;          // 100 = a real Diffuse_Map, 80 = a Pattern_Map, 0 = none
    char bakedPath[MAX_PATH];   // a DDS holding what this slot's PIXEL SHADER computed
    char albedoName[32];        // which sampler was chosen as this slot's base colour
    char firstName[32];         // and the first sampler its shader declares
    IDirect3DBaseTexture9* pattern;
    // HAIR. Its colour is in constants, not in a texture - the Dob_Map is white strands on a
    // green field and the Diffuse_Map is directional data, which is why hair renders white.
    // ir_sr3pchair_c shader[8] ends with  mul r0.xyz, r0, c4  then  mad r0.xyz, r2, c3, r0,
    // so Hair_Spec_Color2 (c4) is the base and Hair_Spec_Color1 (c3) the highlight.
    //
    // That shader is LIT - it samples the L-buffer - so baking its output would burn the game's
    // lighting into the albedo. The constant is taken instead and Remix lights it.
    bool hair;
    // THE DOB MAP ITSELF, not just the colour.
    //
    // Measured from the shipped asset (game-textures\clothes\cf_hair_longasym-01_dob):
    //     R  min 16  max 222   the STRANDS - fine filaments over the hair card
    //     G  min 247 max 248   flat, carries nothing
    //     B  identical to R
    // so it is one greyscale channel duplicated, and the green it appears to be is just G
    // sitting at 247 in an RGB view. The strand detail this project has been missing is in R.
    //
    // The note above is right that BAKING THE SHADER would burn the game's lighting into the
    // albedo, because that shader samples the L-buffer. This does not bake the shader: it
    // multiplies the chosen hair colour by an authored, unlit mask. Different operation, and it
    // keeps Remix doing the lighting.
    IDirect3DBaseTexture9* dob;
    float hairColor[4];
    float dcA[4], dcB[4], dcC[4];
};

// ---------------------------------------------------------------------------------------------
// EVERY PART CAPTURED IN ONE FRAME.
//
// The first version of step 3e captured one buffer per frame - build it, reset, take the next one
// on a later frame. That places each part at the objTM and in the POSE it had on ITS OWN frame, so
// the moment the player moves the parts scatter and each is frozen mid-different-animation. It
// looked like a placement bug and it is really a capture-window bug.
//
// So: one capture frame takes every part of the character, and the builds are drained afterwards,
// one per Present. The build path itself is unchanged - each pending entry is loaded into the
// globals it already reads.
struct ApiPending {
    IDirect3DVertexBuffer9* vb;
    long long firstVertex;
    float objTM[3][4];
    IDirect3DBaseTexture9* albedo;      // this part's OWN texture, not the body's atlas
    std::vector<SkinnedVertex> verts;   // BIND POSE now, not skinned output - Remix skins it
    std::vector<float> weights;         // 4 per vertex
    std::vector<unsigned> bones;        // 4 per vertex
    std::vector<float> uv2;             // 2 per vertex, raw - the PATTERN's coordinates
    unsigned boneCount, boneReg;
    // THE UV SCALE THIS PART NEEDS, captured with its vertices.
    //
    // It cannot be recomputed at build time: builds drain from Present, one per frame, long after
    // the draw that produced them, so g_curVS and g_curPS describe some unrelated draw by then.
    // The scale is a property of the captured draw and has to travel with it.
    float uvScale[2];
    int uvTilingReg[2];        // -1 when no tiling constant matched, for the report
    std::vector<ApiSlot> slots;
    std::vector<unsigned char> ib;
    UINT ibSize;
    bool ib32, ibHave;
};
ApiPending g_apiPending[kMaxApiMeshes];
unsigned g_apiPendingCount = 0, g_apiPendingNext = 0;
bool g_apiCaptureClosed = false;

std::vector<SkinnedVertex> g_apiCapVerts;
// The captured part's own uv scale, and which vertex-shader constants supplied its tiling.
// Defaulted to the bare short scale so a part captured before this existed behaves as it did.
float g_apiCapUvScale[2] = {kShortUVScale, kShortUVScale};
int g_apiCapTilingReg[2] = {-1, -1};
std::vector<unsigned> g_apiCapIndices;
UINT g_apiCapMinIndex = 0;
// Declared here rather than with the rest of the step-3b state: SkinAndBind writes them at
// capture time and it precedes the builder.
IDirect3DVertexBuffer9* g_apiCapVB = nullptr;
unsigned g_apiCapFrame = 0xFFFFFFFFu;
// The ABSOLUTE index of captured vertex 0, i.e. baseVertex + minIndex.
//
// MEASURED 2026-08-31: with 36 of 36 slots collected, every index in range (0 collapsed) and the
// placement correct, roughly half the body's polygons were still wrong. That combination rules out
// an out-of-bounds rebasing and leaves an OFF-BY-A-PER-SLOT-AMOUNT one. Splitting the builder in
// two dropped baseVertex from the collect signature, so every slot was rebased as (idx - minIndex)
// - correct only for slots whose baseVertex equals the captured draw's. The rest landed on VALID
// BUT WRONG vertices, which is exactly "the whole body with half the polygons missing at random"
// and is invisible to a range check.
//
// The correct rebasing is absolute: (slotBaseVertex + idx) - (capBaseVertex + capMinIndex).
long long g_apiCapFirstVertex = 0;
bool g_apiCapHaveVerts = false;
unsigned g_apiCharVertexCount = 0, g_apiCharTriangles = 0;
float g_apiCharAt[3] = {0, 0, 0};
const char* g_apiCharWhyNot = "not attempted yet";

// ---------------------------------------------------------------- the skinning worker pool
// Persistent workers, started on first use and never joined (an ASI has no orderly shutdown; the
// threads are detached and die with the process). One job at a time: the caller publishes the
// job and a chunk size, workers take chunks 1..N-1, the caller does chunk 0 itself, then waits.
// The job must touch only its own vertex range and no globals - the fast path below does.
struct SkinPool {
    std::vector<std::thread> workers;
    std::mutex mu;
    std::condition_variable cvStart, cvDone;
    std::function<void(UINT, UINT)> job;
    UINT chunk = 0, total = 0;
    // The generation and the pending count are atomics so both sides can SPIN on them briefly
    // before sleeping: ~50 jobs a frame through a condition variable cost more in wake latency
    // than the blending itself (loop 4.9 ms on four threads for ~1 ms of arithmetic).
    std::atomic<unsigned> gen{0}, pending{0};
    unsigned count = 0;
    bool started = false;
};
SkinPool g_skinPool;
unsigned g_skinFastDraws = 0, g_skinFastThreaded = 0;
void SkinPoolWorker(unsigned idx) {
    unsigned seen = 0;
    for (;;) {
        // spin for the next job (draws within a frame are microseconds apart), then sleep
        unsigned g = seen;
        for (int spin = 0; spin < 60000; ++spin) {
            g = g_skinPool.gen.load(std::memory_order_acquire);
            if (g != seen) break;
            _mm_pause();
        }
        if (g == seen) {
            std::unique_lock<std::mutex> lk(g_skinPool.mu);
            g_skinPool.cvStart.wait(lk, [&] { return g_skinPool.gen.load(std::memory_order_acquire) != seen; });
            g = g_skinPool.gen.load(std::memory_order_acquire);
        }
        seen = g;
        // the job fields were written before the generation was published (release)
        const std::function<void(UINT, UINT)>& j = g_skinPool.job;
        UINT a = (idx + 1) * g_skinPool.chunk;
        UINT b = a + g_skinPool.chunk;
        if (b > g_skinPool.total) b = g_skinPool.total;
        if (a < b) j(a, b);
        if (g_skinPool.pending.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            std::lock_guard<std::mutex> lk(g_skinPool.mu);
            g_skinPool.cvDone.notify_all();
        }
    }
}
void SkinPoolRun(const std::function<void(UINT, UINT)>& job, UINT total) {
    unsigned want = g_settings.skinThreads > 0 ? static_cast<unsigned>(g_settings.skinThreads) : 0u;
    if (!want) {
        const unsigned hw = std::thread::hardware_concurrency();
        want = hw ? hw / 2 : 1;
        if (want < 1) want = 1;
        if (want > 4) want = 4;
    }
    if (want > 8) want = 8;
    if (!g_skinPool.started) {
        g_skinPool.started = true;
        g_skinPool.count = want;
        for (unsigned i = 0; i + 1 < want; ++i) {
            try {
                std::thread t(SkinPoolWorker, i);
                t.detach();
            } catch (...) { g_skinPool.count = i + 1; break; }
        }
        Log("SKIN FAST PATH: policy gates all off, straight blend; %u thread%s for draws of "
            "2048+ vertices (hardware_concurrency %u)",
            g_skinPool.count, g_skinPool.count == 1 ? "" : "s", std::thread::hardware_concurrency());
    }
    const unsigned n = g_skinPool.count;
    if (n <= 1 || total < 2048) { job(0, total); return; }
    ++g_skinFastThreaded;
    {
        std::lock_guard<std::mutex> lk(g_skinPool.mu);
        g_skinPool.job = job;
        g_skinPool.total = total;
        g_skinPool.chunk = (total + n - 1) / n;
        g_skinPool.pending.store(n - 1, std::memory_order_release);
        g_skinPool.gen.fetch_add(1, std::memory_order_acq_rel);
    }
    g_skinPool.cvStart.notify_all();   // for any worker that went to sleep
    job(0, g_skinPool.chunk < total ? g_skinPool.chunk : total);
    for (int spin = 0; spin < 200000; ++spin) {
        if (g_skinPool.pending.load(std::memory_order_acquire) == 0) return;
        _mm_pause();
    }
    std::unique_lock<std::mutex> lk(g_skinPool.mu);
    g_skinPool.cvDone.wait(lk, [] { return g_skinPool.pending.load(std::memory_order_acquire) == 0; });
}


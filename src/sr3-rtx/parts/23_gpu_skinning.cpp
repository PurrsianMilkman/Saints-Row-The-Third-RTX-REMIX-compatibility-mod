// parts/23_gpu_skinning.cpp - GPU (fixed-function) skinning, incl. namespace sm3.
// NOT a standalone translation unit: #included, in order, by ../sr3rtx.cpp inside its anonymous
// namespace. Originally sr3rtx.cpp lines 18121-19985 (old file:line citations map here).
// ---------------------------------------------------------------- GPU (fixed-function) skinning
//
// skinViaFixedFunction, OFF by default. The alternative to CPU skinning above: instead of
// blending every vertex on the CPU into a ring buffer, leave the game's own vertex buffer bound
// exactly as it is and let the FIXED-FUNCTION PIPELINE do the blend, through D3D9's indexed
// vertex blending (D3DRS_VERTEXBLEND / D3DRS_INDEXEDVERTEXBLENDENABLE). CPU skinning is 88% of
// this shim's frame cost (see the PROFILE line's "skin" phase), and it also means Remix hashes
// the ALREADY-POSED geometry every frame - a moving hash, which is what the compatibility wiki
// means by "software animation will flicker". Hardware skinning keeps Remix's geometry hash on
// the BIND POSE (the hash is computed before vertex capture replaces the input, which is "should
// be stable" in the same wiki entry) - so this is a plausible fix for the character stutter
// class as well as a large performance win.
//
// WHY THIS WAS THOUGHT CLOSED, AND WHY IT ISN'T: dxvk-remix's own D3DCAPS9 report gives
// MaxVertexBlendMatrixIndex = 8 (see the Log call beside g_uvStream's own device-caps init,
// above HookDevice's closing brace), which reads as "at most 9 bone matrices" and closed this
// route the first time it was considered. That cap describes something else: Remix's fixed-
// function skinning path stages `maxBone + 1` matrices starting at D3DTS_WORLDMATRIX(0),
// tracking the highest world-matrix index the game has actually SET (defaulting to 255), and
// both its SetTransform handler and the 32-to-64 bridge validate transform-state indices up to
// WORLDMATRIX(255). A 58-bone palette (this shim tracks up to kBonesMax=64) fits inside that
// with room to spare; the reported cap never gated it.
//
// THE APPROACH: leave stream 0 (the game's own bind-pose buffer) bound untouched, add a second
// stream carrying NORMALISED float weights and remapped byte indices (built once per source
// vertex buffer, below), stage the bones this mesh actually uses as world matrices with the
// object transform folded in (see SkinViaFixedFunction's own comment for the multiplication
// order and how it was determined), and enable D3DVBF_3WEIGHTS indexed blending for the draw.
// Refuses - loudly, with a counted reason - for anything this first version does not attempt:
// cloth, morph, a declaration this decoder cannot read, a bone the shader's own palette register
// cannot reach. CPU skinning is the fallback for every one of those, unchanged.

// The device stream the weight/index side buffer binds to, and whether there was room for it
// alongside the uv stream. Set once in HookDevice, from D3DCAPS9.MaxStreams, exactly like
// g_uvStream itself.
DWORD g_skinFFStream = kSkinFFStreamDefault;
bool g_skinFFStreamsOk = false;

// ---- the weight/index side stream ----
//
// One converted buffer per SOURCE VERTEX BUFFER, covering every vertex it holds - not windowed
// to one draw's [firstVertex, firstVertex+numVertices) range the way the bind-pose cache
// (BaseMesh, above) is. That is deliberate: a device stream is addressed as
// (streamOffset + (baseVertex+rawIndex)*ourStride), the SAME baseVertex D3D9 applies to every
// bound stream including stream 0 - so entry i of this buffer must BE source vertex i for that
// arithmetic to land on the right vertex regardless of which window a later draw asks for.
// Windowing it per draw, the way the CPU skin RING does, only works there because the ring
// controls its own write position and can place each mesh's data at or past
// (baseVertex+minIndex)*stride (see SkinAndBind's "vertex 0 must land on index minIndex" note,
// above); a long-lived buffer keyed by draw range the same way BaseMesh is would need a
// DIFFERENT stream offset for every draw that shares the source buffer, and a stream offset is
// one number for the whole draw call. Same mechanism and caching discipline as the UV side
// stream (g_uvStream, UvBufferFor, UvKey) otherwise: one buffer per source range (here, the
// whole buffer, like UvBufferFor's own STATIC-source branch), cached, and invalidated the moment
// the game writes the source again - DrainPendingInvalidations calls InvalidateSkinFFBuffers
// exactly where it already calls InvalidateUvBuffers/InvalidateBaseMeshes.
//
// SR3's skinned source buffers are STATIC (measured 2026-08-18: WRITEONLY, no DYNAMIC bit - see
// the CPU skinning section's own comment, above), so there is no dynamic-ring case to support
// here: GetBaseMesh already refuses a DYNAMIC source outright, and a hardware-skinned draw whose
// bind pose GetBaseMesh could not decode never reaches this function at all.
struct SkinFFBuffer {
    IDirect3DVertexBuffer9* src = nullptr;   // referenced: the key is its address
    IDirect3DVertexBuffer9* buf = nullptr;   // BLENDWEIGHT(float3) + BLENDINDICES(ubyte4), one entry per source vertex
    UINT vertexCount = 0;
    // The source stride this was decoded with, checked on every cache hit - GetBaseMesh's own
    // hasUv2 comment explains why a shared buffer can be touched by more than one declaration
    // across the draws that use it (typically a reduced prepass declaration first); a stride
    // mismatch here means exactly that has happened, and the entry is rebuilt rather than trusted.
    UINT stride = 0;
    // FIX 2 (skinFFHonourStreamOffset): the byte g_stream0Offset was split into when this entry
    // was decoded - phase = offset % stride, folded into the decode below; base = offset / stride
    // is NOT part of this, on purpose, because it does not change which bytes get decoded, only
    // where a draw BINDS this entry (SkinFFBufferFor's own out-param, g_skinFFBindOffset, carries
    // base instead) - so two draws into the same pooled buffer at different whole-vertex offsets
    // but the SAME phase correctly share one decoded entry here.
    UINT phase = 0;
    // FIX 5 (skinFFFollowShaderInfluences): whether THIS entry was decoded in single-bone mode
    // (weight [1,0,0], index 0 only) rather than the full four-influence decode. A declaration
    // without BLENDWEIGHT is always single (see SkinFFBufferFor's own rigidSingleBone), but a
    // declaration WITH real weight data can still be asked for in single mode by a shader that
    // ignores BLENDWEIGHT entirely - a different decode of the SAME source buffer, so it needs
    // its own cache entry rather than overwriting the four-bone one.
    bool single = false;
    bool ok = false;
    const char* refuseWhy = nullptr;
};
// One VB can now hold several entries - one per distinct (stride, phase) this shim has actually
// seen bound at stream 0, small in practice (a handful of declarations/offsets per buffer, not
// unbounded). See SkinFFBufferFor's own comment on why phase, not the whole offset, is the key.
std::unordered_map<IDirect3DVertexBuffer9*, std::vector<SkinFFBuffer>> g_skinFFBuffers;
constexpr UINT kSkinFFBytesPerVertex = 16;   // float3 weight (12) + ubyte4 index (4)
unsigned g_skinFFBuildOk = 0, g_skinFFBuildFail = 0;
unsigned g_skinFFBuildFailLayout = 0, g_skinFFBuildFailDesc = 0, g_skinFFBuildFailCreate = 0,
        g_skinFFBuildFailLock = 0;
// FIX 2's out-param, exactly like g_uvBindOffset: the stream offset (base*kSkinFFBytesPerVertex)
// this call's side buffer must be BOUND at, read immediately by the caller before anything else
// can overwrite it.
UINT g_skinFFBindOffset = 0;
// Measured whether or not the switch is on - the denominator that says how often the bug this
// fix targets could even fire. Incremented once per FF-SKINNED draw (SkinViaFixedFunction's own
// success path), not per SkinFFBufferFor call, so it matches "FF-skinned draws" exactly.
unsigned g_skinFFNonZeroOffset = 0, g_skinFFNonZeroOffsetPhase = 0;
// FIX 1's counter: a reused-palette draw (the sameStage branch, below) that needed WORLDMATRIX(0)
// restaged because its mesh uses bone 0.
unsigned g_skinFFBone0Restaged = 0;

void InvalidateSkinFFBuffers(IDirect3DVertexBuffer9* vb) {
    const auto it = g_skinFFBuffers.find(vb);
    if (it == g_skinFFBuffers.end()) return;
    // FIX 2: release EVERY (stride, phase) entry this buffer holds, not only one - the map now
    // holds a small vector per VB instead of a single entry.
    for (auto& e : it->second) {
        if (e.src) e.src->Release();
        if (e.buf) e.buf->Release();
    }
    g_skinFFBuffers.erase(it);
}

// Builds (or returns the cached) side buffer for g_stream0 as this draw has it bound. Only ever
// called after GetBaseMesh has already decoded a bind pose from the SAME g_curLayout (see
// SkinViaFixedFunction below), so the weight/index element types are already known good for this
// draw - the gate here is repeated anyway because this cache is keyed on the buffer alone (to
// get the whole-buffer addressing described above) rather than on the declaration, so it has to
// stand on its own.
IDirect3DVertexBuffer9* SkinFFBufferFor(IDirect3DDevice9* dev, bool wantSingle) {
    g_skinFFBindOffset = 0;
    if (!g_stream0 || g_stream0Stride == 0) { ++g_skinFFBuildFailLayout; ++g_skinFFBuildFail; return nullptr; }

    // FIX 2 (skinFFHonourStreamOffset): D3D9 addresses stream 0 at g_stream0Offset + n*stride for
    // vertex n (the SAME rule for every bound stream, including this side one) - but this buffer
    // is decoded WHOLE, once, entry j from byte j*stride, which only equals vertex j's bytes when
    // g_stream0Offset is 0. Split the offset instead: phase = offset % stride shifts where the
    // decode STARTS (folded into every read below, and into the cache lookup just below); base =
    // offset / stride does not change the decode at all, only where the draw must BIND this
    // buffer (entry j then holds vertex (j - base) - see the struct's own comment on why base is
    // not part of the key), returned through g_skinFFBindOffset for the caller to bind at. Off:
    // phase = base = 0, byte-identical to the old whole-buffer, offset-blind addressing.
    const UINT phase = g_settings.skinFFHonourStreamOffset ? (g_stream0Offset % g_stream0Stride) : 0;
    const UINT base = g_settings.skinFFHonourStreamOffset ? (g_stream0Offset / g_stream0Stride) : 0;
    g_skinFFBindOffset = base * kSkinFFBytesPerVertex;

    // FIX 5 (skinFFFollowShaderInfluences): a declaration without BLENDWEIGHT is always
    // single-bone (GetBaseMesh's own rigidSingleBone, recomputed below from g_curLayout directly
    // since the `L` alias is not in scope yet) regardless of what the caller asked for - there is
    // no per-vertex weight data to decode a four-bone buffer from. See SkinViaFixedFunction's own
    // singleBone comment for why the caller asks in the first place.
    const bool single = (g_settings.skinRigidSingleBone && g_curLayout.blendWeightType < 0) ||
                        (g_settings.skinFFFollowShaderInfluences && wantSingle);

    std::vector<SkinFFBuffer>& entries = g_skinFFBuffers[g_stream0];
    for (const auto& e : entries)
        if (e.stride == g_stream0Stride && e.phase == phase && e.single == single)
            return e.ok ? e.buf : nullptr;

    const VertexLayout& L = g_curLayout;
    const int stride = static_cast<int>(g_stream0Stride);
    auto fits = [&](int off, int type) {
        const int size = DeclTypeSize(type);
        return size > 0 && off >= 0 && off + size <= stride;
    };
    // Mirrors GetBaseMesh's own gate exactly, rigidSingleBone special case included (a
    // declaration that carries BLENDINDICES and no BLENDWEIGHT at all is rigid single-bone
    // attachment when the setting is on - see GetBaseMesh's own long comment on it, above).
    const bool rigidSingleBone = g_settings.skinRigidSingleBone && (L.blendWeightType < 0);
    const bool weightOk = rigidSingleBone ||
        ((L.blendWeightType == D3DDECLTYPE_UBYTE4 || L.blendWeightType == D3DDECLTYPE_UBYTE4N ||
          L.blendWeightType == D3DDECLTYPE_D3DCOLOR || L.blendWeightType == D3DDECLTYPE_FLOAT4) &&
         fits(L.blendWeightOffset, L.blendWeightType));
    const bool indexOk = (L.blendIndexType == D3DDECLTYPE_UBYTE4 || L.blendIndexType == D3DDECLTYPE_D3DCOLOR) &&
                         fits(L.blendIndexOffset, L.blendIndexType);
    SkinFFBuffer entry;
    entry.stride = g_stream0Stride;
    entry.phase = phase;
    entry.single = single;
    if (!weightOk || !indexOk) {
        ++g_skinFFBuildFailLayout; ++g_skinFFBuildFail;
        entry.src = g_stream0; entry.src->AddRef();
        entry.ok = false; entry.refuseWhy = "declaration does not carry weights/indices this decoder reads";
        entries.push_back(entry);
        return nullptr;
    }

    D3DVERTEXBUFFER_DESC vbd{};
    if (FAILED(g_stream0->GetDesc(&vbd)) || (vbd.Usage & D3DUSAGE_DYNAMIC) ||
        vbd.Size < phase + static_cast<UINT>(stride)) {
        ++g_skinFFBuildFailDesc; ++g_skinFFBuildFail;
        entry.src = g_stream0; entry.src->AddRef();
        entry.ok = false; entry.refuseWhy = "GetDesc failed, the source is DYNAMIC, or the stream-0 offset leaves no whole vertex";
        entries.push_back(entry);
        return nullptr;
    }
    const UINT vertexCount = (vbd.Size - phase) / static_cast<UINT>(stride);

    void* srcMapped = nullptr;
    if (FAILED(g_stream0->Lock(0, 0, &srcMapped, D3DLOCK_READONLY | D3DLOCK_NOSYSLOCK)) || !srcMapped) {
        ++g_skinFFBuildFailLock; ++g_skinFFBuildFail;
        entry.src = g_stream0; entry.src->AddRef();
        entry.ok = false; entry.refuseWhy = "read-lock of the source buffer failed";
        entries.push_back(entry);
        return nullptr;
    }

    g_internal = true;
    g_internalThread = GetCurrentThreadId();
    IDirect3DVertexBuffer9* buf = nullptr;
    const UINT bytes = vertexCount * kSkinFFBytesPerVertex;
    const bool created = bytes > 0 &&
        SUCCEEDED(dev->CreateVertexBuffer(bytes, D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &buf, nullptr)) &&
        buf;
    if (!created) {
        g_internal = false;
        g_stream0->Unlock();
        ++g_skinFFBuildFailCreate; ++g_skinFFBuildFail;
        entry.src = g_stream0; entry.src->AddRef();
        entry.ok = false; entry.refuseWhy = "CreateVertexBuffer failed for the side stream";
        entries.push_back(entry);
        return nullptr;
    }
    void* dstMapped = nullptr;
    if (FAILED(buf->Lock(0, 0, &dstMapped, D3DLOCK_DISCARD)) || !dstMapped) {
        buf->Release();
        g_internal = false;
        g_stream0->Unlock();
        ++g_skinFFBuildFailLock; ++g_skinFFBuildFail;
        entry.src = g_stream0; entry.src->AddRef();
        entry.ok = false; entry.refuseWhy = "lock of the side stream failed";
        entries.push_back(entry);
        return nullptr;
    }

    const unsigned char* src8 = static_cast<const unsigned char*>(srcMapped);
    unsigned char* dst8 = static_cast<unsigned char*>(dstMapped);
    for (UINT i = 0; i < vertexCount; ++i) {
        const unsigned char* v = src8 + phase + static_cast<size_t>(i) * stride;
        // Exactly GetBaseMesh's own weight/index decode (UBYTE4/UBYTE4N/D3DCOLOR/FLOAT4 weights,
        // the BGRA reorder for a D3DCOLOR-typed field, rigidSingleBone's implicit single full
        // weight) - reproduced rather than shared because GetBaseMesh writes into a BaseVertex
        // windowed to one draw and this needs the whole buffer once, on a different cache.
        float w[4];
        if (single) {
            w[0] = 1.0f; w[1] = w[2] = w[3] = 0.0f;
        } else {
            const unsigned char* wb = v + L.blendWeightOffset;
            if (L.blendWeightType == D3DDECLTYPE_FLOAT4) {
                const float* wf = reinterpret_cast<const float*>(wb);
                for (int k = 0; k < 4; ++k) w[k] = std::isfinite(wf[k]) ? wf[k] : 0.0f;
            } else if (L.blendWeightType == D3DDECLTYPE_D3DCOLOR) {
                w[0] = static_cast<float>(wb[2]); w[1] = static_cast<float>(wb[1]);
                w[2] = static_cast<float>(wb[0]); w[3] = static_cast<float>(wb[3]);
            } else {
                for (int k = 0; k < 4; ++k) w[k] = static_cast<float>(wb[k]);
            }
        }
        unsigned idxRaw[4];
        const unsigned char* ib = v + L.blendIndexOffset;
        if (L.blendIndexType == D3DDECLTYPE_D3DCOLOR) {
            idxRaw[0] = ib[2]; idxRaw[1] = ib[1]; idxRaw[2] = ib[0]; idxRaw[3] = ib[3];
        } else {
            for (int k = 0; k < 4; ++k) idxRaw[k] = ib[k];
        }
        // BLENDWEIGHT is UBYTE4, not UBYTE4N - the raw bytes sum to ~254-255, not 1.0, exactly the
        // CPU blend's own comment above (SkinAndBind divides its accumulated matrix by the sum of
        // the surviving weights). Fixed-function indexed blending has no such divide, so the
        // normalising has to happen here, once, rather than every frame in a shader. An index at
        // or beyond kBonesMax is not a bone this shim's palette shadow can stage at all (the
        // shadow is exactly kBonesMax*kRegsPerBone registers) - drop that ONE influence from the
        // sum, the same as the CPU blend's own `if (bone >= safeBones) continue`, rather than let
        // it corrupt the whole vertex.
        float sum = 0.0f;
        for (int k = 0; k < 4; ++k) {
            if (idxRaw[k] >= static_cast<unsigned>(kBonesMax)) w[k] = 0.0f;
            sum += w[k];
        }
        float wn[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        if (sum > 0.0f) {
            const float inv = 1.0f / sum;
            for (int k = 0; k < 4; ++k) wn[k] = w[k] * inv;
        }
        // Index 255 is this format's "no influence" sentinel, always paired with weight 0 (see
        // BaseVertex's own comment, above). Left as 255 it addresses a matrix nothing ever
        // stages, and Remix would try to stage a 256th WORLDMATRIX to cover it. Remapped to 0 -
        // ANY zero-weight slot is remapped to 0, not only a literal 255 - it addresses
        // WORLDMATRIX(0), which is always finite (it is D3DTS_WORLD, already set to this draw's
        // object transform by BeginFFP's own ApplyTransforms before this ever runs) and always
        // contributes exactly zero at zero weight, whether or not bone 0 is really among this
        // mesh's used bones.
        unsigned char idxOut[4];
        for (int k = 0; k < 4; ++k)
            idxOut[k] = (wn[k] == 0.0f) ? 0 : static_cast<unsigned char>(idxRaw[k]);

        float* wDst = reinterpret_cast<float*>(dst8 + static_cast<size_t>(i) * kSkinFFBytesPerVertex);
        wDst[0] = wn[0]; wDst[1] = wn[1]; wDst[2] = wn[2];   // 4th weight is implicit: 1-w0-w1-w2
        unsigned char* iDst = dst8 + static_cast<size_t>(i) * kSkinFFBytesPerVertex + 12;
        iDst[0] = idxOut[0]; iDst[1] = idxOut[1]; iDst[2] = idxOut[2]; iDst[3] = idxOut[3];
    }

    buf->Unlock();
    g_stream0->Unlock();
    g_internal = false;

    entry.src = g_stream0; entry.src->AddRef();
    entry.buf = buf;
    entry.vertexCount = vertexCount;
    entry.ok = true;
    entries.push_back(entry);
    ++g_skinFFBuildOk;
    return buf;
}

// ---- the declaration ----
std::unordered_map<IDirect3DVertexDeclaration9*, IDirect3DVertexDeclaration9*> g_skinFFDecls;
unsigned g_skinFFDeclsMade = 0, g_skinFFDeclFailures = 0;
unsigned g_skinFFDeclFailLogged = 0;   // FIX 4: caps the one-time failure dump, below

// A clone of the game's own skinned declaration with THREE elements retargeted, following
// FloatUVDeclaration's own cloning approach exactly (GetDeclaration into a fixed array, retarget
// matching elements IN PLACE, then CreateVertexDeclaration) rather than building one from
// scratch: TEXCOORD0, when it is SHORT2, onto the existing float2 uv stream - Remix still
// discards that format, the same reason FloatUVDeclaration exists for every other converted draw
// - and BLENDWEIGHT/BLENDINDICES onto the new side stream above, as a normalised FLOAT3 and a
// remapped UBYTE4. Retargeted IN PLACE, not appended: the source declaration already carries a
// usable BLENDWEIGHT/BLENDINDICES pair (GetBaseMesh already required one to decode this mesh at
// all, before this is ever called), and appending a second pair would leave two elements
// claiming the same usage, which CreateVertexDeclaration correctly refuses - the exact pitfall
// FloatUVDeclaration's own set-1 TEXCOORD0 displacement comment describes for TEXCOORD.
IDirect3DVertexDeclaration9* SkinFFDeclarationFor(IDirect3DDevice9* dev,
                                                  IDirect3DVertexDeclaration9* src) {
    if (!src) return nullptr;
    const auto it = g_skinFFDecls.find(src);
    if (it != g_skinFFDecls.end()) return it->second;

    D3DVERTEXELEMENT9 elems[64]{};
    UINT count = 0;
    IDirect3DVertexDeclaration9* clone = nullptr;
    bool sawWeight = false, sawIndex = false;
    if (SUCCEEDED(src->GetDeclaration(elems, &count)) && count > 0) {
        for (UINT i = 0; i < count; ++i) {
            if (elems[i].Stream == 0xFF) break;
            if (elems[i].Usage == D3DDECLUSAGE_TEXCOORD && elems[i].UsageIndex == 0 &&
                elems[i].Type == D3DDECLTYPE_SHORT2) {
                elems[i].Stream = static_cast<WORD>(g_uvStream);
                elems[i].Offset = 0;
                elems[i].Type = D3DDECLTYPE_FLOAT2;
            } else if (elems[i].Usage == D3DDECLUSAGE_BLENDWEIGHT && elems[i].UsageIndex == 0) {
                elems[i].Stream = static_cast<WORD>(g_skinFFStream);
                elems[i].Offset = 0;
                elems[i].Type = D3DDECLTYPE_FLOAT3;
                sawWeight = true;
            } else if (elems[i].Usage == D3DDECLUSAGE_BLENDINDICES && elems[i].UsageIndex == 0) {
                elems[i].Stream = static_cast<WORD>(g_skinFFStream);
                elems[i].Offset = 12;
                elems[i].Type = D3DDECLTYPE_UBYTE4;
                sawIndex = true;
            }
        }
        // FIX 4 (skinFFRigidDecl): a rigid single-bone declaration carries BLENDINDICES and
        // legitimately NO BLENDWEIGHT at all (SkinFFBufferFor's own rigidSingleBone branch already
        // writes an implicit [1,0,0] weight for these) - but this loop only RETARGETS an existing
        // BLENDWEIGHT element, so a declaration with none never set sawWeight and every rigid mesh
        // fell back to CPU skinning. Append the missing element instead of retargeting one that
        // was never there: find the D3DDECL_END sentinel the loop above stopped at (elems[i] with
        // Stream==0xFF) and use its slot, bounds-checked against the fixed 64-element array.
        if (!sawWeight && sawIndex && g_settings.skinFFRigidDecl && g_settings.skinRigidSingleBone) {
            UINT endIdx = count;
            for (UINT i = 0; i < count; ++i) if (elems[i].Stream == 0xFF) { endIdx = i; break; }
            if (endIdx + 1 < 64) {
                D3DVERTEXELEMENT9 w{};
                w.Stream = static_cast<WORD>(g_skinFFStream);
                w.Offset = 0;
                w.Type = D3DDECLTYPE_FLOAT3;
                w.Method = D3DDECLMETHOD_DEFAULT;
                w.Usage = D3DDECLUSAGE_BLENDWEIGHT;
                w.UsageIndex = 0;
                const D3DVERTEXELEMENT9 endMarker = D3DDECL_END();
                elems[endIdx] = w;
                elems[endIdx + 1] = endMarker;
                sawWeight = true;
                count = endIdx + 2;
            }
        }
        if (sawWeight && sawIndex) {
            if (FAILED(dev->CreateVertexDeclaration(elems, &clone))) clone = nullptr;
        }
    }
    if (!clone) {
        ++g_skinFFDeclFailures;
        // Log EACH failed clone once (this only ever runs the first time a distinct source
        // declaration is seen, because the cache check above returns early on every later draw
        // that reuses it) with its reason and its full element list, reusing the same decl-element
        // Log format Hook_SetVertexDeclaration already uses for the skinned-declaration dump.
        if (g_skinFFDeclFailLogged < 32) {
            ++g_skinFFDeclFailLogged;
            const char* why = (count == 0) ? "GetDeclaration failed or returned no elements"
                             : (!sawIndex) ? "no BLENDINDICES element"
                             : (!sawWeight) ? "no BLENDWEIGHT element (and skinFFRigidDecl/skinRigidSingleBone did not supply one)"
                             : "CreateVertexDeclaration failed";
            Log("SKIN FF DECL CLONE FAILED (#%u): %s", g_skinFFDeclFailLogged, why);
            for (UINT i = 0; i < count; ++i) {
                if (elems[i].Stream == 0xFF) break;
                Log("    stream=%u offset=%2u type=%-8s(%u) usage=%-12s index=%u",
                    elems[i].Stream, elems[i].Offset, DeclTypeName(elems[i].Type),
                    elems[i].Type, UsageName(elems[i].Usage), elems[i].UsageIndex);
            }
        }
    } else ++g_skinFFDeclsMade;
    src->AddRef();
    g_skinFFDecls[src] = clone;
    return clone;
}

// ---- staging and the draw itself ----
unsigned g_skinFFConverted = 0;          // draws that took the hardware path
unsigned g_skinFFVertsConverted = 0;     // vertices skinned on the GPU rather than the CPU
unsigned g_skinFFTransforms = 0;         // SetTransform(WORLDMATRIX) calls this cost, total
unsigned g_skinFFPaletteReused = 0;      // draws whose staged palette matched the previous draw's - restaging skipped
unsigned g_skinFFSingleBoneDraws = 0;    // FIX 5: hardware-skinned draws whose shader reads no BLENDWEIGHT (staged from idx[0] alone, not mesh->usedBones)

unsigned g_skinFFRefuseNoStreams = 0;    // device has no free stream slot for the side buffers
unsigned g_skinFFRefuseCloth = 0;        // this draw uses the cloth remap - not attempted in this first version
unsigned g_skinFFRefuseMorph = 0;        // this draw applies a morph delta - not attempted in this first version
unsigned g_skinFFRefuseNoMesh = 0;       // GetBaseMesh could not decode a bind pose (dynamic source, bad declaration, wrong vertex count...)
unsigned g_skinFFRefuseUsedBones = 0;    // mesh uses no bones at all - nothing to stage
unsigned g_skinFFRefuseBoneRange = 0;    // a used bone sits beyond what this shader's own palette register can stage safely
unsigned g_skinFFRefuseDecl = 0;         // the retargeted declaration failed to build
unsigned g_skinFFRefuseSideBuffer = 0;   // the weight/index or uv side-stream conversion failed
unsigned g_skinFFRefuseNoBoneDecl = 0;   // FIX 5: vertex shader reads no BLENDINDICES - the CPU path places it by objTM alone
unsigned g_skinFFRefuseSingleBoneRange = 0; // FIX 5: single-bone mode, a vertex's blend index is beyond what the shader's register can stage safely
unsigned g_skinFFRefuseNoInfluence = 0;  // FIX 5: four-bone mode, a vertex has no usable influence - the CPU path leaves it at its bind pose
const char* g_skinFFRefuseWhy = nullptr;

// Which mesh/objTM/palette-epoch the currently staged WORLDMATRIX(0..63) set belongs to, so a
// run of draws sharing one character (several parts, same skeleton, same frame) restages nothing
// after the first. Keyed on SkinMeshKey's own hash rather than the BaseMesh pointer on purpose -
// a pointer could in principle be handed back to a NEW entry after an invalidation+rebuild (the
// same address-reuse class of bug this file has hit before, see BaseMesh::owner's own comment),
// and a value key does not have that failure mode. Conservative beyond that: any of the four
// fields changing forces a full restage, even when the actual register VALUES that changed do
// not touch this particular mesh's bones - a false restage costs bridge calls, a false skip
// renders stale bones, and only one of those is a correctness bug.
unsigned long long g_skinFFStageMeshKey = 0;
unsigned g_skinFFStageFrame = 0xFFFFFFFFu;
unsigned g_skinFFStageBoneDraw = 0xFFFFFFFFu;
D3DMATRIX g_skinFFStageObjTM = kIdentity;
bool g_skinFFStageValid = false;

void UnbindSkinFF(IDirect3DDevice9* dev) {
    g_internal = true;
    g_internalThread = GetCurrentThreadId();
    g_origSetStreamSource(dev, g_skinFFStream, nullptr, 0, 0);
    g_origSetStreamSource(dev, g_uvStream, nullptr, 0, 0);
    // THE SHAPE BAKE: undo the stream-0 rebind SkinViaFixedFunction made when it bound a baked
    // copy in place of the game's own bind-pose buffer. g_internal is already set for this whole
    // function (see below).
    if (g_skinFFBakeBound) {
        g_origSetStreamSource(dev, 0, g_stream0, g_stream0Offset, g_stream0Stride);
        g_skinFFBakeBound = false;
    }
    g_origSetVertexDeclaration(dev, g_curDecl);
    ShadowSetRS(dev, D3DRS_INDEXEDVERTEXBLENDENABLE, FALSE);
    ShadowSetRS(dev, D3DRS_VERTEXBLEND, D3DVBF_DISABLE);
    // WORLDMATRIX(0) IS D3DTS_WORLD - staging bone 0 (when this mesh uses it) overwrote it with
    // Bone[0]*objTM, and ApplyTransforms' own shadow (g_appliedWorld) was never told, because
    // staging bypasses ApplyTransforms on purpose (see SkinViaFixedFunction's own comment on
    // why). Left alone, the NEXT draw's ApplyTransforms could see its own world matrix already
    // matches the shadow and skip calling SetTransform - while the device actually still held
    // the bone-composed matrix from this draw. Restoring it here, unconditionally, keeps the
    // shadow truthful without needing to track whether bone 0 was actually touched this time.
    g_origSetTransform(dev, D3DTS_WORLD, &g_appliedWorld);
    g_internal = false;
}

// Attempts the hardware path for a draw that would otherwise go to SkinAndBind. Returns false -
// with g_skinFFRefuseWhy set and the matching counter above already incremented - for anything
// not right, so the caller's existing CPU fallback runs exactly as it does today.
bool SkinViaFixedFunction(IDirect3DDevice9* dev, INT baseVertex, UINT minIndex, UINT numVertices) {
    g_skinFFRefuseWhy = nullptr;
    // FIX 5: see the Settings::skinFFFollowShaderInfluences comment, and SkinAndBind's own
    // "reads"/"influences" gate (above the fast path) that this mirrors exactly.
    const bool followInfl = g_settings.skinFFFollowShaderInfluences;
    const bool singleBone = followInfl && !g_curVS.usesBlendWeights;
    if (!g_skinFFStreamsOk) {
        g_skinFFRefuseWhy = "device has no free stream slot for the side buffers";
        ++g_skinFFRefuseNoStreams;
        return false;
    }
    // FIX 5, step 1: a vertex shader that reads no BLENDINDICES is not skinned by the game at
    // all - SkinAndBind leaves it at its bind pose, placed by objTM alone. The GPU path has no
    // "stage nothing" state (WORLDMATRIX(0) is always something), so refuse outright; the caller
    // already falls back to SkinAndBind for any refusal from this function.
    if (followInfl && g_settings.skinRequireBoneDecl && !g_curVS.usesBlendIndices) {
        g_skinFFRefuseWhy = "vertex shader reads no BLENDINDICES - the CPU path places it by objTM alone";
        ++g_skinFFRefuseNoBoneDecl;
        return false;
    }
    if (numVertices == 0) { g_skinFFRefuseWhy = "no vertices"; ++g_skinFFRefuseNoMesh; return false; }
    const INT first = baseVertex + static_cast<INT>(minIndex);
    if (first < 0) {
        g_skinFFRefuseWhy = "baseVertex + minIndex is negative";
        ++g_skinFFRefuseNoMesh;
        return false;
    }
    const UINT firstVertex = static_cast<UINT>(first);

    // Cloth adds seam-duplicate vertices with a per-tile UV shift (g_clothRemap); morph adds a
    // per-character delta read from a snooped dynamic buffer (MorphForThisDraw). Both modify the
    // mesh per-draw in ways this first version does not reproduce on the GPU - refuse and let the
    // proven CPU path (which already handles both) render this one, exactly as today.
    if (g_clothRemap) { g_skinFFRefuseWhy = "cloth remap active"; ++g_skinFFRefuseCloth; return false; }
    const MorphSource morph = MorphForThisDraw(firstVertex, numVertices);
    // THE SHAPE BAKE: with morphBake on, a morph-active draw is no longer an automatic refusal -
    // MorphBakeFor either returns a whole-buffer copy of the source mesh with this draw's delta
    // already applied, or refuses with a counted reason (falls through to the same refusal as
    // before). Obtained HERE, at the morph-check position, exactly where the plan requires: a
    // LATER refusal in this function (bone range, declaration, side buffer...) simply abandons
    // the bakedVb pointer - the cache still owns it via its own src/morphVb references, so
    // nothing leaks and no counter/binding is left inconsistent.
    IDirect3DVertexBuffer9* bakedVb = nullptr;
    if (morph.active) {
        if (!g_settings.morphBake) {
            g_skinFFRefuseWhy = "morph delta active";
            ++g_skinFFRefuseMorph;
            return false;
        }
        bakedVb = MorphBakeFor(dev, firstVertex, numVertices, morph);
        if (!bakedVb) {
            g_skinFFRefuseWhy = g_morphBakeRefuseWhy ? g_morphBakeRefuseWhy : "morph bake failed";
            ++g_skinFFRefuseMorph;
            return false;
        }
    }

    const BaseMesh* mesh = GetBaseMesh(firstVertex, numVertices);
    if (!mesh || mesh->verts.size() != numVertices) {
        g_skinFFRefuseWhy = g_skinRefuseWhy ? g_skinRefuseWhy : "no usable bind pose";
        ++g_skinFFRefuseNoMesh;
        return false;
    }
    if (!mesh->usedBones) {
        g_skinFFRefuseWhy = "mesh uses no bones - nothing to stage";
        ++g_skinFFRefuseUsedBones;
        return false;
    }

    // Exactly SkinAndBind's own boneBase/safeBones (above): the register this shader's own CTAB
    // declares for Bone_weights, or c52 if it declared none, and how many bones actually fit
    // between there and the end of the shadow without reading past it.
    const UINT boneBase = (g_curVS.boneReg >= 0) ? static_cast<UINT>(g_curVS.boneReg) : kRegBonePalette;
    const unsigned safeBones =
        (boneBase + static_cast<unsigned>(kBonesMax) * kRegsPerBone <= kMaxVsConst)
            ? static_cast<unsigned>(kBonesMax)
            : (kMaxVsConst > boneBase ? (kMaxVsConst - boneBase) / kRegsPerBone : 0u);

    // FIX 5, step 2/3: which bones this DRAW actually needs to stage, and whether it can be
    // expressed on the GPU at all - NOT mesh->usedBones/maxBone, which GetBaseMesh built from the
    // declaration's weight bytes and which the shader may not agree with.
    unsigned long long drawUsedBones = mesh->usedBones;
    unsigned drawMaxBone = mesh->maxBone;
    if (singleBone) {
        // A shader with no BLENDWEIGHT input uses exactly bone idx[0] at implicit weight 1,
        // whatever the declaration's other three weight bytes hold (SkinAndBind's own "A shader
        // that declares no blendweight input uses exactly ONE bone" comment) - so the bones to
        // stage are a mask over idx[0] alone, not the whole mesh and not the declaration's other
        // three slots.
        //
        // FIX 5 CACHE (2026-09-22): mesh->singleBoneMask/singleBoneMaxIdx0/singleBoneIdx0Overflow
        // are the raw, shader-independent facts GetBaseMesh already computed once at decode time
        // (this draw's own window IS the whole cached mesh - GetBaseMesh's key already includes
        // firstVertex/numVertices). safeBones is the only shader-dependent input, so it is the
        // only thing still compared per draw - a single O(1) comparison replaces what used to be
        // an O(numVertices) scan on every one of the 140 hardware-skinned draws/frame.
        if (mesh->singleBoneIdx0Overflow || mesh->singleBoneMaxIdx0 >= safeBones) {
            // The CPU path just drops this one influence (bone >= safeBones) and leaves the
            // vertex at its bind pose. The GPU path has no per-vertex fallback once bone 0 is
            // staged for the rest of the draw - WORLDMATRIX(0) would then hold a real
            // Bone[0]*objTM, not the plain objTM this vertex needs - so refuse the whole draw
            // rather than mispose one vertex.
            g_skinFFRefuseWhy = "single-bone mode: a vertex's blend index is beyond what this shader's palette register can stage safely";
            ++g_skinFFRefuseSingleBoneRange;
            return false;
        }
        drawUsedBones = mesh->singleBoneMask;
        drawMaxBone = mesh->singleBoneMaxIdx0;
    } else {
        if (mesh->maxBone >= safeBones) {
            g_skinFFRefuseWhy = "a used bone sits beyond what this shader's palette register can stage safely";
            ++g_skinFFRefuseBoneRange;
            return false;
        }
        if (followInfl && mesh->noUsableInfluence) {
            // A vertex with NO usable influence at all (every weight 0, or every weighted index
            // beyond kBonesMax) is left at its bind pose by the CPU path (SkinAndBind: "sum <=
            // 0.0f -> leave the vertex in its bind pose"). The GPU path has no such fallback: an
            // all-zero explicit weight leaves the IMPLICIT fourth weight at 1, which
            // D3DVBF_3WEIGHTS stages onto WORLDMATRIX(0) - not this vertex's own bind pose.
            //
            // FIX 5 CACHE (2026-09-22): mesh->noUsableInfluence is shader-independent (bv.w/bv.idx
            // are fixed at decode) - GetBaseMesh already scanned every vertex once to build it, so
            // this is an O(1) flag check instead of the O(numVertices*4) scan it replaces.
            g_skinFFRefuseWhy = "a vertex in this draw has no usable bone influence - the CPU path leaves it at its bind pose, which the GPU path cannot express";
            ++g_skinFFRefuseNoInfluence;
            return false;
        }
    }

    IDirect3DVertexDeclaration9* decl = SkinFFDeclarationFor(dev, g_curDecl);
    if (!decl) { g_skinFFRefuseWhy = "declaration clone failed"; ++g_skinFFRefuseDecl; return false; }

    // GetBaseMesh's own accepted texcoord types are exactly {SHORT2, FLOAT2} (its ok-to-decode
    // gate requires one of the two, above), so this draw's texcoordType is already guaranteed to
    // be one of them here - the else branch below guards against that gate changing under this
    // code later, not a path this build can currently reach.
    IDirect3DVertexBuffer9* uvBuf = nullptr;
    const bool needsUv = (g_curLayout.texcoordType == D3DDECLTYPE_SHORT2);
    if (needsUv) {
        uvBuf = UvBufferFor(dev);
        if (!uvBuf) {
            g_skinFFRefuseWhy = "uv side-stream conversion failed";
            ++g_skinFFRefuseSideBuffer;
            return false;
        }
    } else if (g_curLayout.texcoordType != D3DDECLTYPE_FLOAT2) {
        g_skinFFRefuseWhy = "texcoord is a type this path does not convert";
        ++g_skinFFRefuseSideBuffer;
        return false;
    }
    const UINT uvOffset = g_uvBindOffset;   // UvBufferFor's own out-param, read immediately - nothing between here and its use below calls it again

    IDirect3DVertexBuffer9* sideBuf = SkinFFBufferFor(dev, singleBone);
    if (!sideBuf) {
        g_skinFFRefuseWhy = "weight/index side-stream conversion failed";
        ++g_skinFFRefuseSideBuffer;
        return false;
    }
    const UINT sideOffset = g_skinFFBindOffset;   // FIX 2: SkinFFBufferFor's own out-param, read immediately for the same reason as uvOffset above

    // ---- everything needed is in hand: stage the palette, bind the streams, enable blending ----
    //
    // MULTIPLICATION ORDER, and how it was determined: this file's own row-vector convention is
    // v' = v*M (see TransformPoint, used everywhere a position is placed), under which
    // Multiply(a, b) composes "apply a, then apply b" - v*(a*b) = (v*a)*b - which is exactly how
    // the decal-pull code above already uses it (`world = Multiply(world, pull)` applies the pull
    // AFTER the object placement). FromRegisters reads a bone's raw c52.. registers into a
    // D3DMATRIX in that SAME convention (it is the identical call objTM itself is built with, a
    // few lines below), so the CPU blend's own arithmetic - posObject = posBind * Bone[i], then
    // BeginFFP's ApplyTransforms posts posWorld = posObject * objTM - composes to a single matrix
    // as Multiply(Bone[i], objTM): bone first, object second. g_appliedWorld is reused directly
    // for "objTM" rather than re-deriving it from g_vsConst, because ApplyTransforms already
    // computed and applied EXACTLY this draw's placement there (objTM register read, instance
    // stream, or identity, whichever this draw actually uses) before this function ever runs.
    const D3DMATRIX objTM = g_appliedWorld;
    unsigned long long meshKey = SkinMeshKey(g_stream0, g_stream0Offset, g_stream0Stride,
                                             firstVertex, numVertices);
    // FIX 5, step 4: fold in the shader's own bone register and single/four-bone mode, so two
    // consecutive draws of the SAME vertex window through DIFFERENT shaders - one single-bone,
    // one four-bone, or two with different boneReg - never share a staged palette.
    if (followInfl) {
        meshKey ^= (static_cast<unsigned long long>(boneBase) << 1) | (singleBone ? 1ull : 0ull);
        meshKey *= 1099511628211ull;
    }
    const bool sameStage = g_skinFFStageValid && g_skinFFStageMeshKey == meshKey &&
                           g_skinFFStageFrame == g_frames &&
                           g_skinFFStageBoneDraw == g_lastBoneUploadDraw &&
                           Same(g_skinFFStageObjTM, objTM);
    g_internal = true;
    g_internalThread = GetCurrentThreadId();
    if (!sameStage) {
        for (unsigned b = 0; b < static_cast<unsigned>(kBonesMax); ++b) {
            if (!(drawUsedBones & (1ull << b))) continue;
            const D3DMATRIX bone = FromRegisters(&g_vsConst[boneBase + b * kRegsPerBone][0], 3);
            const D3DMATRIX staged = Multiply(bone, objTM);
            g_origSetTransform(dev, D3DTS_WORLDMATRIX(b), &staged);
            ++g_skinFFTransforms;
        }
        g_skinFFStageMeshKey = meshKey;
        g_skinFFStageFrame = g_frames;
        g_skinFFStageBoneDraw = g_lastBoneUploadDraw;
        g_skinFFStageObjTM = objTM;
        g_skinFFStageValid = true;
    } else {
        ++g_skinFFPaletteReused;
        // FIX 1 (skinFFRestageBone0): WORLDMATRIX(0) IS D3DTS_WORLD. The staging loop above
        // overwrites it with Bone[0]*objTM for any mesh using bone 0, and UnbindSkinFF restores it
        // to plain objTM after every draw (see UnbindSkinFF's own comment) - so THIS branch, which
        // restages nothing, left WORLDMATRIX(0) at the PREVIOUS draw's restored objTM instead of
        // THIS mesh's Bone[0]*objTM for every vertex weighted to bone 0. SUSPECTED (source-read,
        // not yet measured) behind the stretched bracelet and glasses lens of 2026-09-21. g_internal is
        // already true here, set just above before this branch runs.
        if (g_settings.skinFFRestageBone0 && (drawUsedBones & 1ull)) {
            const D3DMATRIX bone0 = FromRegisters(&g_vsConst[boneBase + 0 * kRegsPerBone][0], 3);
            const D3DMATRIX staged0 = Multiply(bone0, objTM);
            g_origSetTransform(dev, D3DTS_WORLDMATRIX(0), &staged0);
            ++g_skinFFTransforms;
            ++g_skinFFBone0Restaged;
        }
    }

    g_origSetVertexDeclaration(dev, decl);
    // FIX 2: bind at sideOffset, not always 0 - see SkinFFBufferFor's own comment on phase/base.
    g_origSetStreamSource(dev, g_skinFFStream, sideBuf, sideOffset, kSkinFFBytesPerVertex);
    if (needsUv) g_origSetStreamSource(dev, g_uvStream, uvBuf, uvOffset, kUvBytesPerVertex);
    // THE SHAPE BAKE: stream 0 is normally left exactly as the game bound it (the whole point of
    // hardware skinning); a successful bake replaces it, here, with the baked whole-buffer copy -
    // same offset and stride, since the bake preserves the source's own addressing exactly.
    if (bakedVb) {
        g_origSetStreamSource(dev, 0, bakedVb, g_stream0Offset, g_stream0Stride);
        g_skinFFBakeBound = true;
        ++g_morphBakeDraws;
    }
    g_internal = false;

    // D3DVBF_3WEIGHTS: three EXPLICIT weights (our BLENDWEIGHT float3) plus one implicit
    // (1 - their sum), which is FOUR bones per vertex - matching this shim's own four-influence
    // CPU blend exactly, and matching the normalisation above: since w0+w1+w2+w3 was made to sum
    // to 1, the implicit 1-w0-w1-w2 already equals w3.
    ShadowSetRS(dev, D3DRS_VERTEXBLEND, D3DVBF_3WEIGHTS);
    ShadowSetRS(dev, D3DRS_INDEXEDVERTEXBLENDENABLE, TRUE);

    // Only on the dump frame - see g_dumpSkin's own reset, above SkinAndBind's identical pattern
    // for the CPU path (RecordFrameDraw reads these into the frame log's ffskin= annotation).
    g_dumpSkinFF = g_frameLog && !g_frameDumpDone && (g_frames + 1 == g_frameDumpTarget);
    if (g_dumpSkinFF) {
        g_dumpSkinFFReuse = sameStage;
        g_dumpSkinFFSingle = singleBone;
        g_dumpSkinFFBone0 = (drawUsedBones & 1ull) != 0;
        g_dumpSkinFFBones = BonesUsedCount(drawUsedBones);
        g_dumpSkinFFHighBone = drawMaxBone;
        g_dumpSkinFFLowBone = 0;
        for (unsigned b = 0; b < static_cast<unsigned>(kBonesMax); ++b)
            if (drawUsedBones & (1ull << b)) { g_dumpSkinFFLowBone = b; break; }
        g_dumpSkinFFSideOffset = sideOffset;
        g_dumpSkinFFUvOffset = needsUv ? uvOffset : 0;
        g_dumpSkinFFBake = bakedVb ? g_morphBakeLastOutcome : 0;   // THE SHAPE BAKE
    }

    // Measured whether or not skinFFHonourStreamOffset is on - see g_skinFFNonZeroOffset's own
    // comment, above the struct it is declared beside.
    if (g_stream0Offset != 0) {
        ++g_skinFFNonZeroOffset;
        if ((g_stream0Offset % g_stream0Stride) != 0) ++g_skinFFNonZeroOffsetPhase;
    }

    if (singleBone) ++g_skinFFSingleBoneDraws;
    ++g_skinFFConverted;
    g_skinFFVertsConverted += numVertices;
    return true;
}

unsigned g_shapeReports = 0;
bool g_shapeProbeDone = false;

void ProbeShapes(UINT minIndex, UINT numVertices, UINT primitiveCount, Disp d) {
    if (!g_settings.shapeProbe || g_shapeProbeDone || !g_haveCamera) return;
    if (!g_shapeProbeTarget) {
        if (g_lastFrameDraws < kDumpMinDraws) return;       // wait until we are in the world
        g_shapeProbeTarget = g_frames + 1;
    }
    if (g_frames + 1 != g_shapeProbeTarget) {
        if (g_frames + 1 > g_shapeProbeTarget && g_shapeLog) {
            fprintf(g_shapeLog, "%u\n", g_shapeReports);
            fclose(g_shapeLog);
            g_shapeLog = nullptr;
            g_shapeProbeDone = true;
            Log("shape probe written to sr3-rtx-shapes.log (%u shapes)", g_shapeReports);
        }
        return;
    }
    // Only draws Remix can actually SEE. A marked draw is dropped by the ignore list and a
    // skipped one never reaches the device, so neither can be a shape the user is looking at -
    // and the first run proved the point the expensive way: the prepass is submitted first, so
    // 148 of the 200 slots went to geometry that is already invisible and the probe never
    // reached the material pass at all. Filtering by what is visible is both the right question
    // and a large saving on locks.
    if (d == Disp::Mark || d == Disp::Skip) return;
    if (g_shapeReports >= 400) return;
    if (!g_stream0 || g_stream0Stride < 12 || g_curLayout.posOffset < 0) return;
    if (numVertices < 3 || numVertices > 8192) return;

    // Never read-lock a dynamic buffer - it is the one rule this file already learned the hard
    // way. A DYNAMIC buffer is written by the CPU every frame and locking it for reading forces
    // a sync; the instance stream is snooped through the game's own Lock/Unlock for this reason.
    D3DVERTEXBUFFER_DESC vbd{};
    if (FAILED(g_stream0->GetDesc(&vbd)) || (vbd.Usage & D3DUSAGE_DYNAMIC)) return;

    const UINT first = g_stream0Offset + minIndex * g_stream0Stride;
    void* mapped = nullptr;
    if (FAILED(g_stream0->Lock(first, numVertices * g_stream0Stride, &mapped,
                               D3DLOCK_READONLY | D3DLOCK_NOSYSLOCK)) || !mapped)
        return;

    // Object space unless the shader says otherwise: a vertex shader with no objTM emits world
    // or clip space itself, so its positions are already where they will appear.
    D3DMATRIX world = kIdentity;
    // Instanced draws carry their transform in the instance stream, not c32, so ask the same
    // helper the conversion path uses rather than reading stale registers.
    if (g_instancedDraw) { if (!InstanceWorld(world)) world = kIdentity; }
    else if (g_curVS.usesObjTM) world = FromRegisters(&g_vsConst[kRegObjTM][0], 3);
    if (!IsFinite(world)) world = kIdentity;

    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    bool finite = true;
    const unsigned char* base = static_cast<const unsigned char*>(mapped);
    const UINT step = (numVertices > 256) ? (numVertices / 256) : 1;
    for (UINT i = 0; i < numVertices; i += step) {
        const float* v = reinterpret_cast<const float*>(
            base + i * g_stream0Stride + g_curLayout.posOffset);
        if (!std::isfinite(v[0]) || !std::isfinite(v[1]) || !std::isfinite(v[2])) { finite = false; break; }
        const D3DVECTOR w = TransformPoint(v, world);
        const float c[3] = {w.x, w.y, w.z};
        for (int k = 0; k < 3; ++k) {
            if (c[k] < lo[k]) lo[k] = c[k];
            if (c[k] > hi[k]) hi[k] = c[k];
        }
    }
    g_stream0->Unlock();
    if (!finite) return;

    const float sx = hi[0] - lo[0], sy = hi[1] - lo[1], sz = hi[2] - lo[2];
    const float cx = (lo[0] + hi[0]) * 0.5f, cy = (lo[1] + hi[1]) * 0.5f, cz = (lo[2] + hi[2]) * 0.5f;
    const float dx = cx - g_camX, dy = cy - g_camY, dz = cz - g_camZ;
    const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);

    // "Around me" - compact, and close. The world is thousands of units across, so a shape that
    // is both small and near the player is exactly the population being described.
    const float biggest = max(max(sx, sy), sz);
    if (biggest > 400.0f || dist > 300.0f) return;

    if (!g_shapeLog) {
        char sname[64];
        CaptureFileName(sname, sizeof(sname), "shapes");
        g_shapeLog = _fsopen(sname, "w", _SH_DENYNO);
        if (!g_shapeLog) { g_shapeProbeDone = true; return; }
        fprintf(g_shapeLog,
                "sr3-rtx shape probe - world-space bounds of every draw near the camera, one frame\n"
                "frame %u, camera (%.1f %.1f %.1f)\n"
                "flat = smallest axis relative to the largest; a dish, cap or ring is very flat\n\n"
                "%5s %-8s %-6s %-6s %-22s %-22s %7s %5s  %s\n",
                g_shapeProbeTarget, g_camX, g_camY, g_camZ,
                "draw", "disp", "verts", "prims", "size x/y/z", "centre", "dist", "flat", "shader / why");
    }
    ++g_shapeReports;
    static const char* kD[] = {"CONVERT", "HIDE", "SKIP", "PASS", "MARK"};
    char sizeStr[32], centreStr[32];
    _snprintf_s(sizeStr, sizeof(sizeStr), _TRUNCATE, "%.1f/%.1f/%.1f", sx, sy, sz);
    _snprintf_s(centreStr, sizeof(centreStr), _TRUNCATE, "%.0f/%.0f/%.0f", cx, cy, cz);
    const float smallest = min(min(sx, sy), sz);
    fprintf(g_shapeLog, "%5u %-8s %-6u %-6u %-22s %-22s %7.1f %5.2f  ps='%s' rank=%d | %s\n",
            g_drawIndexThisFrame, kD[static_cast<int>(d)], numVertices, primitiveCount,
            sizeStr, centreStr, dist, (biggest > 1e-4f) ? (smallest / biggest) : 0.0f,
            g_curPS.firstSampler, g_curPS.albedoRank, g_dispReason);
}


// ---------------------------------------------------------------------------------------------
// STEP 3b - ALL of the character's slots, as surfaces of one mesh.
//
// SR3's body is ONE 7,977-vertex buffer drawn 35 times with different INDEX RANGES, one per
// clothing/body slot (frame dump 2026-08-31: same vb, same bone palette, primitive counts from 84
// to 3408). Step 3a took a single range and produced "some polygons", correctly.
//
// remixapi_MeshInfo::surfaces_count takes an array of surfaces, each with its own indices and its
// own material - which is exactly the shape of the problem. One mesh, N surfaces, one shared
// vertex array.
//
// Two phases, because a slot list is only complete at the END of the frame that produced it:
//   COLLECT  during the capture frame, note every draw that indexes the captured vertex buffer,
//            and copy the game's index buffer ONCE (a character IB is ~90 KB, so copying it whole
//            costs one lock instead of one per slot).
//   BUILD    on the first frame AFTER that, when no further slots can arrive.

std::vector<unsigned char> g_apiCapIB;
UINT g_apiCapIBSize = 0;
bool g_apiCapIB32 = false;
bool g_apiCapIBHave = false;
std::vector<ApiSlot> g_apiSlots;

// THE MESH DATA MUST OUTLIVE THE BUILDER.
//
// remixapi_MeshInfoSurfaceTriangles holds POINTERS to the vertices and indices - vertices_values,
// indices_values - and nothing in the header says CreateMesh copies them. The step-2 cube has
// worked from the first attempt precisely because its arrays are static globals
// (g_remixCubeVerts / g_remixCubeIdx). The character built its arrays as LOCAL std::vectors, so
// they were freed the instant RemixBuildCharacter returned and every DrawInstance afterwards read
// released memory - which is a complete, sound, correctly rebased mesh that renders as holes and
// smeared texture, exactly as reported.
//
// The vertex audit is what made this findable: 0 zeroed vertices, a proper character bounding box
// and clean 0..1 uvs proved the DATA was right, which left only the DESCRIPTION.
// One set per finished mesh, indexed by g_apiDoneCount. See the note on ApiDoneMesh: a shared
// set would be overwritten by the next build while Remix still points into it.
struct ApiMeshStore {
    std::vector<RemixHardcodedVertex> verts;
    std::vector<float> weights;
    std::vector<unsigned> bones;
    std::vector<float> uv2;
    std::vector<std::vector<unsigned>> slotIndices;
    std::vector<RemixMeshInfoSurfaceTriangles> surfaces;
};
ApiMeshStore g_apiStore[kMaxApiMeshes];
unsigned g_apiSlotsSeen = 0, g_apiOutOfRange = 0, g_apiSurfaces = 0;
IDirect3DBaseTexture9* g_apiCapAlbedo = nullptr;
std::vector<float> g_apiCapWeights;
std::vector<unsigned> g_apiCapBones;
std::vector<float> g_apiCapUv2;
unsigned g_apiCapBoneCount = 0, g_apiCapBoneReg = 0;
unsigned g_apiSlotsDuplicate = 0, g_apiDegenerate = 0, g_apiStripSlots = 0;
unsigned g_apiDegeneratePrev = 0;   // so each part reports its OWN degenerate count
unsigned g_apiClothGenerated = 0, g_apiClothApprox = 0, g_apiClothBlackConst = 0;
// Diagnostics for the two generators that fired zero times on 2026-09-07.
unsigned g_hairSlotReports = 0, g_clothSlotReports = 0;
unsigned g_uv2Reports = 0;
unsigned g_psDumped = 0;
IDirect3DPixelShader9* g_psDumpedPtr[8] = {};
constexpr unsigned kMaxApiSlots = 64;

// Called from the draw hook for every draw, during the capture frame only.
// ---------------------------------------------------------------------------------------------
// BAKING WHAT THE PIXEL SHADER COMPUTES
//
// SR3's clothing and accessories have no texture holding their colour. The colour is computed in
// the PIXEL SHADER from three constants weighted per texel by a Pattern_Map at gamma 2.2, with a
// selector branch, then multiplied by Tint_color. Remix never runs a pixel shader - it picks one
// bound texture as the albedo and applies its own PBR model - so that colour cannot survive by any
// amount of choosing a better texture or a better constant. Approximating it with Diffuse_Color_a
// is what blacked the top.
//
// The answer is to RUN the game's own pixel shader and keep the result: render the item into an
// offscreen target whose SCREEN SPACE IS UV SPACE. Every texel of the output then holds exactly
// the colour that shader computes for that texel, and the result is a texture Remix can sample.
// This is not an approximation of the recipe - it IS the recipe, executed by the same shader with
// the same constants and the same textures.
//
// The one piece the game does not provide is a vertex shader that maps UV to clip space. We build
// it. In SM3.0 vertex inputs bind by DECLARATION USAGE rather than register number, so a
// standalone shader that declares dcl_texcoord v0 receives the texture coordinates from the game's
// own vertex declaration without touching the game's shader at all.
//
// The bytecode is assembled here directly. It is checked by D3D9 itself: CreateVertexShader
// validates the token stream, so a mistake in this assembler is reported as a failed HRESULT and
// the feature disables itself, rather than corrupting a draw.

namespace sm3 {
constexpr DWORD kVersionVs30 = 0xFFFE0300u;
constexpr DWORD kEnd         = 0x0000FFFFu;

// Register types (D3DSHADER_PARAM_REGISTER_TYPE). The type is split across two bit ranges in a
// parameter token, which is the usual source of hand-assembly mistakes, so it is done in one place.
constexpr unsigned kRegTemp = 0, kRegInput = 1, kRegConst = 2, kRegOutput = 3;

constexpr unsigned kOpMov = 1, kOpMad = 4, kOpDcl = 31, kOpDef = 81;

// Swizzles, 2 bits per component starting at bit 16 of a source token.
constexpr unsigned kSwzXYZW = 0xE4, kSwzX = 0x00, kSwzY = 0x55, kSwzZ = 0xAA, kSwzW = 0xFF;
// Write masks, bits 16..19 of a destination token.
constexpr unsigned kMaskX = 0x1, kMaskY = 0x2, kMaskZ = 0x4, kMaskW = 0x8;
constexpr unsigned kMaskXY = kMaskX | kMaskY, kMaskAll = 0xF;

inline DWORD Instr(unsigned opcode, unsigned lengthTokens) {
    return opcode | (lengthTokens << 24);
}
inline DWORD Param(unsigned type, unsigned reg) {
    // bits 28..30 hold the low three bits of the type, bits 11..12 the high two.
    return 0x80000000u | reg | ((type & 7u) << 28) | (((type >> 3) & 3u) << 11);
}
inline DWORD Dest(unsigned type, unsigned reg, unsigned mask) {
    return Param(type, reg) | (mask << 16);
}
inline DWORD Source(unsigned type, unsigned reg, unsigned swizzle) {
    return Param(type, reg) | (swizzle << 16);
}
inline DWORD AsBits(float f) {
    DWORD d;
    memcpy(&d, &f, sizeof(d));
    return d;
}
}  // namespace sm3

// D3DDECLUSAGE_TEXCOORD
constexpr unsigned kUsageTexcoord = 5;
constexpr unsigned kUsagePosition = 0;

// Constant registers for our two literals. Chosen at the very top of the file's range: SR3's bone
// palette runs c52..c243 and projTM/objTM sit at c28..c34, so c250+ cannot collide with anything
// the game uploads.
constexpr unsigned kBakeConstA = 250, kBakeConstB = 251;

// vs_3_0: position = the texture coordinate, mapped from [0,1] to clip space, with v flipped
// because texture space runs downwards and clip space upwards. Texture coordinates reach us as RAW
// SHORTS - SR3 scales them by 1/1024 in its own shaders - so that scale is folded into the
// literal: 2/1024 = 0.001953125.
std::vector<DWORD> BuildUvBakeVs(unsigned passthroughCount) {
    using namespace sm3;
    std::vector<DWORD> t;
    t.push_back(kVersionVs30);

    // dcl_texcoord0 v0
    t.push_back(Instr(kOpDcl, 2));
    t.push_back(0x80000000u | kUsageTexcoord | (0u << 16));
    t.push_back(Dest(kRegInput, 0, kMaskAll));

    // dcl_position o0
    t.push_back(Instr(kOpDcl, 2));
    t.push_back(0x80000000u | kUsagePosition | (0u << 16));
    t.push_back(Dest(kRegOutput, 0, kMaskAll));

    // dcl_texcoord<i> o<1+i> - the pixel shader reads several interpolants and we do not know
    // which one carries the pattern's coordinates, so every one is given the same uv. A wrong
    // guess here shows as a mis-sampled bake, not a crash.
    for (unsigned i = 0; i < passthroughCount; ++i) {
        t.push_back(Instr(kOpDcl, 2));
        t.push_back(0x80000000u | kUsageTexcoord | (i << 16));
        t.push_back(Dest(kRegOutput, 1 + i, kMaskAll));
    }

    // def c250, 2/1024, -1, -2/1024, 1
    t.push_back(Instr(kOpDef, 5));
    t.push_back(Dest(kRegConst, kBakeConstA, kMaskAll));
    t.push_back(AsBits(0.001953125f));
    t.push_back(AsBits(-1.0f));
    t.push_back(AsBits(-0.001953125f));
    t.push_back(AsBits(1.0f));

    // def c251, 0, 0, 0, 1
    t.push_back(Instr(kOpDef, 5));
    t.push_back(Dest(kRegConst, kBakeConstB, kMaskAll));
    t.push_back(AsBits(0.0f));
    t.push_back(AsBits(0.0f));
    t.push_back(AsBits(0.0f));
    t.push_back(AsBits(1.0f));

    // mad o0.x, v0.x, c250.x, c250.y      ->  u*2/1024 - 1
    t.push_back(Instr(kOpMad, 4));
    t.push_back(Dest(kRegOutput, 0, kMaskX));
    t.push_back(Source(kRegInput, 0, kSwzX));
    t.push_back(Source(kRegConst, kBakeConstA, kSwzX));
    t.push_back(Source(kRegConst, kBakeConstA, kSwzY));

    // mad o0.y, v0.y, c250.z, c250.w      ->  1 - v*2/1024
    t.push_back(Instr(kOpMad, 4));
    t.push_back(Dest(kRegOutput, 0, kMaskY));
    t.push_back(Source(kRegInput, 0, kSwzY));
    t.push_back(Source(kRegConst, kBakeConstA, kSwzZ));
    t.push_back(Source(kRegConst, kBakeConstA, kSwzW));

    // mov o0.zw, c251   -> z = 0, w = 1
    t.push_back(Instr(kOpMov, 2));
    t.push_back(Dest(kRegOutput, 0, kMaskZ | kMaskW));
    t.push_back(Source(kRegConst, kBakeConstB, kSwzXYZW));

    for (unsigned i = 0; i < passthroughCount; ++i) {
        t.push_back(Instr(kOpMov, 2));
        t.push_back(Dest(kRegOutput, 1 + i, kMaskAll));
        t.push_back(Source(kRegInput, 0, kSwzXYZW));
    }

    t.push_back(kEnd);
    return t;
}

IDirect3DVertexShader9* g_bakeVs = nullptr;
bool g_bakeTried = false, g_bakeDead = false;
unsigned g_bakeOk = 0, g_bakeFail = 0;

// Built once. If D3D9 rejects the bytecode the whole feature retires itself and says so - the
// assembler above is hand-written and this is the check that it is right.
bool BakeShaderReady(IDirect3DDevice9* dev) {
    if (g_bakeDead) return false;
    if (g_bakeVs) return true;
    if (g_bakeTried) return false;
    g_bakeTried = true;
    const std::vector<DWORD> code = BuildUvBakeVs(7);
    const bool wasInternal = g_internal;
    g_internal = true;
    g_internalThread = GetCurrentThreadId();
    const HRESULT hr = dev->CreateVertexShader(code.data(), &g_bakeVs);
    g_internal = wasInternal;
    if (FAILED(hr) || !g_bakeVs) {
        g_bakeDead = true;
        Log("bake: D3D9 REJECTED the hand-assembled uv-bake vertex shader (hr=0x%08lX, %u tokens)"
            " - the token stream is wrong and baking is disabled", hr,
            static_cast<unsigned>(code.size()));
        return false;
    }
    Log("bake: uv-bake vertex shader accepted by D3D9 (%u tokens). Screen space is now UV space, "
        "so the game's own pixel shader can be run into an offscreen target and its computed "
        "albedo kept.", static_cast<unsigned>(code.size()));
    return true;
}


// ---------------------------------------------------------------------------------------------
// The bake pass itself.
//
// Re-issues the game's draw with OUR vertex shader and the game's own pixel shader, into an
// offscreen target. Everything else is left exactly as the game set it: the same vertex and index
// buffers, the same declaration, the same pixel-shader constants, the same textures. Only where
// the triangles LAND changes, and they land in UV space.
//
// Two things are neutralised for the duration:
//   depth and culling, because a UV unwrap has no meaningful depth and its winding is arbitrary;
//   alpha test and blending, because the point is to capture the colour the shader COMPUTES, not
//   the subset of it that would have survived a depth-tested composite.
//
// The whole thing runs with g_internal set, so none of the shim's own hooks react to it.
// 512, not 1024. A clothing item's unwrap does not need more, and this is a THIRTY-TWO BIT
// process already holding a skinning ring, a 96 MB vertex snoop, a 48 MB uv arena and Remix's own
// client. The target, its system-memory readback surface and the transient copy are 1 MB each at
// this size instead of 4.
constexpr UINT kBakeSize = 512;
IDirect3DTexture9* g_bakeTex = nullptr;
IDirect3DSurface9* g_bakeSurf = nullptr;
IDirect3DSurface9* g_bakeSys = nullptr;
// HIDE THE BAKE DRAW FROM REMIX.
//
// Remix crashed its 64-bit server on the first bake (0xc0000005, 2026-09-03). Vertex capture is on,
// so it tries to capture the output of our hand-assembled shader and falls over.
//
// Remix declines to raytrace any draw issued inside an OCCLUSION QUERY - its own log says
// "Trying to raytrace an occlusion query. Ignoring." - while D3D9 still rasterises it normally.
// That is precisely what a bake needs: the pixel shader runs, the target fills, and Remix never
// looks at it. The query result is never read.
IDirect3DQuery9* g_bakeQuery = nullptr;
unsigned g_bakeDumped = 0, g_bakeAttempts = 0, g_bakeThisFrame = 0;
unsigned g_bakeFrame = 0xFFFFFFFFu;

bool BakeTargetsReady(IDirect3DDevice9* dev) {
    if (g_bakeTex && g_bakeSurf && g_bakeSys) return true;
    const bool wasInternal = g_internal;
    g_internal = true;
    g_internalThread = GetCurrentThreadId();
    bool ok = true;
    if (!g_bakeTex &&
        FAILED(dev->CreateTexture(kBakeSize, kBakeSize, 1, D3DUSAGE_RENDERTARGET,
                                  D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g_bakeTex, nullptr)))
        ok = false;
    if (ok && !g_bakeSurf && FAILED(g_bakeTex->GetSurfaceLevel(0, &g_bakeSurf))) ok = false;
    // SYSTEMMEM, because GetRenderTargetData is the only sanctioned way back from a render target
    // and it requires one. Reading the GAME's targets this way froze SR3 twice; reading OUR OWN,
    // which we created and filled, was proved to work by the 2026-08-29 copy self-test.
    if (ok && !g_bakeSys &&
        FAILED(dev->CreateOffscreenPlainSurface(kBakeSize, kBakeSize, D3DFMT_A8R8G8B8,
                                                D3DPOOL_SYSTEMMEM, &g_bakeSys, nullptr)))
        ok = false;
    if (ok && !g_bakeQuery &&
        FAILED(dev->CreateQuery(D3DQUERYTYPE_OCCLUSION, &g_bakeQuery)))
        ok = false;
    g_internal = wasInternal;
    if (!ok) {
        g_bakeDead = true;
        Log("bake: could not create the 1024x1024 target or its readback surface - disabled");
    }
    return ok;
}

// Returns a path to a written DDS, or nullptr.
const char* BakeSlotAlbedo(IDirect3DDevice9* dev, D3DPRIMITIVETYPE type, INT baseVertex,
                           UINT minIndex, UINT numVertices, UINT startIndex, UINT primitiveCount) {
    if (!g_settings.bakeShaderAlbedo || g_bakeDead) return nullptr;
    if (!BakeShaderReady(dev) || !BakeTargetsReady(dev)) return nullptr;

    // CAP ON ATTEMPTS, NOT SUCCESSES.
    //
    // This read `if (g_bakeDumped >= 16)`, and g_bakeDumped only increments on SUCCESS - so every
    // failing path returned without advancing anything and the bake was retried for every
    // customised slot, every frame, each attempt a full draw plus a GetRenderTargetData readback
    // across the 32-to-64-bit bridge. That is an unbounded loop in a 32-bit address space, and it
    // exhausted memory.
    //
    // A budget that only counts the good outcomes is not a budget.
    ++g_bakeAttempts;
    if (g_bakeAttempts > 24) {
        if (!g_bakeDead) {
            g_bakeDead = true;
            Log("bake: attempt budget spent (%u attempts, %u written, %u failed) - baking off",
                g_bakeAttempts, g_bakeOk, g_bakeFail);
        }
        return nullptr;
    }
    if (g_bakeFail >= 3) {
        if (!g_bakeDead) {
            g_bakeDead = true;
            Log("bake: %u failures - baking off rather than retrying", g_bakeFail);
        }
        return nullptr;
    }
    // At most two a frame. Eleven 512x512 readbacks and DDS writes in ONE frame is a stall even
    // when every one of them succeeds.
    if (g_bakeFrame == g_frames && g_bakeThisFrame >= 2) return nullptr;
    if (g_bakeFrame != g_frames) { g_bakeFrame = g_frames; g_bakeThisFrame = 0; }
    ++g_bakeThisFrame;

    const bool wasInternal = g_internal;
    g_internal = true;
    g_internalThread = GetCurrentThreadId();

    IDirect3DSurface9* oldRt = nullptr;
    IDirect3DSurface9* oldDs = nullptr;
    IDirect3DVertexShader9* oldVs = nullptr;
    D3DVIEWPORT9 oldVp{};
    dev->GetRenderTarget(0, &oldRt);
    dev->GetDepthStencilSurface(&oldDs);
    dev->GetVertexShader(&oldVs);
    dev->GetViewport(&oldVp);
    const DWORD oldZ = ShadowGetRS(dev, D3DRS_ZENABLE);
    const DWORD oldZW = ShadowGetRS(dev, D3DRS_ZWRITEENABLE);
    const DWORD oldCull = ShadowGetRS(dev, D3DRS_CULLMODE);
    const DWORD oldAT = ShadowGetRS(dev, D3DRS_ALPHATESTENABLE);
    const DWORD oldAB = ShadowGetRS(dev, D3DRS_ALPHABLENDENABLE);
    const DWORD oldCW = ShadowGetRS(dev, D3DRS_COLORWRITEENABLE);

    bool ok = SUCCEEDED(dev->SetRenderTarget(0, g_bakeSurf));
    if (ok) {
        dev->SetDepthStencilSurface(nullptr);
        D3DVIEWPORT9 vp{0, 0, kBakeSize, kBakeSize, 0.0f, 1.0f};
        dev->SetViewport(&vp);
        dev->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_ARGB(0, 0, 0, 0), 1.0f, 0);
        dev->SetVertexShader(g_bakeVs);
        dev->SetRenderState(D3DRS_ZENABLE, FALSE);
        dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
        // Inside the query, so Remix ignores it.
        g_bakeQuery->Issue(D3DISSUE_BEGIN);
        ok = SUCCEEDED(g_origDrawIndexedPrimitive(dev, type, baseVertex, minIndex, numVertices,
                                                  startIndex, primitiveCount));
        g_bakeQuery->Issue(D3DISSUE_END);
    }

    dev->SetVertexShader(oldVs);
    if (oldVs) oldVs->Release();
    dev->SetRenderState(D3DRS_ZENABLE, oldZ);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, oldZW);
    dev->SetRenderState(D3DRS_CULLMODE, oldCull);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, oldAT);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, oldAB);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, oldCW);
    if (oldRt) { dev->SetRenderTarget(0, oldRt); oldRt->Release(); }
    dev->SetDepthStencilSurface(oldDs);
    if (oldDs) oldDs->Release();
    dev->SetViewport(&oldVp);

    static char path[MAX_PATH];
    path[0] = 0;
    if (ok && SUCCEEDED(dev->GetRenderTargetData(g_bakeSurf, g_bakeSys))) {
        D3DLOCKED_RECT lr{};
        if (SUCCEEDED(g_bakeSys->LockRect(&lr, nullptr, D3DLOCK_READONLY)) && lr.pBits) {
            std::vector<unsigned char> bgra;
            try {
                bgra.resize(static_cast<size_t>(kBakeSize) * kBakeSize * 4);
                for (UINT y = 0; y < kBakeSize; ++y)
                    memcpy(&bgra[static_cast<size_t>(y) * kBakeSize * 4],
                           static_cast<const unsigned char*>(lr.pBits) +
                               static_cast<size_t>(y) * lr.Pitch,
                           static_cast<size_t>(kBakeSize) * 4);
                // What did the shader actually produce? A mean of zero means the draw landed
                // outside the target - the uv mapping - rather than the shader misbehaving.
                double acc = 0.0;
                unsigned n = 0, opaque = 0;
                for (UINT i = 0; i < kBakeSize * kBakeSize; i += 97) {
                    acc += bgra[i * 4] + bgra[i * 4 + 1] + bgra[i * 4 + 2];
                    if (bgra[i * 4 + 3]) ++opaque;
                    ++n;
                }
                char dir[MAX_PATH] = {0};
                GetModuleFileNameA(GetModuleHandleW(nullptr), dir, MAX_PATH);
                char* sl = strrchr(dir, '\\');
                if (sl) *(sl + 1) = 0;
                ++g_bakeDumped;
                sprintf_s(path, "%ssr3-remix-bake-%u.dds", dir, g_bakeDumped);
                if (WriteBgraDds(path, kBakeSize, kBakeSize, bgra.data())) {
                    ++g_bakeOk;
                    Log("bake #%u: wrote %s | mean %.1f of 255 over %u samples, %u%% covered",
                        g_bakeDumped, path, n ? acc / (3.0 * n) : 0.0, n,
                        n ? (opaque * 100 / n) : 0);
                } else {
                    path[0] = 0;
                    ++g_bakeFail;
                }
            } catch (...) { path[0] = 0; ++g_bakeFail; }
            g_bakeSys->UnlockRect();
        } else {
            ++g_bakeFail;
        }
    } else if (ok) {
        ++g_bakeFail;
        Log("bake: GetRenderTargetData failed on our own target");
    } else {
        ++g_bakeFail;
    }
    g_internal = wasInternal;
    return path[0] ? path : nullptr;
}

void RemixCollectCharacterSlot(IDirect3DDevice9* dev, D3DPRIMITIVETYPE type, INT baseVertex,
                               UINT minIndex, UINT numVertices, UINT startIndex,
                               UINT primitiveCount) {
    if (!g_settings.remixApiCharacter || g_apiCharBuilt || g_apiCharFailed) return;
    if (!g_apiPendingCount || g_frames != g_apiCapFrame) return;
    unsigned pi = kMaxApiMeshes;
    for (unsigned d = 0; d < g_apiPendingCount; ++d)
        if (g_apiPending[d].vb == g_stream0) { pi = d; break; }
    if (pi == kMaxApiMeshes) return;
    ApiPending& P = g_apiPending[pi];
    // minIndex deliberately NOT required to match: the rebasing is absolute now, so a slot
    // with a different window is handled correctly rather than skipped. The vertex COUNT still
    // has to match, as a cheap "is this the same body mesh" test.
    (void)minIndex;
    if (numVertices != P.verts.size()) return;
    if (primitiveCount == 0 || P.slots.size() >= kMaxApiSlots) return;

    // Copy the whole index buffer once, on the first slot. Every later slot reads out of this
    // copy, so 35 slots cost one lock rather than 35 bridge round trips.
    if (!P.ibHave) {
        IDirect3DIndexBuffer9* ib = nullptr;
        g_internal = true;
        g_internalThread = GetCurrentThreadId();
        const HRESULT hr = dev->GetIndices(&ib);
        g_internal = false;
        if (FAILED(hr) || !ib) return;
        D3DINDEXBUFFER_DESC d{};
        void* mapped = nullptr;
        if (SUCCEEDED(ib->GetDesc(&d)) && d.Size &&
            SUCCEEDED(ib->Lock(0, 0, &mapped, D3DLOCK_READONLY)) && mapped) {
            try {
                P.ib.assign(static_cast<const unsigned char*>(mapped),
                            static_cast<const unsigned char*>(mapped) + d.Size);
                P.ibSize = d.Size;
                P.ib32 = (d.Format == D3DFMT_INDEX32);
                P.ibHave = true;
            } catch (...) { P.ib.clear(); }
            ib->Unlock();
        }
        ib->Release();
        if (!P.ibHave) return;
    }
    // DEDUP. SR3 submits the same skinned geometry more than once a frame - the shim's own
    // skinning report has said "19 exact duplicates dropped/frame" all along, and its FFP path
    // drops them for exactly this reason. Collected without that filter, an identical index range
    // enters the mesh twice, and two coplanar copies of a surface z-fight against each other. The
    // result is a correctly built, correctly textured mesh full of holes - which is what was on
    // screen, and what no counter could show, because every slot is individually valid.
    ++g_apiSlotsSeen;
    if (type != D3DPT_TRIANGLESTRIP && type != D3DPT_TRIANGLELIST) return;
    for (size_t i = 0; i < P.slots.size(); ++i)
        if (P.slots[i].start == startIndex && P.slots[i].prims == primitiveCount &&
            P.slots[i].base == baseVertex) {
            ++g_apiSlotsDuplicate;
            return;
        }
    ApiSlot slot{};
    slot.start = startIndex;
    slot.prims = primitiveCount;
    slot.base = baseVertex;
    slot.type = type;
    slot.tint[0] = slot.tint[1] = slot.tint[2] = slot.tint[3] = 1.0f;
    if (g_curPS.tintColorReg >= 0 &&
        g_curPS.tintColorReg < static_cast<int>(kMaxPsConst))
        memcpy(slot.tint, g_psConst[g_curPS.tintColorReg], sizeof(slot.tint));
    // The ranked albedo stage if the shader has one, otherwise whatever is actually bound at
    // stage 0. Falling back to the BODY's atlas - which is what happened - is never right for a
    // part that simply has no ranked sampler.
    slot.albedo = nullptr;
    // If the shim can GENERATE the customised albedo - pattern combined with the three colours -
    // that is the correct texture and beats anything bound. It only handles the family where the
    // pattern IS the albedo; the player's own clothing samples a Diffuse_Map as well through a
    // second UV set, which cannot be folded into one texture, so it falls through.
    slot.cloth = false;
    slot.clothGenerated = false;
    slot.dcMul[0] = slot.dcMul[1] = slot.dcMul[2] = slot.dcMul[3] = 1.0f;
    slot.albedoRank = g_curPS.albedoRank;
    slot.bakedPath[0] = 0;
    // DUMP THE ACTUAL PIXEL SHADER.
    //
    // The recipe has been read from files picked out of the corpus by name, and twice it was the
    // WRONG file - the NPC cloth shader has a sum/deviation branch the player's does not. The only
    // way to stop guessing is to take the bytecode the device is actually running and disassemble
    // that. One file per distinct shader, capped.
    if (g_curPS.diffuseColorReg[0] >= 0 && g_psDumped < 6) {
        IDirect3DPixelShader9* ps = nullptr;
        const bool wi = g_internal;
        g_internal = true;
        g_internalThread = GetCurrentThreadId();
        if (SUCCEEDED(dev->GetPixelShader(&ps)) && ps) {
            bool seen = false;
            for (unsigned k = 0; k < g_psDumped; ++k)
                if (g_psDumpedPtr[k] == ps) { seen = true; break; }
            if (!seen) {
                UINT sz = 0;
                if (SUCCEEDED(ps->GetFunction(nullptr, &sz)) && sz) {
                    std::vector<unsigned char> code;
                    try {
                        code.resize(sz);
                        if (SUCCEEDED(ps->GetFunction(code.data(), &sz))) {
                            char dd[MAX_PATH] = {0};
                            GetModuleFileNameA(GetModuleHandleW(nullptr), dd, MAX_PATH);
                            char* dsl = strrchr(dd, 0x5C);
                            if (dsl) *(dsl + 1) = 0;
                            char pth[MAX_PATH];
                            sprintf_s(pth, "%ssr3-ps-%u.fxo", dd, g_psDumped);
                            FILE* f = nullptr;
                            if (fopen_s(&f, pth, "wb") == 0 && f) {
                                fwrite(code.data(), 1, sz, f);
                                fclose(f);
                                Log("dumped the LIVE cloth pixel shader to %s (%u bytes, "
                                    "albedo '%s')", pth, sz, g_curPS.albedoSampler);
                            }
                        }
                    } catch (...) {}
                }
                g_psDumpedPtr[g_psDumped++] = ps;
            }
            ps->Release();
        }
        g_internal = wi;
    }
    strncpy_s(slot.albedoName, g_curPS.albedoSampler, _TRUNCATE);
    strncpy_s(slot.firstName, g_curPS.firstSampler, _TRUNCATE);
    slot.hair = false;
    slot.dob = nullptr;
    slot.hairColor[0] = slot.hairColor[1] = slot.hairColor[2] = slot.hairColor[3] = 1.0f;
    if (g_curPS.hairColorReg[1] >= 0 &&
        g_curPS.hairColorReg[1] < static_cast<int>(kMaxPsConst)) {
        slot.hair = true;
        memcpy(slot.hairColor, g_psConst[g_curPS.hairColorReg[1]], sizeof(slot.hairColor));
        // WIDENED, and it reports itself. "Dob_Map" was matched with a 7-character prefix
        // compare, which is exact and therefore brittle - a sampler called Dob_Map_1 or dob_map
        // would miss. A case-insensitive substring cannot miss those, and if it still finds
        // nothing the log says what the stages WERE, so the next step is reading rather than
        // another guess.
        for (int st = 0; st < 8; ++st) {
            const char* nm = g_curPS.samplerName[st];
            if (g_curTexture[st] && nm[0] && StrStrIA(nm, "dob")) {
                slot.dob = g_curTexture[st];
                break;
            }
        }
        if (g_hairSlotReports < 4) {
            ++g_hairSlotReports;
            char stages[256] = {0};
            int p = 0;
            for (int st = 0; st < 8 && p < 200; ++st)
                if (g_curTexture[st] || g_curPS.samplerName[st][0])
                    p += _snprintf_s(stages + p, sizeof(stages) - p, _TRUNCATE, "s%d='%s'%s ",
                                     st, g_curPS.samplerName[st][0] ? g_curPS.samplerName[st]
                                                                    : "(unnamed)",
                                     g_curTexture[st] ? "" : "(NO TEXTURE)");
            Log("HAIR SLOT #%u: Dob_Map %s | stages: %s", g_hairSlotReports,
                slot.dob ? "FOUND" : "NOT FOUND - the strand generator cannot run", stages);
        }
    } else if (g_curPS.hairColorReg[0] >= 0 &&
               g_curPS.hairColorReg[0] < static_cast<int>(kMaxPsConst)) {
        slot.hair = true;
        memcpy(slot.hairColor, g_psConst[g_curPS.hairColorReg[0]], sizeof(slot.hairColor));
    }
    // WHICH PATTERNS THE API PATH SEES.
    //
    // The 2026-09-07 run put ten garments through the texture-space generator and every pattern
    // it saw was 32x32 - yet the CPU bake on this side handled a 64x64 one, the heart. So the
    // underwear's draw reaches HERE and does not reach ClothAlbedo, which runs only for CONVERTED
    // draws. Everything built for that garment sits downstream of a gate it never passes, and
    // this says which gate by naming the pattern each slot carries.
    if (g_clothSlotReports < 16 && g_curPS.patternStage >= 0 && g_curPS.patternStage < 8 &&
        g_curTexture[g_curPS.patternStage]) {
        UINT pw2 = 0, ph2 = 0;
        if (g_curTexture[g_curPS.patternStage]->GetType() == D3DRTYPE_TEXTURE) {
            D3DSURFACE_DESC sd{};
            if (SUCCEEDED(static_cast<IDirect3DTexture9*>(g_curTexture[g_curPS.patternStage])
                              ->GetLevelDesc(0, &sd))) { pw2 = sd.Width; ph2 = sd.Height; }
        }
        if (pw2 != 32 || ph2 != 32) {
            ++g_clothSlotReports;
            // AND WHAT THE CLASSIFIER DID WITH IT. ClothAlbedo runs only for a draw that
            // Classify returns Convert for, so the disposition IS the gate. g_dispReason is set
            // by Classify and this runs later in the same hook, so it still describes this draw.
            char uvmap[160] = {0};
            {
                int q = 0;
                for (int st = 0; st < 8 && q < 130; ++st)
                    if (g_curPS.samplerName[st][0] || g_curTexture[st])
                        q += _snprintf_s(uvmap + q, sizeof(uvmap) - q, _TRUNCATE,
                                         "s%d<-%s%d%s ", st,
                                         g_curPS.samplerUv[st] < 0 ? "unknown(" : "TEXCOORD",
                                         g_curPS.samplerUv[st] < 0 ? 0 : g_curPS.samplerUv[st],
                                         g_curPS.samplerUv[st] < 0
                                             ? ")"
                                             : (g_curPS.samplerUvDirect[st] ? "" : "*traced"));
            }
            Log("      UV SET PER SAMPLER (from the shader's own texld): %s", uvmap);
            Log("CLOTH SLOT (API) #%u: pattern %ux%u at stage %d, albedo stage %d, "
                "blend %lu (src %lu dst %lu) | CLASSIFIED AS: %s",
                g_clothSlotReports, pw2, ph2, g_curPS.patternStage, g_curPS.albedoStage,
                ShadowGetRS(dev, D3DRS_ALPHABLENDENABLE), ShadowGetRS(dev, D3DRS_SRCBLEND),
                ShadowGetRS(dev, D3DRS_DESTBLEND),
                g_dispReason && g_dispReason[0] ? g_dispReason : "(no reason recorded)");
        }
    }
    slot.pattern = (g_curPS.patternStage >= 0 && g_curPS.patternStage < 8)
                       ? g_curTexture[g_curPS.patternStage] : nullptr;

    // FILL THE PART'S SECOND UV SET FROM *THIS* DRAW.
    //
    // The part was captured from the FIRST skinned draw on its vertex buffer, and that draw's
    // declaration has no TEXCOORD1 - so uv2 came back empty every time. The draw that DOES carry
    // it is this one, the cloth material draw, arriving later as a slot. Re-decoding here, with
    // the current declaration, is the only point where both the buffer and a declaration
    // containing TEXCOORD1 are in hand at once.
    if (P.uv2.size() != P.verts.size() * 2 && g_curLayout.texcoord1Offset >= 0) {
        const INT first = baseVertex + static_cast<INT>(minIndex);
        if (first >= 0) {
            const BaseMesh* m2 = GetBaseMesh(static_cast<UINT>(first), numVertices);
            if (m2 && m2->hasUv2 && m2->verts.size() == P.verts.size()) {
                try {
                    P.uv2.clear();
                    P.uv2.reserve(m2->verts.size() * 2);
                    for (size_t k = 0; k < m2->verts.size(); ++k) {
                        P.uv2.push_back(m2->verts[k].uv2[0]);
                        P.uv2.push_back(m2->verts[k].uv2[1]);
                    }
                    Log("cloth uv2: filled %u coordinates for a part from its material draw "
                        "(the capture draw had no TEXCOORD1)", (unsigned)m2->verts.size());
                } catch (...) { P.uv2.clear(); }
            }
        }
    }
    // Does this customised draw actually carry the second UV set the pattern needs? Reported once
    // per distinct answer, because the CPU bake depends entirely on it being there.
    if (g_curPS.diffuseColorReg[0] >= 0 && g_uv2Reports < 4) {
        ++g_uv2Reports;
        Log("cloth uv2: texcoord1 type=%d offset=%d stream=%d (texcoord0 type=%d offset=%d) - "
            "the pattern map is sampled with TEXCOORD1, so a CPU bake needs this present",
            g_curLayout.texcoord1Type, g_curLayout.texcoord1Offset, g_curLayout.texcoord1Stream,
            g_curLayout.texcoordType, g_curLayout.texcoordOffset);
    }
    // BAKE. Only for slots that carry the customisation recipe, because those are the ones whose
    // colour exists nowhere but in the pixel shader. Run here, immediately after the game's draw,
    // because every piece of state the bake needs - textures, ps constants, buffers - is still
    // exactly as the game left it.
    if (g_settings.bakeShaderAlbedo && g_curPS.diffuseColorReg[0] >= 0 &&
        g_curPS.diffuseColorReg[1] >= 0 && g_curPS.diffuseColorReg[2] >= 0) {
        const char* baked = BakeSlotAlbedo(dev, type, baseVertex, minIndex, numVertices,
                                           startIndex, primitiveCount);
        if (baked) strncpy_s(slot.bakedPath, baked, _TRUNCATE);
    }
    for (int c = 0; c < 4; ++c) slot.dcA[c] = slot.dcB[c] = slot.dcC[c] = 1.0f;
    // NOT gated on remixApiClothAlbedo. That switch exists to stop ClothAlbedo() REPLACING a
    // working texture - it was never meant to stop the slot being RECOGNISED as customised. It
    // did, so slot.cloth and the three colours were never set and the CPU baker could not fire.
    // A guard placed on the wrong side of a condition.
    if (g_curPS.diffuseColorReg[0] >= 0 && g_curPS.diffuseColorReg[1] >= 0 &&
        g_curPS.diffuseColorReg[2] >= 0) {
        slot.cloth = true;
        const int* r = g_curPS.diffuseColorReg;
        if (r[0] < static_cast<int>(kMaxPsConst)) memcpy(slot.dcA, g_psConst[r[0]], 16);
        if (r[1] < static_cast<int>(kMaxPsConst)) memcpy(slot.dcB, g_psConst[r[1]], 16);
        if (r[2] < static_cast<int>(kMaxPsConst)) memcpy(slot.dcC, g_psConst[r[2]], 16);
        if (g_curPS.diffuseColorMulReg >= 0 &&
            g_curPS.diffuseColorMulReg < static_cast<int>(kMaxPsConst))
            memcpy(slot.dcMul, g_psConst[g_curPS.diffuseColorMulReg], 16);
        slot.albedo = g_settings.remixApiClothAlbedo ? ClothAlbedo(dev) : nullptr;
        slot.clothGenerated = (slot.albedo != nullptr);
        if (slot.clothGenerated) ++g_apiClothGenerated;
        else ++g_apiClothApprox;
    }
    if (!slot.albedo && g_curPS.albedoStage >= 0 && g_curPS.albedoStage < 8)
        slot.albedo = g_curTexture[g_curPS.albedoStage];
    if (!slot.albedo) slot.albedo = g_curTexture[0];
    slot.alphaTestEnable  = ShadowGetRS(dev, D3DRS_ALPHATESTENABLE);
    slot.alphaFunc        = ShadowGetRS(dev, D3DRS_ALPHAFUNC);
    slot.alphaRef         = ShadowGetRS(dev, D3DRS_ALPHAREF);
    slot.alphaBlendEnable = ShadowGetRS(dev, D3DRS_ALPHABLENDENABLE);
    slot.srcBlend         = ShadowGetRS(dev, D3DRS_SRCBLEND);
    slot.destBlend        = ShadowGetRS(dev, D3DRS_DESTBLEND);
    P.slots.push_back(slot);
}


// ---------------------------------------------------------------------------------------------
// Write ANY game texture to a DDS, so an API material can name it.
//
// The character atlas had its own path because the snoop already held its pixels. Every OTHER part
// of the character - head, hair, clothing, accessories - carries its own texture, and pointing all
// of them at the atlas is why they came out wrong. From the frame dump those are 4923C980,
// 16381958, 4923F2A0 and about sixteen more; only the body uses 3BD233F0.
//
// Compressed formats are copied through UNCHANGED rather than decoded: a DDS can carry DXT blocks
// directly, so the fourCC is simply set and the bytes written. That avoids writing a block decoder
// and keeps the texture bit-exact. Uncompressed BGRA is written as before, with alpha forced to
// 255 for X8R8G8B8 whose alpha byte is undefined.
//
// Cached by texture pointer: each is dumped once and the path reused.
struct DumpedTex { IDirect3DBaseTexture9* tex; char path[MAX_PATH]; };
std::vector<DumpedTex> g_dumpedTex;
unsigned g_texDumpOk = 0, g_texDumpFail = 0;

bool DumpTextureDds(IDirect3DBaseTexture9* base, const char** outPath) {
    if (!base) return false;
    for (size_t i = 0; i < g_dumpedTex.size(); ++i)
        if (g_dumpedTex[i].tex == base) {
            if (!g_dumpedTex[i].path[0]) return false;      // known-bad, do not retry
            *outPath = g_dumpedTex[i].path;
            return true;
        }

    DumpedTex entry{};
    entry.tex = base;
    g_dumpedTex.push_back(entry);
    DumpedTex& slot = g_dumpedTex.back();

    IDirect3DTexture9* tex = nullptr;
    if (base->GetType() != D3DRTYPE_TEXTURE ||
        FAILED(base->QueryInterface(__uuidof(IDirect3DTexture9),
                                    reinterpret_cast<void**>(&tex))) || !tex) {
        ++g_texDumpFail;
        return false;
    }

    D3DSURFACE_DESC d{};
    if (FAILED(tex->GetLevelDesc(0, &d)) || !d.Width || !d.Height) {
        tex->Release(); ++g_texDumpFail; return false;
    }
    const DWORD levels = tex->GetLevelCount();
    unsigned blockBytes = 0, fourCC = 0;
    switch (d.Format) {
        case D3DFMT_DXT1: blockBytes = 8;  fourCC = MAKEFOURCC('D','X','T','1'); break;
        case D3DFMT_DXT3: blockBytes = 16; fourCC = MAKEFOURCC('D','X','T','3'); break;
        case D3DFMT_DXT5: blockBytes = 16; fourCC = MAKEFOURCC('D','X','T','5'); break;
        case D3DFMT_A8R8G8B8:
        case D3DFMT_X8R8G8B8: break;
        default:
            Log("remix api: texture %p has format %u, which this dumper does not write - the part "
                "using it keeps the atlas", static_cast<void*>(base),
                static_cast<unsigned>(d.Format));
            tex->Release(); ++g_texDumpFail; return false;
    }

    char dir[MAX_PATH] = {0};
    if (!GetModuleFileNameA(GetModuleHandleW(nullptr), dir, MAX_PATH)) {
        tex->Release(); ++g_texDumpFail; return false;
    }
    char* sl = strrchr(dir, '\\');
    if (sl) *(sl + 1) = 0;
    char path[MAX_PATH];
    sprintf_s(path, "%ssr3-remix-tex-%08X.dds", dir, static_cast<unsigned>(
        reinterpret_cast<uintptr_t>(base)));

    FILE* f = nullptr;
    if (fopen_s(&f, path, "wb") != 0 || !f) { tex->Release(); ++g_texDumpFail; return false; }

    unsigned hdr[32] = {0};
    hdr[0] = 124;
    hdr[1] = 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | (fourCC ? 0x80000u : 0x8u);
    hdr[2] = d.Height;
    hdr[3] = d.Width;
    hdr[4] = fourCC ? ((d.Width + 3) / 4) * ((d.Height + 3) / 4) * blockBytes : d.Width * 4;
    hdr[6] = levels;
    hdr[18] = 32;
    if (fourCC) {
        hdr[19] = 0x4;              // DDPF_FOURCC
        hdr[20] = fourCC;
    } else {
        hdr[19] = 0x1 | 0x40;       // ALPHAPIXELS | RGB
        hdr[21] = 32;
        hdr[22] = 0x00FF0000;
        hdr[23] = 0x0000FF00;
        hdr[24] = 0x000000FF;
        hdr[25] = 0xFF000000;
    }
    hdr[26] = 0x1000 | (levels > 1 ? (0x400000 | 0x8) : 0);
    const unsigned magic = 0x20534444;
    fwrite(&magic, 4, 1, f);
    fwrite(hdr, 4, 31, f);

    bool ok = true;
    // Is level 0 actually READABLE? A DEFAULT-pool or render-target texture can return SUCCESS
    // from LockRect and hand back zeroes, and we then wrote a fully black DDS and used it as an
    // albedo. That is worse than not dumping at all, because a black texture looks like a
    // deliberate result. Measured 2026-09-03: a 512x512 dump was black from end to end.
    double lvl0 = 0.0;
    unsigned lvl0n = 0;
    const bool forceAlpha = (d.Format == D3DFMT_X8R8G8B8);
    for (DWORD lv = 0; lv < levels && ok; ++lv) {
        D3DSURFACE_DESC ld{};
        D3DLOCKED_RECT lr{};
        if (FAILED(tex->GetLevelDesc(lv, &ld)) ||
            FAILED(tex->LockRect(lv, &lr, nullptr, D3DLOCK_READONLY)) || !lr.pBits) {
            ok = false;
            break;
        }
        const unsigned rows = fourCC ? ((ld.Height + 3) / 4) : ld.Height;
        const unsigned rowBytes = fourCC ? (((ld.Width + 3) / 4) * blockBytes) : (ld.Width * 4);
        std::vector<unsigned char> line;
        try { line.resize(rowBytes); } catch (...) { ok = false; }
        for (unsigned y = 0; y < rows && ok; ++y) {
            const unsigned char* src =
                static_cast<const unsigned char*>(lr.pBits) + static_cast<size_t>(y) * lr.Pitch;
            memcpy(line.data(), src, rowBytes);
            if (forceAlpha)
                for (unsigned x = 3; x < rowBytes; x += 4) line[x] = 255;
            if (lv == 0)
                for (unsigned x = 0; x < rowBytes; x += 16) { lvl0 += line[x]; ++lvl0n; }
            if (fwrite(line.data(), 1, rowBytes, f) != rowBytes) ok = false;
        }
        tex->UnlockRect(lv);
    }
    fclose(f);
    tex->Release();
    if (ok && lvl0n && (lvl0 / lvl0n) < 0.5) {
        remove(path);
        ++g_texDumpFail;
        Log("remix api: texture %p read back EMPTY (%ux%u) - it cannot be locked for real, so the "
            "dump is discarded rather than shipping a black albedo", static_cast<void*>(base),
            d.Width, d.Height);
        return false;
    }
    if (!ok) {
        ++g_texDumpFail;
        Log("remix api: could not read texture %p (a DEFAULT-pool texture cannot be locked) - the "
            "part using it keeps the atlas", static_cast<void*>(base));
        return false;
    }
    strncpy_s(slot.path, path, _TRUNCATE);
    *outPath = slot.path;
    ++g_texDumpOk;
    Log("remix api: dumped texture %p as %s (%ux%u, %u levels, %s)", static_cast<void*>(base),
        path, d.Width, d.Height, levels, fourCC ? "compressed, copied through" : "BGRA8");
    return true;
}



// ---------------------------------------------------------------------------------------------
// CPU BAKE of SR3's customisation recipe.
//
// The GPU route is closed: driving a render target through the Remix device crashed its server
// twice at the same address, with the bake draw hidden inside an occlusion query and everything
// restored. So the recipe is evaluated here instead.
//
// The recipe, read out of ir_sr3npcclothfull_c shader[8] and recorded above ShaderInfo:
//
//     p    = Pattern_Map sampled with TEXCOORD1        (NOT TEXCOORD0 - that is the diffuse set)
//     sum  = p.r + p.g + p.b
//     dev  = |p.r-sum/3| + |p.g-sum/3| + |p.b-sum/3|
//     test = sum - (dev*165.016495 + 256)/255
//     test <  0 -> albedo = p.r^2.2*Diffuse_Color_a + p.g^2.2*_b + p.b^2.2*_c
//     test >= 0 -> albedo = saturate((p - 0.372549) * 1.59375)^2.2
//     result    = albedo * Tint_color
//
// The second branch is what lets trim and skin escape being tinted: a texel that is roughly grey
// takes its own colour instead of the three customisation colours.
//
// Output is in TEXCOORD0 space, because that is the space the mesh's albedo is sampled in.

// --- DXT decode. Pattern maps are compressed, so the blocks have to be expanded to read them. ---
void DecodeDxtBlockColour(const unsigned char* b, unsigned char out[16][4], bool dxt1) {
    const unsigned short c0 = static_cast<unsigned short>(b[0] | (b[1] << 8));
    const unsigned short c1 = static_cast<unsigned short>(b[2] | (b[3] << 8));
    unsigned char col[4][3];
    auto expand = [](unsigned short c, unsigned char* o) {
        o[0] = static_cast<unsigned char>(((c >> 11) & 31) * 255 / 31);   // r
        o[1] = static_cast<unsigned char>(((c >> 5) & 63) * 255 / 63);    // g
        o[2] = static_cast<unsigned char>((c & 31) * 255 / 31);           // b
    };
    expand(c0, col[0]);
    expand(c1, col[1]);
    const bool fourColour = !dxt1 || c0 > c1;
    for (int k = 0; k < 3; ++k) {
        if (fourColour) {
            col[2][k] = static_cast<unsigned char>((2 * col[0][k] + col[1][k]) / 3);
            col[3][k] = static_cast<unsigned char>((col[0][k] + 2 * col[1][k]) / 3);
        } else {
            col[2][k] = static_cast<unsigned char>((col[0][k] + col[1][k]) / 2);
            col[3][k] = 0;
        }
    }
    const unsigned bits = static_cast<unsigned>(b[4]) | (static_cast<unsigned>(b[5]) << 8) |
                          (static_cast<unsigned>(b[6]) << 16) | (static_cast<unsigned>(b[7]) << 24);
    for (int i = 0; i < 16; ++i) {
        const unsigned sel = (bits >> (i * 2)) & 3u;
        out[i][2] = col[sel][0];   // stored BGRA
        out[i][1] = col[sel][1];
        out[i][0] = col[sel][2];
        // DXT1's 3-colour mode (c0 <= c1) makes index 3 the TRANSPARENT texel. It was written
        // as opaque black, which is what turned a garment's cutout into black squares. Its colour
        // is left black - that is what the block encodes - and only the alpha says "not there".
        out[i][3] = (dxt1 && !fourColour && sel == 3 && g_settings.clothCutout) ? 0 : 255;
    }
}

// Whole texture level 0 to BGRA. Handles DXT1/3/5 and the uncompressed 32-bit formats.
bool TextureToBgra(IDirect3DBaseTexture9* base, std::vector<unsigned char>& out,
                   UINT& w, UINT& h) {
    if (!base || base->GetType() != D3DRTYPE_TEXTURE) return false;
    IDirect3DTexture9* tex = nullptr;
    if (FAILED(base->QueryInterface(__uuidof(IDirect3DTexture9),
                                    reinterpret_cast<void**>(&tex))) || !tex)
        return false;
    D3DSURFACE_DESC d{};
    D3DLOCKED_RECT lr{};
    bool ok = SUCCEEDED(tex->GetLevelDesc(0, &d)) && d.Width && d.Height &&
              SUCCEEDED(tex->LockRect(0, &lr, nullptr, D3DLOCK_READONLY)) && lr.pBits;
    if (!ok) { tex->Release(); return false; }
    w = d.Width;
    h = d.Height;
    try {
        out.assign(static_cast<size_t>(w) * h * 4, 0);
    } catch (...) { tex->UnlockRect(0); tex->Release(); return false; }

    const unsigned char* src = static_cast<const unsigned char*>(lr.pBits);
    if (d.Format == D3DFMT_A8R8G8B8 || d.Format == D3DFMT_X8R8G8B8) {
        for (UINT y = 0; y < h; ++y)
            memcpy(&out[static_cast<size_t>(y) * w * 4], src + static_cast<size_t>(y) * lr.Pitch,
                   static_cast<size_t>(w) * 4);
    } else if (d.Format == D3DFMT_DXT1 || d.Format == D3DFMT_DXT3 || d.Format == D3DFMT_DXT5) {
        const bool dxt1 = (d.Format == D3DFMT_DXT1);
        const unsigned blockBytes = dxt1 ? 8u : 16u;
        const unsigned bw = (w + 3) / 4, bh = (h + 3) / 4;
        for (unsigned by = 0; by < bh; ++by)
            for (unsigned bx = 0; bx < bw; ++bx) {
                const unsigned char* blk = src + static_cast<size_t>(by) * lr.Pitch +
                                           static_cast<size_t>(bx) * blockBytes;
                unsigned char texels[16][4];
                DecodeDxtBlockColour(dxt1 ? blk : blk + 8, texels, dxt1);
                // ALPHA. Skipping this wrote 255 everywhere, which is what made a decal's
                // surround opaque. DXT3 stores 4 bits per texel; DXT5 stores two endpoints and
                // a 3-bit index per texel.
                if (d.Format == D3DFMT_DXT3) {
                    for (int i = 0; i < 16; ++i) {
                        const unsigned char n = blk[i >> 1];
                        const unsigned a4 = (i & 1) ? (n >> 4) : (n & 0xF);
                        texels[i][3] = static_cast<unsigned char>(a4 * 17);
                    }
                } else if (d.Format == D3DFMT_DXT5) {
                    const unsigned char a0 = blk[0], a1 = blk[1];
                    unsigned char av[8];
                    av[0] = a0; av[1] = a1;
                    if (a0 > a1)
                        for (int k = 1; k < 7; ++k)
                            av[k + 1] = static_cast<unsigned char>(((7 - k) * a0 + k * a1) / 7);
                    else {
                        for (int k = 1; k < 5; ++k)
                            av[k + 1] = static_cast<unsigned char>(((5 - k) * a0 + k * a1) / 5);
                        av[6] = 0; av[7] = 255;
                    }
                    unsigned long long bits = 0;
                    for (int k = 0; k < 6; ++k)
                        bits |= static_cast<unsigned long long>(blk[2 + k]) << (8 * k);
                    for (int i = 0; i < 16; ++i)
                        texels[i][3] = av[(bits >> (i * 3)) & 7u];
                }
                for (int i = 0; i < 16; ++i) {
                    const unsigned px = bx * 4 + (i & 3), py = by * 4 + (i >> 2);
                    if (px >= w || py >= h) continue;
                    memcpy(&out[(static_cast<size_t>(py) * w + px) * 4], texels[i], 4);
                }
            }
    } else {
        ok = false;
    }
    tex->UnlockRect(0);
    tex->Release();
    return ok;
}


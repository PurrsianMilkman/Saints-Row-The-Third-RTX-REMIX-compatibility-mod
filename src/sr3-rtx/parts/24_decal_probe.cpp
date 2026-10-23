// parts/24_decal_probe.cpp - the decal probe.
// NOT a standalone translation unit: #included, in order, by ../sr3rtx.cpp inside its anonymous
// namespace. Originally sr3rtx.cpp lines 19986-21428 (old file:line citations map here).
// ---------------------------------------------------------------- decal probe
//
// Building decals - the sign/logo/poster quads named by a "Decal" sampler at the shader's lowest
// register - render as a flat quad showing a fine repeating stripe pattern instead of their
// image. Two fixes have already shipped (decalUvFromDeclaredSet, decalAlphaFromTexture) and
// neither changed how it looks. Depth, blinking, albedo churn, the alpha path, the texcoord SET
// these shaders read (set 0, which is what is converted) and the tiling lookup have all been
// measured and ruled out. What has NEVER been measured is the only thing left that decides what
// appears on the quad: which texture is actually bound as albedo, and what coordinate range is
// fed to it. This probe measures exactly those two things and changes no render state.

// Dump the bound albedo's top mip beside the shim's other DDS dumps, reusing TextureToBgra (which
// decodes each format's OWN alpha channel - see its DXT3/DXT5 branches) and WriteBgraDds (which
// writes that alpha through unchanged). An earlier version of WriteBgraDds fabricated alpha 255
// and hid a real cutout finding for a whole day; this path never does that.
void DumpDecalProbeTexture(IDirect3DBaseTexture9* tex, unsigned n) {
    std::vector<unsigned char> bgra;
    UINT w = 0, h = 0;
    if (TextureToBgra(tex, bgra, w, h)) {
        char dir[MAX_PATH] = {0};
        GetModuleFileNameA(GetModuleHandleW(nullptr), dir, MAX_PATH);
        if (char* slash = strrchr(dir, '\\')) *(slash + 1) = 0;
        char path[MAX_PATH];
        sprintf_s(path, "%ssr3-remix-decalprobe-%u-%ux%u.dds", dir, n, w, h);
        if (WriteBgraDds(path, w, h, bgra.data()))
            Log("    dumped albedo #%u -> %s (%ux%u, real alpha channel)", n, path, w, h);
        else
            Log("    decal probe #%u: WriteBgraDds FAILED for %s", n, path);
        return;
    }
    // TextureToBgra failed either because the lock itself failed or because the format is one it
    // does not decode (only A8R8G8B8/X8R8G8B8/DXT1/DXT3/DXT5 are handled) - say which, with the
    // real HRESULT, instead of failing silently.
    HRESULT hr = E_FAIL;
    if (tex && tex->GetType() == D3DRTYPE_TEXTURE) {
        IDirect3DTexture9* t2 = static_cast<IDirect3DTexture9*>(tex);
        D3DLOCKED_RECT lr{};
        hr = t2->LockRect(0, &lr, nullptr, D3DLOCK_READONLY);
        if (SUCCEEDED(hr)) t2->UnlockRect(0);
    }
    if (SUCCEEDED(hr))
        Log("    decal probe #%u: could not dump albedo - LockRect succeeded (hr=0x%08lX) but "
            "TextureToBgra does not decode this format", n, static_cast<unsigned long>(hr));
    else
        Log("    decal probe #%u: could not dump albedo - LockRect FAILED, hr=0x%08lX", n,
            static_cast<unsigned long>(hr));
}

// STRICT TARGET, 2026-09-18: the ORIGINAL arming test (decal-named albedo, the winning
// sampler by AlbedoRank, which is also the lowest register whenever nothing outranks it - see
// CHANGE 2, 2026-09-18, for why arming was rewritten to scan every sampler the shader names
// instead) let through the wrong population. The run this was written against reported 8
// identical blocks, all the same
// particle billboard: verts=20 prims=10 at(0.00 0.00 0.00), usesObjTM=0, and then "REFUSED:
// stream 0 is DYNAMIC - not safe to read-lock" for every one of them. A particle's corners are
// computed on the CPU into a buffer the game rewrites every frame and sit at the origin before
// any object transform - there is nothing here this probe could ever read. The target, a
// BUILDING decal, is the opposite on every count: STATIC, INDEXED, and placed by objTM away
// from the origin (historically v=28336 p=2|4 at a real world position). Testing for that
// shape - not just the sampler name - is what actually reaches it.
//
// Counted separately rather than folded into one bucket, because a guard that refuses silently
// is this project's most repeated failure: if the probe goes back to reporting nothing, these
// counters say which test is doing the excluding instead of costing another session of
// guessing. Reported periodically in the PROFILE block below (search DECAL PROBE ARMING).
unsigned g_decalProbeCandidates = 0;         // passed the sampler test - would have armed under the OLD test alone
unsigned g_decalProbeRejectDynamic = 0;      // stream 0 is a DYNAMIC buffer (particles, overwhelmingly)
unsigned g_decalProbeRejectNotIndexed = 0;   // not an indexed draw
unsigned g_decalProbeRejectOrigin = 0;       // no object transform, or objTM places it AT the origin

void ProbeDecalDraw() {
    if (!g_settings.decalProbe || g_decalProbeReports >= 8) return;
    // CHANGE 2, 2026-09-18: this used to arm only when the CHOSEN albedo sampler (the
    // AlbedoRank winner) named "Decal", which is the Decal map on just 19 of the 230
    // decal-material draws in the reference capture - AlbedoRank scores Diffuse 100 against
    // Decal_Map's 90, so the Diffuse map wins the albedo slot on the other 211 (175
    // rank=100 low='Diffuse_MapSampler'@0, 36 more @1) and this probe never saw them. Every
    // report this probe had ever produced came from the 19, in practice one car probed
    // twice. Arm instead on whether the shader NAMES a Decal sampler ANYWHERE, using the
    // reflected per-stage sampler list (samplerName[]) rather than the chosen albedo, so the
    // 211 draws where Diffuse won are reached too.
    int decalStage = -1;
    for (int st = 0; st < 8; ++st) {
        if (g_curPS.samplerName[st][0] && StrStrIA(g_curPS.samplerName[st], "Decal")) {
            decalStage = st;
            break;
        }
    }
    if (decalStage < 0) return;
    if (!g_lastEffectiveAlbedo) return;   // nothing bound - a different, already-counted fault
    ++g_decalProbeCandidates;
    // A candidate must also LOOK LIKE a world decal, not merely name one. decalProbeStrictTarget
    // is ON by default; OFF restores the old, permissive arming test (candidates are still
    // counted either way, but nothing is rejected below when it is OFF).
    if (g_settings.decalProbeStrictTarget) {
        // Same guard ProbeShapes/ProbeHudDraw/ProbeSkyDraw/GetBaseMesh already apply before a
        // read-lock: a DYNAMIC buffer is not safe to read, and this is exactly the class of
        // draw (particles) that buffer usage identifies.
        if (IsDynamicVB(g_stream0)) { ++g_decalProbeRejectDynamic; return; }
        if (!g_curDrawIndexed) { ++g_decalProbeRejectNotIndexed; return; }
        // Same fields the neighbouring "DECAL/PARTICLE" log line already prints (usesObjTM and
        // the world translation from g_lastWorld, stashed by BeginFFP for this exact draw): a
        // draw with no object transform at all, or whose objTM still places it exactly at the
        // origin, is not placed in the world - which is what a particle billboard looks like
        // and a building decal never does.
        if (!g_curVS.usesObjTM ||
            (g_lastWorld._41 == 0.0f && g_lastWorld._42 == 0.0f && g_lastWorld._43 == 0.0f)) {
            ++g_decalProbeRejectOrigin;
            return;
        }
    }
    ++g_decalProbeReports;
    const unsigned n = g_decalProbeReports;
    IDirect3DBaseTexture9* albedo = g_lastEffectiveAlbedo;

    // ---- 1: identity ----
    // CHANGE 2, 2026-09-18: also print which texture actually WON the albedo (already
    // named above by albedoSampler/stage) against which register the Decal-named sampler
    // this draw armed on actually sits at, so the report itself says whether this is one of
    // the 211 draws where Diffuse won the albedo or one of the 19 where Decal did.
    char idLine[320];
    _snprintf_s(idLine, _TRUNCATE,
                "albedoSampler='%s' lowest='%s'@%d stage=%d | decalSampler='%s'@%d %s | "
                "verts=%u prims=%u at(%.2f %.2f %.2f) | bound=%p",
                g_curPS.albedoSampler, g_curPS.lowestSampler, g_curPS.lowestSamplerReg,
                g_lastEffectiveAlbedoStage, g_curPS.samplerName[decalStage], decalStage,
                (decalStage == g_lastEffectiveAlbedoStage) ? "(WON the albedo)"
                                                            : "(albedo went to a different sampler)",
                g_curDrawVertexCount, g_curDrawPrimCount,
                g_lastWorld._41, g_lastWorld._42, g_lastWorld._43, static_cast<void*>(albedo));

    // ---- 2: the texture, from GetLevelDesc on the bound albedo ----
    char texLine[192];
    D3DSURFACE_DESC d{};
    if (albedo->GetType() == D3DRTYPE_TEXTURE &&
        SUCCEEDED(static_cast<IDirect3DTexture9*>(albedo)->GetLevelDesc(0, &d))) {
        const char* fmtName = (d.Format == D3DFMT_A8R8G8B8) ? "A8R8G8B8"
                             : (d.Format == D3DFMT_X8R8G8B8) ? "X8R8G8B8"
                             : (d.Format == D3DFMT_DXT1)     ? "DXT1"
                             : (d.Format == D3DFMT_DXT3)     ? "DXT3"
                             : (d.Format == D3DFMT_DXT5)     ? "DXT5" : "?";
        _snprintf_s(texLine, _TRUNCATE,
                    "%ux%u fmt=%u(%s) mips=%u pool=%u(0=DEFAULT 1=MANAGED 2=SYSTEMMEM 3=SCRATCH) "
                    "usage=%#lx",
                    d.Width, d.Height, static_cast<unsigned>(d.Format), fmtName,
                    static_cast<unsigned>(albedo->GetLevelCount()), static_cast<unsigned>(d.Pool),
                    static_cast<unsigned long>(d.Usage));
    } else {
        _snprintf_s(texLine, _TRUNCATE, "GetLevelDesc FAILED (type=%d)",
                    static_cast<int>(albedo->GetType()));
    }

    // ---- 3: the scale actually applied, published by SetupTextureStages for this exact draw ----
    char scaleLine[96];
    _snprintf_s(scaleLine, _TRUNCATE, "su=%.6f sv=%.6f transformflags=%s",
                g_decalProbeSu, g_decalProbeSv, g_decalProbeCount2 ? "COUNT2" : "DISABLE");

    // ---- 4 & 5: raw source coordinates and the resulting UV, over the vertices the draw
    // ACTUALLY INDEXES - not the [g_curDrawFirstVertex, +g_curDrawVertexCount) window, which can
    // hold 28,336 vertices while the draw itself indexes only 2-4 triangles out of it. That
    // distinction - window versus what is actually indexed - is the whole point of this probe.
    char rawLine[288] = "index buffer unavailable, not indexed, or texcoord is not SHORT2";
    char uvLine[192] = "";
    // 2026-09-18 AUDIT FIX: this gate used to test set 0 only (g_curLayout.texcoordType), so
    // every FIX A set-1 draw (decalonly/fauxinterior - exactly the population this probe exists
    // to measure) printed the generic "texcoord is not SHORT2" default and was never probed. Same
    // set decision SetupTextureStages and UvBufferFor already make.
    const int decalUvSet = DecalUvSourceSet();
    const int decalSrcType =
        (decalUvSet == 1) ? g_curLayout.texcoord1Type : g_curLayout.texcoordType;
    const int decalSrcOffset =
        (decalUvSet == 1) ? g_curLayout.texcoord1Offset : g_curLayout.texcoordOffset;
    const bool decalShapeOk = g_curDrawIndexed && g_curIB && g_curDrawPrimCount && g_stream0 &&
        g_stream0Stride &&
        (g_curDrawPrimType == D3DPT_TRIANGLELIST || g_curDrawPrimType == D3DPT_TRIANGLESTRIP) &&
        decalSrcType == D3DDECLTYPE_SHORT2 && decalSrcOffset >= 0 &&
        VertexRangeFits(g_stream0, g_stream0Offset, g_curDrawFirstVertex, g_curDrawVertexCount,
                        g_stream0Stride);
    // Same two guards ProbeShapes, ProbeHudDraw, ProbeSkyDraw and GetBaseMesh all apply before
    // reading a vertex buffer back, and the same stride check UvBufferFor applies before
    // trusting a texcoord offset - this probe had neither. A DYNAMIC buffer is not safe to
    // read-lock (the driver may still be mid-write, or the read stalls the pipeline), and an
    // offset within 4 bytes of the end of the stride reads past the vertex into the next one.
    // Named here rather than folded into a silent refusal: this project has been bitten
    // repeatedly by a guard that refuses without saying why.
    D3DVERTEXBUFFER_DESC decalVbd{};
    const bool decalDynamic = decalShapeOk && SUCCEEDED(g_stream0->GetDesc(&decalVbd)) &&
        (decalVbd.Usage & D3DUSAGE_DYNAMIC) != 0;
    const bool decalTexcoordFits = !decalShapeOk ||
        (decalSrcOffset + 4 <= static_cast<int>(g_stream0Stride));
    if (decalShapeOk && decalDynamic) {
        strcpy_s(rawLine, "REFUSED: stream 0 is DYNAMIC - not safe to read-lock (same guard as "
                          "ProbeShapes/ProbeHudDraw/ProbeSkyDraw/GetBaseMesh)");
    } else if (decalShapeOk && !decalTexcoordFits) {
        _snprintf_s(rawLine, _TRUNCATE,
                    "REFUSED: texcoordOffset(%d)+4 exceeds stride %u (same check as "
                    "UvBufferFor)", decalSrcOffset, g_stream0Stride);
    } else if (decalShapeOk) {
        const UINT nIdx = (g_curDrawPrimType == D3DPT_TRIANGLESTRIP) ? g_curDrawPrimCount + 2
                                                                      : g_curDrawPrimCount * 3;
        std::vector<unsigned> raw;
        {
            const bool wasInt = g_internal;
            g_internal = true;
            g_internalThread = GetCurrentThreadId();
            D3DINDEXBUFFER_DESC ibd{};
            if (SUCCEEDED(g_curIB->GetDesc(&ibd)) && ibd.Size) {
                const UINT isz = (ibd.Format == D3DFMT_INDEX32) ? 4 : 2;
                void* im = nullptr;
                if (static_cast<unsigned long long>(g_curDrawStartIndex) * isz +
                            static_cast<unsigned long long>(nIdx) * isz <=
                        ibd.Size &&
                    SUCCEEDED(g_curIB->Lock(g_curDrawStartIndex * isz, nIdx * isz, &im,
                                            D3DLOCK_READONLY)) &&
                    im) {
                    try {
                        raw.resize(nIdx);
                        for (UINT i = 0; i < nIdx; ++i)
                            raw[i] = (isz == 4) ? static_cast<const unsigned*>(im)[i]
                                                : static_cast<const unsigned short*>(im)[i];
                    } catch (...) { raw.clear(); }
                    g_curIB->Unlock();
                }
            }
            g_internal = wasInt;
        }

        if (raw.empty()) {
            strcpy_s(rawLine, "index buffer lock failed");
        } else {
            int rMinU = 32767, rMaxU = -32768, rMinV = 32767, rMaxV = -32768;
            long long sumU = 0, sumV = 0;
            unsigned used = 0;
            short f4u[4] = {0, 0, 0, 0}, f4v[4] = {0, 0, 0, 0};
            unsigned f4n = 0;
            const bool wasInt = g_internal;
            g_internal = true;
            g_internalThread = GetCurrentThreadId();
            void* vm = nullptr;
            if (SUCCEEDED(g_stream0->Lock(g_stream0Offset + g_curDrawFirstVertex * g_stream0Stride,
                                          g_curDrawVertexCount * g_stream0Stride, &vm,
                                          D3DLOCK_READONLY | D3DLOCK_NOSYSLOCK)) &&
                vm) {
                const unsigned char* vb = static_cast<const unsigned char*>(vm);
                for (UINT i = 0; i < nIdx; ++i) {
                    // Indices are relative to baseVertex, the way minIndex is - same arithmetic
                    // OcclMeshFor uses. The locked buffer holds only the window starting at
                    // g_curDrawFirstVertex, so rel is this index's offset into THAT window.
                    const long long rel = static_cast<long long>(g_curDrawBaseVertex) +
                                         static_cast<long long>(raw[i]) -
                                         static_cast<long long>(g_curDrawFirstVertex);
                    if (rel < 0 || rel >= static_cast<long long>(g_curDrawVertexCount)) continue;
                    const short* uv = reinterpret_cast<const short*>(
                        vb + static_cast<size_t>(rel) * g_stream0Stride +
                        decalSrcOffset);
                    if (uv[0] < rMinU) rMinU = uv[0];
                    if (uv[0] > rMaxU) rMaxU = uv[0];
                    if (uv[1] < rMinV) rMinV = uv[1];
                    if (uv[1] > rMaxV) rMaxV = uv[1];
                    sumU += uv[0];
                    sumV += uv[1];
                    ++used;
                    if (f4n < 4) { f4u[f4n] = uv[0]; f4v[f4n] = uv[1]; ++f4n; }
                }
                g_stream0->Unlock();
            }
            g_internal = wasInt;

            if (!used) {
                strcpy_s(rawLine, "every index fell outside its own vertex window");
            } else {
                char f4Txt[96] = "";
                int off = 0;
                for (unsigned i = 0; i < f4n && off >= 0 && off < 90; ++i) {
                    const int wn = _snprintf_s(f4Txt + off, sizeof(f4Txt) - off, _TRUNCATE,
                                               "%s(%d,%d)", i ? " " : "", f4u[i], f4v[i]);
                    if (wn < 0) break;
                    off += wn;
                }
                _snprintf_s(rawLine, _TRUNCATE,
                            "%u/%u indices in-window | u %d..%d (mean %.1f)  v %d..%d (mean "
                            "%.1f) | first4 %s",
                            used, nIdx, rMinU, rMaxU, static_cast<double>(sumU) / used, rMinV,
                            rMaxV, static_cast<double>(sumV) / used, f4Txt);

                // Raw * the applied scale. su/sv are the 1/1024-and-tiling scale SetupTextureStages
                // computed above - always positive in every case this shim builds them from - so
                // multiplying preserves min/max order without needing a sort.
                const float uMinF = rMinU * g_decalProbeSu, uMaxF = rMaxU * g_decalProbeSu;
                const float vMinF = rMinV * g_decalProbeSv, vMaxF = rMaxV * g_decalProbeSv;
                char f4uv[128] = "";
                off = 0;
                for (unsigned i = 0; i < f4n && off >= 0 && off < 120; ++i) {
                    const int wn = _snprintf_s(f4uv + off, sizeof(f4uv) - off, _TRUNCATE,
                                               "%s(%.4f,%.4f)", i ? " " : "",
                                               f4u[i] * g_decalProbeSu, f4v[i] * g_decalProbeSv);
                    if (wn < 0) break;
                    off += wn;
                }
                _snprintf_s(uvLine, _TRUNCATE,
                            "u %.4f..%.4f  v %.4f..%.4f | first4 %s | spans %.2f x %.2f texture "
                            "repeats",
                            uMinF, uMaxF, vMinF, vMaxF, f4uv, uMaxF - uMinF, vMaxF - vMinF);
            }
        }
    }

    Log("DECAL PROBE #%u:\n"
        "    identity: %s\n"
        "    texture:  %s\n"
        "    scale:    %s\n"
        "    raw:      %s\n"
        "    uv:       %s",
        n, idLine, texLine, scaleLine, rawLine, uvLine[0] ? uvLine : "(not computed)");

    if (g_settings.decalProbeDumpTexture) DumpDecalProbeTexture(albedo, n);
}

// The device, kept for work that happens at Present rather than inside a draw. The character
// builds drain from Present and had no device to hand, which is why the CPU baker could only ever
// write a FILE - and a file is no use to the game's own converted draw, which needs a bound
// D3D texture.
IDirect3DDevice9* g_presentDevice = nullptr;

constexpr UINT kCpuBakeSize = 512;
unsigned g_cpuBakeCount = 0, g_cpuBakeFail = 0;
// Triangles that reached outside the unit square, and triangles so far outside that the box had
// to be bounded. The first is normal for a tiling garment; the second means the UVs are wrong
// after all, and the two must not be confused for each other.
unsigned g_cpuBakeWrapped = 0, g_cpuBakeClipped = 0;
unsigned g_clothBakedRegistered = 0;
unsigned g_hairGenerated = 0;

inline float SrgbPow22(float v) { return powf(v < 0.0f ? 0.0f : v, 2.2f); }

// Rasterise the slot's triangles into TEXCOORD0 space, evaluating the recipe per texel.
// THE RECIPE, read from the PLAYER's own cloth shader (ir_at_sr3pccloth_bs shader[8]):
//
//     texld r4, r4, s0            ; Diffuse_Map
//     mul   r1.yzw, r4.xxyz, c11  ; diffuse * Diffuse_Color
//     texld r5, v1, s2            ; Pattern_Map, at TEXCOORD1
//     r7/r8/r9 = pow(Diffuse_Color_a/b/c, c12.w)    with c12.w = 2.2
//     lrp r6, r5.z, r9, 1.0       ; start at WHITE, blend toward C by pattern.b
//     lrp r9, r5.y, r8, r6        ;                 toward B by pattern.g
//     lrp r6, r5.x, r7, r9        ;                 toward A by pattern.r
//     mul r5.xyz, r1.yzw, r6      ; DIFFUSE * that
//
// Two things the earlier implementation got wrong, both because it came from a DIFFERENT shader
// (ir_sr3npcclothfull_c, which has a sum/deviation branch this one does not):
//   it never multiplied by the DIFFUSE MAP, and
//   it treated a black mask region as "all three colours at zero" - i.e. black - when the lerp
//     chain leaves such a region at WHITE, meaning "keep the diffuse unchanged".
// That is exactly the black bands and the cyan block seen on the underwear.
const char* CpuBakeCloth(const std::vector<RemixHardcodedVertex>& verts,
                         const std::vector<float>& uv2,
                         const std::vector<unsigned>& indices,
                         IDirect3DBaseTexture9* pattern,
                         IDirect3DBaseTexture9* diffuse,
                         const float* dcA, const float* dcB, const float* dcC,
                         const float* dcMul) {
    if (verts.empty() || indices.size() < 3 || uv2.size() != verts.size() * 2) return nullptr;
    std::vector<unsigned char> pat;
    UINT pw = 0, ph = 0;
    if (!TextureToBgra(pattern, pat, pw, ph)) { ++g_cpuBakeFail; return nullptr; }
    // The diffuse map is sampled in the SAME space we are baking into, so its texel for an output
    // pixel is simply that pixel's own uv - no interpolation needed.
    std::vector<unsigned char> dif;
    UINT dw = 0, dh = 0;
    const bool haveDiffuse = TextureToBgra(diffuse, dif, dw, dh);

    // Dump the INPUTS of every bake, not only the first one's pattern.
    //
    // Three hypotheses about the underwear have now died - a per-material tiling factor, a
    // different UV divide in the shader, and (only partly) the baker clipping tiled UVs away.
    // Each was reasoned from summary numbers: a mean, a coverage percentage, a uv range. The two
    // times this project has actually converged on a rendering fault quickly, it was by LOOKING
    // at the thing - the slot table that showed triangle strips, and the contact sheet that
    // showed cloth-9 was empty rather than scrambled.
    //
    // So every bake now writes what it was GIVEN alongside what it produced. A pattern that is
    // uniform means the customisation colour is uniform, and a uniform colour times a diffuse map
    // does not need a UV-space rasterisation at all - it can be done in the diffuse's own texture
    // space, where no UV assumption can break it. That is a different fix from anything tried so
    // far, and whether it applies is a question about these two images.
    {
        char pdir[MAX_PATH] = {0};
        GetModuleFileNameA(GetModuleHandleW(nullptr), pdir, MAX_PATH);
        char* psl = strrchr(pdir, 0x5C);
        if (psl) *(psl + 1) = 0;
        char ppath[MAX_PATH];
        sprintf_s(ppath, "%ssr3-remix-in-%u-pattern.dds", pdir, g_cpuBakeCount + 1);
        if (WriteBgraDds(ppath, pw, ph, pat.data()))
            Log("cloth bake #%u INPUT pattern -> %s (%ux%u)", g_cpuBakeCount + 1, ppath, pw, ph);
        if (haveDiffuse) {
            sprintf_s(ppath, "%ssr3-remix-in-%u-diffuse.dds", pdir, g_cpuBakeCount + 1);
            if (WriteBgraDds(ppath, dw, dh, dif.data()))
                Log("cloth bake #%u INPUT diffuse -> %s (%ux%u)", g_cpuBakeCount + 1, ppath,
                    dw, dh);
        }
        // Is the pattern UNIFORM? If every texel is the same, the lerp chain produces one colour
        // for the whole garment and the rasterisation is doing no work that a texture-space
        // multiply could not do more safely. Measured, not eyeballed, because it decides which
        // fix is the right one.
        bool uniform = true;
        if (pat.size() >= 4)
            for (size_t q = 4; q < pat.size(); q += 4)
                if (pat[q] != pat[0] || pat[q + 1] != pat[1] || pat[q + 2] != pat[2]) {
                    uniform = false; break;
                }
        Log("      pattern is %s (first texel BGRA %u %u %u %u)",
            uniform ? "UNIFORM - one colour for the whole garment"
                    : "spatially varying - the rasterisation is doing real work",
            pat.size() >= 4 ? pat[0] : 0, pat.size() >= 4 ? pat[1] : 0,
            pat.size() >= 4 ? pat[2] : 0, pat.size() >= 4 ? pat[3] : 0);
    }

    std::vector<unsigned char> img;
    try { img.assign(static_cast<size_t>(kCpuBakeSize) * kCpuBakeSize * 4, 0); }
    catch (...) { ++g_cpuBakeFail; return nullptr; }

    auto samplePattern = [&](float u, float v, float* rgb, float* a) {
        int x = static_cast<int>(u * static_cast<float>(pw)) % static_cast<int>(pw);
        int y = static_cast<int>(v * static_cast<float>(ph)) % static_cast<int>(ph);
        if (x < 0) x += pw;
        if (y < 0) y += ph;
        const unsigned char* p = &pat[(static_cast<size_t>(y) * pw + x) * 4];
        rgb[0] = p[2] / 255.0f;   // stored BGRA
        rgb[1] = p[1] / 255.0f;
        rgb[2] = p[0] / 255.0f;
        if (a) *a = p[3] / 255.0f;
    };

    for (size_t t = 0; t + 2 < indices.size(); t += 3) {
        const unsigned ia = indices[t], ib = indices[t + 1], ic = indices[t + 2];
        if (ia >= verts.size() || ib >= verts.size() || ic >= verts.size()) continue;
        // TEXCOORD0 is the destination space; both sets are raw shorts, so 1/1024 applies to both.
        // verts already carry TEXCOORD0 scaled to 0..1; uv2 is still raw shorts.
        const float ax = verts[ia].texcoord[0], ay = verts[ia].texcoord[1];
        const float bx = verts[ib].texcoord[0], by = verts[ib].texcoord[1];
        const float cx = verts[ic].texcoord[0], cy = verts[ic].texcoord[1];
        const float area = (bx - ax) * (cy - ay) - (cx - ax) * (by - ay);
        if (std::fabs(area) < 1e-12f) continue;
        const float inv = 1.0f / area;

        float lo0 = min(ax, min(bx, cx)), hi0 = max(ax, max(bx, cx));
        float lo1 = min(ay, min(by, cy)), hi1 = max(ay, max(by, cy));
        int x0 = static_cast<int>(lo0 * kCpuBakeSize) - 1, x1 = static_cast<int>(hi0 * kCpuBakeSize) + 1;
        int y0 = static_cast<int>(lo1 * kCpuBakeSize) - 1, y1 = static_cast<int>(hi1 * kCpuBakeSize) + 1;
        // THE DESTINATION BOX IS NOT CLAMPED TO THE TEXTURE. It used to be, and that is why the
        // underwear baked at 3% coverage while every other garment reached 29-98%.
        //
        // Its UVs run u[-1.072 2.787] v[-0.414 2.734] - roughly four tiles - and the mesh is not
        // broken for that. A tiling garment is ordinary, the game samples it with ADDRESS_WRAP,
        // and the pattern lookup in this very function has always wrapped its source with `% pw`.
        // Only the WRITE clamped, so every triangle outside the unit square was clipped away and
        // simply never contributed. 96% of this garment was discarded by four lines.
        //
        // Two hypotheses died before this one - a per-material tiling factor (no tiling constant
        // matched any part) and a different UV divide in the shader (reading it out of the
        // bytecode picked 1/8192 for the BODY and broke it, while leaving the underwear at
        // 1/1024). Both assumed the UVs were wrong. They are not: the baker's assumption that
        // UVs live in 0..1 is what was wrong.
        //
        // The barycentric test below still runs in UNWRAPPED space, so the triangle keeps its
        // true shape and a triangle straddling a tile boundary is rasterised correctly; only the
        // address it writes to wraps. That is what makes this different from taking frac() of
        // each vertex, which would tear those triangles apart.
        //
        // Bounded, because a genuinely broken UV could otherwise ask for millions of pixels: at
        // most four tiles in each direction, which is more than any real garment tiles and still
        // finite if the data is nonsense.
        const int kMaxTiles = 4;
        if (x1 - x0 > static_cast<int>(kCpuBakeSize) * kMaxTiles) {
            x1 = x0 + static_cast<int>(kCpuBakeSize) * kMaxTiles;
            ++g_cpuBakeClipped;
        }
        if (y1 - y0 > static_cast<int>(kCpuBakeSize) * kMaxTiles) {
            y1 = y0 + static_cast<int>(kCpuBakeSize) * kMaxTiles;
            ++g_cpuBakeClipped;
        }
        if (x0 < 0 || y0 < 0 || x1 > static_cast<int>(kCpuBakeSize) ||
            y1 > static_cast<int>(kCpuBakeSize))
            ++g_cpuBakeWrapped;

        for (int py = y0; py < y1; ++py)
            for (int px = x0; px < x1; ++px) {
                const float fx = (px + 0.5f) / kCpuBakeSize, fy = (py + 0.5f) / kCpuBakeSize;
                const float w0 = ((bx - fx) * (cy - fy) - (cx - fx) * (by - fy)) * inv;
                const float w1 = ((cx - fx) * (ay - fy) - (ax - fx) * (cy - fy)) * inv;
                const float w2 = 1.0f - w0 - w1;
                if (w0 < -0.002f || w1 < -0.002f || w2 < -0.002f) continue;

                // uv2 arrives already scaled to 0..1 - see the call site, which also supplies
                // the fallback for parts whose shader samples the pattern with TEXCOORD0.
                const float pu = w0 * uv2[ia * 2] + w1 * uv2[ib * 2] + w2 * uv2[ic * 2];
                const float pv = w0 * uv2[ia * 2 + 1] + w1 * uv2[ib * 2 + 1] +
                                 w2 * uv2[ic * 2 + 1];
                float p[3], pa = 1.0f;
                samplePattern(pu, pv, p, &pa);

                // Start WHITE and blend toward each customisation colour by its own channel.
                // A black mask region therefore stays white and leaves the diffuse untouched -
                // which is what the black bands should have been all along.
                float alb[3];
                for (int k = 0; k < 3; ++k) {
                    // The colours are used AS AUTHORED, not raised to 2.2.
                    //
                    // The shader does pow(colour, 2.2) to move an sRGB constant into linear light,
                    // and its result is later written out through the game's own gamma. We do the
                    // same conversion on the way OUT (the sRGB encode below), so applying it here
                    // as well counts it twice: colour a(0.40 0.00 0.21) came back as roughly
                    // (96 0 48) - the dark maroon in bake 3 - where the game shows bright pink.
                    //
                    // The diffuse texel is likewise already sRGB and is used as-is.
                    const float ca = dcA[k], cb = dcB[k], cc = dcC[k];
                    float v = 1.0f + (cc - 1.0f) * p[2];   // lrp toward C by pattern.b
                    v = v + (cb - v) * p[1];               // then toward B by pattern.g
                    v = v + (ca - v) * p[0];               // then toward A by pattern.r
                    alb[k] = v;
                }
                // ... times the diffuse map and Diffuse_Color.
                if (haveDiffuse && g_settings.clothUseDiffuse) {
                    const int dx = static_cast<int>(fx * dw) % static_cast<int>(dw);
                    const int dy = static_cast<int>(fy * dh) % static_cast<int>(dh);
                    const unsigned char* dp =
                        &dif[(static_cast<size_t>(dy < 0 ? dy + dh : dy) * dw +
                              (dx < 0 ? dx + dw : dx)) * 4];
                    alb[0] *= (dp[2] / 255.0f) * dcMul[0];
                    alb[1] *= (dp[1] / 255.0f) * dcMul[1];
                    alb[2] *= (dp[0] / 255.0f) * dcMul[2];
                    pa = dp[3] / 255.0f;
                }
                // TINT ONLY IF IT IS A COLOUR.
                //
                // The first bake produced a correct garment unwrap in flat magenta. Working it
                // back: the pattern is solid BLUE, so the recipe takes its first branch and the
                // albedo is Diffuse_Color_c = (0.40, 0.00, 0.21), a dark maroon - and then
                // Tint_color = 5.0 multiplies it to (2.0, 0, 1.05), which clamps to pure magenta.
                //
                // A value of 5 is not a colour. The same 5.0 turned up on every blended material
                // earlier. Components above 1 are therefore treated as "no tint" rather than
                // scaled, so a mis-read constant cannot destroy a correct albedo.
                // Wrap the ADDRESS, not the geometry. C++ leaves the sign of % on a negative
                // operand implementation-defined for our purposes here, so it is folded up
                // explicitly rather than trusted.
                int wx = px % static_cast<int>(kCpuBakeSize);
                int wy = py % static_cast<int>(kCpuBakeSize);
                if (wx < 0) wx += static_cast<int>(kCpuBakeSize);
                if (wy < 0) wy += static_cast<int>(kCpuBakeSize);
                unsigned char* dst = &img[(static_cast<size_t>(wy) * kCpuBakeSize + wx) * 4];
                for (int k = 0; k < 3; ++k) {
                    // A TUNING KNOB, and labelled as one.
                    //
                    // The recipe is verified correct: the exe divides the swatch by 255, the
                    // shader lerps from white toward each colour by a pattern channel, and
                    // r1.x is c6.x = 1.0 so that lerp is exactly what it looks like. Bake #2
                    // computes (84 0 0) from colour C = 0.40 and a diffuse of 0.82, which is
                    // what the shader would produce.
                    //
                    // It still looks too dark in the path traced image, which means Remix lights
                    // this mesh differently from how the game lit it - not that the albedo is
                    // wrong. That is a lighting question, not a recipe one, and it is NOT solved
                    // here. This scales the baked albedo so the result can be made usable while
                    // that stays open.
                    float c = alb[k] * g_settings.clothBrightness;
                    c = c < 0.0f ? 0.0f : (c > 1.0f ? 1.0f : c);
                    // BACK TO sRGB BEFORE STORING.
                    //
                    // The recipe works in LINEAR light - both its branches raise the pattern to
                    // the 2.2 - but this DDS is an 8-bit texture that Remix samples as sRGB.
                    // Writing the linear value straight in applies the 2.2 a SECOND time, which
                    // is why the clothes came out too dark with visible noise in the shadows: an
                    // 8-bit encoding of a linear value has almost no precision left down there.
                    // No gamma on the way out either, now that none was applied on the way in -
                    // the values are sRGB from end to end.
                    dst[2 - k] = static_cast<unsigned char>(c * 255.0f + 0.5f);   // BGRA
                }
                // The pattern's own alpha, so a decal keeps its cut-out. Writing 255 here is
                // what made the logo's surround a solid rectangle.
                dst[3] = static_cast<unsigned char>(pa * 255.0f + 0.5f);
            }
    }

    double acc = 0.0;
    unsigned n = 0, covered = 0;
    for (UINT i = 0; i < kCpuBakeSize * kCpuBakeSize; i += 37) {
        acc += img[i * 4] + img[i * 4 + 1] + img[i * 4 + 2];
        if (img[i * 4 + 3]) ++covered;
        ++n;
    }
    char dir[MAX_PATH] = {0};
    GetModuleFileNameA(GetModuleHandleW(nullptr), dir, MAX_PATH);
    char* sl = strrchr(dir, '\\');
    if (sl) *(sl + 1) = 0;
    static char path[MAX_PATH];
    ++g_cpuBakeCount;
    sprintf_s(path, "%ssr3-remix-cloth-%u.dds", dir, g_cpuBakeCount);
    if (!WriteBgraDds(path, kCpuBakeSize, kCpuBakeSize, img.data())) {
        ++g_cpuBakeFail;
        return nullptr;
    }
    // ------------------------------------------------------------------ ONE BAKER, BOTH COPIES
    //
    // The underwear is the one garment neither texture-space generator can do. Its pattern is a
    // heart-shaped LOGO that varies across the surface, and it is sampled through a SECOND uv set
    // while the diffuse is sampled through the first - so a per-texel answer genuinely needs the
    // mesh to relate the two, which is exactly what this rasteriser has. ClothAlbedoUniform
    // declines it correctly; there is nothing to fix there.
    //
    // What was missing is that this result only ever became a FILE. The API copy reads that file;
    // the game's own converted draw cannot, so it fell back to the raw Diffuse_Map - a mostly
    // BLACK 256x128 map with two pale panels and a waistband on it. Black underwear with a pale
    // patch is not a colour bug, it is that map shown untinted.
    //
    // So the bake is registered into the very cache ClothAlbedo consults, under the key
    // ClothAlbedoUniform builds. The first draws of a new outfit still miss and bind the raw map
    // for a frame or two, then the build lands and every later draw gets the baked albedo.
    //
    // ONLY when this slot has a SEPARATE diffuse map. Without that guard the key collides.
    //
    // ClothAlbedoUniform builds its key as ClothKey(pattern, col) ^ diffuse*prime. When a slot
    // has no separate diffuse - the corset and the shoes, where the Pattern_Map IS the albedo -
    // that XOR is against nullptr, so the key collapses to exactly ClothKey(pattern, col): the
    // key the ORIGINAL pattern-space path uses. Registering here then OVERWROTE those garments
    // correct pattern-space texture with a UV-space bake, which is why the shoes went black (bake
    // #3 covers 11% of its texture) and why the corset colour transition moved - a resample of a
    // 512x512 rasterisation has softer edges in different places than the 32x32 pattern it
    // replaced.
    //
    // A slot with no separate diffuse never needed this bridge in the first place: the
    // pattern-space generator already handles it, and handles it better.
    if (g_presentDevice && !img.empty() && diffuse) {
        float col[3][4]{};
        for (int k = 0; k < 4; ++k) {
            col[0][k] = dcA[k];
            col[1][k] = dcB[k];
            col[2][k] = dcC[k];
        }
        unsigned long long bkey = ClothKey(pattern, col);
        bkey ^= reinterpret_cast<uintptr_t>(diffuse) * 1099511628211ull;
        IDirect3DTexture9* staging = nullptr;
        IDirect3DTexture9* baked = nullptr;
        const bool wasInternal = g_internal;
        g_internal = true;
        g_internalThread = GetCurrentThreadId();
        if (SUCCEEDED(g_presentDevice->CreateTexture(kCpuBakeSize, kCpuBakeSize, 1, 0,
                                                     D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM,
                                                     &staging, nullptr)) && staging) {
            D3DLOCKED_RECT dst{};
            if (SUCCEEDED(staging->LockRect(0, &dst, nullptr, 0)) && dst.pBits) {
                for (UINT y = 0; y < kCpuBakeSize; ++y)
                    memcpy(static_cast<unsigned char*>(dst.pBits) + y * dst.Pitch,
                           &img[static_cast<size_t>(y) * kCpuBakeSize * 4],
                           static_cast<size_t>(kCpuBakeSize) * 4);
                staging->UnlockRect(0);
            }
            if (SUCCEEDED(g_presentDevice->CreateTexture(kCpuBakeSize, kCpuBakeSize, 1, 0,
                                                         D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                                                         &baked, nullptr)) && baked &&
                FAILED(g_presentDevice->UpdateTexture(staging, baked))) {
                baked->Release();
                baked = nullptr;
            }
            staging->Release();
        }
        g_internal = wasInternal;
        if (baked) {
            // Overwrites the "declined, do not retry" entry ClothAlbedoUniform left behind, which
            // is what that entry is for: it stops the decode running every draw without claiming
            // the answer is permanently nothing.
            // BOTH old references, not just the texture. ReleaseClothCache releases pattern
            // AND generated, so an entry that already held a pattern owns a reference to it -
            // dropping only the generated one and then AddRef'ing the pattern again leaks a
            // reference every time an outfit is re-baked. The common case is overwriting the
            // {nullptr, nullptr} entry the decline path leaves, where this is a no-op; it is the
            // second bake of the same garment that would have leaked.
            const auto old = g_clothCache.find(bkey);
            if (old != g_clothCache.end()) {
                if (old->second.generated) old->second.generated->Release();
                if (old->second.pattern) old->second.pattern->Release();
            }
            if (pattern) pattern->AddRef();
            g_clothCache[bkey] = ClothTex{pattern, baked};
            ++g_clothBakedRegistered;
            Log("      registered this bake as the albedo for the GAME's own draw too "
                "(key %016llX) - %u so far", bkey, g_clothBakedRegistered);
        }
    }

    Log("cloth bake #%u: %s | pattern %ux%u diffuse %ux%u%s | mean %.1f of 255, %u%% covered | "
        "colours a(%.2f %.2f %.2f) b(%.2f %.2f %.2f) c(%.2f %.2f %.2f) | %u triangles TILED "
        "outside 0..1 (wrapped, not discarded), %u had to be bounded at 4 tiles",
        g_cpuBakeCount, path, pw, ph, dw, dh, haveDiffuse ? "" : " MISSING",
        n ? acc / (3.0 * n) : 0.0, n ? covered * 100 / n : 0,
        dcA[0], dcA[1], dcA[2], dcB[0], dcB[1], dcB[2], dcC[0], dcC[1], dcC[2],
        g_cpuBakeWrapped, g_cpuBakeClipped);
    {
        // The two inputs that decide brightness, and the pattern's own level. If the pattern is
        // mostly black the lerp chain should leave the diffuse ALONE, so a dark result then means
        // the multiply is at fault rather than the colours.
        double pm = 0.0; unsigned pn = 0;
        for (UINT i = 0; i < pw * ph; i += 7) {
            pm += pat[i * 4] + pat[i * 4 + 1] + pat[i * 4 + 2]; ++pn;
        }
        double dm = 0.0; unsigned dn = 0;
        if (haveDiffuse)
            for (UINT i = 0; i < dw * dh; i += 13) {
                dm += dif[i * 4] + dif[i * 4 + 1] + dif[i * 4 + 2]; ++dn;
            }
        Log("      Diffuse_Color (c11) = (%.3f %.3f %.3f) | pattern mean %.1f | diffuse mean %.1f",
            dcMul[0], dcMul[1], dcMul[2], pn ? pm / (3.0 * pn) : 0.0,
            dn ? dm / (3.0 * dn) : 0.0);
        if (g_cpuBakeCount == 1 && haveDiffuse) {
            char dd[MAX_PATH] = {0};
            GetModuleFileNameA(GetModuleHandleW(nullptr), dd, MAX_PATH);
            char* dsl = strrchr(dd, 0x5C);
            if (dsl) *(dsl + 1) = 0;
            char dpath[MAX_PATH];
            sprintf_s(dpath, "%ssr3-remix-diffuse.dds", dd);
            if (WriteBgraDds(dpath, dw, dh, dif.data()))
                Log("      dumped the diffuse map used by this bake to %s", dpath);
        }
    }
    return path;
}

// Abandon the buffer being captured and remember not to try it again. Used everywhere the
// builder used to set a permanent failure flag.
void ApiSkipCurrentCapture() {
    if (g_apiCapVB && g_apiSkipCount < kMaxApiMeshes)
        g_apiSkipVB[g_apiSkipCount++] = g_apiCapVB;
    Log("remix api: skipping a character mesh - %s (%u skipped so far)", g_apiCharWhyNot,
        g_apiSkipCount);
    g_apiCapVerts.clear();
    g_apiCapIndices.clear();
    g_apiCapIB.clear();
    g_apiSlots.clear();
    g_apiCapHaveVerts = false;
    g_apiCapIBHave = false;
    g_apiCharBuilt = false;
    g_apiCapVB = nullptr;
    // g_apiCapFrame is deliberately NOT reset here. It was, and that one line stopped the drain
    // dead after the first part: RemixBuildCharacter returns early on g_apiCapFrame == 0xFFFFFFFF,
    // so finishing one mesh disabled every later call - no skip, no failure, no log, which is
    // exactly how it presented. Correct when each build began a fresh capture; with the pending
    // table the window frame is history that has to be kept.
}

// Called from Present, on the first frame after the capture frame.
// Load the next captured part into the globals the builder already reads. Nothing about the
// build path changes; only where its input comes from.
bool ApiLoadNextPending() {
    if (g_apiCapHaveVerts) return true;                       // one already loaded
    if (g_apiPendingNext >= g_apiPendingCount) return false;   // all drained
    ApiPending& p = g_apiPending[g_apiPendingNext++];
    g_apiCapVerts = p.verts;
    g_apiCapUvScale[0] = p.uvScale[0];
    g_apiCapUvScale[1] = p.uvScale[1];
    g_apiCapTilingReg[0] = p.uvTilingReg[0];
    g_apiCapTilingReg[1] = p.uvTilingReg[1];
    g_apiSlots = p.slots;
    g_apiCapIB = p.ib;
    g_apiCapIBSize = p.ibSize;
    g_apiCapIB32 = p.ib32;
    g_apiCapIBHave = p.ibHave;
    g_apiCapVB = p.vb;
    g_apiCapFirstVertex = p.firstVertex;
    g_apiCapAlbedo = p.albedo;
    g_apiCapWeights = p.weights;
    g_apiCapBones = p.bones;
    g_apiCapUv2 = p.uv2;
    g_apiCapBoneCount = p.boneCount;
    g_apiCapBoneReg = p.boneReg;
    memcpy(g_apiCharObjTM, p.objTM, sizeof(g_apiCharObjTM));
    g_apiCapHaveVerts = !g_apiCapVerts.empty() && !g_apiSlots.empty() && p.ibHave;
    if (!g_apiCapHaveVerts) {
        Log("remix api: pending part %u has no usable slots - skipped", g_apiPendingNext - 1);
        return ApiLoadNextPending();
    }
    return true;
}

void RemixBuildCharacter() {
    if (!g_settings.remixApiCharacter || g_apiCharBuilt) return;
    if (!g_remixMeshReady) return;
    if (g_apiCapFrame == 0xFFFFFFFFu || g_frames <= g_apiCapFrame) return;   // window still open
    if (!g_apiCaptureClosed) {
        g_apiCaptureClosed = true;
        Log("remix api: capture window closed - %u parts of the character taken in one frame",
            g_apiPendingCount);
    }
    if (!ApiLoadNextPending()) return;
    if (!g_atlasDdsReady) { g_apiCharWhyNot = "no character atlas"; return; }
    if (g_apiSlots.empty() || !g_apiCapIBHave) {
        g_apiCharWhyNot = "no index ranges were collected";
        ApiSkipCurrentCapture();
        return;
    }

    const UINT idxSize = g_apiCapIB32 ? 4u : 2u;
    const size_t vertCount = g_apiCapVerts.size();

    // One index array per slot, all rebased by (idx - minIndex). Held together so the surface
    // array can point into them - a surface keeps a POINTER, so these must outlive CreateMesh.
    ApiMeshStore& store = g_apiStore[g_apiDoneCount];
    std::vector<std::vector<unsigned>>& slotIndices = store.slotIndices;
    try {
        slotIndices.clear();
        slotIndices.resize(g_apiSlots.size());
    } catch (...) { g_apiCharWhyNot = "out of memory"; ApiSkipCurrentCapture(); return; }

    g_apiOutOfRange = 0;
    for (size_t sIdx = 0; sIdx < g_apiSlots.size(); ++sIdx) {
        const ApiSlot& sl = g_apiSlots[sIdx];
        const bool strip = (sl.type == D3DPT_TRIANGLESTRIP);
        // A strip of N triangles is N+2 indices; a list of N triangles is N*3.
        const UINT wanted = strip ? (sl.prims + 2) : (sl.prims * 3);
        std::vector<unsigned>& out = slotIndices[sIdx];

        std::vector<unsigned> raw;
        try { raw.reserve(wanted); } catch (...) { continue; }
        for (UINT k = 0; k < wanted; ++k) {
            const size_t at = static_cast<size_t>(sl.start + k) * idxSize;
            if (at + idxSize > g_apiCapIBSize) break;
            const unsigned v = g_apiCapIB32
                ? *reinterpret_cast<const unsigned*>(&g_apiCapIB[at])
                : *reinterpret_cast<const unsigned short*>(&g_apiCapIB[at]);
            // Absolute: where this index lands in the buffer, minus where our vertex 0 is.
            const long long abs = static_cast<long long>(sl.base) + v;
            const long long rel = abs - g_apiCapFirstVertex;
            if (rel < 0 || rel >= static_cast<long long>(vertCount)) {
                raw.push_back(0u);
                ++g_apiOutOfRange;
            } else {
                raw.push_back(static_cast<unsigned>(rel));
            }
        }

        if (!strip) {
            out.swap(raw);
            if (out.size() % 3) out.resize(out.size() - (out.size() % 3));
        } else {
            // Strip -> list. Winding alternates, and a repeated index is a degenerate join used
            // to stitch strips together; those must be dropped rather than emitted as slivers.
            try { out.reserve(sl.prims * 3); } catch (...) {}
            for (size_t t = 0; t + 2 < raw.size(); ++t) {
                unsigned a = raw[t], b = raw[t + 1], c = raw[t + 2];
                if (a == b || b == c || a == c) { ++g_apiDegenerate; continue; }
                if (t & 1) { const unsigned tmp = b; b = c; c = tmp; }
                out.push_back(a);
                out.push_back(b);
                out.push_back(c);
            }
        }
    }

    store.weights = g_apiCapWeights;
    store.bones = g_apiCapBones;
    store.uv2 = g_apiCapUv2;
    std::vector<RemixHardcodedVertex>& verts = store.verts;
    try { verts.assign(vertCount, RemixHardcodedVertex{}); }
    catch (...) { g_apiCharWhyNot = "out of memory"; ApiSkipCurrentCapture(); return; }
    // UV SCALE. The bind-pose uv values are RAW SHORTS, not 0..1 - SR3's vertex shaders end with
    // `mul o1.xy, r0, 0.0009765625`, i.e. x1/1024, and the fixed-function path reproduces that with
    // a uv texture matrix instead of touching the vertices (the report's "last scale 0.00098").
    // An API mesh has no texture matrix, so the scale has to be baked into the vertex - otherwise
    // the texture tiles about a thousand times over and reads as untextured.
    //
    // DIAGNOSTIC, because "missing polygons" and "wrong texture" have so far both been read off
    // the picture and both been guessed at. These numbers say whether the captured VERTEX DATA is
    // sound, independently of anything Remix does with it.
    unsigned zeroVerts = 0;
    float pLo[3] = {1e30f, 1e30f, 1e30f}, pHi[3] = {-1e30f, -1e30f, -1e30f};
    float uLo[2] = {1e30f, 1e30f}, uHi[2] = {-1e30f, -1e30f};
    for (size_t i = 0; i < vertCount; ++i) {
        const SkinnedVertex& sv = g_apiCapVerts[i];
        RemixHardcodedVertex& hv = verts[i];
        memcpy(hv.position, sv.pos, sizeof(sv.pos));
        memcpy(hv.normal, sv.nrm, sizeof(sv.nrm));
        hv.texcoord[0] = sv.uv[0] * g_apiCapUvScale[0];
        hv.texcoord[1] = sv.uv[1] * g_apiCapUvScale[1];
        hv.color = 0xFFFFFFFFu;
        if (sv.pos[0] == 0.0f && sv.pos[1] == 0.0f && sv.pos[2] == 0.0f &&
            sv.nrm[0] == 0.0f && sv.nrm[1] == 0.0f && sv.nrm[2] == 0.0f)
            ++zeroVerts;
        for (int a = 0; a < 3; ++a) {
            if (sv.pos[a] < pLo[a]) pLo[a] = sv.pos[a];
            if (sv.pos[a] > pHi[a]) pHi[a] = sv.pos[a];
        }
        for (int a = 0; a < 2; ++a) {
            if (hv.texcoord[a] < uLo[a]) uLo[a] = hv.texcoord[a];
            if (hv.texcoord[a] > uHi[a]) uHi[a] = hv.texcoord[a];
        }
    }
    Log("remix api: captured vertex audit - %u of %u vertices are all-zero (never written by the "
        "skinning loop) | pos x[%.2f %.2f] y[%.2f %.2f] z[%.2f %.2f] | uv after x1/1024 "
        "u[%.3f %.3f] v[%.3f %.3f]",
        zeroVerts, static_cast<unsigned>(vertCount),
        pLo[0], pHi[0], pLo[1], pHi[1], pLo[2], pHi[2],
        uLo[0], uHi[0], uLo[1], uHi[1]);
    if (zeroVerts * 4 > vertCount)
        Log("      MORE THAN A QUARTER of the captured vertices are zero. The capture is "
            "incomplete, not the index rebasing - every triangle touching one of these collapses "
            "to the origin and reads as a hole.");

    ++g_remixBuildGen;
    typedef unsigned (__stdcall *PFN_CreateMaterial)(const RemixMaterialInfo*, void**);
    typedef unsigned (__stdcall *PFN_CreateMesh)(const RemixMeshInfo*, void**);
    const PFN_CreateMaterial createMaterial =
        reinterpret_cast<PFN_CreateMaterial>(g_remix.CreateMaterial);
    const PFN_CreateMesh createMesh = reinterpret_cast<PFN_CreateMesh>(g_remix.CreateMesh);

    RemixMaterialInfoOpaqueEXT opaque{};
    opaque.sType = kRemixStructMaterialInfoOpaqueEXT;
    opaque.albedoConstant = {1.0f, 1.0f, 1.0f};
    opaque.opacityConstant = 1.0f;
    opaque.roughnessConstant = 0.6f;
    opaque.metallicConstant = 0.0f;
    opaque.alphaTestType = 7;          // ALWAYS. Zero means NEVER - that cost a run.
    // This part's OWN texture if it can be written; the atlas only as a fallback. Pointing every
    // part at the atlas is what made the hair and clothes come out wrong.
    const char* texPath = g_atlasDdsPath;
    if (g_apiCapAlbedo) {
        const char* own = nullptr;
        if (DumpTextureDds(g_apiCapAlbedo, &own) && own) texPath = own;
    }
    MultiByteToWideChar(CP_ACP, 0, texPath, -1, g_atlasDdsPathW, MAX_PATH);

    RemixMaterialInfo mi{};
    mi.sType = kRemixStructMaterialInfo;
    mi.pNext = &opaque;
    mi.hash = 0x5233C0DE00000000ull + g_remixBuildGen;
    mi.albedoTexture = g_atlasDdsPathW;
    mi.spriteSheetRow = 1;
    mi.spriteSheetCol = 1;
    mi.filterMode = 1;
    mi.wrapModeU = 1;
    mi.wrapModeV = 1;
    // One material per DISTINCT tint. Building one per slot would be 36 registrations of what
    // are mostly the same colour; building one for the whole mesh would throw the customisation
    // away, which is what the untinted copy was.
    // One material per distinct (tint, ALPHA STATE). Alpha is part of a material's identity, not
    // a property of the mesh: two slots of one part legitimately differ only in whether they are
    // cut out, and giving them a shared opaque material is what loses the eyes.
    struct TintMat {
        float c[3];
        int aTest;
        unsigned aRef;
        int blend;
        IDirect3DBaseTexture9* tex;
        void* handle;
        DWORD src, dst;
    };
    std::vector<TintMat> tintMats;
    size_t sIdxForBake = 0;
    auto materialFor = [&](const ApiSlot& sl) -> void* {
        // D3DCMPFUNC 1..8 -> Remix AlphaTestType 0..7 (it mirrors VkCompareOp). Alpha test off
        // means ALWAYS.
        int aTest = 7;
        unsigned aRef = 0;
        if (sl.alphaTestEnable && sl.alphaFunc >= 1 && sl.alphaFunc <= 8) {
            aTest = static_cast<int>(sl.alphaFunc) - 1;
            aRef = sl.alphaRef & 0xFF;
        }
        // TRANSLUCENT ONLY IF THE FACTORS ACTUALLY BLEND.
        //
        // Measured 2026-09-02: 11 of the 12 slots with D3DRS_ALPHABLENDENABLE set use
        // src=D3DBLEND_ONE(2), dst=D3DBLEND_ZERO(1) - the IDENTITY blend, result = source, i.e.
        // an ordinary opaque write with the enable flag left on. Treating the enable flag as
        // "translucent" gave all of them opacityConstant 0.85 and thinned the whole character.
        // Exactly one slot is genuinely blended: src=SRCALPHA(5), dst=INVSRCALPHA(6).
        //
        // So the flag is necessary but not sufficient; the FACTORS decide.
        const bool identityBlend = (sl.srcBlend == 2 /*ONE*/ && sl.destBlend == 1 /*ZERO*/);
        const int blend = (sl.alphaBlendEnable && !identityBlend) ? 1 : 0;
        for (size_t i = 0; i < tintMats.size(); ++i)
            if (std::fabs(tintMats[i].c[0] - sl.tint[0]) < 0.004f &&
                std::fabs(tintMats[i].c[1] - sl.tint[1]) < 0.004f &&
                std::fabs(tintMats[i].c[2] - sl.tint[2]) < 0.004f &&
                tintMats[i].aTest == aTest && tintMats[i].aRef == aRef &&
                tintMats[i].blend == blend && tintMats[i].tex == sl.albedo)
                return tintMats[i].handle;
        if (tintMats.size() >= 32) return tintMats.empty() ? nullptr : tintMats[0].handle;

        // This slot's OWN texture. The part-level texture is only a fallback now, and the atlas
        // only a fallback to that.
        // A baked texture is the shader's own output and beats anything bound, because it IS
        // what the game draws rather than an input to it.
        // CPU BAKE. A customised slot's colour is computed by the pixel shader and exists in no
        // texture, so it is evaluated here from the pattern and the three colours and written out.
        // Cached per material key, so each distinct item is baked once.
        const char* clothTex = nullptr;
        // Blended slots ARE baked again. Skipping them turned the underwear white, because it
        // is itself a blended slot and lost its baked colour. It needs BOTH - the recipe's colour
        // and the texture's alpha - so the bake now carries alpha through instead of writing 255.
        if (sl.cloth && g_cpuBakeCount < 12 && sIdxForBake < slotIndices.size()) {
            if (!sl.pattern) {
                Log("cloth bake skipped: no Pattern_Map bound on this slot");
            } else {
                // Pattern coordinates, scaled to 0..1 here so the baker takes one convention.
                //
                // FALLBACK TO TEXCOORD0. Three parts - 416, 130 and 1128 vertices - carry no
                // TEXCOORD1 on any draw, and one of them is the corset, which is visibly showing
                // its segmentation map raw. Those shader variants sample the pattern with the
                // FIRST uv set. Using set 0 there is better than not baking at all, and a wrong
                // guess shows as a mis-sampled bake in the dump rather than as a silent skip.
                std::vector<float> puv;
                bool fromSet1 = (store.uv2.size() == verts.size() * 2);
                try {
                    puv.resize(verts.size() * 2);
                    for (size_t k = 0; k < verts.size(); ++k) {
                        if (fromSet1) {
                            // The pattern's own coordinates take the same scale. When a part
                            // has no TEXCOORD1 the baker falls back to TEXCOORD0, which is
                            // exactly the stream this scale was measured on - so a wrong scale
                            // here is what drove the underwear's bake to 3% coverage.
                            puv[k * 2] = store.uv2[k * 2] * g_apiCapUvScale[0];
                            puv[k * 2 + 1] = store.uv2[k * 2 + 1] * g_apiCapUvScale[1];
                        } else {
                            puv[k * 2] = verts[k].texcoord[0];
                            puv[k * 2 + 1] = verts[k].texcoord[1];
                        }
                    }
                } catch (...) { puv.clear(); }
                if (puv.size() == verts.size() * 2) {
                    if (!fromSet1)
                        Log("cloth bake: this part has no TEXCOORD1, sampling the pattern with "
                            "TEXCOORD0 instead (%u verts)", (unsigned)verts.size());
                    // ONLY pass a diffuse map if the slot HAS one.
                    //
                    // For a slot whose ranked albedo is the Pattern_Map itself - the corset is
                    // one - sl.albedo and sl.pattern are the same texture, and multiplying the
                    // pattern by the colours reproduced the segmentation map. Those slots have no
                    // diffuse: the lerp chain alone is their colour.
                    IDirect3DBaseTexture9* dif =
                        (sl.albedo && sl.albedo != sl.pattern) ? sl.albedo : nullptr;
                    clothTex = CpuBakeCloth(verts, puv, slotIndices[sIdxForBake], sl.pattern,
                                            dif, sl.dcA, sl.dcB, sl.dcC, sl.dcMul);
                }
            }
        }

        const char* slotTex = texPath;
        if (sl.hair) {
            // STRANDS, when the Dob_Map is available.
            //
            // albedo = chosen hair colour * (dob.R / 255). The mask is authored and unlit, so
            // this adds the strand variation the flat constant could never have while leaving
            // every photon to Remix.
            if (g_settings.hairStrandsFromDob && sl.dob) {
                std::vector<unsigned char> dob;
                UINT hw = 0, hh = 0;
                if (TextureToBgra(sl.dob, dob, hw, hh) && hw && hh &&
                    dob.size() >= static_cast<size_t>(hw) * hh * 4) {
                    std::vector<unsigned char> out;
                    bool ok = true;
                    try { out.assign(static_cast<size_t>(hw) * hh * 4, 0); }
                    catch (...) { ok = false; }
                    if (ok) {
                        double acc = 0.0;
                        for (size_t i = 0; i < static_cast<size_t>(hw) * hh; ++i) {
                            const float m = dob[i * 4 + 2] / 255.0f;   // BGRA -> R is the strands
                            for (int k = 0; k < 3; ++k) {
                                float v = sl.hairColor[k] * m * g_settings.clothTintScale;
                                v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
                                const unsigned char q =
                                    static_cast<unsigned char>(v * 255.0f + 0.5f);
                                out[i * 4 + (2 - k)] = q;      // write B,G,R
                                acc += q;
                            }
                            out[i * 4 + 3] = 255;
                        }
                        char hdir[MAX_PATH] = {0};
                        GetModuleFileNameA(GetModuleHandleW(nullptr), hdir, MAX_PATH);
                        char* hsl = strrchr(hdir, 0x5C);
                        if (hsl) *(hsl + 1) = 0;
                        static char hairPath[MAX_PATH];
                        ++g_hairGenerated;
                        sprintf_s(hairPath, "%ssr3-remix-hair-%u.dds", hdir, g_hairGenerated);
                        if (WriteBgraDds(hairPath, hw, hh, out.data())) {
                            Log("HAIR STRANDS #%u: %ux%u = colour (%.3f %.3f %.3f) * the Dob_Map "
                                "strand channel | mean %.1f of 255 -> %s",
                                g_hairGenerated, hw, hh, sl.hairColor[0], sl.hairColor[1],
                                sl.hairColor[2],
                                acc / (3.0 * static_cast<double>(hw) * hh), hairPath);
                            slotTex = hairPath;
                            goto hairDone;
                        }
                        --g_hairGenerated;
                    }
                }
            }
            // NO TEXTURE FOR HAIR.
            //
            // The hair slot's chosen albedo is its Diffuse_Map, and for hair that map is smooth
            // DIRECTIONAL data - the blue/green/purple image the user sees. It is not a colour,
            // so binding it wins over the colour constant and the hair renders as the mask.
            // Hair_Spec_Color2 carries the chosen colour; a flat constant with no texture is
            // closer to right than a mask, and the strand detail is a later problem.
            slotTex = nullptr;
            hairDone: ;
        } else if (clothTex) {
            slotTex = clothTex;
        } else if (sl.bakedPath[0]) {
            slotTex = sl.bakedPath;
        } else if (sl.albedo) {
            const char* own = nullptr;
            if (DumpTextureDds(sl.albedo, &own) && own) slotTex = own;
        }
        if (slotTex) {
            MultiByteToWideChar(CP_ACP, 0, slotTex, -1, g_atlasDdsPathW, MAX_PATH);
            mi.albedoTexture = g_atlasDdsPathW;
        } else {
            mi.albedoTexture = nullptr;
        }
        ++g_remixBuildGen;
        // ALBEDO IS A COLOUR: 0..1. The captured Tint_color came back as 5.0 on every part's
        // blended slot, which is not a base colour - it is a multiplier, and handing it to
        // albedoConstant would coat the character in blown-out white. Clamped, and the raw value
        // is kept in the log so the difference stays visible.
        // Which constant colours this surface. For a customised item whose albedo could NOT be
        // generated, Tint_color is not it - the colour lives in Diffuse_Color_a/b/c, and the
        // dominant term is _a (the pattern's red channel). Using it makes the item its CHOSEN
        // colour instead of the pattern's white.
        //
        // This is an APPROXIMATION and is labelled as one: the real recipe weights all three by
        // the pattern channels at gamma 2.2 per texel, which a single constant cannot express.
        // It is the difference between a white bracelet and a roughly right one, not correctness.
        // ONLY where the chosen albedo is a PATTERN.
        //
        // The first version applied Diffuse_Color_a to EVERY slot carrying the cloth recipe,
        // including the top - which already had a real Diffuse_Map at rank 100 and rendered
        // correctly. Its constant is near-black, so the top went black. A regression caused by
        // widening a fix past the case it was for.
        //
        // The white items are the ones whose chosen albedo ranks 80, i.e. a Pattern_Map bound as
        // base colour. A slot with a genuine diffuse map keeps it and keeps its tint.
        //
        // And a near-black constant is never an improvement on white: if the colour reads as
        // unset, leave the surface alone rather than making it black.
        const float* src = sl.tint;
        if (sl.hair) {
            src = sl.hairColor;
        } else if (sl.cloth && !sl.clothGenerated && sl.albedoRank <= 80) {
            const float lum = sl.dcA[0] + sl.dcA[1] + sl.dcA[2];
            if (lum > 0.02f) src = sl.dcA;
            else ++g_apiClothBlackConst;
        }
        float alb[3];
        for (int c = 0; c < 3; ++c)
            alb[c] = (src[c] < 0.0f) ? 0.0f : (src[c] > 1.0f ? 1.0f : src[c]);
        opaque.albedoConstant = {alb[0], alb[1], alb[2]};
        // A GENUINELY BLENDED SLOT MUST BE CUT OUT, NOT DRAWN AS A TRANSLUCENT QUAD.
        //
        // The one slot with real alpha blending (src SRCALPHA / dst INVSRCALPHA) is a decal layer
        // - the logo on the underwear. Giving it alphaTestType ALWAYS drew its whole quad, so the
        // transparent area around the logo appeared as a solid rectangle over the garment.
        //
        // Discarding only texels whose alpha is essentially zero keeps every visible part of the
        // logo, including its soft edges, while removing the empty area around it. A high
        // reference value would eat the logo's own edges, so it is deliberately low.
        if (blend && aTest == 7) {
            opaque.alphaTestType = 6;              // GREATER_OR_EQUAL
            opaque.alphaReferenceValue = 8;        // only fully transparent texels are dropped
            opaque.opacityConstant = 1.0f;
        } else {
            opaque.alphaTestType = aTest;
            opaque.alphaReferenceValue = static_cast<unsigned char>(aRef);
        }
        // A blended draw is translucent; say so through opacityConstant rather than guessing at
        // blendType_value, whose enumeration is not in the header.
        opaque.opacityConstant = 1.0f;
        mi.hash = 0x5233C0DE00000000ull + g_remixBuildGen;
        void* h = nullptr;
        const unsigned rc2 = createMaterial(&mi, &h);
        if (rc2 != 0 || !h) {
            Log("remix api: material failed rc=%u (%s)", rc2, RemixErrName(rc2));
            return tintMats.empty() ? nullptr : tintMats[0].handle;
        }
        tintMats.push_back(
            TintMat{{sl.tint[0], sl.tint[1], sl.tint[2]}, aTest, aRef, blend, sl.albedo, h,
                    sl.srcBlend, sl.destBlend});
        return h;
    };

    // A white fallback, and the first entry, so a slot with no Tint_color still has a material.
    ApiSlot whiteSlot{};
    whiteSlot.tint[0] = whiteSlot.tint[1] = whiteSlot.tint[2] = whiteSlot.tint[3] = 1.0f;
    whiteSlot.alphaFunc = 8;   // D3DCMP_ALWAYS
    whiteSlot.albedo = g_apiCapAlbedo;
    void* material = materialFor(whiteSlot);
    if (!material) {
        g_apiCharWhyNot = "CreateMaterial failed";
        ApiSkipCurrentCapture();
        return;
    }

    // Every slot shares the one vertex array and the one material; only the indices differ.
    // Built only AFTER every inner index vector is final: taking a pointer into a vector that
    // is later resized is the same class of mistake as the one above, one level down.
    std::vector<RemixMeshInfoSurfaceTriangles>& surfaces = store.surfaces;
    surfaces.clear();
    unsigned tris = 0;
    try { surfaces.reserve(slotIndices.size()); } catch (...) {}
    for (size_t sIdx = 0; sIdx < slotIndices.size(); ++sIdx) {
        if (slotIndices[sIdx].size() < 3) continue;
        RemixMeshInfoSurfaceTriangles su{};
        su.vertices_values = verts.data();
        su.vertices_count = verts.size();
        su.indices_values = slotIndices[sIdx].data();
        su.indices_count = slotIndices[sIdx].size();
        // Remix does the skinning. Both arrays are 4 per vertex and live in the same store as
        // the vertices, so they outlive CreateMesh for the same reason.
        if (g_settings.remixApiSkinning && !store.weights.empty() &&
            store.weights.size() == verts.size() * 4 && !store.bones.empty() &&
            store.bones.size() == verts.size() * 4) {
            su.skinning_hasvalue = 1;
            su.skinning_value.bonesPerVertex = 4;
            su.skinning_value.blendWeights_values = store.weights.data();
            su.skinning_value.blendWeights_count =
                static_cast<unsigned>(store.weights.size());
            su.skinning_value.blendIndices_values = store.bones.data();
            su.skinning_value.blendIndices_count = static_cast<unsigned>(store.bones.size());
        } else {
            su.skinning_hasvalue = 0;
        }
        sIdxForBake = sIdx;
        su.material = materialFor(g_apiSlots[sIdx]);
        if (!su.material) su.material = material;
        surfaces.push_back(su);
        tris += static_cast<unsigned>(slotIndices[sIdx].size() / 3);
    }
    if (surfaces.empty()) {
        g_apiCharWhyNot = "every slot was empty after rebasing";
        ApiSkipCurrentCapture();
        return;
    }

    // ---------------------------------------------------------------------------------------
    // SLOT STRUCTURE, logged in full.
    //
    // Four theories for the holes have now been proposed from the picture and all four are dead:
    // overlap with the game's own character (offsetting changed nothing), a baseVertex mistake
    // (the fix was a no-op), out-of-range indices (0), and duplicate index ranges (0 dropped).
    // Every aggregate number describes a healthy mesh. So stop inferring and print the thing
    // itself: what each slot actually covers, and how much of the vertex set anything references.
    //
    // What to look for:
    //   ranges that OVERLAP without being identical - LOD variants or nested submissions would
    //     z-fight exactly like duplicates while defeating an equality test;
    //   a referenced-vertex count well below 7977 - then the slots simply do not cover the body
    //     and the holes are geometry that was never submitted.
    {
        std::vector<unsigned char> used;
        unsigned referenced = 0, overlapping = 0;
        try {
            used.assign(vertCount, 0);
            for (size_t a = 0; a < slotIndices.size(); ++a)
                for (size_t k = 0; k < slotIndices[a].size(); ++k) {
                    const unsigned v = slotIndices[a][k];
                    if (v < vertCount && !used[v]) { used[v] = 1; ++referenced; }
                }
        } catch (...) {}
        // A SLOT'S INDEX RANGE DEPENDS ON ITS TOPOLOGY, and this used prims*3 for every slot.
        //
        // The builder has known since the strip discovery that a STRIP of N triangles occupies
        // N+2 indices, not N*3 - but this diagnostic never learned it, so it reported every strip
        // slot as three times too long, found them all "overlapping" their neighbours, and then
        // stated a conclusion as fact: "coplanar duplicates ... they z-fight exactly like
        // identical ones."
        //
        // Every one of those warnings was false. Read with the right formula the slots TILE
        // exactly - part 2 of the 2026-09-04 run reported slot 0 as [0..5715) and slot 1 starting
        // at 1907, and 1907 is precisely 0 + 1905 + 2. That is a perfect strip boundary, not an
        // overlap.
        //
        // Kept as a warning rather than deleted, because a genuine overlap is worth catching -
        // but it now has to be genuine. A diagnostic that cries wolf on healthy data is worse
        // than none: it sends the next reader chasing a duplicate that was never there.
        auto slotIndexCount = [](const ApiSlot& sl) -> UINT {
            return (sl.type == D3DPT_TRIANGLESTRIP) ? (sl.prims + 2) : (sl.prims * 3);
        };
        // Per PART, not since the process started. These were cumulative globals printed as
        // though they described the part being built, so part 6 read "13 triangle STRIPS" for a
        // 3-slot mesh and "3914 degenerate joins" for one that dropped 636.
        unsigned stripsHere = 0;
        for (size_t a = 0; a < g_apiSlots.size(); ++a)
            if (g_apiSlots[a].type == D3DPT_TRIANGLESTRIP) ++stripsHere;
        g_apiStripSlots += stripsHere;
        const unsigned degenerateHere = g_apiDegenerate - g_apiDegeneratePrev;
        g_apiDegeneratePrev = g_apiDegenerate;
        Log("remix api: SLOT STRUCTURE - %u slots (%u triangle STRIPS), %u of %u vertices "
            "referenced, %u degenerate strip joins dropped in this part",
            static_cast<unsigned>(g_apiSlots.size()), stripsHere, referenced,
            static_cast<unsigned>(vertCount), degenerateHere);
        // Say what scale this part was built with and where it came from. A uv range outside
        // 0..1 with tiling reg -1 means no tiling constant matched and the fault is elsewhere -
        // which is a different next step from "the tiling was found and applied".
        Log("      uv scale %.8f x %.8f  (bare 1/1024 is %.8f) | tiling from vs const u=%d v=%d "
            "(-1 = no tiling constant matched this albedo)",
            g_apiCapUvScale[0], g_apiCapUvScale[1], kShortUVScale,
            g_apiCapTilingReg[0], g_apiCapTilingReg[1]);
        for (size_t a = 0; a < g_apiSlots.size(); ++a) {
            const UINT aLo = g_apiSlots[a].start, aHi = aLo + slotIndexCount(g_apiSlots[a]);
            unsigned hits = 0;
            for (size_t b = 0; b < g_apiSlots.size(); ++b) {
                if (b == a) continue;
                const UINT bLo = g_apiSlots[b].start, bHi = bLo + slotIndexCount(g_apiSlots[b]);
                if (aLo < bHi && bLo < aHi) ++hits;
            }
            if (hits) ++overlapping;
            if (a < 40)
                Log("      slot %2u: startIndex %6u, %5u tris %s, base %d, indices [%u..%u)%s",
                    static_cast<unsigned>(a), g_apiSlots[a].start, g_apiSlots[a].prims,
                    g_apiSlots[a].type == D3DPT_TRIANGLESTRIP ? "STRIP" : "list ",
                    g_apiSlots[a].base, aLo, aHi,
                    hits ? "  <- OVERLAPS another slot" : "");
        }
        if (overlapping)
            Log("      %u of %u slots share index ranges with another, read with each slot's OWN "
                "topology. This one is real: overlapping ranges are coplanar duplicates that an "
                "equality test cannot see, and they z-fight exactly like identical ones.",
                overlapping, static_cast<unsigned>(g_apiSlots.size()));
    }

    RemixMeshInfo meshInfo{};
    meshInfo.sType = kRemixStructMeshInfo;
    meshInfo.hash = 0x5233C0DE10000000ull + g_remixBuildGen;
    meshInfo.surfaces_values = surfaces.data();
    meshInfo.surfaces_count = static_cast<unsigned>(surfaces.size());
    const unsigned rc = createMesh(&meshInfo, &g_apiCharMesh);
    if (rc != 0 || !g_apiCharMesh) {
        Log("remix api: character CreateMesh failed rc=%u (%s)", rc, RemixErrName(rc));
        g_apiCharWhyNot = "CreateMesh failed";
        ApiSkipCurrentCapture();
        return;
    }
    g_apiCharVertexCount = static_cast<unsigned>(vertCount);
    g_apiCharTriangles = tris;
    g_apiSurfaces = static_cast<unsigned>(surfaces.size());
    g_apiCharBuilt = true;
    g_apiCharWhyNot = "built";

    // File it, then RESET so the next frame captures another of the character's buffers. The
    // store this mesh used is not touched again - the next build takes the following slot.
    ApiDoneMesh& done = g_apiDone[g_apiDoneCount];
    done.mesh = g_apiCharMesh;
    done.tris = tris;
    done.surfaces = g_apiSurfaces;
    done.vb = g_apiCapVB;
    done.boneCount = g_apiCapBoneCount;
    done.boneReg = g_apiCapBoneReg;
    done.haveBones = false;
    for (unsigned b = 0; b < kMaxApiBones; ++b) {
        memset(done.bones[b], 0, sizeof(done.bones[b]));
        done.bones[b][0] = done.bones[b][5] = done.bones[b][10] = 1.0f;   // identity 3x4
    }
    memcpy(done.objTM, g_apiCharObjTM, sizeof(done.objTM));
    ++g_apiDoneCount;
    g_apiDoneTris += tris;

    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f}, mid[3] = {0, 0, 0};
    for (size_t i = 0; i < verts.size(); ++i)
        for (int a = 0; a < 3; ++a) {
            const float v = verts[i].position[a];
            if (v < lo[a]) lo[a] = v;
            if (v > hi[a]) hi[a] = v;
            mid[a] += v;
        }
    if (!verts.empty()) for (int a = 0; a < 3; ++a) mid[a] /= static_cast<float>(verts.size());
    for (int r = 0; r < 3; ++r)
        g_apiCharAt[r] = g_apiCharObjTM[r][0] * mid[0] + g_apiCharObjTM[r][1] * mid[1] +
                         g_apiCharObjTM[r][2] * mid[2] + g_apiCharObjTM[r][3];
    for (size_t hi = 0; hi < g_apiSlots.size(); ++hi)
        if (g_apiSlots[hi].hair) {
            Log("hair: slot %u uses Hair_Spec_Color (%.3f %.3f %.3f) as its albedo - the Dob_Map "
                "holds strands, not colour", (unsigned)hi, g_apiSlots[hi].hairColor[0],
                g_apiSlots[hi].hairColor[1], g_apiSlots[hi].hairColor[2]);
            break;
        }
    if (g_apiClothGenerated || g_apiClothApprox)
        Log("remix api: CUSTOMISED items - %u albedos GENERATED, %u could NOT be generated "
            "(the player's own clothing samples a Diffuse_Map through a second UV set). Of those, "
            "only slots whose albedo ranks <= 80 (a Pattern_Map) take Diffuse_Color_a; a slot with "
            "a real Diffuse_Map keeps its texture - applying it to those is what blacked the top.",
            g_apiClothGenerated, g_apiClothApprox);
    if (g_apiClothBlackConst)
        Log("      %u customised slots had a near-BLACK Diffuse_Color_a and were left with their "
            "own texture - applying it would have blacked the surface, which is what happened to "
            "the top when this was applied to every cloth slot", g_apiClothBlackConst);
    // What each slot IS, by name. Identifying a part from the picture and then guessing which
    // shader it uses has cost several runs; this says it outright.
    for (size_t li = 0; li < g_apiSlots.size() && li < 16; ++li)
        Log("      slot %u: albedo '%s', first '%s', %s%s%u tris", (unsigned)li,
            g_apiSlots[li].albedoName, g_apiSlots[li].firstName,
            g_apiSlots[li].cloth ? "CLOTH " : "", g_apiSlots[li].hair ? "HAIR " : "",
            g_apiSlots[li].prims);
    Log("remix api: %u distinct materials across %u slots (tint x alpha state)",
        static_cast<unsigned>(tintMats.size()), static_cast<unsigned>(g_apiSlots.size()));
    static const char* kCmp[8] = {"NEVER", "LESS", "EQUAL", "LESS_EQ", "GREATER", "NOT_EQ",
                                  "GREATER_EQ", "ALWAYS"};
    for (size_t i = 0; i < tintMats.size() && i < 12; ++i)
        // D3DBLEND: 1 ZERO, 2 ONE, 3 SRCCOLOR, 4 INVSRCCOLOR, 5 SRCALPHA, 6 INVSRCALPHA,
        // 9 DESTCOLOR. src ONE / dst ONE is additive; SRCALPHA / INVSRCALPHA is ordinary alpha.
        Log("      mat %u: tex %p, tint (%.2f %.2f %.2f)%s, alphaTest %s ref %u, blend %s "
            "(src %lu dst %lu)%s",
            static_cast<unsigned>(i), static_cast<void*>(tintMats[i].tex),
            tintMats[i].c[0], tintMats[i].c[1], tintMats[i].c[2],
            (tintMats[i].c[0] > 1.0f || tintMats[i].c[1] > 1.0f || tintMats[i].c[2] > 1.0f)
                ? " CLAMPED to 1" : "",
            (tintMats[i].aTest >= 0 && tintMats[i].aTest < 8) ? kCmp[tintMats[i].aTest] : "?",
            tintMats[i].aRef, tintMats[i].blend ? "ON" : "off",
            tintMats[i].src, tintMats[i].dst,
            tintMats[i].tex ? "" : "  <- NO texture of its own, using the fallback");
    Log("remix api: STEP 3b - character mesh built: %u surfaces from %u slots (%u duplicate "
        "index ranges dropped), %u verts, %u triangles, placed at (%.1f %.1f %.1f), extent "
        "%.2f x %.2f x %.2f",
        g_apiSurfaces, g_apiSlotsSeen, g_apiSlotsDuplicate, g_apiCharVertexCount,
        g_apiCharTriangles,
        g_apiCharAt[0], g_apiCharAt[1], g_apiCharAt[2],
        hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]);
    // Reset the capture so another buffer can be taken. Everything the finished mesh needs
    // lives in g_apiStore[its index] and g_apiDone[its index] from here on.
    g_apiCapVerts.clear();
    g_apiCapIndices.clear();
    g_apiCapIB.clear();
    g_apiSlots.clear();
    g_apiCapHaveVerts = false;
    g_apiCapIBHave = false;
    g_apiCharBuilt = false;
    g_apiCapVB = nullptr;
    // g_apiCapFrame is deliberately NOT reset here. It was, and that one line stopped the drain
    // dead after the first part: RemixBuildCharacter returns early on g_apiCapFrame == 0xFFFFFFFF,
    // so finishing one mesh disabled every later call - no skip, no failure, no log, which is
    // exactly how it presented. Correct when each build began a fresh capture; with the pending
    // table the window frame is history that has to be kept.

    if (g_apiOutOfRange)
        Log("      %u indices fell outside the captured vertex window and were collapsed to "
            "degenerates - if this is a large fraction of %u, the missing geometry is those, not "
            "a partial capture", g_apiOutOfRange, g_apiCharTriangles * 3);
}


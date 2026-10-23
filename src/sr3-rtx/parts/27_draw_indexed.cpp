// parts/27_draw_indexed.cpp - Hook_DrawIndexedPrimitive.
// NOT a standalone translation unit: #included, in order, by ../sr3rtx.cpp inside its anonymous
// namespace. Originally sr3rtx.cpp lines 23119-23643 (old file:line citations map here).
// DIAGNOSTIC headBodyProbe (measurement only). For CONVERTED SKINNED draws: the albedo's sRGB sampler
// state (g_ssShadow, the game's own SetSamplerState as the shim saw it), size, format, render-target
// status, prim count and rounded world position. Draws at the same rounded position within a frame
// are grouped as one character; a frame is closed (and its mixed-state counts added) when the first
// probed draw of the next frame arrives. Bounded (48 characters x 8 draws).
struct BhDraw { UINT prims; int srgb; UINT w, h; DWORD fmt; bool rt, haveDesc; void* tex; };
struct BhChar { int x, y, z; unsigned n; BhDraw d[8]; };
constexpr unsigned kBhMaxChars = 48, kBhMaxDraws = 8;
BhChar g_bhChars[kBhMaxChars];
unsigned g_bhNChars = 0, g_bhFrame = 0, g_bhOverflow = 0, g_bhExamples = 0;
unsigned long long g_bhFramesClosed = 0, g_bhDraws = 0, g_bhNoAlbedo = 0, g_bhCharsSeen = 0, g_bhMulti = 0,
                   g_bhMixSrgb = 0, g_bhMixRT = 0, g_bhMixFmt = 0, g_bhMixSize = 0, g_bhMixAny = 0;
const char* BhFmtName(DWORD f) {
    switch (f) {
        case D3DFMT_A8R8G8B8: return "A8R8G8B8"; case D3DFMT_X8R8G8B8: return "X8R8G8B8";
        case D3DFMT_R5G6B5: return "R5G6B5"; case D3DFMT_A16B16G16R16F: return "A16B16G16R16F";
        case D3DFMT_L8: return "L8"; case D3DFMT_A8: return "A8";
        case D3DFMT_DXT1: return "DXT1"; case D3DFMT_DXT3: return "DXT3"; case D3DFMT_DXT5: return "DXT5";
        default: return "?";
    }
}
void BhCloseFrame() {
    for (unsigned c = 0; c < g_bhNChars; ++c) {
        const BhChar& ch = g_bhChars[c];
        ++g_bhCharsSeen;
        if (ch.n < 2) continue;
        ++g_bhMulti;
        bool ms = false, mr = false, mf = false, mz = false;
        for (unsigned i = 1; i < ch.n; ++i) {
            if (ch.d[i].srgb != ch.d[0].srgb) ms = true;
            if (ch.d[i].rt != ch.d[0].rt) mr = true;
            if (ch.d[i].fmt != ch.d[0].fmt) mf = true;
            if (ch.d[i].w != ch.d[0].w || ch.d[i].h != ch.d[0].h) mz = true;
        }
        if (ms) ++g_bhMixSrgb;
        if (mr) ++g_bhMixRT;
        if (mf) ++g_bhMixFmt;
        if (mz) ++g_bhMixSize;
        const bool any = ms || mr || mf || mz;
        if (any) ++g_bhMixAny;
        // Up to 10 examples: mixed characters always; uniform multi-draw ones only while fewer
        // than 7 are logged, as a control.
        if (g_bhExamples < 10 && (any || g_bhExamples < 7)) {
            ++g_bhExamples;
            char buf[900]; buf[0] = 0;
            for (unsigned i = 0; i < ch.n; ++i)
                sprintf_s(buf + strlen(buf), sizeof(buf) - strlen(buf),
                          " [prims=%u srgb=%d %ux%u %s%s rt=%d tex=%p]", ch.d[i].prims, ch.d[i].srgb,
                          ch.d[i].w, ch.d[i].h, BhFmtName(ch.d[i].fmt),
                          ch.d[i].haveDesc ? "" : "(nodesc)", ch.d[i].rt ? 1 : 0, ch.d[i].tex);
            Log("HEAD/BODY PROBE example #%u (%s) frame %u at(%d,%d,%d): %u draws:%s", g_bhExamples,
                any ? "MIXED" : "uniform", g_bhFrame, ch.x, ch.y, ch.z, ch.n, buf);
        }
    }
    ++g_bhFramesClosed;
    g_bhNChars = 0;
}
void BhNoteDraw(UINT primitiveCount) {
    if (g_bhFrame != g_frames) { if (g_bhNChars) BhCloseFrame(); g_bhFrame = g_frames; }
    ++g_bhDraws;
    IDirect3DBaseTexture9* const tex = g_lastBoundAlbedo;
    BhDraw bd{};
    bd.prims = primitiveCount; bd.srgb = -1; bd.tex = static_cast<void*>(tex);
    int st = g_lastEffectiveAlbedoStage;
    if (st < 0 || st > 15) st = (g_curPS.albedoStage >= 0 && g_curPS.albedoStage < 16) ? g_curPS.albedoStage : 0;
    if (g_ssKnown[st][D3DSAMP_SRGBTEXTURE]) bd.srgb = static_cast<int>(g_ssShadow[st][D3DSAMP_SRGBTEXTURE]);
    if (tex) {
        bd.rt = g_rtTextures.count(tex) != 0;
        if (tex->GetType() == D3DRTYPE_TEXTURE) {
            D3DSURFACE_DESC sd{};
            if (SUCCEEDED(static_cast<IDirect3DTexture9*>(tex)->GetLevelDesc(0, &sd))) {
                bd.w = sd.Width; bd.h = sd.Height; bd.fmt = static_cast<DWORD>(sd.Format); bd.haveDesc = true;
            }
        }
    } else ++g_bhNoAlbedo;
    const D3DMATRIX o = FromRegisters(&g_vsConst[kRegObjTM][0], 3);
    const int x = static_cast<int>(floorf(o._41 + 0.5f)), y = static_cast<int>(floorf(o._42 + 0.5f)),
              z = static_cast<int>(floorf(o._43 + 0.5f));
    BhChar* ch = nullptr;
    for (unsigned c = 0; c < g_bhNChars; ++c)
        if (g_bhChars[c].x == x && g_bhChars[c].y == y && g_bhChars[c].z == z) { ch = &g_bhChars[c]; break; }
    if (!ch) {
        if (g_bhNChars >= kBhMaxChars) { ++g_bhOverflow; return; }
        ch = &g_bhChars[g_bhNChars++];
        ch->x = x; ch->y = y; ch->z = z; ch->n = 0;
    }
    if (ch->n >= kBhMaxDraws) { ++g_bhOverflow; return; }
    ch->d[ch->n++] = bd;
}
void BhReport(double f) {
    Log("    HEAD/BODY PROBE (headBodyProbe=ON, converted SKINNED draws, grouped by rounded objTM position): "
        "%.1f probed draws/frame, %llu frames closed | characters seen %llu, of which %llu have 2+ draws | "
        "MIXED sRGB state %llu, MIXED render-target-vs-not %llu, MIXED format %llu, MIXED size %llu, "
        "any mix %llu | %llu draws had no albedo bound | %u draws dropped by the 48-char/8-draw cap | "
        "%u/10 examples logged (srgb=-1 means the game never set it)",
        g_bhDraws / f, g_bhFramesClosed, g_bhCharsSeen, g_bhMulti, g_bhMixSrgb, g_bhMixRT, g_bhMixFmt,
        g_bhMixSize, g_bhMixAny, g_bhNoAlbedo, g_bhOverflow, g_bhExamples);
}

HRESULT WINAPI Hook_DrawIndexedPrimitive(IDirect3DDevice9* dev, D3DPRIMITIVETYPE type,
                                         INT baseVertex, UINT minIndex, UINT numVertices,
                                         UINT startIndex, UINT primitiveCount) {
    const LONGLONG tShim = Now();
    ++g_drawsTotal;
    ++g_drawIndexThisFrame;
    if (OcclusionProxyDraw(dev, primitiveCount, numVertices,
                           static_cast<UINT>(max(0, baseVertex + static_cast<INT>(minIndex)))))
        return D3D_OK;
    { const LONGLONG t0 = Now(); NoteCommandBlock(); g_phaseMs[PH_ENTRY_CMD] += MsSince(t0); }
    // Before anything takes a pointer into the caches these would erase from.
    { const LONGLONG t0 = Now(); DrainPendingInvalidations(); g_phaseMs[PH_ENTRY_DRAIN] += MsSince(t0); }
    CreateSkinBuffer(dev);
    { const LONGLONG t0 = Now(); ProbeSkyOrder(dev, numVertices, primitiveCount); g_phaseMs[PH_ENTRY_SKY] += MsSince(t0); }
    g_meshKeyBaseVertex = static_cast<unsigned>(baseVertex) + minIndex;
    g_curDrawFirstVertex = static_cast<UINT>(max(0, baseVertex + static_cast<INT>(minIndex)));
    g_curDrawVertexCount = numVertices;
    g_curDrawIndexed = true;
    g_curDrawBaseVertex = baseVertex;
    g_curDrawMinIndex = minIndex;
    g_curDrawStartIndex = startIndex;
    g_curDrawPrimCount = primitiveCount;
    g_curDrawPrimType = type;
    if (g_settings.lockThreadProbe) LtNoteDraw(g_stream0, numVertices, primitiveCount);   // diagnostic only
    NoteIfFinalComposite(dev);   // injectProbe/injectControl PART 1/2: before Classify runs
    { const LONGLONG t0 = Now(); EmitLight(dev); g_phaseMs[PH_ENTRY_LIGHT] += MsSince(t0); }
    LONGLONG tP = tShim;
    PhaseAdd(PH_ENTRY, tP);
    FFPScope scope;
    Disp d = BeginFFP(dev, scope);
    tP = Now();
    // Fresh-buffer deferral (see g_recentWrites). SKIPPED, not hidden: a Hide draw still goes
    // to the bridge, under an identity projection, which is itself a way to make a stretched
    // quad. Skinned draws are exempt - they render from our own ring, decoded after the write.
    int freshAge = -1;
    if (d == Disp::Convert && !g_curLayout.skinned && g_stream0 && g_stream0Stride) {
        const UINT b0 = g_stream0Offset + g_curDrawFirstVertex * g_stream0Stride;
        freshAge = FramesSinceWrite(g_stream0, b0, b0 + numVertices * g_stream0Stride,
                                    static_cast<unsigned>(max(0, g_settings.deferFreshFrames)));
        if (freshAge >= 0 && IsDynamicVB(g_stream0)) freshAge = -1;   // rewritten every frame by design
        if (freshAge >= 0) {
            ++g_freshSeen;
            if (g_settings.deferFreshStaticDraws) {
                ++g_deferredFresh;
                EndFFP(dev, scope);
                scope = FFPScope{};
                d = Because("fresh: its static buffer range was written within deferFreshFrames - skipped this frame", Disp::Skip);
            }
        }
    }
    // Occluder candidates: rigid, non-instanced converted draws - the world. Opaque means no
    // blend, no alpha test and no shader cutout; a fence's holes must never occlude.
    if (d == Disp::Convert && !g_curLayout.skinned) {
        ++g_occlCandDraws;
        if (g_instancedDraw) ++g_occlCandInstanced;
        g_occlCandTris += primitiveCount;
        if (primitiveCount > 1000) ++g_occlCandBig; else if (primitiveCount > 100) ++g_occlCandMid;
        // The blend FLAG is not the blend; the FACTORS are. SR3 leaves ALPHABLENDENABLE on with
        // ONE/ZERO for opaque draws (measured: the control garment drew src=2 dst=1).
        const bool blend = g_rsShadow[D3DRS_ALPHABLENDENABLE] != 0 &&
                           !(g_rsShadow[D3DRS_SRCBLEND] == D3DBLEND_ONE && g_rsShadow[D3DRS_DESTBLEND] == D3DBLEND_ZERO);
        const bool atest = g_rsShadow[D3DRS_ALPHATESTENABLE] != 0 && g_rsShadow[D3DRS_ALPHAREF] > 0;
        const bool texkill = g_curPS.isPixelShader && g_curPS.alphaThresholdReg >= 0;
        if (blend) ++g_occlCandBlend;
        if (atest) ++g_occlCandATest;
        if (texkill) ++g_occlCandTexkill;
        if (!blend && !atest && !texkill) {
            ++g_occlCandOpaque;
            RecordOccluder(dev, type, baseVertex, minIndex, numVertices, startIndex, primitiveCount);
        }
    }

    // An EXACT duplicate: same buffer, same vertex range, same index range, same triangle count,
    // already converted earlier in this frame. The rasteriser blends such passes into one
    // surface; a path tracer gets two surfaces occupying the same space, which is what "drawn
    // twice" looks like. Only the first is converted - the rest are hidden, not skipped, because
    // the engine still needs them for its own buffers.
    bool skinned = false;
    bool skinnedFF = false;   // took the hardware (fixed-function) skin path instead - see SkinViaFixedFunction
    // Skinned draws need the pose in the key; rigid ones do not have one. Everything else about
    // the test is identical, and the duplicates measured on 2026-08-27 were overwhelmingly NOT
    // skinned - 25 world draws against 4 character draws in one frame - so restricting this to
    // skinned geometry left most of the coincident surfaces in place.
    const bool dedupThis = (d == Disp::Convert) &&
                           (g_curLayout.skinned ? g_settings.dedupSkinned : g_settings.dedupAll);
    if (dedupThis) {
        unsigned long long dk = SkinMeshKey(g_stream0, g_stream0Offset, g_stream0Stride,
                                            minIndex, numVertices);
        dk ^= (static_cast<unsigned long long>(startIndex) << 32) ^ primitiveCount;
        dk *= 1099511628211ull;
        // The POSE, folded in. Without it this key is identical for two different NPCs wearing
        // the same garment - they differ only in their bone palette - and the first attempt at
        // this hid the second character's clothing, which the user saw as parts of NPCs
        // disappearing and coming back. Geometry alone does not identify an object.
        //
        // Eight bones is enough to separate two characters and cheap enough to run per draw:
        // 96 floats against ~94 skinned draws a frame. A full-palette hash would be 768.
        //
        // objTM is hashed TOO, and it is not redundant with the palette. The bones are in OBJECT
        // space - objTM is applied after the blend - so two idle NPCs holding the same pose have
        // byte-identical palettes and differ only in where objTM puts them. Without this the
        // pose key merges them and the second one loses parts, which is the third time this key
        // has been too small: first no index range, then no pose, then no position.
        for (int b = 0; g_curLayout.skinned && b < 8; ++b) {
            const float* r = &g_vsConst[kRegBonePalette + b * kRegsPerBone][0];
            for (int e = 0; e < 4; ++e) {
                unsigned bits;
                memcpy(&bits, &r[e], sizeof(bits));
                dk ^= bits;
                dk *= 1099511628211ull;
            }
        }
        for (int r = 0; r < kRegsPerBone; ++r) {
            for (int e = 0; e < 4; ++e) {
                unsigned bits;
                memcpy(&bits, &g_vsConst[kRegObjTM + r][e], sizeof(bits));
                dk ^= bits;
                dk *= 1099511628211ull;
            }
        }
        // The ALBEDO, and this is the FOURTH thing this key has been missing. Measured on a real
        // frame before switching the dedup on: of 68 groups the key would have merged, 67 shared
        // one texture and one did not -
        //
        //     v=1080 p=499 at(96.9 145.7 29.7) -> textures 16484C98, 16484E58, 16484F38
        //
        // three draws of one mesh, one pose and one position carrying three DIFFERENT textures.
        // They are separate material layers on the same geometry, and merging them would have
        // dropped two of the three - which is exactly how the previous four attempts failed, in a
        // form no amount of staring at the key would have revealed.
        //
        // Keyed on what was actually bound rather than on stage 0, because that is what Remix
        // receives and what makes two draws genuinely the same surface.
        dk ^= reinterpret_cast<uintptr_t>(g_lastBoundAlbedo);
        dk *= 1099511628211ull;
        // For an INSTANCED draw objTM is identical across the whole batch - the placement
        // lives in the instance stream instead, and the frame dump shows every one of them
        // reporting at(0.0 0.0 -1024.0). Without this the key cannot tell two instances of a
        // street prop apart, and the second would be dropped as a duplicate of the first.
        // InstanceWorld is a cache lookup and never locks, so this costs no bridge traffic.
        bool canJudge = true;
        if (g_instancedDraw) {
            D3DMATRIX iw{};
            if (InstanceWorld(iw)) {
                const float* m = &iw._11;
                for (int e = 0; e < 16; ++e) {
                    unsigned bits;
                    memcpy(&bits, &m[e], sizeof(bits));
                    dk ^= bits;
                    dk *= 1099511628211ull;
                }
            } else {
                // Instanced, but its placement could not be read. Then nothing in the key
                // separates two instances of the same prop, and merging them would delete
                // one. Refuse to judge rather than guess: a missed duplicate is a second
                // surface, a wrong merge is a missing object.
                canJudge = false;
                ++g_dedupUnjudged;
            }
        }
        bool dup = false;
        for (unsigned i = 0; canJudge && i < g_skinFrameKeyCount; ++i)
            if (g_skinFrameKeys[i] == dk) { dup = true; break; }
        if (dup) {
            ++g_skinRepeatDraws;
            ++g_skinRepeatHidden;
            EndFFP(dev, scope);
            scope = FFPScope{};
            d = Because(g_curLayout.skinned
                            ? "skinned duplicate: identical triangles already converted this frame"
                            : "duplicate: identical triangles already converted this frame",
                        HiddenDisp(true));
        } else if (canJudge && g_skinFrameKeyCount < kMaxSkinFrameKeys) {
            g_skinFrameKeys[g_skinFrameKeyCount++] = dk;
        } else {
            ++g_dedupKeyOverflow;
        }
    }
    // A skinned draw converted WITHOUT skinning would be worse than not converting it: fixed
    // function cannot read the bone palette, so the mesh would render as a rigid bind pose - a
    // T-posed statue instead of a stuttering character. So the substitution has to succeed
    // before the conversion is allowed to stand, and if it fails the draw goes back to passing
    // through exactly as it did before.
    PhaseAdd(PH_DEDUP, tP);
    if (d == Disp::Convert && g_curLayout.skinned) {
        ProbeIndexWindow(g_curIB, type, baseVertex, minIndex, numVertices, startIndex, primitiveCount);
        // Hardware skinning first, when the switch is on: fixed-function indexed vertex
        // blending stages the bone palette as D3DTS_WORLDMATRIX(0..63) and leaves stream 0
        // untouched - see SkinViaFixedFunction, above the shape probe. It refuses (and says
        // why, counted) for anything it cannot yet handle - cloth, morph, a declaration
        // missing weights/indices, a bone beyond what it staged - and CPU skinning below is
        // the fallback for every one of those, exactly as it already is for a plain
        // SkinAndBind refusal. OFF by default: with the switch off this call is skipped
        // entirely and nothing below it changes.
        if (g_settings.skinViaFixedFunction)
            skinnedFF = SkinViaFixedFunction(dev, baseVertex, minIndex, numVertices);
        if (!skinnedFF) {
        skinned = SkinAndBind(dev, baseVertex, minIndex, numVertices);
        if (!skinned) {
            ++g_skinnedRefused;
            ProbeRigidSkinned(static_cast<UINT>(max(0, baseVertex + static_cast<INT>(minIndex))),
                              numVertices);
            // Name each distinct reason once. A refusal is not a missed optimisation: the draw
            // falls back to pass-through, so Remix reconstructs it as a SECOND copy beside the
            // skinned one - which is what "my character looks like it is drawn twice" would
            // look like. The reason decides the fix, so it must be measured, not assumed.
            if (g_skinRefuseWhy && g_skinRefuseReports < 8) {
                bool seen = false;
                for (unsigned i = 0; i < g_skinRefuseReports; ++i)
                    if (!strcmp(g_skinRefuseNames[i], g_skinRefuseWhy)) { seen = true; break; }
                if (!seen) {
                    strncpy_s(g_skinRefuseNames[g_skinRefuseReports], g_skinRefuseWhy, 63);
                    // Numeric D3DDECLTYPE alongside the name: the previous run printed "?" for
                    // the weights, which says only that the type is outside DeclTypeName's
                    // table and not WHICH type it is. A number can be looked up; a "?" cannot.
                    Log("skinning REFUSED #%u: %s | verts=%u stride=%u decl: pos=%s(%d)@%d "
                        "normal=%s(%d)@%d weights=%s(%d)@%d indices=%s(%d)@%d uv=%s(%d)@%d",
                        g_skinRefuseReports + 1, g_skinRefuseWhy, numVertices, g_stream0Stride,
                        DeclTypeName(g_curLayout.posType), g_curLayout.posType,
                        g_curLayout.posOffset,
                        DeclTypeName(g_curLayout.normalType), g_curLayout.normalType,
                        g_curLayout.normalOffset,
                        DeclTypeName(g_curLayout.blendWeightType), g_curLayout.blendWeightType,
                        g_curLayout.blendWeightOffset,
                        DeclTypeName(g_curLayout.blendIndexType), g_curLayout.blendIndexType,
                        g_curLayout.blendIndexOffset,
                        DeclTypeName(g_curLayout.texcoordType), g_curLayout.texcoordType,
                        g_curLayout.texcoordOffset);
                    ++g_skinRefuseReports;
                }
            }
            EndFFP(dev, scope);
            scope = FFPScope{};
            d = Because("skinned, but the bind pose could not be read", Disp::PassThrough);
        }
        }
    }

    // Population of the deferred G-buffer pass, measured against what is actually converted.
    // Purely observational until the numbers say what hiding it would cost.
    PhaseAdd(PH_SKIN, tP);
    if (g_settings.headBodyProbe && d == Disp::Convert && g_curLayout.skinned) BhNoteDraw(primitiveCount);   // diagnostic only
    // Census: does every hair/skin draw reach the albedo generators? Only 1 hair texture was ever
    // built in a run full of NPCs. Diagnostic only.
    if (d == Disp::Convert && g_curLayout.skinned) NoteSkinnedLayout();
    if (g_curPS.hairParamsReg >= 0) { ++g_hairShaderSeen; if (d == Disp::Convert) ++g_hairShaderConverted; }
    else if (g_curPS.diffuseColorMulReg >= 0 && g_curPS.samplerName[2][0] && g_curPS.samplerName[3][0] &&
             StrStrIA(g_curPS.samplerName[3], "Sphere_Map")) {
        ++g_skinShaderSeen; if (d == Disp::Convert) ++g_skinShaderConverted;
    }
    if (g_curPS.isPixelShader && g_curPS.rtCount > 1) {
        ++g_mrtWouldHide;
        if (d == Disp::Convert) {
            ++g_mrtConverted;
            if (g_mrtSamplerReports < 10 && g_curPS.albedoSampler[0]) {
                bool seen = false;
                for (unsigned i = 0; i < g_mrtSamplerReports; ++i)
                    if (!strcmp(g_mrtSamplerNames[i], g_curPS.albedoSampler)) { seen = true; break; }
                if (!seen) {
                    strncpy_s(g_mrtSamplerNames[g_mrtSamplerReports], g_curPS.albedoSampler, 27);
                    ++g_mrtSamplerReports;
                }
            }
        }
    } else if (g_curPS.isPixelShader && d == Disp::Convert) {
        ++g_singleTargetConverted;
    }

    const bool ffp = (d == Disp::Convert);
    if (ffp) {
        NoteConvertedTarget();
        Trace('D');
        MeasureShortUVs(minIndex, numVertices);
        MeasureWorldExtent(minIndex, numVertices);
        // Diagnostic only - see g_convertedNonZeroStreamOffset's own comment, above.
        if (g_stream0Offset != 0) ++g_convertedNonZeroStreamOffset;
    } else {
        Trace(d == Disp::Hide ? 'h' : (d == Disp::Mark ? 'm' : 'x'));
    }
    ProbeDraw(dev, numVertices, primitiveCount, ffp);
    ClearBackBufferOnce(dev);
    ProbeSkyDraw(minIndex, numVertices, d);
    ProbeHudDraw(minIndex, numVertices, d);
    ProbeShapes(minIndex, numVertices, primitiveCount, d);
    ProbeSkinned(minIndex, numVertices, d);
    ProbeDecal(dev, numVertices, d);
    ProbeBoundAlbedo(d);
    ProbeAtlasComposite(dev, numVertices, primitiveCount, d);
    RecordFrameDraw(dev, numVertices, primitiveCount, d);
    RingRecordDraw(numVertices, primitiveCount, d, freshAge);

    // TIGHTEN WINDOW: rigid (non-skinned) converted draws only - a skinned draw is re-issued from
    // the shim's own ring at its true part size already (see SkinAndBind's "vertex 0 must land on
    // index minIndex" note, above) and must not be touched here. Always computed and measured,
    // whether or not tightenConvertedWindow is on, so the report line states the size of the
    // prize before anyone flips the switch; only APPLIED to the actual draw when the switch is
    // on, and tightMinIndex/tightNumVertices otherwise stay identical to the game's own values.
    UINT tightMinIndex = minIndex, tightNumVertices = numVertices;
    if (ffp && !skinned && !skinnedFF) {
        const bool albedoIsDecal = g_curPS.isPixelShader && g_curPS.albedoSampler[0] &&
                                   StrStrIA(g_curPS.albedoSampler, "Decal") != nullptr;
        UINT tMin = minIndex, tNum = numVertices;
        if (TightenConvertedWindow(g_curIB, type, baseVertex, minIndex, numVertices, startIndex, primitiveCount,
                                   albedoIsDecal, &tMin, &tNum) &&
            g_settings.tightenConvertedWindow) {
            tightMinIndex = tMin;
            tightNumVertices = tNum;
        }
    }

    // ASSET HASH PROBE: after tighten-window, so a rigid draw's tightMinIndex/tightNumVertices
    // (if applied) are already final - though the probe deliberately uses the ORIGINAL
    // minIndex/numVertices/startIndex/primitiveCount throughout, per its own spec (the index
    // range read from the bound IB is [StartIndex, StartIndex+indexCount) regardless of any
    // vertex-window tightening). Only CONVERTED draws are eligible; the function itself gates
    // further on the burst being active and on the decal-named/skinned class filter, and costs
    // nothing when assetHashProbe is off.
    if (ffp) ProbeAssetHash(dev, type, baseVertex, minIndex, numVertices, startIndex, primitiveCount,
                            skinned, skinnedFF, scope);

    // STRETCH DETECT: same eligibility (CONVERTED draws only) and the same parameter list as
    // ProbeAssetHash immediately above - the two diagnostics are gated independently and pay
    // nothing for each other. See ProbeStretchDetect's own comment, above, for what it looks
    // for and why it is built the way it is.
    if (ffp && g_settings.stretchDetect)
        ProbeStretchDetect(dev, type, baseVertex, minIndex, numVertices, startIndex, primitiveCount,
                           skinned, skinnedFF, scope);

    // INDEX WINDOW PROBE: same eligibility as ProbeAssetHash/ProbeStretchDetect above (CONVERTED
    // draws only) but unconditionally measured whenever indexWindowProbe is on (default ON - see
    // CheckIndexWindow's own comment for why this is cheap enough to leave on). Uses the game's
    // ORIGINAL minIndex/numVertices, not tightMinIndex/tightNumVertices - see the file comment
    // above CheckIndexWindow for why tightening/widening the reported window cannot affect this
    // bug either way. iwVerdict is read again below, at the draw-issue site, to decide whether
    // indexWindowClamp re-issues this draw from a corrected index buffer.
    IndexWindowVerdict iwVerdict;
    if (ffp) {
        iwVerdict = CheckIndexWindow(g_curIB, type, minIndex, numVertices, startIndex, primitiveCount,
                                     skinned || skinnedFF);
        if (iwVerdict.outside && g_iwpReports < 10) {
            ++g_iwpReports;
            const unsigned belowBy = iwVerdict.below ? (minIndex - iwVerdict.lo) : 0;
            Log("INDEX WINDOW PROBE #%u: frame %u draw %u | ps(albedo sampler)='%s' | v=%u "
                "minIndex=%u prims=%u | referenced=[%u,%u] declared=[%u,%u) | below=%u "
                "overshoot=%u | ib=%p startIndex=%u vb=%p off=%u",
                g_iwpReports, g_frames, g_drawIndexThisFrame, g_curPS.albedoSampler, numVertices,
                minIndex, primitiveCount, iwVerdict.lo, iwVerdict.hi, minIndex,
                minIndex + numVertices, belowBy, iwVerdict.overshoot, static_cast<void*>(g_curIB),
                startIndex, static_cast<void*>(g_stream0), g_stream0Offset);
        }
    }

    // Skip: never reaches the device, so Remix cannot capture it. Reporting success is correct -
    // the engine ignores the return value of a draw, and pretending it failed could send it down
    // an error path.
    if (d == Disp::Skip) {
        ++g_skippedDraws;
        g_worldWrittenSinceDraw = false;
        g_shimMsThisFrame += MsSince(tShim);
        return D3D_OK;
    }

    if (d == Disp::PassThrough) NotePassedThrough(primitiveCount);
    // The character atlas composite, re-issued as fixed function so Remix executes it. On
    // success the game's own draw is NOT also issued - that would composite twice.
    if (IsAtlasComposite(primitiveCount, d) && DrawCompositeFixedFunction(dev)) {
        g_shimMsThisFrame += MsSince(tShim);
        g_worldWrittenSinceDraw = false;
        return D3D_OK;
    }
    // A pass-through draw with a vertex shader bound is one Remix drops with capture off. The
    // census above has already counted it; the bridge round trip is all that is saved here.
    if (d == Disp::PassThrough) {
        if (g_settings.skipDeclinedPassThrough && g_lastVS) {
            ++g_declinedPassSkipped;
            EndFFP(dev, scope);
            g_shimMsThisFrame += MsSince(tShim);
            g_worldWrittenSinceDraw = false;
            return D3D_OK;
        }
        ++g_passForwarded;
    }
    const bool ui = ffp ? false : BeginUIDemote(dev, d == Disp::Hide);
    const unsigned marked = (d == Disp::Mark) ? BeginMark(dev, g_markClearAllStages) : 0u;
    // Everything up to here is ours; the draw call itself is not, so the timer stops before it
    // and resumes after. Otherwise the measurement would blame us for the bridge's own cost.
    g_shimMsThisFrame += MsSince(tShim);
    // A tiled cloth draw: the same mesh as a triangle list over originals + seam duplicates.
    // The game's index buffer is put back the moment the draw returns.
    PhaseAdd(PH_PROBES, tP);
    // The bridge's vertex shader, for THIS draw: null for a converted one (BeginFFP already
    // did it), the game's own for a raw pass-through that is still forwarded.
    SyncBridgeVS(dev, ffp ? nullptr : g_lastVS);
    // injectProbe/injectControl PART 1/2: covers BOTH branches below (the cloth-remap one and
    // the ordinary one) in one call, since the site/hasVS/rhw this draw carries is decided by
    // `d` alone and is identical whichever branch actually issues it - a cloth remap only
    // ever runs when `skinned` is true, which only ever happens when d == Disp::Convert.
    const InjectSuppression injectSup = BeforeShimDraw(dev, d, primitiveCount);
    HRESULT hr;
    // INDEX WINDOW CLAMP eligibility, computed once: a converted, non-skinned draw the probe
    // above (iwVerdict) found referencing an index outside its declared window. Topology
    // (TRIANGLELIST only) is checked separately, where this is used below, so a strip/fan can
    // still be counted apart as "flagged but not correctable" instead of silently folded into
    // "not eligible at all".
    const bool clampEligible = ffp && !skinned && !skinnedFF && g_settings.indexWindowClamp &&
                               iwVerdict.outside;
    if (skinned && g_clothRemap && g_clothRemap->ib && g_clothRemap->tris) {
        // g_curIB already holds what the game last bound (see its declaration comment: "what
        // the game last bound; no bridge call to ask") - GetIndices would only read the same
        // pointer back across the bridge, a synchronous round trip, plus an AddRef/Release pair
        // on it. Safe here because the only thing that ever moves the DEVICE's index buffer
        // without going through Hook_SetIndices (which is what keeps g_curIB current) is this
        // very swap, and it always restores the game's own binding before returning below - so
        // at this point, before the swap, the device's index buffer is exactly g_curIB.
        IDirect3DIndexBuffer9* const gameIB = g_curIB;
        g_internal = true;
        g_internalThread = GetCurrentThreadId();
        g_origSetIndices(dev, g_clothRemap->ib);
        g_internal = false;
        hr = g_origDrawIndexedPrimitive(dev, D3DPT_TRIANGLELIST, baseVertex, minIndex,
                                        numVertices + static_cast<UINT>(g_clothRemap->dupSrc.size()),
                                        0, g_clothRemap->tris);
        g_internal = true;
        g_internalThread = GetCurrentThreadId();
        g_origSetIndices(dev, gameIB);
        g_internal = false;
    } else {
        bool clampCorrected = false;
        if (clampEligible) {
            if (type == D3DPT_TRIANGLELIST) {
                // On success the corrected draw has already been issued and the game's own index
                // buffer already restored - both inside IssueIndexWindowClampedDraw itself.
                clampCorrected = IssueIndexWindowClampedDraw(dev, g_curIB, baseVertex, minIndex,
                                                              numVertices, startIndex, primitiveCount,
                                                              &hr);
            } else if (g_settings.indexWindowClampStrips &&
                      (type == D3DPT_TRIANGLESTRIP || type == D3DPT_TRIANGLEFAN)) {
                // indexWindowClampStrips: the population the TRIANGLELIST-only branch above can
                // never reach - see its own setting comment for the 266342-flagged-draws
                // measurement this exists to fix. Same "already issued and restored" contract as
                // IssueIndexWindowClampedDraw above. g_iwcTopologyRefused is deliberately NOT
                // incremented on a fallback here (its own g_iwcAllocFail already counts that) -
                // that counter means "never attempted", and this was.
                clampCorrected = IssueIndexWindowClampedStripFanDraw(dev, g_curIB, type, baseVertex,
                                                                     minIndex, numVertices, startIndex,
                                                                     primitiveCount, &hr);
            } else {
                ++g_iwcTopologyRefused;
            }
        }
        if (!clampCorrected) {
            // minIndex/numVertices become tightMinIndex/tightNumVertices only for a rigid converted
            // draw with tightenConvertedWindow on and a safe tight range computed above; every other
            // draw through this branch (a pass-through, a converted draw where tightening was
            // refused or is disabled, or one the clamp itself was ineligible for / refused / fell
            // back on) keeps them identical to the game's own declared window, because
            // tightMinIndex/tightNumVertices were seeded from minIndex/numVertices and only
            // overwritten in that one case.
            hr = g_origDrawIndexedPrimitive(dev, type, baseVertex, tightMinIndex, tightNumVertices,
                                            startIndex, primitiveCount);
        }
    }
    AfterShimDrawSite(dev, injectSup);
    PhaseAdd(PH_DRAW, tP);
    const LONGLONG tShim2 = Now();
    // AFTER the draw, not before it.
    //
    // This ran at the TOP of the hook, which is before Classify -> SkinAndBind has captured the
    // part. So the FIRST draw of every new vertex buffer found no pending entry and its slot was
    // dropped; a part drawn only once therefore ended up with NO slots at all and was skipped with
    // "no usable slots". Four of ten parts died that way.
    //
    // Here the capture has already happened, and g_stream0 / g_curPS still describe the game draw -
    // EndFFP below is what restores state, and it has not run yet.
    RemixCollectCharacterSlot(dev, type, baseVertex, minIndex, numVertices, startIndex,
                              primitiveCount);
    // LIVE. Every frame the game draws one of our captured parts, take its objTM and bone palette.
    // This is the whole of the animation: the mesh never changes, only these.
    for (unsigned m = 0; m < g_apiDoneCount; ++m) {
        if (g_apiDone[m].vb != g_stream0) continue;
        ApiDoneMesh& dm = g_apiDone[m];
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c)
                dm.objTM[r][c] = g_vsConst[kRegObjTM + r][c];
        const unsigned reg = dm.boneReg;
        unsigned n = dm.boneCount;
        if (n > kMaxApiBones) n = kMaxApiBones;
        if (reg + n * kRegsPerBone <= kMaxVsConst) {
            for (unsigned b = 0; b < n; ++b)
                memcpy(dm.bones[b], &g_vsConst[reg + b * kRegsPerBone][0], 12 * sizeof(float));
            dm.haveBones = (n > 0);
        }
        break;
    }
    if (skinned) UnbindSkinned(dev);
    else if (skinnedFF) UnbindSkinFF(dev);
    EndMark(dev, marked);
    EndUIDemote(dev, ui);
    EndFFP(dev, scope);
    g_worldWrittenSinceDraw = false;
    g_shimMsThisFrame += MsSince(tShim2);
    return hr;
}

HRESULT WINAPI Hook_SetRenderTarget(IDirect3DDevice9* dev, DWORD index,
                                    IDirect3DSurface9* target) {
    // GetDesc below is a bridge round trip, so this hook is a candidate for the hitch even
    // though it runs only on a target CHANGE. Timed for that reason.
    const LONGLONG tRT = Now();
    if (!InternalHere() && index < kMaxRTSlots) {
        // ONE GetDesc, feeding both questions. Two would double the bridge round trips on the
        // hook this file already names as a hitch candidate.
        D3DSURFACE_DESC d{};
        const bool haveDesc = target && SUCCEEDED(target->GetDesc(&d));
        g_rtBound[index] = (target != nullptr);
        g_rtChannels[index] = haveDesc ? FormatColourChannels(d.Format) : 4u;
        if (index == 0) {
            g_curRenderTarget = target;
            g_rt0Width = haveDesc ? d.Width : 0;
            g_rt0Height = haveDesc ? d.Height : 0;
            // Is this the UI pass? The test is the target's own description, not its position
            // in a list: SR3 composites its UI into a back-buffer-sized 8-BIT surface while the
            // scene stays in A16B16G16R16F, so format and size say what the pass is FOR.
            // Selecting passes by render-target index is a recorded dead end because indices are
            // not stable; format and size are stable descriptions.
            g_uiRenderTarget = haveDesc &&
                               (d.Format == D3DFMT_A8R8G8B8 || d.Format == D3DFMT_X8R8G8B8) &&
                               d.Width >= 1000;
        }
    }
    g_otherMsThisFrame += MsSince(tRT);
    return g_origSetRenderTarget(dev, index, target);
}

void ProbeRenderTargetContents(IDirect3DDevice9* dev);

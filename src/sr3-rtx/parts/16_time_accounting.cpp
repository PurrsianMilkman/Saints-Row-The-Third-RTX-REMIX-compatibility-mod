// parts/16_time_accounting.cpp - where the shim's own time goes.
// NOT a standalone translation unit: #included, in order, by ../sr3rtx.cpp inside its anonymous
// namespace. Originally sr3rtx.cpp lines 10794-11401 (old file:line citations map here).
// ---------------------------------------------------------------- where the shim's own time goes
// The frame report measures the shim at 12.65 ms a frame in the city - 36% of the frame, before a
// single bridge call. Guessing which stage was responsible is what this project keeps recording
// as the wrong move, so each stage of the indexed draw path is timed and printed per frame.
enum Phase { PH_ENTRY, PH_CLASSIFY, PH_TRANSFORMS, PH_UV, PH_APPLY, PH_STAGES, PH_ALBEDO,
             PH_BIND, PH_DEDUP, PH_SKIN, PH_PROBES, PH_DRAW,
             PH_SKIN_PRE, PH_SKIN_LOOP, PH_SKIN_POST, PH_ENTRY_CMD, PH_ENTRY_LIGHT, PH_ENTRY_DRAIN, PH_ENTRY_SKY,
             PH_TIGHTEN, PH_COUNT };
double g_phaseMs[PH_COUNT] = {};
unsigned long long g_decalOffsetDraws = 0, g_stippleColourDraws = 0, g_depthBiasDraws = 0, g_layerDraws = 0, g_biasOffsetDraws = 0,
                   g_decalNamedDraws = 0, g_blendedOffsetDraws = 0, g_pulledDraws = 0,
                   g_decalLowestRegDraws = 0, g_decalFirstAlphaDraws = 0;
unsigned g_curPullWhy = 0;   // 1 layer, 2 decal-named, 4 no depth write / ZFUNC=EQUAL, 8 blended, 16 bias
std::unordered_map<unsigned long long, unsigned> g_frameWindowSeen;   // vertex window + placement -> converted count this frame
// albedoChurnProbe: the effective-albedo texture bound to each draw identity, this frame and
// last. Same identity shape as g_frameWindowSeen above (stream 0 pointer, offset, stride, vertex
// window, index range, world translation to the cm) - built separately in BeginFFP because this
// one remembers WHAT was bound rather than counting repeats.
std::unordered_map<unsigned long long, IDirect3DBaseTexture9*> g_albedoIdentityThisFrame, g_albedoIdentityLastFrame;
unsigned g_curLayer = 0;
D3DMATRIX g_lastWorld = kIdentity, g_lastViewProj = kIdentity;   // stashed by BeginFFP per draw, read by the occlusion culler
double g_phaseMsPrev[PH_COUNT] = {};
unsigned g_phaseFramesPrev = 0;
inline void PhaseAdd(int ph, LONGLONG& t) { g_phaseMs[ph] += MsSince(t); t = Now(); }

// Defined far below, after TextureToBgra (its optional texture dump needs that). Fires for
// the first 8 converted draws whose chosen albedo sampler names a Decal map: measures which
// texture is actually bound and what UV range is sampled - read-only, it changes no render
// state and no pixel.
void ProbeDecalDraw();

Disp BeginFFP(IDirect3DDevice9* dev, FFPScope& scope) {
    g_clothRemap = nullptr;
    LONGLONG tB = Now();
    const Disp d = Classify(dev);
    PhaseAdd(PH_CLASSIFY, tB);
    if (d != Disp::Convert) {
        // cameraOnly touches nothing EXCEPT this, and this is value-identical to what the
        // game's own shader would have read. Without it Remix discards the texcoords and the
        // world renders as flat material colour.
        // NOT in cameraOnly by default. Doing this for every pass-through draw meant 2,320
        // draws a frame and 6,252 D3D9 vertex buffers created across the 32->64-bit bridge,
        // against ~700 on the convert path. The 08-30 run ended in a Vulkan out-of-memory inside
        // Remix - "Heap 1: 33536 MB allocated ... 31571 MB total" - and this was a contributor.
        // The dominant cost is Remix building geometry for every unfiltered draw, but this is
        // ours and it is not obviously needed: no "Unsupported texcoord buffer format (80)"
        // warning appeared in that run at all.
        if (g_settings.cameraOnly && g_settings.cameraOnlyFloatUV) {
            // 2026-09-18 AUDIT FIX: scope.active was never set on this path, so EndFFP's own
            // `if (!scope.active) return;` skipped every restore below and the declaration/uv
            // stream InstallFloatUV is about to bind stayed bound for every draw after this one.
            // Latent while cameraOnly is off (the default); this is the one experiment built to
            // answer whether our own geometry conversion reaches the path tracer at all, so it
            // must not silently corrupt itself the one time it runs.
            //
            // scope.vs/scope.ps are set here too, NOT left null. EndFFP restores them
            // unconditionally once active - SyncBridgeVS(dev, scope.vs) when the vs-bind diet is
            // off, and g_origSetPixelShader(dev, scope.ps) always - and this branch is a
            // PASS-THROUGH draw whose own pixel shader was never touched in the first place. A
            // null scope.ps would have cleared the game's own shader out from under it instead of
            // restoring what was already bound.
            scope.vs = g_lastVS;
            scope.ps = g_lastPS;
            scope.active = true;
            InstallFloatUV(dev, scope, false);
        }
        return d;
    }

    D3DMATRIX world, view, proj;
    // No usable transform means we cannot place it, but Remix's vertex capture reconstructs from
    // shader output and may still get it right - so this is a pass-through, not a hide.
    if (!ComputeTransforms(world, view, proj)) {
        ++g_skipNoObjTM;
        return Because("no usable transform", Disp::PassThrough);
    }

    // An orthographic projection is a shadow-map or utility pass, not the visible scene.
    // Converting it duplicates the world into Remix from a second viewpoint.
    // An auxiliary pass renders the world from somewhere the player is not looking. Hide it:
    // it is a genuine second copy of the scene, and letting Remix capture it is what produced
    // the duplicated and sideways worlds of earlier runs.
    if (kPerspectiveOnly && !IsPerspective(proj)) {
        ++g_skipOrtho;
        g_dispReason = "orthographic projection (shadow/utility pass)";
        return HiddenDisp(false);
    }

    // Mirrored passes. The 2026-08-16 capture holds 500 of 729 mesh instances with a mirrored
    // transform - scale (-1, 1, 1), i.e. an X flip - sitting at the camera position with X
    // negated. An old capture showed the same thing: the player at x=+96.2 and a duplicate at
    // x=-96.2.
    //
    // The test is deliberately NOT "determinant < 0". Whether SR3's view matrix is
    // handedness-preserving at all is unverified, and if its convention flips handedness then
    // an absolute test would reject every draw in the game. Instead the frame's FIRST camera
    // defines what normal looks like, and only passes whose handedness DIFFERS from it are
    // refused. That is self-calibrating: correct whichever convention the engine uses, and it
    // still isolates the odd pass out.
    // Latch the frame's main camera, then refuse anything drawn from a different one.
    if (kMainCameraOnly) {
        if (!g_haveMainCamera) {
            // Aspect is the discriminator: the player's camera matches the back buffer, while
            // shadow maps and cubemap faces are square and paraboloid passes are their own
            // shape. A camera parked at the world origin is a utility pass - Steelport's
            // coordinates are in the hundreds - so it is refused as a main-camera candidate.
            const float aspect =
                (std::fabs(proj._11) > 1e-6f) ? (proj._22 / proj._11) : 0.0f;
            D3DMATRIX invView;
            const bool placed = Invert(view, invView) &&
                                (invView._41 * invView._41 + invView._42 * invView._42 +
                                 invView._43 * invView._43) > 1.0f;
            // THE LOST CAMERA, 2026-09-21. This latch used to adopt the FIRST convertible draw's
            // camera as the frame's main one, and at some angles every early draw of the scene pass
            // is multi-instance or a prepass, so nothing qualified until the 512x288 WATER
            // REFLECTION pass: also 16:9, so the aspect test passed, but with the camera MIRRORED
            // below the water line. It was latched as main; its own draws then died on handedness
            // and every draw from the real camera died as "auxiliary camera". The 60-frame record
            // at the bad angle reads 0 CONVERT on every frame (83 mirrored + 165 auxiliary), so no
            // fixed-function draw reached Remix, its Main camera was never updated that frame,
            // RtCamera::isValid() was false, and injectRTX path traced nothing. That is the user's
            // "the path tracing stops" and the game's raster frame left on screen.
            //
            // So a draw may only become the main camera if it draws INTO the screen-sized target
            // and its view is NOT mirrored: the same two discriminators IsMainSceneCamera already
            // uses for cameraOnly. Before the baseline handedness has been measured, a
            // non-positive determinant counts as mirrored (the baseline measured positive, the
            // reflection camera -1.000).
            if (g_settings.mainCameraScreenSizedOnly) {
                const bool screenSized = g_backBufferW && g_rt0Width == g_backBufferW &&
                                         g_rt0Height == g_backBufferH;
                const float latchDet = RotationDeterminant(view);
                const bool mirrored = g_haveBaseHandedness ? ((latchDet > 0.0f) != g_baseHandedness)
                                                           : (latchDet <= 0.0f);
                if (!screenSized || mirrored) {
                    ++g_skipOtherCamera;
                    g_dispReason = !screenSized
                        ? "camera not yet latched: render target is not the screen's size"
                        : "camera not yet latched: mirrored view (reflection pass)";
                    return HiddenDisp(false);
                }
            }
            if (!placed || (g_backAspect > 0.0f && std::fabs(aspect - g_backAspect) > 0.1f)) {
                ++g_skipOtherCamera;
                g_dispReason = "camera not yet latched, aspect/origin mismatch";
                return HiddenDisp(false);
            }
            g_mainView = view;
            g_mainProj = proj;
            g_haveMainCamera = true;
        } else if (!Same(view, g_mainView)) {
            ++g_skipOtherCamera;
            g_dispReason = "auxiliary camera (view differs from the frame's main one)";
            // Report where this camera actually sits, once. The user describes the remaining
            // artefact as "shapes around me... like the egg", which is a claim about POSITION, and
            // this pass is the largest thing still reconstructed by Remix. If its viewpoint is on
            // the player then a reflection probe is being rebuilt as a shell around them; if it is
            // somewhere else entirely, this is the wrong suspect and the log says so instead of me
            // guessing again.
            if (!g_auxCameraReported) {
                D3DMATRIX invAux;
                if (Invert(view, invAux)) {
                    g_auxCameraReported = true;
                    const float dx = invAux._41 - g_camX, dy = invAux._42 - g_camY,
                                dz = invAux._43 - g_camZ;
                    // Direction as well as position. The first run measured this camera 3.9
                    // units directly BELOW the player's, at the same X/Z and the same 16:9
                    // aspect - which is what a planar reflection about a horizontal plane looks
                    // like (a plane at y = 145.75 mirrors a camera at 147.7 to 143.8). A mirror
                    // negates the view's vertical component; a genuine second viewpoint does not.
                    // Position alone cannot tell those apart, so log the forward and up vectors.
                    D3DMATRIX invMain;
                    const bool haveMain = Invert(g_cameraView, invMain);
                    Log("auxiliary camera at (%.1f %.1f %.1f), main camera at (%.1f %.1f %.1f), "
                        "%.1f units apart | aspect %.3f vs back buffer %.3f",
                        invAux._41, invAux._42, invAux._43, g_camX, g_camY, g_camZ,
                        std::sqrt(dx * dx + dy * dy + dz * dz),
                        (std::fabs(proj._11) > 1e-6f) ? (proj._22 / proj._11) : 0.0f, g_backAspect);
                    Log("    aux  forward (%.3f %.3f %.3f) up (%.3f %.3f %.3f) det %.3f",
                        invAux._31, invAux._32, invAux._33, invAux._21, invAux._22, invAux._23,
                        RotationDeterminant(view));
                    if (haveMain)
                        Log("    main forward (%.3f %.3f %.3f) up (%.3f %.3f %.3f) det %.3f",
                            invMain._31, invMain._32, invMain._33, invMain._21, invMain._22,
                            invMain._23, RotationDeterminant(g_cameraView));
                }
            }
            // Measured 2026-08-18: 415 draws a frame, 395 of them sampling Diffuse_Map, all but
            // three into ONE 512x288 A16B16G16R16F target. 512x288 is 16:9 at 0.4 scale, so this
            // is a reflection or secondary-view render, not a shadow map (those are square).
            // Remix reconstructs every one, which is a whole second copy of the world drawn from
            // somewhere the player is not.
            //
            // Marking them rebinds stage 0, which DOES change what the game renders into that
            // reflection buffer. That only matters if the game's rasterised output can still be
            // seen - so this stays off until the post chain is confirmed hidden, and then it is
            // one switch. Path tracing computes reflections itself; the game's are redundant.
            return HiddenDisp(g_settings.markAuxCamera);
        }
    }

    const float det = RotationDeterminant(view);
    if (!g_haveBaseHandedness && std::fabs(det) > 1e-6f) {
        g_baseHandedness = (det > 0.0f);
        g_haveBaseHandedness = true;
        Log("view handedness baseline: determinant %.3f (%s)", det,
            g_baseHandedness ? "positive" : "negative");
    }
    if (g_settings.skipMirrored && g_haveBaseHandedness && std::fabs(det) > 1e-6f &&
        ((det > 0.0f) != g_baseHandedness)) {
        ++g_skipMirrored;
        g_dispReason = "mirrored handedness (reflection pass)";
        return HiddenDisp(false);
    }

    PhaseAdd(PH_TRANSFORMS, tB);
    g_lastWorld = world;
    g_lastViewProj = FromRegisters(&g_vsConst[kRegProjTM][0], 4);
    scope.vs = g_lastVS;
    scope.ps = g_lastPS;
    scope.active = true;

    g_internal = true;
    g_internalThread = GetCurrentThreadId();
    SyncBridgeVS(dev, nullptr);
    g_origSetPixelShader(dev, nullptr);
    g_ffpActive = true;

    // Bind the converted float2 texture coordinates before the texture stages are set up.
    //
    // Skinned draws are excluded. SkinAndBind replaces the declaration with a float2 FVF of its
    // own, so their coordinates already arrive in a format Remix reads - which is why characters
    // are the one textured population today, and the evidence this fix is aimed at the right
    // thing.
    // ALMOST EVERY converted world draw is a D3D9 instanced draw - 377 of 378 in the run that
    // exposed this - and every one of them carries exactly ONE instance, which the classifier
    // already requires before it will convert at all. So the instancing conveys nothing here: the
    // per-instance transform has already been read out of the stream and applied as the world
    // matrix, and the draw renders a single copy either way.
    //
    // What it does convey is state Remix has to cope with on a fixed-function draw. With
    // rtx.useVertexCapture on, the shader copies of this geometry filled the scene and hid
    // whatever Remix made of it; with vertex capture off - which is what finally removed the
    // magenta - our converted draws ARE the scene, and they enter the view and are dropped again.
    //
    // Resetting the frequency makes them ordinary indexed draws, which is what they already are in
    // every respect that matters. The exact prior setting is restored afterwards because the
    // engine's own draws depend on it.
    const bool deinstance = g_settings.deinstanceConverted && g_instancedDraw &&
                            g_instanceCount <= 1;
    if (deinstance) {
        g_origSetStreamSourceFreq(dev, 0, 1);
        g_origSetStreamSourceFreq(dev, 1, 1);
        scope.deinstanced = true;
        ++g_deinstancedDraws;
    }

    InstallFloatUV(dev, scope, deinstance);

    PhaseAdd(PH_UV, tB);
    // Layers, decals and biased draws are all "a surface in another surface's plane"; in a
    // path tracer they shimmer. Each is pulled toward the eye by a fraction of its distance -
    // scale about the eye: p' = eye + k (p - eye) - so a poster leaves its wall and a second
    // material pass sits 1/1000 nearer than the first. Not for skinned draws (our own ring).
    g_curLayer = 0;
    g_curPullWhy = 0;
    int pullPermille = 0;
    if (!g_curLayout.skinned) {
        // layer: the same vertex window at the same placement, converted already this frame
        if (g_settings.layerOffset && g_stream0) {
            unsigned long long k = static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(g_stream0)) * 0x9E3779B97F4A7C15ull;
            k ^= (static_cast<unsigned long long>(g_stream0Offset) << 32) ^ g_curDrawMinIndex;
            k *= 0x100000001B3ull;
            k ^= (static_cast<unsigned long long>(g_curDrawVertexCount) << 32) ^ g_stream0Stride;
            k *= 0x100000001B3ull;
            // the INDEX range too: material groups of one building share a vertex window with
            // different triangles, and are not layers - only the same triangles drawn again are
            k ^= (static_cast<unsigned long long>(g_curDrawStartIndex) << 32) ^ g_curDrawPrimCount;
            k *= 0x100000001B3ull;
            const float t[3] = {world._41, world._42, world._43};   // placement, instance stream included
            for (int i = 0; i < 3; ++i) {
                const long long q = static_cast<long long>(std::floor(t[i] * 100.0f));
                k ^= static_cast<unsigned long long>(q) * 0x9E3779B1ull;
                k *= 0x100000001B3ull;
            }
            try { g_curLayer = g_frameWindowSeen[k]++; } catch (...) { g_curLayer = 0; }
            if (g_curLayer) { ++g_layerDraws; g_curPullWhy |= 1; pullPermille += static_cast<int>(g_curLayer) * g_settings.layerOffsetPermille; }
        }
        // The four overlay signatures, MEASURED on the 2026-09-11 capture at the shimmering
        // spot: the signs and building logos are `Decal_MapSampler` draws that WRITE depth
        // (zw=1), carry NO bias, test LESSEQUAL and are not repeats of other triangles - so the
        // first three rules could not fire on a single one of them. The rasteriser let them win
        // by draw order under LESSEQUAL; a path tracer has no draw order, and the tie is the
        // shimmer. Whichever signature matches, the draw is pulled ONCE.
        const bool biased = g_rsShadow[D3DRS_DEPTHBIAS] != 0 || g_rsShadow[D3DRS_SLOPESCALEDEPTHBIAS] != 0;
        if (biased) ++g_depthBiasDraws;
        const bool noWrite = g_rsShadow[D3DRS_ZENABLE] != 0 &&
                             (g_rsShadow[D3DRS_ZWRITEENABLE] == 0 || g_rsShadow[D3DRS_ZFUNC] == D3DCMP_EQUAL);
        // decalByLowestReg is the CORRECT test: the sampler at the shader's lowest register.
        // decalByFirstAlpha is the OLD test: whichever sampler name sorts alphabetically first
        // in the CTAB - not necessarily the same sampler at all (see lowestSampler's comment).
        // Both are counted every converted draw regardless of the ini switch, so a capture shows
        // exactly how often the old test was wrong.
        const bool decalByLowestReg = g_curPS.isDecalLowest;
        const bool decalByFirstAlpha = g_curPS.isDecalFirst;
        if (decalByLowestReg) ++g_decalLowestRegDraws;
        if (decalByFirstAlpha) ++g_decalFirstAlphaDraws;
        const bool decalNamed = g_settings.decalByLowestSampler ? decalByLowestReg : decalByFirstAlpha;
        const bool trulyBlended = g_rsShadow[D3DRS_ALPHABLENDENABLE] != 0 &&
                                  !(g_rsShadow[D3DRS_SRCBLEND] == D3DBLEND_ONE &&
                                    g_rsShadow[D3DRS_DESTBLEND] == D3DBLEND_ZERO);
        if (g_settings.decalOffsetPermille > 0) {
            bool pull = false;
            if (noWrite) { ++g_decalOffsetDraws; g_curPullWhy |= 4; pull = true; }
            if (decalNamed && g_settings.decalPrimaryOffset) { ++g_decalNamedDraws; g_curPullWhy |= 2; pull = true; }
            if (trulyBlended && g_settings.blendedOffset) { ++g_blendedOffsetDraws; g_curPullWhy |= 8; pull = true; }
            if (biased && g_settings.biasOffset) { ++g_biasOffsetDraws; g_curPullWhy |= 16; pull = true; }
            if (pull) { ++g_pulledDraws; pullPermille += g_settings.decalOffsetPermille; }
        }
    }
    if (pullPermille > 0) {
        D3DMATRIX invView;
        if (Invert(view, invView)) {
            const float k = 1.0f - static_cast<float>(pullPermille) / 1000.0f;
            const float ex = invView._41, ey = invView._42, ez = invView._43;
            D3DMATRIX pull = kIdentity;
            pull._11 = k; pull._22 = k; pull._33 = k;
            pull._41 = ex * (1.0f - k); pull._42 = ey * (1.0f - k); pull._43 = ez * (1.0f - k);
            world = Multiply(world, pull);
        }
    }
    if (g_curPS.hasStipple) ++g_stippleColourDraws;
    ApplyTransforms(dev, world, view, proj);
    PhaseAdd(PH_APPLY, tB);
    SetupTextureStages(dev);
    PhaseAdd(PH_STAGES, tB);
    if (!g_lightingSetUp) { SetupLighting(dev); g_lightingSetUp = true; }

    // Only stages we actually change need restoring afterwards. Restoring all eight
    // unconditionally cost eight SetTexture calls per converted draw - at ~1,500 draws a frame
    // that is 12,000 bridge round-trips the state shadow never gets to see.
    IDirect3DBaseTexture9* albedo = EffectiveAlbedo();

    // DIAGNOSTIC ONLY: reads g_lastEffectiveAlbedo/g_curPS and the vertex/index buffers and
    // writes a Log block for a qualifying decal draw. Sets no render state.
    ProbeDecalDraw();

    // albedoChurnProbe: has EffectiveAlbedo() picked a DIFFERENT texture for this exact draw
    // than it picked the last time this same draw appeared? The identity deliberately excludes
    // the texture - it is the same shape of key as the layer-offset one above (stream 0 pointer,
    // offset, stride, vertex window, index range, world translation to the nearest cm) - so a
    // hit means the SAME triangles at the SAME place picked a different albedo between frames.
    // That is exactly what the comment above EffectiveAlbedo's render-target exclusion names:
    // "what makes surfaces flicker through unrelated images as the frame's targets are rewritten".
    if (g_settings.albedoChurnProbe && g_stream0) {
        // Which texcoord set the CHOSEN albedo sampler reads, for this converted draw. See the
        // g_albedoUv* counters' declaration above for what the three buckets mean.
        if (g_lastEffectiveAlbedoStage >= 0 && g_lastEffectiveAlbedoStage < 8) {
            const int albUv = g_curPS.samplerUv[g_lastEffectiveAlbedoStage];
            if (albUv < 0) ++g_albedoUvUnknownDraws;
            else if (albUv == 0) ++g_albedoUvSet0Draws;
            else ++g_albedoUvNonZeroDraws;
        }
        unsigned long long ck = static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(g_stream0)) * 0x9E3779B97F4A7C15ull;
        ck ^= (static_cast<unsigned long long>(g_stream0Offset) << 32) ^ g_curDrawMinIndex;
        ck *= 0x100000001B3ull;
        ck ^= (static_cast<unsigned long long>(g_curDrawVertexCount) << 32) ^ g_stream0Stride;
        ck *= 0x100000001B3ull;
        ck ^= (static_cast<unsigned long long>(g_curDrawStartIndex) << 32) ^ g_curDrawPrimCount;
        ck *= 0x100000001B3ull;
        const float wt[3] = {g_lastWorld._41, g_lastWorld._42, g_lastWorld._43};
        for (int i = 0; i < 3; ++i) {
            const long long q = static_cast<long long>(std::floor(wt[i] * 100.0f));
            ck ^= static_cast<unsigned long long>(q) * 0x9E3779B1ull;
            ck *= 0x100000001B3ull;
        }
        try {
            const auto churnIt = g_albedoIdentityLastFrame.find(ck);
            if (churnIt != g_albedoIdentityLastFrame.end() && churnIt->second != g_lastEffectiveAlbedo) {
                ++g_albedoChurnDraws;
                if (g_albedoChurnReports < 10) {
                    ++g_albedoChurnReports;
                    Log("ALBEDO CHURN #%u: ps first='%s' lowest='%s'@%d rank=%d stage=%d | "
                        "prev=%p%s -> new=%p%s | v=%u p=%u at(%.1f %.1f %.1f)",
                        g_albedoChurnReports, g_curPS.firstSampler, g_curPS.lowestSampler,
                        g_curPS.lowestSamplerReg, g_curPS.albedoRank, g_lastEffectiveAlbedoStage,
                        static_cast<void*>(churnIt->second),
                        churnIt->second && g_rtTextures.count(churnIt->second) ? "(RT)" : "",
                        static_cast<void*>(g_lastEffectiveAlbedo),
                        g_lastEffectiveAlbedo && g_rtTextures.count(g_lastEffectiveAlbedo) ? "(RT)" : "",
                        g_curDrawVertexCount, g_curDrawPrimCount,
                        g_lastWorld._41, g_lastWorld._42, g_lastWorld._43);
                }
            }
            g_albedoIdentityThisFrame[ck] = g_lastEffectiveAlbedo;
        } catch (...) {}
    }

    // SR3's character atlas is a RENDER TARGET, and Remix hashes only what the game
    // uploads - so it receives an all-zero texture and the character renders black.
    // Re-uploaded here through UpdateTexture, which is the path Remix does hash.
    albedo = RemixReadableAlbedo(dev, albedo);

    // Keep each mesh on the texture it was first drawn with. Keyed on the vertex buffer plus
    // the base vertex, which together identify a mesh inside SR3's shared buffers; the pointer
    // alone would merge every mesh sharing a buffer onto one texture.
    // The player's chosen clothing colours, evaluated per texel into a texture of our own.
    // Hair and skin take the same route, and the same cutout: their ranked albedo is a mask
    // (hair's Diffuse_Map is directional data) or a greyscale map the shader colours from
    // constants and matcaps - see HairAlbedoHsv and SkinAlbedo.
    IDirect3DBaseTexture9* cloth = ClothAlbedo(dev);
    if (!cloth) cloth = HairAlbedo(dev);
    if (!cloth) cloth = SkinAlbedo(dev);
    if (cloth) {
        albedo = cloth;
        // A generated texture that carries a cutout is alpha-tested, the way a shader texkill is
        // in SetupTextureStages. That function has already run, and decided TFACTOR alpha for
        // this draw because the game's own state shows no test and ps[6] has no texkill - the
        // game gets its cutout from the pixel's alpha and whatever blend it draws with. Under
        // fixed function the texture's alpha has to be routed through the test explicitly, and
        // the test is also the signal Remix reads as "cutout" rather than "glass". The game
        // blends this alpha (src=5 dst=6); 128 splits a DXT5 cutout cleanly. Saved and restored
        // by the same EndFFP path as the texkill case.
        if (g_settings.clothCutout && g_clothCutoutTex.count(cloth)) {
            if (!g_alphaStateOverridden) {
                g_savedAlphaTest = ShadowGetRS(dev, D3DRS_ALPHATESTENABLE);
                g_savedAlphaRef = ShadowGetRS(dev, D3DRS_ALPHAREF);
                g_savedAlphaFunc = ShadowGetRS(dev, D3DRS_ALPHAFUNC);
                g_alphaStateOverridden = true;
            }
            ShadowSetRS(dev, D3DRS_ALPHATESTENABLE, TRUE);
            ShadowSetRS(dev, D3DRS_ALPHAREF, 128);
            ShadowSetRS(dev, D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);
            ShadowSetTSS(dev, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        }
        // A TILED texture: the skinned copy shifts each component's u0 into its tile and appends
        // the duplicated seam vertices (SkinAndBind reads g_clothRemap), the draw goes through a
        // private triangle list that references them, and the texture matrix spreads `tiles`
        // tiles across u. Skinned draws only - that is the path that rewrites vertices. The
        // layout is only valid for the draw it was baked from; anything else keeps one tile.
        if (g_settings.clothDecalTiles && g_curLayout.skinned) {
            const auto lt = g_clothTileLayout.find(cloth);
            if (lt != g_clothTileLayout.end() && lt->second.tiles > 1 &&
                lt->second.firstVertex == g_curDrawFirstVertex &&
                lt->second.vertexCount == g_curDrawVertexCount &&
                lt->second.minIndex == g_curDrawMinIndex &&
                lt->second.shiftRaw.size() == g_curDrawVertexCount) {
                ClothTileLayout& lay = lt->second;
                if (!lay.ib && !lay.listIdx.empty()) {
                    const bool wasInt = g_internal;
                    g_internal = true;
                    g_internalThread = GetCurrentThreadId();
                    IDirect3DIndexBuffer9* nib = nullptr;
                    if (SUCCEEDED(dev->CreateIndexBuffer(static_cast<UINT>(lay.listIdx.size() * 4),
                                                         D3DUSAGE_WRITEONLY, D3DFMT_INDEX32,
                                                         D3DPOOL_DEFAULT, &nib, nullptr)) && nib) {
                        void* m = nullptr;
                        if (SUCCEEDED(nib->Lock(0, 0, &m, 0)) && m) {
                            memcpy(m, lay.listIdx.data(), lay.listIdx.size() * 4);
                            nib->Unlock();
                            lay.ib = nib;
                            lay.tris = static_cast<UINT>(lay.listIdx.size() / 3);
                        } else {
                            nib->Release();
                            lay.listIdx.clear();   // do not retry into a buffer that will not lock
                        }
                    } else {
                        lay.listIdx.clear();
                    }
                    g_internal = wasInt;
                }
                g_clothRemap = &lay;
                float su = 1.0f, sv = 1.0f;
                if (g_curLayout.texcoordType == D3DDECLTYPE_SHORT2) su = sv = CurrentShortUVScale();
                int tu = -1, tv = -1;
                TilingForAlbedo(tu, tv);
                if (tu >= 0 && tu < static_cast<int>(kMaxVsConst)) su *= g_vsConst[tu][0];
                if (tv >= 0 && tv < static_cast<int>(kMaxVsConst)) sv *= g_vsConst[tv][0];
                D3DMATRIX uv = kIdentity;
                uv._11 = su / static_cast<float>(lay.tiles);
                uv._22 = sv;
                g_origSetTransform(dev, D3DTS_TEXTURE0, &uv);
                g_appliedU = uv._11;
                g_appliedV = sv;
                ShadowSetTSS(dev, 0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT2);
            }
        }
    }

    PhaseAdd(PH_ALBEDO, tB);
    g_lastAlbedoFromCache = false;
    if (g_settings.cacheMeshAlbedo && g_stream0 && !g_curLayout.skinned) {
        // The pixel shader is part of the key. Without it, one mesh drawn with several
        // materials - the same car body in different paint, a wall with a variant decal -
        // collapses onto whichever texture was seen first, which is a texture-substitution bug
        // of exactly the kind this cache exists to prevent. Measured before the change: 921
        // "restores" per frame against 1,396 converted draws, far too many to be streaming
        // eviction alone. The shader does not change when the streamer evicts a texture, so
        // including it costs nothing against the case we actually want to catch.
        // SKINNED draws are excluded outright. The key is (buffer, base vertex, pixel shader),
        // and every NPC face in the game shares all three: the same head mesh out of the same
        // buffer, drawn by the same character shader, differing only in which face TEXTURE is
        // bound. So the cache treats them as one mesh and rebinds whichever face it saw first
        // onto all of them - which is the reported "faces of npcs alternate between the wrong
        // faces", with the alternation being whichever NPC happened to populate the entry.
        //
        // Adding the shader to the key was the previous fix for this same shape of bug (a car
        // body in two paints) and it cannot help here, because the shader is shared too. There
        // is no property of a skinned draw available at this point that separates two characters
        // - the pose and objTM would, but this cache is consulted for every draw and hashing
        // those per draw is the cost the dedup already showed is not worth paying.
        //
        // Nor is the cache needed for them: it exists to survive the STREAMER evicting a mesh's
        // unique texture, and character faces are bound per character every frame anyway.
        const unsigned long long key =
            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(g_stream0)) * 1000003ull
            + static_cast<unsigned long long>(g_meshKeyBaseVertex) * 31ull
            + static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(g_lastPS));
        const auto it = g_meshAlbedo.find(key);
        if (it != g_meshAlbedo.end()) {
            if (it->second && it->second != albedo) {
                albedo = it->second;      // the streamer has swapped it; put the original back
                ++g_albedoRestored;
                g_lastAlbedoFromCache = true;
            }
        } else if (albedo && !g_rtTextures.count(albedo) &&
                   g_meshAlbedo.size() < kMaxMeshAlbedo) {
            // AddRef. This cache exists PRECISELY to hold a texture across the streamer evicting
            // it, so without a reference of our own it is holding a pointer across the one event
            // that destroys the object - and then binding it. That is a use-after-free whose
            // trigger is heavy streaming, which is why the crash shows up on fast movement and
            // on freefalling off the penthouse and not while standing still.
            //
            // It is also a texture-substitution bug short of the crash: a freed texture's address
            // gets recycled by the next one streamed in, and the cache then binds an unrelated
            // texture to the mesh. "Surfaces flickering through unrelated images" is a symptom
            // this project has chased before under other explanations.
            //
            // Capped, because a reference held is a texture the streamer cannot evict, and
            // pinning an unbounded number of them would trade a crash for VRAM exhaustion.
            //
            // "At the cap the cache simply stops growing" was the original plan and it was
            // wrong. Measured 2026-08-19 with the cache sitting at its full 4,096 entries: once
            // there, no NEWLY streamed mesh can ever be cached, so no newly streamed mesh is
            // protected from the streamer swapping its texture - and the world progressively
            // loses its textures the longer you play. That is the same permanent cliff the
            // bind-pose cache had, and it wants the same answer: flush and rebuild. The meshes
            // actually on screen are re-cached within a frame or two, and the pinned textures
            // are released back to the streamer.
            albedo->AddRef();
            g_meshAlbedo[key] = albedo;
            if (g_meshAlbedo.size() >= kMaxMeshAlbedo) {
                for (auto& kv : g_meshAlbedo)
                    if (kv.second) kv.second->Release();
                g_meshAlbedo.clear();
                ++g_meshAlbedoFlushes;
                Log("mesh albedo cache flushed at %u entries (flush #%u) - it had stopped "
                    "protecting anything newly streamed",
                    static_cast<unsigned>(kMaxMeshAlbedo), g_meshAlbedoFlushes);
            }
        }
    }

    g_lastBoundAlbedo = albedo;
    if (albedo != g_curTexture[0]) {
        g_origSetTexture(dev, 0, albedo);
        scope.touched |= 1u;
    }
    for (DWORD s = 1; s < 8; ++s) {
        if (g_curTexture[s]) {
            g_origSetTexture(dev, s, nullptr);
            scope.touched |= (1u << s);
        }
    }
    g_internal = false;

    ++g_ffpConverted;
    PhaseAdd(PH_BIND, tB);
    return Disp::Convert;
}

void EndFFP(IDirect3DDevice9* dev, const FFPScope& scope) {
    g_clothRemap = nullptr;
    if (!scope.active) return;
    g_internal = true;
    g_internalThread = GetCurrentThreadId();
    // Put the alpha test back exactly as the engine had it. This function restores textures and
    // shaders and nothing else, so a render state left changed here would follow the engine into
    // its own draws - and alpha test decides which of its pixels survive.
    if (g_alphaStateOverridden) {
        ShadowSetRS(dev, D3DRS_ALPHATESTENABLE, g_savedAlphaTest);
        ShadowSetRS(dev, D3DRS_ALPHAREF, g_savedAlphaRef);
        ShadowSetRS(dev, D3DRS_ALPHAFUNC, g_savedAlphaFunc);
        g_alphaStateOverridden = false;
    }
    for (DWORD s = 0; s < 8; ++s)
        if (scope.touched & (1u << s)) g_origSetTexture(dev, s, g_curTexture[s]);
    if (scope.uvFreq) g_origSetStreamSourceFreq(dev, g_uvStream, 1);
    if (scope.deinstanced) {
        g_origSetStreamSourceFreq(dev, 0, g_streamFreq[0]);
        g_origSetStreamSourceFreq(dev, 1, g_streamFreq[1]);
    }
    if (scope.uvStream) g_origSetStreamSource(dev, g_uvStream, nullptr, 0, 0);
    if (scope.decl) g_origSetVertexDeclaration(dev, scope.decl);
    // With the diet the bridge keeps null here; the next forwarded draw syncs what it needs.
    if (!g_settings.dietVsBinds) SyncBridgeVS(dev, scope.vs);
    g_origSetPixelShader(dev, scope.ps);
    g_ffpActive = false;
    g_internal = false;
}


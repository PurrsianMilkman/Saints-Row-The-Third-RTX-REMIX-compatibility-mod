// parts/29_present.cpp - Hook_Present and the end-of-frame report.
// NOT a standalone translation unit: #included, in order, by ../sr3rtx.cpp inside its anonymous
// namespace. Originally sr3rtx.cpp lines 23953-25143 (old file:line citations map here).
// Set before anything Present-time runs, so the cloth baker can register its result as a real
// texture instead of only a file on disk.
HRESULT WINAPI Hook_Present(IDirect3DDevice9* dev, const RECT* src, const RECT* dst, HWND window,
                            const RGNDATA* dirty) {
    g_presentDevice = dev;
    ++g_frames;
    TickAssetHashProbeBurst();   // ASSET HASH PROBE: end the burst and log its summary, if due
    if ((g_frames % 300) == 0) PruneRecentWrites();
    KickOcclusionJob();
    SweepTightWin();
    SweepOrphans();
    // One render-target read per frame, on the render thread, after the scene is drawn.
    if (g_settings.rtContentProbe) ProbeRenderTargetContents(dev);
    if (g_settings.bakeShaderAlbedo) BakeShaderReady(dev);
    RemixBuildCharacter();
    RemixApiTestTick();

    // Between frames, so a capture never starts with a frame already half recorded.
    PollCaptureKey();

    // Present-to-Present is the frame time the player actually experiences.
    const LONGLONG tPresentEntry = Now();
    if (g_qpcFreq.QuadPart && g_lastPresent.QuadPart) {
        const double ms = static_cast<double>(tPresentEntry - g_lastPresent.QuadPart) * 1000.0 /
                          static_cast<double>(g_qpcFreq.QuadPart);
        if (ms < 10000.0 && !g_heldSinceLastPresent) {   // ignore alt-tab, loading pauses and frame-step holds
            g_frameMsAccum += ms;
            g_shimMsAccum += g_shimMsThisFrame;
            g_otherMsAccum += g_otherMsThisFrame;
            g_presentMsAccum += g_presentMsLast;
            ++g_timedFrames;
            if (ms > g_frameMsWorst) g_frameMsWorst = ms;
            if (g_shimMsThisFrame > g_shimMsWorst) g_shimMsWorst = g_shimMsThisFrame;
            if (g_presentMsLast > g_presentMsWorst) g_presentMsWorst = g_presentMsLast;
            if (ms >= 33.0) ++g_hitches33;
            if (ms >= 100.0) ++g_hitches100;

            // One line per hitch, partitioning the interval. Everything not accounted for by the
            // four measured terms is the game's own CPU work plus bridge submission, so the
            // remainder is as informative as the parts: a hitch that is nearly all "present" is
            // Remix's end-of-frame work and no amount of tuning at the D3D9 boundary will touch
            // it, whereas one that is nearly all remainder is submission stalling on the bridge.
            if (ms >= static_cast<double>(g_settings.hitchMs) && g_hitchLines < kMaxHitchLines) {
                ++g_hitchLines;
                const double rest = ms - g_presentMsLast - g_ourPresentMsLast -
                                    g_shimMsThisFrame - g_otherMsThisFrame;
                Log("HITCH frame %u: %.1f ms = present %.1f + our-present %.1f + draws %.1f + "
                    "other-hooks %.1f + game/bridge %.1f | draws %u | new shaders %u textures %u",
                    g_frames, ms, g_presentMsLast, g_ourPresentMsLast, g_shimMsThisFrame,
                    g_otherMsThisFrame, rest, g_drawIndexThisFrame, g_shadersCreatedThisFrame,
                    g_texturesCreatedThisFrame);
            }
        }
    }
    g_lastPresent.QuadPart = tPresentEntry;
    g_heldSinceLastPresent = false;
    // The skinning ring restarts every frame. DISCARD on the first lock of a frame tells the
    // driver the old contents are dead, so it can hand back fresh memory instead of waiting
    // for the GPU to finish reading last frame's vertices.
    g_skinRingPos = 0;
    g_skinRingFresh = true;
    g_skinFrameKeyCount = 0;
    // THE SHAPE BAKE: per-frame build/rebuild budgets and the once-per-frame churn log latch.
    g_morphBakeBuildsThisFrame = 0;
    g_morphBakeRebuildsThisFrame = 0;
    g_morphBakeChurnLoggedThisFrame = false;
    g_frameWindowSeen.clear();
    // albedoChurnProbe: this frame's identities become "last frame" for the next comparison.
    g_albedoIdentityLastFrame.swap(g_albedoIdentityThisFrame);
    g_albedoIdentityThisFrame.clear();
    g_shimMsThisFrame = 0.0;
    g_otherMsThisFrame = 0.0;
    g_shadersCreatedThisFrame = 0;
    g_texturesCreatedThisFrame = 0;
    g_inOcclusionQuery = false;
    if (g_frames == 60 || g_frames % 600 == 0) {
        const double f = static_cast<double>(g_frames);
        Log("frame %u | draws %.0f/frame | FFP converted %.0f/frame (%.1f%%)",
            g_frames, g_drawsTotal / f, g_ffpConverted / f,
            g_drawsTotal ? 100.0 * g_ffpConverted / g_drawsTotal : 0.0);
        Log("    NOT FORWARDED: occlusion proxies %.1f/frame skipped of %.1f seen inside queries "
            "(%.1f <=%d prims, %.1f larger and forwarded) | Issue calls skipped %.1f/frame | "
            "declined pass-through (VS bound, Remix drops them with capture off) skipped %.1f/frame, "
            "%.1f/frame still forwarded",
            g_occlDrawsSkipped / f, g_occlDrawsSeen / f, g_occlDrawsSmall / f,
            g_settings.occlusionProxyMaxPrims, g_occlDrawsLarge / f, g_occlIssueSkipped / f,
            g_declinedPassSkipped / f, g_passForwarded / f);
        Log("    RASTERIZED-FRAME INVARIANT (blockEngineOutputToScreen=%d): %.2f/frame blocked - "
            "classify(indexed) pass %.2f skip %.2f, classify(non-indexed) pass %.2f skip %.2f, "
            "DrawPrimitiveUP pass %.2f skip %.2f, DrawIndexedPrimitiveUP pass %.2f skip %.2f | "
            "g_rtTextures-missed fallback hits %.2f/frame",
            g_settings.blockEngineOutputToScreen,
            (g_rtInvariantIndexedPass + g_rtInvariantIndexedSkip + g_rtInvariantDrawPass +
             g_rtInvariantDrawSkip + g_rtInvariantUPPass + g_rtInvariantUPSkip +
             g_rtInvariantIndexedUPPass + g_rtInvariantIndexedUPSkip) / f,
            g_rtInvariantIndexedPass / f, g_rtInvariantIndexedSkip / f,
            g_rtInvariantDrawPass / f, g_rtInvariantDrawSkip / f,
            g_rtInvariantUPPass / f, g_rtInvariantUPSkip / f,
            g_rtInvariantIndexedUPPass / f, g_rtInvariantIndexedUPSkip / f,
            g_rtInvariantFallbackHits / f);
        Log("    INJECT PROBE/CONTROL (injectProbe=%d injectControl=%d): %.0f frames measured - "
            "trigger preceded the final composite %.3f%% of frames (THE BUG), followed it (correct) "
            "%.3f%%, no shim trigger seen (Remix's own Present fallback injects) %.3f%%",
            g_settings.injectProbe, g_settings.injectControl, static_cast<double>(g_injectFramesTotal),
            g_injectFramesTotal ? 100.0 * g_injectFramesTriggerBeforeComposite / g_injectFramesTotal : 0.0,
            g_injectFramesTotal ? 100.0 * g_injectFramesTriggerAfterComposite / g_injectFramesTotal : 0.0,
            g_injectFramesTotal ? 100.0 * g_injectFramesNoShimTrigger / g_injectFramesTotal : 0.0);
        Log("        counts: before-composite %llu, after-composite %llu, no-trigger %llu | PART 2(a) "
            "suppressed (RHW, forced off-identity) %llu, reported-only (satisfied, NOT RHW, left "
            "unaltered) %llu | PART 2(b) deliberate quad issued %llu, failed %llu (last hr 0x%08lX)",
            g_injectFramesTriggerBeforeComposite, g_injectFramesTriggerAfterComposite,
            g_injectFramesNoShimTrigger, g_injectSuppressedRHW, g_injectReportedNonRHW,
            g_deliberateQuadsIssued, g_deliberateQuadsFailed,
            static_cast<unsigned long>(g_deliberateQuadLastFail));
        {
            const double fr = static_cast<double>(g_frames - g_phaseFramesPrev);
            double d2[PH_COUNT];
            for (int i = 0; i < PH_COUNT; ++i) { d2[i] = (g_phaseMs[i] - g_phaseMsPrev[i]) / (fr > 0 ? fr : 1); g_phaseMsPrev[i] = g_phaseMs[i]; }
            g_phaseFramesPrev = g_frames;
            Log("    PROFILE ms/frame (indexed draw path, since the last report): entry %.2f (of which command-block %.2f, lights %.2f, drain %.2f, sky-probe %.2f) | "
                "BeginFFP: classify %.2f transforms %.2f uv %.2f apply %.2f stages %.2f albedo %.2f "
                "bind %.2f | dedup %.2f | skin %.2f | probes %.2f | tighten %.2f | draw(bridge) %.2f",
                d2[PH_ENTRY], d2[PH_ENTRY_CMD], d2[PH_ENTRY_LIGHT], d2[PH_ENTRY_DRAIN], d2[PH_ENTRY_SKY], d2[PH_CLASSIFY], d2[PH_TRANSFORMS], d2[PH_UV], d2[PH_APPLY],
                d2[PH_STAGES], d2[PH_ALBEDO], d2[PH_BIND], d2[PH_DEDUP], d2[PH_SKIN],
                d2[PH_PROBES], d2[PH_TIGHTEN], d2[PH_DRAW]);
            Log("    SKIN split ms/frame: pre-loop %.2f (mesh, palette provenance, ring lock) | "
                "loop %.2f | post %.2f (api collect, dump, bind) | fast-path draws %u, threaded %u",
                d2[PH_SKIN_PRE], d2[PH_SKIN_LOOP], d2[PH_SKIN_POST], g_skinFastDraws, g_skinFastThreaded);
            Log("    BRIDGE CALLS/frame (what the game asked for): vsConst %.0f (%.0f float4s, %.0f KB) "
                "dropped %.0f | psConst %.0f (%.0f float4s) | setVS %.0f dropped %.0f, bridge syncs %.0f "
                "| setPS %.0f | setTexture %.0f | renderState %.0f | tss %.0f | sampler %.0f | "
                "stream %.0f | decl %.0f | transform %.0f",
                g_callVsConst / f, g_callVsConstRegs / f, g_callVsConstRegs * 16.0 / 1024.0 / f,
                g_dietVsConstDropped / f, g_callPsConst / f, g_callPsConstRegs / f,
                g_callSetVS / f, g_dietVsBindDropped / f, g_bridgeVsSyncs / f, g_callSetPS / f,
                g_callSetTex / f, g_callRS / f, g_callTSS / f, g_callSS / f, g_callStream / f,
                g_callDecl / f, g_callTransform / f);
            Log("    OCCLUSION PLAN: proxies %.0f/frame (%.0f placed by objTM, %.0f not; %u distinct "
                "proxy vertex buffers) | occluder candidates %.0f/frame rigid converted draws (%.0f "
                "instanced), %.0f opaque [blend %.0f, alpha-test %.0f, shader cutout %.0f], %.0f "
                "triangles/frame (%.0f draws >1000 tris, %.0f draws 100-1000)",
                g_occlDrawsSeen / f, g_occlProxyObjTM / f, g_occlProxyNoObjTM / f, g_occlProxyVBCount,
                g_occlCandDraws / f, g_occlCandInstanced / f, g_occlCandOpaque / f, g_occlCandBlend / f,
                g_occlCandATest / f, g_occlCandTexkill / f, g_occlCandTris / f, g_occlCandBig / f,
                g_occlCandMid / f);
            {
                std::lock_guard<std::mutex> lk(g_occlMu);
                const double j = g_occlJobsRun ? static_cast<double>(g_occlJobsRun) : 1.0;
                Log("    OCCLUSION CULL (%s): jobs %llu run, %llu dropped (worker busy) | per job: %.0f "
                    "boxes tested, %.0f occluders rasterised (%.0f tris), %.1f ms on the worker | "
                    "%.0f boxes/job OCCLUDED (%.1f%%), %.0f visible at the near plane, %.0f within %d units "
                    "(never culled), %.0f partly off screen | answered 0 pixels %.0f/frame | mesh cache %u "
                    "(%llu unreadable) | budget-skipped occluders %.0f/job | "
                    "occluders offered %.0f/frame: no mesh yet %.0f, off screen %.0f, taken %.0f -> %.0f per job",
                    g_settings.occlusionDryRun ? "DRY RUN - answers still visible" : "LIVE",
                    g_occlJobsRun, g_occlJobsDropped, g_occlTested / j, g_occlOccluders / j,
                    g_occlTrisRaster / j, g_occlWorkerMs / j, g_occlCulled / j,
                    g_occlTested ? 100.0 * g_occlCulled / g_occlTested : 0.0, g_occlNearVisible / j,
                    g_occlNearDistVisible / j, g_settings.occlusionMinDistance,
                    g_occlOffscreenVisible / j, g_occlAnswered0 / f,
                    static_cast<unsigned>(g_occlMeshes.size()), g_occlMeshBad, g_occlSkippedBudget / j,
                    g_occlRecOffered / f, g_occlRecNoMesh / f, g_occlRecOffscreen / f, g_occlRecTaken / f,
                    g_occlJobOccluders / j);
                const double sj = g_occlStillJobs ? static_cast<double>(g_occlStillJobs) : 1.0;
                Log("    OCCLUSION FLICKER: camera-still jobs %llu | verdict flips: to visible %llu (own object %llu, "
                    "pixel unwritten %llu, inside margin %llu, farther %llu), to culled %llu | pool churn while still: "
                    "used +%.1f/-%.1f, offered +%.1f/-%.1f per job | self id found for %.0f of %.0f boxes/job | "
                    "own geometry outside its box %.1f boxes/job (max poke %.2f u) | query->object remaps %.1f/frame | "
                    "render offset max %.1f | keyed by %s, self tolerance %d cm",
                    g_occlStillJobs, g_occlFlipVis, g_occlFlipSelf, g_occlFlipUnwritten, g_occlFlipMargin, g_occlFlipFarther,
                    g_occlFlipCull, g_occlChurnUsedAdd / sj, g_occlChurnUsedRem / sj, g_occlChurnOffAdd / sj, g_occlChurnOffRem / sj,
                    g_occlSelfFound / j, g_occlTested / j, g_occlPokeBoxes / j, g_occlPokeMax, g_occlQueryRemaps / f,
                    g_occlRenderOffsetMax, g_settings.occlusionKeyByObject ? "object" : "query pointer",
                    g_settings.occlusionSelfToleranceCm);
                Log("    OCCLUSION POOL: unwritten-pixel flips by the writer's fate: not offered %llu, offered-not-used %llu, "
                    "used-not-covering %llu | cap-skipped %.0f/job, sticky kept past the cap %.0f/job | occluder meshes "
                    "invalidated/frame: whole-buffer %.1f, range-hit %.1f, range-spared %.1f (%s)",
                    g_occlFlipNotOffered, g_occlFlipNotUsed, g_occlFlipUsedNotCov, g_occlCapSkipped / j, g_occlStickyKept / j,
                    g_occlMeshInvWhole / f, g_occlMeshInvRange / f, g_occlMeshInvSpared / f,
                    g_settings.occlusionRangeInvalidation ? "range-aware" : "whole-buffer only");
                g_occlFlipExplained = 0;
            }
        }
        Log("    not converted: screen-space %.0f  skinned %.0f  ortho %.0f  "
            "vertex-format %.0f  untextured %.0f  instanced %.0f  mirrored %.0f  other-camera "
            "%.0f  no camera %.0f  other %.0f (per frame)",
            g_skipScreenSpace / f, g_skipSkinned / f, g_skipOrtho / f,
            g_skipVertexFormat / f, g_skipUntextured / f, g_skipInstanced / f, g_skipMirrored / f,
            g_skipOtherCamera / f, g_skipNoVP / f, g_skipNotEligible / f);
        // The two prepass shapes, reported apart. The stipple one used to CONVERT and render
        // white, so a large number here is the size of the "most surfaces are white" problem.
        Log("    prepasses hidden: sampler-less %.0f/frame, stipple-with-no-colour %.0f/frame "
            "(these used to convert WHITE) | post/composite quads marked %.0f/frame",
            g_skipUntextured / f, g_skipStipplePrepass / f, g_screenSpaceMarked / f);
        // FALSIFIER for compositeToTexturePass. SR3 bakes each character into a 2048x1024
        // render target and samples it as that character's albedo; skipping the quads that
        // fill it renders the character BLACK (Remix's own captures: a real face at mean 182.7
        // before 2026-08-23, pure black after). This counts what the rule actually CHANGED -
        // from SKIP to PASS - not how many draws matched it, which is the measurement this
        // project has shipped without twice.
        //
        // 0/frame means no composite targets an off-screen surface and the rule did NOTHING;
        // the black characters then have another cause. It was 41/frame on the frame dump this
        // was written against. A number in the hundreds means it is catching the screen-sized
        // resolve chain as well, which would repaint the rasterised frame - back it out.
        Log("        composite into an off-screen texture (engine reads it back as a texture, "
            "so it must never be SKIPPED): %.1f draws/frame passed through instead - gate %s",
            g_compositeToTexture / f, g_settings.compositeToTexturePass ? "ON" : "OFF");
        // Non-zero here is the doubled-character fix doing work: each of these was a whole mesh
        // path-traced a second time, coincident with its real material-pass copy.
        Log("    non-colour render target (fewer than 3 channels, cannot hold an albedo): "
            "%.1f draws/frame hidden - gate %s",
            g_skipNonColourTarget / f, g_settings.skipNonColourTargets ? "ON" : "OFF");
        Log("        of which: %.1f draws/frame had a non-colour target at slot 0 but a COLOUR "
            "target bound elsewhere (the MRT G-buffer pass - these must NOT be hidden)",
            g_mrtColourElsewhere / f);
        // What a duplicate's disposition WORD actually is - mirrors HiddenDisp(true) without
        // calling it (that would tick g_markRefused from the report path). hiddenPassMode=2,
        // the deployed default, resolves to Skip: these draws are SKIPPED, not hidden.
        const char* dedupDispWord =
            g_settings.hiddenPassMode == 0 ? "passed through" :
            g_settings.hiddenPassMode == 1 ? "demoted to UI overlay" :
            g_settings.hiddenPassMode == 2 ? "SKIPPED" :
            (g_marker ? "marked" : "passed through (marker unavailable)");
        Log("        DEDUP: %.1f draws/frame %s as exact duplicates already converted this "
            "frame (%.0f keys held, %.1f/frame did not fit - non-zero means duplicates were "
            "MISSED) | skinned %s, all %s",
            g_skinRepeatHidden / f, dedupDispWord, static_cast<double>(g_skinFrameKeyCount),
            g_dedupKeyOverflow / f,
            g_settings.dedupSkinned ? "ON" : "OFF", g_settings.dedupAll ? "ON" : "OFF");
        Log("        DEDUP declined on %.1f draws/frame: instanced, and their placement could "
            "not be read - merging those would delete objects rather than copies",
            g_dedupUnjudged / f);
        // MORPH is the per-character customisation delta in stream 2. Draws whose shader
        // reads it (dcl_position1) against meshes actually decoded with it: if the first is
        // non-zero and the second is 0, every one of them is rendering the base mesh - a
        // head that is nobody's.
        // g_morphDecodes was dropped when the morph moved out of GetBaseMesh and into the
        // per-draw path; it read a permanent 0 and said nothing. A counter that lies is
        // worse than no counter - MORPH APPLIED below is the live one.
        Log("        MORPH: %.1f draws/frame use a shader that reads the morph stream",
            g_morphDraws / f);
        {
            char names[300] = "";
            for (unsigned i = 0; i < g_mrtSamplerReports; ++i) {
                if (i) strcat_s(names, ", ");
                strcat_s(names, g_mrtSamplerNames[i]);
            }
            Log("        DEFERRED G-BUFFER PASS (pixel shader writes >1 render target, so it "
                "outputs no colour): %.1f draws/frame, of which %.1f/frame are CONVERTED | "
                "single-target draws converted %.1f/frame | their albedo samplers: %s",
                g_mrtWouldHide / f, g_mrtConverted / f, g_singleTargetConverted / f, names);
            Log("        ...of which %.1f SKINNED draws/frame passed through instead of "
                "converted by skipDeferredGBuffer (gate %s). Pass-through, NOT skipped - the "
                "engine reads this pass back for its deferred lighting. Unskinned G-buffer "
                "draws are LEFT ALONE: the world's only converted copy is this pass",
                g_gbufferHidden / f, g_settings.skipDeferredGBuffer ? "ON" : "OFF");
        }
        // Printed even at zero, so long as stretchDetect is on: a run with zero hits has to
        // be told apart from the detector not running at all. "unreadable" is a THIRD
        // outcome, neither flagged nor cleared - the index buffer was dynamic, or no bind
        // pose was cached yet, so no verdict was possible for that draw.
        if (g_settings.stretchDetect) {
            Log("    STRETCH DETECT (stretchDetectUnits=%d, dump cap %d): %.1f draws/frame "
                "scanned, %.2f/frame unreadable (no verdict possible) | %llu flagged total "
                "(%.4f%% of scanned) | %u dumped to the log%s",
                g_settings.stretchDetectUnits, g_settings.stretchDetectDumps,
                g_stretchDetectChecked / f, g_stretchDetectUnreadable / f, g_stretchDetectFlagged,
                g_stretchDetectChecked
                    ? 100.0 * g_stretchDetectFlagged / g_stretchDetectChecked
                    : 0.0,
                g_stretchDetectDumped,
                g_stretchDetectDumped >= static_cast<unsigned>(max(0, g_settings.stretchDetectDumps))
                    ? " (CAP REACHED - flagged draws are still counted above, just not logged)"
                    : "");
            // THE DISTRIBUTION: independent of stretchDetectUnits, so one run answers both
            // "is anything abnormal happening" (the counters above) and "what does normal
            // look like" (this) - the evidence stretchDetectUnits should be set from,
            // instead of guessed.
            Log("    STRETCH DETECT histogram (world-AABB diagonal, every scanned draw with "
                "a computable diagonal): [0-10)=%llu [10-100)=%llu [100-500)=%llu "
                "[500-1000)=%llu [1000-2000)=%llu [2000-5000)=%llu [5000-20000)=%llu "
                "[20000+)=%llu",
                g_stretchHist[0], g_stretchHist[1], g_stretchHist[2], g_stretchHist[3],
                g_stretchHist[4], g_stretchHist[5], g_stretchHist[6], g_stretchHist[7]);
            for (unsigned i = 0; i < 5; ++i) {
                const StretchTopEntry& e = g_stretchTop5[i];
                if (e.diag < 0.0f) break;   // slots fill left-to-right; an empty one ends the list
                Log("    STRETCH DETECT top %u: diag=%.1f ps(albedo sampler)='%s' v=%u p=%u "
                    "path '%s' | world AABB min(%.1f %.1f %.1f) max(%.1f %.1f %.1f)",
                    i + 1, e.diag, e.ps, e.v, e.p, e.path, e.worldMin.x, e.worldMin.y,
                    e.worldMin.z, e.worldMax.x, e.worldMax.y, e.worldMax.z);
            }
        }
        // HIDE-BY-SHADER-NAME (hideShaderContains): the same hunt as STRETCH DETECT above,
        // from the other direction - instead of measuring every converted draw's shape, this
        // makes a NAMED shader family disappear so the tester can watch whether the artifact
        // stops. Printed only when the list is non-empty; an empty list is fully inert and
        // this line does not appear, so a normal run stays silent about it.
        if (g_settings.hideShaderTermCount) {
            char active[300] = "";
            for (int i = 0; i < g_settings.hideShaderTermCount; ++i) {
                if (i) strcat_s(active, ", ");
                strcat_s(active, g_settings.hideShaderTerms[i]);
            }
            Log("    HIDE-BY-SHADER-NAME (hideShaderContains='%s'): %.2f draws/frame hidden, "
                "%llu total",
                active, g_hideShaderContainsHidden / f, g_hideShaderContainsHidden);
            for (int i = 0; i < g_settings.hideShaderTermCount; ++i)
                Log("        '%s': %.2f/frame (%llu total, albedo %llu / first %llu)",
                    g_settings.hideShaderTerms[i], g_hideShaderContainsHitCount[i] / f,
                    g_hideShaderContainsHitCount[i],
                    g_hideShaderContainsByAlbedo[i], g_hideShaderContainsByFirst[i]);
        }
        // The morph is SNOOPED now, not locked: the buffer is DYNAMIC and read-locking it is
        // what the previous attempt got wrong. "not snooped yet" is expected on the first
        // draws of a new character and should stay near zero afterwards; stale vertices mean
        // the game had not written those bytes since its last discard.
        Log("        VB LOCK HOOK: %u invalidations deferred to the render thread, %u "
            "coalesced (the game locks vertex buffers from more than one thread - see the "
            "2026-08-28 crash) | snoop holds %.1f MB of %d MB, %u writes refused on budget, "
            "%u allocations failed, %u copies REFUSED (stale/unmapped source or an exception "
            "during the copy - the atlas snoop already had this guard; this buffer never did "
            "until now)",
            g_deferredInvalidations, g_invalidationsCoalesced, g_snoopBytes / 1048576.0,
            static_cast<int>(kMaxSnoopBytes >> 20), g_snoopBudgetRefusals,
            g_snoopAllocFailures, g_vbCopyRefused);
        Log("        MORPH APPLIED: %.1f draws/frame skinned WITH the delta | %u draws found "
            "no snooped buffer, %u stride refusals, %.2f draws/frame with a non-fresh block",
            g_morphAppliedDraws / f, g_morphNotSnooped, g_morphNoStride,
            g_morphStaleDraws / f);
        if (g_settings.morphProbe) {
            unsigned blockStatic = 0, blockChanged = 0, changesSum = 0;
            for (const auto& kv : g_morphProbeBlocks) {
                if (kv.second.hashChanges == 0) ++blockStatic;
                else { ++blockChanged; changesSum += kv.second.hashChanges; }
            }
            unsigned medianGap = 0, minGap = 0, maxGap = 0;
            if (!g_morphProbeGaps.empty()) {
                std::vector<unsigned> gaps = g_morphProbeGaps;
                std::sort(gaps.begin(), gaps.end());
                medianGap = gaps[gaps.size() / 2];
                minGap = gaps.front();
                maxGap = gaps.back();
            }
            Log("        MORPH PROBE (morphProbe=ON): morph VB writes %.2f/frame (%.2f DISCARD/frame, "
                "%.1f KB/frame) | blocks seen %u: %u STATIC (hash never changed while seen), %u "
                "CHANGED (%.1f changes/block, median gap %u frames, min %u, max %u) | %u changes "
                "landed after a DISCARD (repack), %u in place | draws with a non-fresh block %.2f/frame",
                g_morphVbWrites / f, g_morphVbDiscards / f, (g_morphVbBytes / 1024.0) / f,
                static_cast<unsigned>(g_morphProbeBlocks.size()), blockStatic, blockChanged,
                blockChanged ? static_cast<double>(changesSum) / blockChanged : 0.0,
                medianGap, minGap, maxGap,
                g_morphProbeChangesAfterDiscard, g_morphProbeChangesInPlace,
                g_morphStaleDraws / f);
            unsigned shown = 0;
            for (const auto& kv : g_morphProbeBlocks) {
                if (shown >= 16) break;
                ++shown;
                const MorphProbeBlock& b = kv.second;
                Log("        MORPH BLOCK #%u: morph vb=%p off=%u verts=%u | s0 vb=%p size=%u stride=%u "
                    "first=%u | seen %u frames [%u..%u] | hash changes %u, last at frame %u",
                    shown, static_cast<void*>(b.morphVb), b.morphBase, b.verts,
                    static_cast<void*>(b.s0vb), b.s0size, b.s0stride, b.firstVertex,
                    b.framesSeen, b.firstFrame, b.lastFrame, b.hashChanges, b.lastChangeFrame);
            }
        }
        Log("    constant-colour materials (no diffuse map, albedo from the shader's own "
            "constant) %.0f/frame | HUD quads demoted to UI %.0f/frame | back-buffer clears "
            "%.0f/frame | sky draws %.0f/frame | UI-pass draws converted %.0f/frame",
            g_constantAlbedo / f, g_hudDemoted / f, g_backBufferClears / f, g_skyDraws / f,
            g_uiConverted / f);
        // The draws this shim was blind to until 2026-09-04. Non-zero means they exist and have
        // been reaching Remix unclassified; zero means the HUD is submitted somewhere else again
        // and the search continues rather than concluding.
        Log("    USER-POINTER DRAWS (never hooked before 2026-09-04, so never classified, never "
            "in the frame dump): DrawPrimitiveUP %.1f/frame (%u total), DrawIndexedPrimitiveUP "
            "%.1f/frame (%u total) | HUD draws rebuilt as fixed function and NOT also sent raw: %.1f/frame; "
            "sent raw after a rebuild (uiRawAfterHud): %.1f/frame",
            g_upDrawsTotal / f, g_upDrawsTotal, g_upIndexedTotal / f, g_upIndexedTotal,
            g_hudRebuiltOnly / f, g_hudRawAfterRebuild / f);
        Log("      of those, the HUD (screen space + depth test OFF) demoted to a UI overlay "
            "%.1f/frame (%u total); depth-tested in-world quads left alone %.1f/frame (%u total)"
            " - gate %s",
            g_upHudDemoted / f, g_upHudDemoted, g_upLeftAlone / f, g_upLeftAlone,
            g_settings.uiDemoteUP ? "ON" : "OFF");
        Log("      HUD REBUILT as fixed function (so Remix accepts it with vertex capture OFF) "
            "%.1f/frame (%u total); not the measured 28-byte format, game's own draw passed "
            "through instead %.1f/frame (%u total) - gate %s",
            g_hudConverted / f, g_hudConverted, g_hudConvertRefused / f, g_hudConvertRefused,
            g_settings.uiConvertUP ? "ON" : "OFF");
        Log("    UV DIVIDE read from each vertex shader own def constant instead of the "
            "hardcoded 1/1024: %u draws took a DIFFERENT scale (last %.9f = 1/%.0f), %u draws "
            "had more than one candidate and kept 1/1024 - gate %s",
            g_uvScaleFromShaderUsed, g_uvScaleLast,
            g_uvScaleLast > 0.0f ? 1.0 / g_uvScaleLast : 0.0,
            g_uvScaleAmbiguous, g_settings.uvScaleFromShader ? "ON" : "OFF");
        Log("    ATLAS COMPOSITE re-issued as fixed function with the game's own pixel shader "
            "(the technique that brought the menu video back): %u done, %u failed, last hr "
            "0x%08lX - gate %s",
            g_compositeFfp, g_compositeFfpFailed,
            static_cast<unsigned long>(g_compositeLastFail),
            g_settings.compositeFfp ? "ON" : "OFF");
        if (g_settings.passCensus) {
            Log("    PASSED THROUGH, therefore INVISIBLE with vertex capture off - what we are "
                "losing, by pixel shader and render target (%u distinct):", g_passCensusUsed);
            for (unsigned i = 0; i < g_passCensusUsed; ++i)
                Log("        %-32s -> %ux%u : %u draws",
                    g_passCensus[i].sampler[0] ? g_passCensus[i].sampler : "(no sampler)",
                    g_passCensus[i].w, g_passCensus[i].h, g_passCensus[i].count);
        }
        Log("    CLOTH generator: %u one-colour garments generated, %u one-POINT (a varying "
            "pattern read at a single uv), %u declined as genuinely varying, %u had no usable "
            "diffuse",
            g_clothUniformGen, g_clothOnePointGen, g_clothUniformNotUniform,
            g_clothUniformNoDiffuse);
        Log("      the game's PIXEL SHADER is %s on the rebuilt draw (nulling it is what made the "
            "video greyscale and the text vanish) | D3D9 REFUSED the rebuilt draw %u times, "
            "last hr 0x%08lX",
            g_settings.uiKeepPixelShader ? "KEPT" : "nulled", g_hudDrawFailed,
            static_cast<unsigned long>(g_hudLastFail));
        // If invalidations are non-zero the buffer really is refilled mid-frame, and the
        // snapshot count will exceed one per buffer - which is the correct, non-stale behaviour.
        Log("    instancing: %.0f converted from the instance stream/frame, %.0f refused/frame "
            "(largest single draw %u instances, %.1f buffer writes snooped/frame)",
            g_instanceConverted / f, g_skipInstanced / f, g_maxInstanceCount,
            g_instanceLocks / f);
        // The question this exists to answer: are converted instanced draws reading transform
        // bytes the game actually wrote, or bytes left over from a previous fill of the buffer?
        Log("        instance transforms: %.0f/frame from freshly written bytes, %.0f/frame from "
            "STALE bytes | %u whole-buffer discards seen",
            g_instFreshTransform / f, g_instStaleTransform / f, g_instDiscards);
        // The falsifier for forceOcclusionVisible. If the setting is on and "forced" reads 0,
        // the hook never fired and nothing observed this run can be credited to it.
        Log("        occlusion queries: %.1f read back/frame, %.1f answered VISIBLE/frame "
            "(%u query objects created, setting %s)",
            g_occlusionQueriesSeen / f, g_occlusionForced / f, g_queriesCreated,
            g_settings.forceOcclusionVisible ? "ON" : "off");
        // Which bone palette each skinned draw was posed by. "own" is the only healthy bucket;
        // the other three each place geometry using another object's bones.
        Log("        BONE PALETTE per skinned draw: %.1f own/frame, %.1f BEYOND the upload's "
            "reach/frame, %.1f from an EARLIER FRAME/frame, %.1f written >16 draws ago/frame "
            "(highest bone referenced %u, smallest upload seen %u bones)",
            g_skinPaletteOwn / f, g_skinPaletteBeyondReach / f, g_skinPaletteStaleFrame / f,
            g_skinPaletteFarUpload / f, g_skinMaxBoneSeen,
            g_skinMinReachSeen == 0xFFFFFFFFu ? 0u : g_skinMinReachSeen);
        // The measurement that supersedes the line above. "mixed" is the diagnosis: this mesh's
        // bones were written at draw indices far enough apart to belong to different objects.
        Log("        BONE PROVENANCE (per-bone, supersedes the reach test): %.1f clean/frame, "
            "%.1f MIXED palettes/frame, %.1f reading a bone NOTHING wrote this frame/frame "
            "(worst spread %u draws)",
            g_skinCleanPalette / f, g_skinMixedPalette / f, g_skinBoneNeverWritten / f,
            g_skinWorstSpread);
        Log("        stale-bone rejection %s: %.1f influences dropped/frame, %.1f vertices left "
            "in bind pose/frame",
            g_settings.rejectStaleBones ? "ON" : "off",
            g_staleInfluencesRejected / f, g_skinVertsBindPose / f);
        Log("        bone palette register: %.1f draws/frame at c52, %.1f draws/frame ELSEWHERE "
            "(non-zero means the c52 assumption is wrong for those shaders)",
            g_skinC52BoneReg / f, g_skinForeignBoneReg / f);
        Log("        SKIN DISPLACEMENT: %.1f draws/frame at rest, %.1f draws/frame moved >3 units "
            "from their bind pose (worst %.1f units)",
            g_skinAtRest / f, g_skinDisplaced / f, g_skinWorstDisplace);
        Log("        INDEX WINDOW (skinned draws, each index range scanned once): %.1f draws/frame use indices OUTSIDE "
            "their declared vertex window (worst overshoot %u vertices) - those triangles read another mesh from our "
            "ring copy; %.1f/frame inside | %llu ranges scanned, %llu scan failures, %u cached | declared window avg "
            "%.0f verts -> tight-referenced avg %.0f verts, %.0f verts/frame declared but never indexed (SkinAndBind "
            "still blends/uploads the whole declared window regardless - see TIGHTEN WINDOW below for what this is "
            "worth on rigid draws already)",
            g_idxWinDrawsOut / f, g_idxWinWorstOver, g_idxWinDrawsOk / f, g_idxWinScans, g_idxWinScanFail,
            static_cast<unsigned>(g_idxWin.size()),
            (g_idxWinDrawsOut + g_idxWinDrawsOk) ? static_cast<double>(g_idxWinDeclaredSum) / (g_idxWinDrawsOut + g_idxWinDrawsOk) : 0.0,
            (g_idxWinDrawsOut + g_idxWinDrawsOk) ? static_cast<double>(g_idxWinRefSum) / (g_idxWinDrawsOut + g_idxWinDrawsOk) : 0.0,
            (static_cast<long long>(g_idxWinDeclaredSum) - static_cast<long long>(g_idxWinRefSum)) / f);
        Log("        TIGHTEN WINDOW (rigid converted draws, each index range scanned once): %.1f/frame had a tight "
            "range computed | declared window avg %.0f verts -> referenced range avg %.0f verts, %.0f verts/frame "
            "removed from what the device is told to process | refused: no IB %.1f/frame, unhandled/empty type "
            "%.1f/frame, GetDesc failed %.1f/frame, DYNAMIC index buffer %.1f/frame, index slice past buffer end "
            "%.1f/frame, lock failed %.1f/frame, budget spent %.1f/frame, outside the declared window with "
            "widenUnderDeclaredWindow OFF %.1f/frame, widen would pass the buffer end %.1f/frame, cache full "
            "(hard cap) %.1f/frame | cache %u entries, %llu evictions, evict-by-age %s | gate %s, widen %s",
            g_tightComputed / f,
            g_tightComputed ? static_cast<double>(g_tightDeclaredSum) / g_tightComputed : 0.0,
            g_tightComputed ? static_cast<double>(g_tightRefSum) / g_tightComputed : 0.0,
            g_tightRemoved / f,
            g_tightRefuseNoIB / f, g_tightRefuseType / f, g_tightRefuseDescFail / f, g_tightRefuseDynamic / f,
            g_tightRefuseOOB / f, g_tightRefuseLockFail / f, g_tightRefuseBudget / f, g_tightRefuseNotSubset / f,
            g_tightRefuseWidenBeyondBuffer / f, g_tightRefuseCacheFull / f,
            static_cast<unsigned>(g_tightWin.size()), g_tightWinEvictions,
            g_settings.tightWinEvictByAge ? "ON" : "off",
            g_settings.tightenConvertedWindow ? "ON (draw issued with the tightened window)"
                                              : "off (measured only, draw unchanged)",
            g_settings.widenUnderDeclaredWindow ? "ON" : "off");
        Log("            ...of those, DECAL-NAMED albedo (StrStrIA sampler contains \"Decal\" - the population this "
            "was written for): %.1f/frame had a tight range computed | declared window avg %.0f verts -> referenced "
            "range avg %.0f verts, %.0f verts/frame removed",
            g_tightDecalComputed / f,
            g_tightDecalComputed ? static_cast<double>(g_tightDecalDeclaredSum) / g_tightDecalComputed : 0.0,
            g_tightDecalComputed ? static_cast<double>(g_tightDecalRefSum) / g_tightDecalComputed : 0.0,
            g_tightDecalRemoved / f);
        Log("        TIGHTEN WINDOW, OUTSIDE THE DECLARED WINDOW (the safety net firing, measured regardless of "
            "widenUnderDeclaredWindow): %.2f/frame below it, %.2f/frame above it, %.2f/frame both (below+above-both "
            "is the total) | of those, %.2f/frame WIDENED (avg %.1f verts added to the declared window), %.2f/frame "
            "refused because the widened window would run past the end of the vertex buffer (worse: the game is "
            "referencing vertices that do not exist), %.2f/frame refused because widenUnderDeclaredWindow is off",
            g_tightOutsideBelow / f, g_tightOutsideAbove / f, g_tightOutsideBoth / f,
            g_tightWidened / f,
            g_tightWidened ? static_cast<double>(g_tightWidenedAddedSum) / g_tightWidened : 0.0,
            g_tightRefuseWidenBeyondBuffer / f, g_tightRefuseNotSubset / f);
        Log("        INDEX WINDOW PROBE (indexWindowProbe=%s, every CONVERTED indexed draw - rigid "
            "reused from TIGHTEN WINDOW's own scan above, skinned scanned fresh): %.1f/frame "
            "examined, %.2f/frame had an index OUTSIDE the declared window (worst overshoot %u), "
            "%.2f/frame of those below MinVertexIndex | reused (zero extra Locks) %.1f/frame, fresh "
            "skinned scan %.1f/frame | refused: dynamic IB (skinned only) %.1f/frame, other "
            "%.1f/frame",
            g_settings.indexWindowProbe ? "ON" : "off", g_iwpExamined / f, g_iwpOutside / f,
            g_iwpWorstOvershoot, g_iwpBelow / f, g_iwpRigidReuse / f, g_iwpSkinnedScan / f,
            g_iwpRefuseDynamic / f, g_iwpRefuseOther / f);
        Log("        INDEX WINDOW CLAMP (indexWindowClamp=%s, the fix - collapses an out-of-window "
            "triangle to degenerate instead of narrowing/widening the window Remix never reads): "
            "%llu draws corrected, %llu triangles collapsed | %llu fell back to the original draw on "
            "an alloc/lock failure, %llu flagged draws left unchanged because they were not a "
            "TRIANGLELIST and indexWindowClampStrips=%s (a strip/fan's indices are shared between "
            "adjacent triangles - see IssueIndexWindowClampedStripFanDraw's own comment) | "
            "sentinel-proof position dumps this session: %u/10",
            g_settings.indexWindowClamp ? "ON" : "off", g_iwcDrawsCorrected, g_iwcTrianglesCollapsed,
            g_iwcAllocFail, g_iwcTopologyRefused, g_settings.indexWindowClampStrips ? "ON" : "off",
            g_iwcSentinelProofLogged);
        Log("        FRESH BUFFERS: %.2f converted draws/frame read a static stream-0 range written within %d frame(s); "
            "%.2f/frame skipped for it (deferFreshStaticDraws=%d) | recent-write table %u buffers | rolling record %d frames (F9 writes it)",
            g_freshSeen / f, g_settings.deferFreshFrames, g_deferredFresh / f, g_settings.deferFreshStaticDraws ? 1 : 0,
            static_cast<unsigned>(g_recentWrites.size()), g_settings.ringFrames);
        if (g_settings.lockThreadProbe) LtReport(f);
        if (g_settings.headBodyProbe) BhReport(f);
        Log("        OVERLAY OFFSET: %.1f converted draws/frame pulled toward the camera by %d/1000 of their distance. "
            "By signature: no depth write or ZFUNC=EQUAL %.1f, first sampler is a DECAL map %.1f, truly blended (by the "
            "FACTORS) %.1f, depth bias set %.1f | LAYERS (same triangles, same placement, drawn again) %.1f/frame, "
            "%d/1000 per pass | depth-biased draws seen %.1f/frame | stipple-sampling colour draws (LOD fade) %.1f/frame | "
            "decal map at the LOWEST register %.1f/frame vs merely alphabetically first (the old, wrong test) %.1f/frame%s",
            g_pulledDraws / f, g_settings.decalOffsetPermille, g_decalOffsetDraws / f, g_decalNamedDraws / f,
            g_blendedOffsetDraws / f, g_biasOffsetDraws / f, g_layerDraws / f, g_settings.layerOffsetPermille,
            g_depthBiasDraws / f, g_stippleColourDraws / f, g_decalLowestRegDraws / f, g_decalFirstAlphaDraws / f,
            // decalPrimaryOffset/biasOffset/blendedOffset/decalByLowestSampler only affect anything
            // inside `if (g_settings.decalOffsetPermille > 0)`, so at 0 every one of them is inert -
            // say so plainly rather than let the counters above read as a live test.
            g_settings.decalOffsetPermille > 0 ? "" : " (pull DISABLED: decalOffsetPermille=0)");
        // ProbeDecalDraw's own arming test, reported here so a run that again finds nothing
        // says WHY instead of leaving that to be guessed at. Raw totals, not per-frame - this
        // is a capped, one-shot-style probe (8 reports and it stops), like the shape/HUD probes
        // above, not a steady-state rate.
        Log("        DECAL PROBE ARMING (ProbeDecalDraw, decalProbeStrictTarget=%s): %u "
            "sampler candidates seen, %u armed (of 8) | rejected: %u DYNAMIC stream 0 (particles, "
            "overwhelmingly), %u not indexed, %u no object transform or objTM places it AT the "
            "origin",
            g_settings.decalProbeStrictTarget ? "ON" : "off", g_decalProbeCandidates,
            g_decalProbeReports, g_decalProbeRejectDynamic, g_decalProbeRejectNotIndexed,
            g_decalProbeRejectOrigin);
        // Non-zero means the game DOES refill buffers that bind poses were decoded from, and
        // every one of those was previously served stale geometry - another character's mesh.
        Log("        bind-pose cache invalidated by a game write: %u times (%.2f/frame)",
            g_baseMeshInvalidations, g_baseMeshInvalidations / f);
        Log("        RIGID PARTS (<=2 bones, can only translate): %.1f/frame, of which %.1f/frame "
            "read their bone from an upload far larger than their own skeleton",
            g_skinFewBones / f, g_skinFewBonesForeign / f);
        // The falsifier for the single-bone fix. Vehicles disassemble to one unweighted bone and
        // characters to four weighted ones, so both buckets should be populated. All four-bone
        // means the dcl scan never fired and nothing changed.
        // If wraps are non-zero the ring is being recycled mid-frame while earlier draws may
        // still refer to it. High-water says how big it would have to be for that never to happen.
        Log("        SKIN RING: %u MB, high water %.2f MB, %.2f wraps/frame, %.2f discards/frame",
            g_skinRingBytes / (1024u*1024u), g_skinRingHighWater / 1048576.0,
            g_skinRingWraps / f, g_skinRingDiscards / f);
        // The cost of the ring, separate from the cost of skinning. A DISCARD renames the WHOLE
        // buffer, so its price scales with skinRingMB and not with how much we write; the keep
        // (NOOVERWRITE) column is the same work without the rename and is the control.
        Log("        SKIN RING LOCK: %.2f ms/frame discarding, %.2f ms/frame appending, "
            "worst single lock %.1f ms",
            g_skinLockDiscardMs / f, g_skinLockKeepMs / f, g_skinLockWorstMs);
        Log("        PALETTE SCOPE %s: %.0f vertex influences/frame refused because the palette "
            "belonged to another draw setup",
            g_settings.paletteSetupScope ? "ON" : "off", g_paletteOutOfScope / f);
        Log("        DRIFT (objTM unchanged between frames): %.1f draws/frame STILL, "
            "%.1f draws/frame MOVED (worst %.3f units)",
            g_driftStill / f, g_driftMoved / f, g_driftWorst);
        Log("        SHADER BLEND FORM: %.1f draws/frame four-bone weighted (characters), "
            "%.1f draws/frame SINGLE unweighted bone (vehicles/rigid attachments)",
            g_skinFourBone / f, g_skinSingleBone / f);
        // Separated from the two blend forms on purpose: these draws are not skinned by the
        // game at all. Non-zero here with the drift gone is the whole explanation.
        Log("        NO BONE DECL: %.1f draws/frame whose vertex shader reads no BLENDINDICES "
            "(placed by objTM alone, palette not read) - gate %s",
            g_skinNoBoneDecl / f, g_settings.skinRequireBoneDecl ? "ON" : "OFF");
        Log("        RIGID BONE OWNERSHIP: %.1f/frame use a bone written under their OWN objTM, "
            "%.1f/frame use one written under a DIFFERENT object's",
            g_rigidOwnObject / f, g_rigidForeignObject / f);
        Log("        BONE CLAMP %s: %.0f verts/frame bound to a bone OUTSIDE this object's own "
            "palette upload, of %.0f skinned verts/frame (%.1f%%)",
            g_settings.clampBonesToUpload ? "ON" : "off",
            g_staleBoneVerts / f, g_skinVertsTotal / f,
            g_skinVertsTotal ? (100.0 * g_staleBoneVerts / g_skinVertsTotal) : 0.0);
        Log("        SHADOW vs DEVICE: %u draws checked, %u matched, %u MISMATCHED",
            g_shadowMatch + g_shadowMismatch, g_shadowMatch, g_shadowMismatch);
        Log("        vehicle bones %s: %.0f verts/frame placed by objTM alone",
            g_settings.vehicleBonesOff ? "IGNORED (diagnostic)" : "in use",
            g_vehicleBonesSkipped / f);
        // HIDDEN is the important number now: those draws still execute for the engine but are
        // kept out of the ray-traced scene, which is what stops the prepass copy of the world
        // being drawn over the material pass.
        // Skinning. "refused" is the number that matters: a refused draw falls back to passing
        // through, so it keeps the old frozen-character behaviour rather than rendering a rigid
        // bind pose - a large number here means the conversion is not reaching the population
        // it was written for, and the cache size says whether decoding is thrashing.
        // "repeat" is the same mesh converted more than once in a single frame, and "refused"
        // falls back to pass-through. BOTH produce a second copy of a character, so both are
        // candidates for "drawn twice" - the numbers say which.
        Log("    SKINNING: %.0f skinned/frame, %.0f refused/frame, %.0f exact duplicates "
            "dropped/frame | %u meshes decoded, %u cached, %u cache flushes",
            g_skinnedConverted / f, g_skinnedRefused / f, g_skinRepeatHidden / f,
            g_baseMeshDecodes, static_cast<unsigned>(g_baseMeshes.size()),
            g_baseMeshCacheFlushes);
        // GPU (fixed-function) skinning, gate skinViaFixedFunction - see SkinViaFixedFunction
        // above the shape probe. "reused" is staging avoided because the previous draw already
        // left the right palette in WORLDMATRIX(0..63) - the main cost of this approach is the
        // SetTransform calls, so that number is what paying for it looks like.
        Log("        GPU SKIN (skinViaFixedFunction=%s): %.1f draws/frame hardware-skinned, "
            "%.0f vertices/frame skinned on the GPU instead of the CPU | %.1f SetTransform "
            "(WORLDMATRIX) calls/frame, %.1f draws/frame reused the previous draw's staged "
            "palette instead of restaging, of those %.1f/frame restaged bone 0 alone "
            "(skinFFRestageBone0=%s - WORLDMATRIX(0) IS D3DTS_WORLD, and a reused palette left it "
            "at the WRONG object's placement without this) | stream 0 bound at a non-zero offset "
            "(skinFFHonourStreamOffset=%s): %.1f/frame, %.1f/frame of those with phase!=0 (not "
            "just a whole-vertex shift - the side stream decodes the WRONG vertex without this) | "
            "skinFFFollowShaderInfluences=%s: %.1f/frame single-bone-mode draws (shader reads no "
            "BLENDWEIGHT - staged from the vertex's own idx[0], not mesh->usedBones)",
            g_settings.skinViaFixedFunction ? "ON" : "off", g_skinFFConverted / f,
            g_skinFFVertsConverted / f, g_skinFFTransforms / f, g_skinFFPaletteReused / f,
            g_skinFFBone0Restaged / f, g_settings.skinFFRestageBone0 ? "ON" : "off",
            g_settings.skinFFHonourStreamOffset ? "ON" : "off",
            g_skinFFNonZeroOffset / f, g_skinFFNonZeroOffsetPhase / f,
            g_settings.skinFFFollowShaderInfluences ? "ON" : "off", g_skinFFSingleBoneDraws / f);
        Log("        GPU SKIN refused, falls back to CPU skinning: %.1f/frame no free stream "
            "slot, %.1f/frame cloth remap, %.1f/frame morph delta, %.1f/frame no usable bind "
            "pose, %.1f/frame mesh uses no bones, %.1f/frame a used bone beyond what the "
            "shader's register can stage, %.1f/frame declaration clone failed, %.1f/frame a "
            "side-stream conversion failed, %.1f/frame vertex shader reads no BLENDINDICES, "
            "%.1f/frame single-bone mode: a blend index beyond safe range, %.1f/frame four-bone "
            "mode: a vertex with no usable influence | side-stream buffers built: %u ok, %u "
            "failed (%u bad layout, %u bad desc/dynamic, %u create failed, %u lock failed) | "
            "declarations: %u made, %u failed",
            g_skinFFRefuseNoStreams / f, g_skinFFRefuseCloth / f, g_skinFFRefuseMorph / f,
            g_skinFFRefuseNoMesh / f, g_skinFFRefuseUsedBones / f, g_skinFFRefuseBoneRange / f,
            g_skinFFRefuseDecl / f, g_skinFFRefuseSideBuffer / f,
            g_skinFFRefuseNoBoneDecl / f, g_skinFFRefuseSingleBoneRange / f, g_skinFFRefuseNoInfluence / f,
            g_skinFFBuildOk,
            g_skinFFBuildFail, g_skinFFBuildFailLayout, g_skinFFBuildFailDesc,
            g_skinFFBuildFailCreate, g_skinFFBuildFailLock, g_skinFFDeclsMade,
            g_skinFFDeclFailures);
        {
            double liveMB = 0.0;
            for (const auto& kv : g_morphBakes) liveMB += kv.second.srcSize / (1024.0 * 1024.0);
            Log("        MORPH BAKE (morphBake=%s, cap %d MB, max %d builds/frame): %.1f draws/frame "
                "GPU-skinned from a baked morphed copy | bakes %u built, %u REBUILT (MorphBakeKey "
                "collision - should be ~0 now the key includes the content hash; a moved-but-"
                "unchanged block just hits), %u evicted, %u live holding %.1f MB | %.1f hits/frame, "
                "%.2f/frame hash recomputed, %.2f/frame hash served from the per-frame memo | "
                "refused: %u not fresh, %u budget, %u per-frame limit, %u pooled source, %u lock, "
                "%u create | bake time %.1f ms total, worst %.2f ms",
                g_settings.morphBake ? "ON" : "off", g_settings.morphBakeMaxMB,
                g_settings.morphBakeMaxPerFrame, g_morphBakeDraws / f,
                g_morphBakeBuilt, g_morphBakeRebuilt, g_morphBakeEvicted,
                static_cast<unsigned>(g_morphBakes.size()), liveMB,
                g_morphBakeHits / f, g_morphBakeSeqRehash / f, g_morphBakeHashMemoHits / f,
                g_morphBakeRefuseNotFresh, g_morphBakeRefuseBudget, g_morphBakeRefusePerFrame,
                g_morphBakeRefusePooled, g_morphBakeRefuseLock, g_morphBakeRefuseCreate,
                g_morphBakeMsTotal, g_morphBakeMsWorst);
        }
        // Zero here means the baseVertex fix is inert and the wrong heads have another cause.
        Log("        skinned draws with a non-zero baseVertex: %u (these decoded the WRONG "
            "window of the shared character buffer before 2026-08-20)", g_skinNonZeroBaseVertex);
        // Stage 0 bound with no reflection to justify it - the path that can hand Remix a normal
        // map as base colour.
        // CAMERA ONLY. The falsifier for the architecture, not for a rule: if Remix has a
        // camera and vertex capture has the geometry, the world path-traces without this shim
        // removing or replacing a single draw.
        if (g_settings.cameraOnly) {
            Log("    CAMERA ONLY (no draw skipped, hidden or converted - Remix is given a camera "
                "and nothing else): %u camera changes applied over %u draws, %u refused for "
                "unusable constants",
                g_camOnlySets, g_camOnlyDraws, g_camOnlyRefused);
            if (!g_camOnlySets)
                Log("      NO camera was ever set - c28/c48 never held a usable pair, so Remix "
                    "had no camera and this mode did nothing");
            Log("      MAIN-VIEW GATE %s: draws whose camera was accepted %u | declined - "
                "target not back-buffer sized %u, orthographic %u, mirrored (reflection) %u",
                g_settings.cameraMainViewOnly ? "ON" : "OFF", g_camDrawsMain, g_camDrawsOffSize,
                g_camDrawsOrtho, g_camDrawsMirrored);
            Log("      every distinct camera THIS frame (%u seen, %u did not fit) - the control "
                "for 'is the gate picking the right one?':", g_camCandCount, g_camCandOverflow);
            for (unsigned i = 0; i < g_camCandCount; ++i) {
                const CamCandidate& c = g_camCand[i];
                Log("        cam %2u: rt %4ux%-4u %-11s %-8s %6u draws  -> %s", i, c.rtW, c.rtH,
                    c.perspective ? "perspective" : "ORTHO",
                    c.mirrored ? "MIRRORED" : "upright", c.draws,
                    c.accepted ? "ACCEPTED (given to Remix)" : "declined");
            }
            if (g_settings.cameraMainViewOnly && g_camCandCount && !g_camOnlySets)
                Log("      the gate declined EVERY camera - Remix is running on a stale one. If "
                    "the world is black, set cameraMainViewOnly=0 to restore the old behaviour "
                    "and compare this listing against it");
        }
        // THE REMIX API. Step 1 answers two questions and both are return codes.
        if (g_settings.remixApi) {
            // Three distinct states, and they must not be collapsed. A FAILED init leaves the
            // interface all zeroes, so reporting "the bridge stubs the interface" in that case
            // asserts something we did not observe - it was the first version of this line and it
            // was wrong. Only a SUCCESSFUL init with null pointers is evidence of stubbing.
            Log("    REMIX API: InitializeLibrary %s | usable %s", RemixErrName(g_remixInitRc),
                g_remixDeviceReady ? "YES" : "NO");
            if (g_remixInitRc == 11)
                Log("      NOT_INITIALIZED means the API is GATED, not absent: set "
                    "exposeRemixApi = True in .trex\\bridge.conf (the bridge log says so itself)");
            if (g_remixReady) {
                // Per-function, because this bridge implements part of the interface. The mesh
                // path is what steps 2-4 need; SetupCamera is not, and its absence must not be
                // reported as though it blocked them.
                Log("      mesh path %s | CreateLight %s | SetupCamera %s | explicit device "
                    "registration %s (not required - the API defaults to the game's device)",
                    g_remixMeshReady ? "AVAILABLE" : "MISSING",
                    g_remix.CreateLight ? "AVAILABLE" : "MISSING",
                    g_remixCameraAvail ? "AVAILABLE" : "NOT IMPLEMENTED",
                    RemixErrName(g_remixRegisterRc));
                if (g_settings.remixApiCamera && !g_remixCameraAvail)
                    Log("      remixApiCamera=1 but SetupCamera is not implemented by this "
                        "bridge - %u ticks declined, the SetTransform camera is still in use",
                        g_remixCamUnavailable);
                else if (g_remixCameraAvail)
                    Log("      SetupCamera: %u calls, %u failed, last rc %s", g_remixCamCalls,
                        g_remixCamFails, RemixErrName(g_remixCamLastRc));
                // STEP 2. A green emissive cube 3 units in front of the camera, submitted with
                // no D3D9 draw behind it. Seeing it proves API geometry reaches the path tracer.
                if (g_settings.remixApiTestCube) {
                    if (g_remixCubeFailed)
                        Log("      STEP 2 CUBE: FAILED to build - CreateMaterial %s, CreateMesh "
                            "%s", RemixErrName(g_remixCubeCreateRc),
                            RemixErrName(g_remixCubeMeshRc));
                    else if (!g_remixCubeBuilt)
                        Log("      STEP 2 CUBE: not built yet");
                    else
                        Log("      STEP 2 CUBE: built (%s) | DrawInstance %u calls, %u failed, "
                            "last rc %s%s",
                            g_remixCubeTextured ? "wearing the CHARACTER ATLAS from disk"
                                                : "green constant - no atlas was available",
                            g_remixDrawCalls, g_remixDrawFails,
                            RemixErrName(g_remixDrawLastRc),
                            g_remixLastViewValid ? "" : "  <- no main-scene view recorded, so it "
                                                        "has nowhere to be placed");
                    Log("      atlas DDS: %s%s", g_atlasDdsReady ? g_atlasDdsPath : "(not written)",
                        g_atlasDdsFails ? "  <- a write FAILED" : "");
                }
                if (g_settings.remixApiCharacter) {
                    if (g_apiDoneCount)
                        Log("      STEP 3e CHARACTER: %u meshes, %u triangles total | latest:",
                            g_apiDoneCount, g_apiDoneTris);
                    if (g_apiDoneCount) {
                        Log("      STEP 3b CHARACTER: %u surfaces (%u slots seen), %u verts, "
                            "%u triangles at (%.1f %.1f %.1f) | DrawInstance %u calls, %u failed, "
                            "last rc %s | %u out-of-range, %u duplicate slots dropped",
                            g_apiSurfaces, g_apiSlotsSeen, g_apiCharVertexCount,
                            g_apiCharTriangles, g_apiCharAt[0], g_apiCharAt[1], g_apiCharAt[2],
                            g_apiCharDrawCalls, g_apiCharDrawFails, RemixErrName(g_apiCharLastRc),
                            g_apiOutOfRange, g_apiSlotsDuplicate);
                        Log("        offset %+.1f on X so it cannot z-fight the game's own copy "
                            "of the same character - set remixApiCharacterOffset=0 to overlay "
                            "them again", g_settings.remixApiCharacterOffset);
                    } else
                        Log("      STEP 3a CHARACTER: not built - %s (vertices captured: %s)",
                            g_apiCharWhyNot, g_apiCapHaveVerts ? "yes" : "no");
                }
            }
            if (g_remixDeviceReady)
                Log("      attached to the game's device. SetupCamera: %u calls, %u failed, "
                    "last rc %s%s", g_remixCamCalls, g_remixCamFails,
                    RemixErrName(g_remixCamLastRc),
                    g_settings.remixApiCamera ? "" : "  (remixApiCamera=0, so none expected)");
        }
        // SR3'S OWN COMMAND BLOCKS - read out of the engine, not inferred.
        //
        // The dispatcher at 0x0049DE20 takes a block off a ring and executes its opcodes; every
        // draw the shim sees runs inside one. The block a draw belongs to IS its pass identity,
        // stated by the engine at a fixed address rather than reconstructed from what the draw
        // happens to sample. This listing is one frame, not a running total.
        if (CommandBlocksAvailable()) {
            Log("    COMMAND BLOCKS (SR3 dispatches draws from a command ring on its own render "
                "thread - this is the engine's OWN pass structure, read at 0x02E5D648): "
                "%.1f blocks/frame over %u frames, %u draws attributed%s",
                g_cbFramesSeen ? (double)g_cbBlocksAccum / g_cbFramesSeen : 0.0,
                g_cbFramesSeen, g_cbDrawsAttributed,
                g_cmdBlockOverflow ? " (TABLE FULL - more blocks exist)" : "");
            Log("      this frame: %u blocks", g_cmdBlockCount);
            for (unsigned i = 0; i < g_cmdBlockCount && i < 24; ++i)
                Log("        block 0x%08X : %5u draws  (opcodes %u..%u)",
                    g_cmdBlocks[i].start, g_cmdBlocks[i].draws,
                    g_cmdBlocks[i].firstOp, g_cmdBlocks[i].lastOp);
            // The engine's own kill-switch. Non-zero means SR3 decided the object was not
            // visible and every draw handler would have been a no-op - which is what
            // forceOcclusionVisible exists to prevent.
            Log("      engine draw kill-switch (0x03395EA4) was SET on %u draws", g_cbKillSwitchSeen);
            // WHAT THE ENGINE IS ABOUT TO DO. The block is already built when the render thread
            // starts it, so the whole command list is readable ahead of execution. This is the
            // rule-1 evidence: `GetRenderTargetData` (ops 55/56/72) in a block means the engine
            // READS BACK what that block drew, and nothing in it may be skipped.
            if (g_settings.scanCommandBlocks) {
                Log("    BLOCK SCAN (the command list read AHEAD of execution): %u blocks walked, "
                    "%u commands | %u draws, %u SetRenderTarget, %u GetRenderTargetData",
                    g_scanBlocks, g_scanCmds, g_scanDraws, g_scanTargets, g_scanReadbacks);
                Log("      %u blocks contain a READBACK - those may never be skipped (rule 1, "
                    "stated by the engine rather than inferred) | %u walks stopped early on an "
                    "opcode whose size is not known",
                    g_blocksWithReadback, g_scanStopped);
                if (g_scanBlocks && g_scanStopped * 2 > g_scanBlocks)
                    Log("      MORE THAN HALF the walks stopped early - the size table is "
                        "incomplete and these counts are lower bounds, not totals");
            }
        }
        Log("    ALBEDO stage 0 taken raw (no shader reflection) %.0f/frame",
            g_albedoStage0Raw / f);
        // Materials whose albedo is not simply a diffuse map, split by how it was recovered.
        // "tinted fallback" is the clothing-customisation population: a pattern or blend map
        // multiplied by the shader's own tint constant.
        // Every converted draw's albedo, by how it was obtained. These four are disjoint and
        // "blank" is the only one that renders white - unlike g_skipNoAlbedo below, which counts
        // rank-0 draws whether or not the constant rule rescued them.
        Log("    ALBEDO: constant-only %.0f/frame, tinted fallback %.0f/frame, genuinely blank "
            "%.0f/frame (of %.0f rank-0 draws/frame)",
            g_constantAlbedo / f, g_tintedAlbedo / f, g_blankAlbedo / f, g_skipNoAlbedo / f);
        // albedoRejectColourlessTexture: a further cross-cut of the same population above (a
        // real texture was bound, no constant, no tint fallback) - how many of THOSE draws had
        // an alpha-only or luminance-only chosen albedo (fewer than 3 colour channels) and so
        // took TFACTOR instead of the texture for colour. A large number here means this is a
        // sizeable population and resolving the real per-family constant (Decal_Map_Color and
        // its siblings) is worth doing next; a small one means it is a curiosity.
        Log("    ALBEDO: colourless-texture rejected %.0f/frame (%u distinct alpha-only/"
            "luminance texture(s) seen)",
            g_albedoColourlessRejected / f,
            static_cast<unsigned>(g_albedoColourlessTextures.size()));
        // TEXTURE FILL PROBE. The A/B on 2026-08-29 settled that SR3's writes to its character
        // texture land on native D3D9 and do NOT land under the Remix stack - with d3d9.dll
        // renamed away the character is textured correctly. Configuration cannot reach it, so
        // the shim has to snoop those writes at the CPU boundary and re-upload them. This names
        // WHICH call to snoop, which is the one thing that has to be known before writing it.
        //
        // Whichever line is non-zero is the entry point. All zero means the game fills the
        // texture by a route none of these cover - which is equally worth knowing, and cheaper
        // to learn here than after building a subsystem aimed at the wrong call.
        if (g_settings.rtAlbedoCopy) {
            Log("    TEXTURE FILL PROBE (how does the game write the character texture?): "
                "%u atlas texture(s) tracked | texture LockRect %u, SURFACE LockRect %u, "
                "UpdateTexture %u, UpdateSurface %u, StretchRect %u, ColorFill %u",
                g_atlasPtrCount, g_fillLockRect, g_fillSurfLock, g_fillUpdateTexture,
                g_fillUpdateSurface, g_fillStretchRect, g_fillColorFill);
            Log("    ATLAS SNOOP (SR3 writes its character texture by locking each mip SURFACE; "
                "those writes reach native D3D9 but not the image Remix samples, so they are "
                "taken here and re-uploaded through UpdateTexture): %u captured, %u copies "
                "uploaded, %u capture failures, %u PENDING-ENTRY ORPHANS (a second lock on the "
                "same surface before the first was ever unlocked - THE SEPTEMBER 14 CRASH CAME "
                "FROM HERE; non-zero means it is still happening, just no longer fatal) | %.1f "
                "draws/frame bound a snooped copy - gate %s",
                g_atlasCaptured, g_atlasCopyBuilt, g_atlasCaptureFailed, g_atlasPendingOrphans,
                g_atlasBound / f, g_settings.atlasSnoop ? "ON" : "OFF");
            Log("      snoopValidateSource %s: %u source range(s) rejected before copying "
                "(stale/unmapped pointer, copy skipped), %u exception(s) caught during the "
                "copy itself (validated but unmapped a moment later)",
                g_settings.snoopValidateSource ? "ON" : "OFF", g_atlasSrcUnmapped,
                g_atlasSrcException);
            for (unsigned i = 0; i < g_atlasSnoopCount; ++i)
                Log("      atlas %u: %ux%u  mean %.1f of 255  <- what the GAME wrote; non-zero "
                    "here proves the pixels exist at this boundary", i + 1,
                    g_atlasSnoop[i].w, g_atlasSnoop[i].h, g_atlasSnoop[i].mean);
            Log("      render targets: %u candidates, %u reads of a %u budget (one every %u "
                "frames, MAX over time - a one-shot read lands during loading and says nothing, "
                "and an unbounded one froze the game)",
                g_rtProbeCount, g_rtProbeRead, 60u, 120u);
            Log("        %u reads FAILED - if this is non-zero the read tool cannot see "
                "game-written surfaces and every zero measured through it must be discarded",
                g_rtProbeFailed);
            for (unsigned i = 0; i < g_rtProbeCount; ++i)
                if (g_rtProbeMean[i] >= 0.0)
                    Log("        RT %u: %ux%u  max mean %.1f", i + 1, g_rtProbeW[i],
                        g_rtProbeH[i], g_rtProbeMean[i]);
            if (!g_atlasCaptured)
                Log("      NOTHING captured - no top-level surface lock on a tracked atlas was "
                    "seen, so the snoop did nothing at all");
            if (g_fillStretchRect)
                Log("      of those StretchRects, %u FAILED - if the game's own composite is "
                    "being refused by DXVK, that is the bug itself", g_fillStretchFailed);
            if (g_fillLockRect)
                Log("      LockRect levels seen = 0x%08X, flags OR = 0x%08lX  (0x2000 DISCARD, "
                    "0x10 READONLY, 0x1000 NOOVERWRITE)", g_fillLockLevels, g_fillLockFlags);
            if (g_atlasPtrOverflow)
                Log("      %u candidates did not fit the %u-entry array - the shape test is "
                    "matching more than the character atlas", g_atlasPtrOverflow, kMaxAtlasPtrs);
            if (!g_atlasPtrCount)
                Log("      NO atlas texture tracked - the classifier never matched one, so every "
                    "count above is meaningless");
        }
        // COMPOSITED ALBEDO. SR3 bakes each character - skin, tattoos, clothing - into one
        // 2048x1024 UNCOMPRESSED DYNAMIC texture and binds it as their diffuse map; Remix
        // hashes what the game UPLOADS, and built that material from an all-zero image -
        // measured in the user's own capture, mat_E881A25E37E37B19, mean 0.0, 100% near-black.
        // Re-uploaded here through UpdateTexture, which is the path Remix does hash.
        //
        // "textures matched" is the population, and it is the number the FIRST version of this
        // had no counter for: it tested D3DUSAGE_RENDERTARGET (0x1) when the atlas carries
        // D3DUSAGE_DYNAMIC (0x200), matched nothing, and did nothing.
        //
        // FALSIFIER, and it separates the two causes this bug has had:
        //   "last readback mean" > 0  -> the pixels WERE on the GPU and only Remix lacked them;
        //                                this fix is the right one and characters should be
        //                                textured.
        //   "last readback mean" = 0  -> the game genuinely never wrote the atlas, so the
        //                                composite draws really are being lost and
        //                                compositeToTexturePass was aimed at the right idea
        //                                but the wrong frames.
        //   "copies made" 0 with "blank" 0 and "bound" 0 -> no render-target albedo was ever
        //                                seen and this whole path did nothing.
        if (g_settings.rtAlbedoCopy) {
            Log("    COMPOSITED ALBEDO (SR3 bakes each character into one uncompressed dynamic "
                "texture; Remix hashes uploads, and saw all zeroes): %u textures matched, "
                "%u copies made, %u blank readbacks, %u StretchRect refusals, %u failed, "
                "%u refused on the %d MB ceiling | %.1f draws/frame bound a copy | holding "
                "%.1f MB",
                g_rtCopyMatched, g_rtCopyMade, g_rtCopyBlank, g_rtCopyStretchFailed,
                g_rtCopyFailed, g_rtCopyBudget,
                static_cast<int>(kMaxRtCopyBytes >> 20), g_rtCopyBound / f,
                g_rtCopyBytes / 1048576.0);
            if (g_rtCopyLastMean >= 0.0)
                Log("      last readback mean = %.1f of 255  <- the read is now a GPU copy "
                    "(StretchRect into our own render target, then GetRenderTargetData), so "
                    "unlike a READONLY lock of a DYNAMIC surface it has DEFINED contents. "
                    "NON-ZERO means the pixels exist and only Remix lacked them; ZERO now really "
                    "does mean the atlas is empty", g_rtCopyLastMean);
            else
                Log("      no readback has run - 'textures matched' says whether the rule ever "
                    "recognised the atlas at all (it must be at least 1)");
        }
        // Diffuse_Color - the albedo multiply this shim drops. See ProbeDiffuseColor.
        //
        // FALSIFIER: "NON-WHITE 0.0/frame" means the constant is white on every draw that
        // carries it, applying it would change no pixel, and the character-tint question is
        // CLOSED with no code change. A non-zero skinned figure is the missing per-object tint
        // and names how many draws a fix would affect - which is the number this project keeps
        // shipping rules without.
        if (g_settings.diffuseColorProbe) {
            Log("    DIFFUSE_COLOR (the game multiplies its diffuse map by this; we bind the map "
                "raw, so it is DROPPED): %.1f draws/frame carry it (%.1f skinned)",
                g_diffColDraws / f, g_diffColSkinned / f);
            Log("      NON-WHITE %.2f draws/frame (%.2f skinned) | %u distinct value%s seen%s",
                g_diffColNonWhite / f, g_diffColNonWhiteSkinned / f, g_diffColUsed,
                g_diffColUsed == 1 ? "" : "s",
                g_diffColOverflow ? " (TABLE FULL, more exist)" : "");
            // The distribution itself, most common first. A single white bucket closes the
            // question; several buckets with skinned draws in them is a per-character tint.
            //
            // Sorted through a COPY. This report runs every 600 frames, so popping the live
            // buckets would empty the table after the first one and every later report would
            // print a confident near-empty distribution - the "counter that does not measure
            // what its name says" failure, twice recorded in YOUR-INSTRUCTIONS.md.
            unsigned order[64];
            for (unsigned i = 0; i < g_diffColUsed; ++i) order[i] = i;
            for (unsigned i = 1; i < g_diffColUsed; ++i) {
                const unsigned v = order[i];
                unsigned j = i;
                while (j && g_diffCol[order[j - 1]].all < g_diffCol[v].all) {
                    order[j] = order[j - 1];
                    --j;
                }
                order[j] = v;
            }
            for (unsigned n = 0; n < g_diffColUsed && n < 8; ++n) {
                const DiffColBucket& b = g_diffCol[order[n]];
                Log("        (%.3f %.3f %.3f) x %u draws (%u skinned)%s",
                    ((b.key >> 16) & 0xFF) / 64.0f, ((b.key >> 8) & 0xFF) / 64.0f,
                    (b.key & 0xFF) / 64.0f, b.all, b.skinned,
                    b.key == ((64u << 16) | (64u << 8) | 64u) ? "   <- white, a no-op" : "");
            }
        }
        // The two ways a draw renders WHITE without any counter having said so. Every number
        // above concerns materials with no colour map; these are materials that HAVE one and
        // still ended up with nothing on stage 0.
        Log("    ALBEDO WHITE (previously uncounted): named-but-not-bound %.0f/frame, "
            "render-target exclusion left nothing %.0f/frame | colourless passes now hidden "
            "%.0f/frame | screen-space passes hidden %.0f/frame | particle billboards hidden "
            "%.0f/frame",
            g_albedoNullRanked / f, g_albedoNullAfterRT / f, g_skipNoColourPass / f,
            g_skipScreenSpacePass / f, g_skipParticlePass / f);
        // CHANGE 1, 2026-09-18: the VFX mask rule in Classify, just before its final return.
        // Distinct shaders is the over-match canary: shader_constants.csv names only four
        // files in the ir_vfxmask/ir_vfxmasktod family, so a number well past that here means
        // this test is catching something it should not and needs to be narrowed again.
        Log("    VFX MASK slabs hidden (opacity lives in a sampler fixed function cannot bind) "
            "%.0f/frame (%u distinct shaders, skipMaskOnlyVfx=%s)",
            g_vfxMaskHidden / f, static_cast<unsigned>(g_vfxMaskShaders.size()),
            g_settings.skipMaskOnlyVfx ? "ON" : "off");
        Log("    SKIPPED entirely %.0f/frame (mode %d) | demoted to UI overlay %.0f/frame | "
            "no-albedo materials left to Remix %.0f/frame",
            g_skippedDraws / f, g_settings.hiddenPassMode, g_demotedToUI / f, g_skipNoAlbedo / f);
        // Marked draws are the ones Remix can be told to drop by a single hash. `refused` counts
        // hidden draws whose shader does sample, where rebinding stage 0 would have been visible
        // in the game's own output - those still pass through, so a large number here means the
        // marker is not covering the population it was aimed at.
        Log("    MARKED with the marker texture %.0f/frame (refused as unsafe %.0f/frame, "
            "%.0f SetTexture calls/frame, marker %s)",
            g_markedDraws / f, g_markRefused / f, g_markSetTextures / f,
            g_marker ? "created" : "NOT CREATED");
        Log("    albedo: moved off stage 0 %.0f/frame, blanked %.0f/frame, "
            "restored from mesh cache %.0f/frame (%u meshes cached, %u flushes) | "
            "RT textures known %u | lights %.1f/frame (%u shaders) | "
            "churned to a different texture than last frame %.0f/frame (%u identities tracked) | "
            "chosen albedo sampler read texcoord set 0 %.0f/frame, a NON-ZERO set %.0f/frame, "
            "an unknown/computed set %.0f/frame | FIX B decal alpha from texture %.0f/frame",
            g_albedoMoved / f, g_albedoBlanked / f, g_albedoRestored / f,
            static_cast<unsigned>(g_meshAlbedo.size()), g_meshAlbedoFlushes,
            static_cast<unsigned>(g_rtTextures.size()), g_lightsEmitted / f, g_lightShadersSeen,
            g_albedoChurnDraws / f, static_cast<unsigned>(g_albedoIdentityLastFrame.size()),
            g_albedoUvSet0Draws / f, g_albedoUvNonZeroDraws / f, g_albedoUvUnknownDraws / f,
            g_decalAlphaFromTexture / f);
        Log("    camera %.1f %.1f %.1f | distinct cameras this frame %u (max %u) | "
            "transform writes %.0f/frame | redundant state calls dropped %.0f/frame",
            g_camX, g_camY, g_camZ, g_frameCameraCount, g_maxFrameCameras,
            g_transformWrites / f, g_shadowSkipped / f);
        // World changes should track the number of distinct OBJECTS drawn. If this sits far
        // below the converted-draw count, objects are sharing a matrix and piling up.
        Log("    world matrix changes %.0f/frame | objTM used without a fresh upload %.0f/frame "
            "| uv matrix writes %.0f/frame (last scale %.5f, %.5f)",
            g_worldMatrixChanges / f, g_objTMWithoutFreshUpload / f, g_uvMatrixWrites / f,
            g_appliedU, g_appliedV);
        // If this is near the converted-draw count, the world's coordinates are now in a format
        // Remix reads. If it is zero while the world is still flat, the swap never fired.
        Log("    SHORT2 texcoords converted to a float2 stream: %.0f draws/frame | "
            "%u buffers converted (%.1f MB held, %u flushes, %u invalidated by the game) | "
            "%u declarations cloned | failures: %u convert, %u declaration | "
            "FIX A decal texcoord1 set: %.1f/frame served, %.1f/frame wanted but unavailable, "
            "%.1f/frame refused for a non-zero stream or unresolved offset, "
            "%u declarations displaced a colliding texcoord0 element | decalUvScaleFromSet "
            "corrected the SHORT2 verdict on %.1f/frame draws",
            g_uvStreamDraws / f, g_uvBuffersMade,
            static_cast<double>(g_uvBytesHeld) / (1024.0 * 1024.0), g_uvBufferFlushes,
            g_uvInvalidations, g_uvDeclsMade, g_uvConvertFailures, g_uvDeclFailures,
            g_uvSet1Draws / f, g_uvSet1Unavailable / f, g_uvSet1WrongStream / f,
            g_uvDeclTexcoord0Displaced, g_decalUvScaleFixChanged / f);
        // ORPHAN SWEEP: how many of the caches above are holding a game-owned resource that
        // NOTHING else references any more (see the orphanSweep setting's comment for why this
        // exists). LIVE sizes let a leak be told apart from a healthy cache that is just large.
        Log("    ORPHAN SWEEP (on=%d every %d frames, report-only=%d): %u sweeps, released "
            "%llu objects / %.1f MB total, last sweep %u / %.1f MB, bookkeeping %s",
            g_settings.orphanSweep ? 1 : 0, max(1, g_settings.orphanSweepFrames),
            g_settings.orphanSweepReportOnly ? 1 : 0, g_orphanSweeps, g_orphanReleasedObjects,
            g_orphanReleasedMB, g_orphanLastReleased, g_orphanLastReleasedMB,
            g_orphanBookkeepingBroken ? "BROKEN - releasing stopped" : "ok");
        Log("        LIVE: uv buffers %u | skinFF streams %u | base meshes %u | instance "
            "streams %u | tight IB pins %u | shaders %u | declarations %u",
            static_cast<unsigned>(g_uvBuffers.size()), static_cast<unsigned>(g_skinFFBuffers.size()),
            static_cast<unsigned>(g_baseMeshes.size()), static_cast<unsigned>(g_instCache.size()),
            static_cast<unsigned>(g_tightIbShapes.size()), static_cast<unsigned>(g_shaders.size()),
            static_cast<unsigned>(g_layouts.size()));
        // FIX 3: stream 0 may be bound at a non-zero byte offset, the same precondition the
        // hardware-skin side stream fixes above - and it hits this whole-buffer UV decode too.
        // Measured whether or not the switch is on.
        Log("        STREAM-0 OFFSET (uvHonourStreamOffset=%s): %.1f/frame UV conversions "
            "requested with stream 0 bound at a non-zero offset - %.1f/frame static (whole-buffer "
            "decode, wrong vertex without this), %.1f/frame dynamic (snooped slice read from the "
            "wrong byte without this), %.1f/frame of those with phase!=0 (not just a whole-vertex "
            "shift), %.1f/frame on DECAL-NAMED albedo (StrStrIA sampler contains \"Decal\") | "
            "%.1f/frame of ALL converted draws (any reason) had a non-zero stream-0 offset",
            g_settings.uvHonourStreamOffset ? "ON" : "off",
            (g_uvNonZeroOffsetStatic + g_uvNonZeroOffsetDynamic) / f,
            g_uvNonZeroOffsetStatic / f, g_uvNonZeroOffsetDynamic / f, g_uvNonZeroOffsetPhase / f,
            g_uvNonZeroOffsetDecalAlbedo / f, g_convertedNonZeroStreamOffset / f);
        Log("    converted draws de-instanced: %.0f/frame", g_deinstancedDraws / f);
        // Which draws keep their unreadable SHORT2 coordinates, and why. Remix names the format
        // it refused in remix-dxvk.log; this names the reason we could not convert it.
        if (g_uvConvertFailures)
            Log("        conversion failures by reason: %.1f/frame layout, %.1f/frame DYNAMIC "
                "source, %.1f/frame desc, %u create, %u lock",
                g_uvFailLayout / f, g_uvFailDynamic / f, g_uvFailDesc / f,
                g_uvFailCreate, g_uvFailLock);
        // DYNAMIC sources - character parts the engine rewrites every frame. Every UV conversion
        // failure in the 2026-08-31 run was this one branch refusing outright.
        Log("        DYNAMIC sources (%s): %u converted from the snooped copy, %u waiting for "
            "the game's next write%s",
            g_settings.convertDynamicUV ? "ON" : "OFF", g_uvDynConverted, g_uvDynWaiting,
            (g_settings.convertDynamicUV && !g_uvDynConverted && g_uvDynWaiting)
                ? "  <- registered but NEVER filled: the snoop is not seeing these locks" : "");
        // Tiling is only applied when a pair NAMED AFTER the albedo map exists. Unmatched means
        // the shader tiles some other map and the albedo is correctly left alone.
        Log("    uv tiling matched to the albedo map %.1f/frame, no pair for it %.1f/frame",
            g_tilingMatched / f, g_tilingUnmatched / f);
        if (g_clothDraws)
            Log("    CLOTH: %.1f draws/frame, %u distinct colour combinations seen%s | "
                "pattern read-lock: %u ok, %u failed",
                g_clothDraws / f, g_clothComboCount,
                g_clothComboCount >= 64 ? " (at the probe's cap - the real number is higher)" : "",
                g_clothLockOk, g_clothLockFail);
        if (g_clothGenerated || g_clothGenFailed)
            Log("        generated %u outfit textures (%u cached, %u flushes), bound %.1f/frame, "
                "%u could not be generated",
                g_clothGenerated, static_cast<unsigned>(g_clothCache.size()),
                g_clothCacheFlushes, g_clothBound / f, g_clothGenFailed);
        Log("    HAIR (hairHsvAlbedo=%s): hair-shader draws %.1f/frame seen, %.1f/frame converted, "
            "%.1f/frame reached the generator | %u textures built | not built: %s %u, %s %u, %s %u, "
            "%s %u, %s %u",
            g_settings.hairHsvAlbedo ? "ON" : "off", g_hairShaderSeen / f, g_hairShaderConverted / f,
            g_hairDraws / f, g_hairHsvGen, kHairNull[0], g_hairNull[0], kHairNull[1], g_hairNull[1],
            kHairNull[2], g_hairNull[2], kHairNull[3], g_hairNull[3], kHairNull[4], g_hairNull[4]);
        Log("    SKIN (skinMatcapAlbedo=%s): skin-shader draws %.1f/frame seen, %.1f/frame converted, "
            "%.1f/frame reached the generator | %u textures built, %u builds deferred by the "
            "%d/frame cap | not built: %s %u, %s %u, %s %u, %s %u, %s %u",
            g_settings.skinMatcapAlbedo ? "ON" : "off", g_skinShaderSeen / f,
            g_skinShaderConverted / f, g_skinDraws / f, g_skinGen, g_skinDeferred,
            g_settings.skinGenPerFrame, kSkinNull[0], g_skinNull[0], kSkinNull[1], g_skinNull[1],
            kSkinNull[2], g_skinNull[2], kSkinNull[3], g_skinNull[3], kSkinNull[4], g_skinNull[4]);
        Log("        of which the one-Sphere_Map SHADERBALL form %.1f/frame (skinSingleMatcap=%s)",
            g_skinSingleDraws / f, g_settings.skinSingleMatcap ? "ON" : "off");
        Log("    SKINNED LAYOUTS (converted skinned draws by the shader's sampler names, cumulative):");
        for (const auto& kv : g_skinnedLayouts)
            Log("        %8.1f/frame  %s", kv.second / f, kv.first.c_str());

        // The measurement that says whether the stall is ours. "shim" is time inside our hooks
        // only - the draw call itself is excluded, so this does not charge us for bridge cost.
        if (g_timedFrames) {
            const double tf = static_cast<double>(g_timedFrames);
            Log("    TIMING: frame %.1f ms avg (%.0f fps), worst %.0f ms | "
                "shim %.2f ms avg (%.1f%% of frame), worst %.1f ms",
                g_frameMsAccum / tf, 1000.0 / (g_frameMsAccum / tf), g_frameMsWorst,
                g_shimMsAccum / tf,
                g_frameMsAccum > 0.0 ? 100.0 * g_shimMsAccum / g_frameMsAccum : 0.0,
                g_shimMsWorst);
            // The average frame is fine; it is the tail the player feels. Counting how MANY
            // frames run long says whether a hitch is a rare event or a steady drumbeat, which
            // one worst-case number can never distinguish.
            Log("    TIMING tail: %u frames >=33 ms, %u >=100 ms in this window of %u | "
                "inside real Present %.2f ms avg, worst %.1f ms | our other hooks %.2f ms avg",
                g_hitches33, g_hitches100, g_frames < 600 ? g_frames : 600u,
                g_presentMsAccum / tf, g_presentMsWorst, g_otherMsAccum / tf);
            g_frameMsWorst = g_shimMsWorst = g_presentMsWorst = 0.0;   // per-interval, not all-time
            g_hitches33 = g_hitches100 = 0;
        }
        // Converted draws per render target. If two targets both receive large numbers, the same
        // geometry is being converted into two passes and Remix gets it twice - which is what
        // z-fighting on static surfaces looks like.
        for (int i = 0; i < g_convTargetCount; ++i) {
            if (!g_convTargets[i].converted) continue;
            Log("    converted -> target %p %ux%u fmt=%d : %.0f draws/frame",
                static_cast<void*>(g_convTargets[i].surface), g_convTargets[i].w,
                g_convTargets[i].h, g_convTargets[i].fmt, g_convTargets[i].converted / f);
        }
    }

    // Dump the call-order trace once it has filled. This is the measurement that settles how
    // the engine sequences objTM uploads against shader binds and draws.
    if (!g_traceDumped && g_traceLen >= kTraceMax) {
        g_traceDumped = true;
        g_trace[g_traceLen] = '\0';
        Log("call-order trace (V/v=SetVertexShader with/without objTM, P=projTM c28, "
            "O=objTM c32, W=view c48, D=draw converted, x=draw skipped):");
        for (unsigned i = 0; i < g_traceLen; i += 100) {
            char chunk[101];
            const unsigned n = min(100u, g_traceLen - i);
            memcpy(chunk, g_trace + i, n);
            chunk[n] = '\0';
            Log("  %s", chunk);
        }
    }

    // A new frame re-picks the camera, and state blocks may have applied device state behind
    // our back, so neither the shadow nor the applied-transform cache can be trusted across it.
    g_cameraCaptured = false;
    g_haveMainCamera = false;
    FinishFrameDump();

    // One whole frame of sky probing, then stop.
    if (!g_skyProbeFrame && g_haveCamera && g_drawIndexThisFrame > 0) {
        g_skyProbeFrame = g_frames;
        Log("sky probe: %u depth-disabled draws in a frame of %u", g_skyReports,
            g_drawIndexThisFrame);
    }
    // injectProbe/injectControl PART 1/2: settled and reset HERE, before g_drawIndexThisFrame
    // itself resets below - this IS "the end of the frame" everywhere else in this file.
    TallyAndResetInjectFrame();
    g_clearedThisFrame = false;
    g_lastFrameDraws = g_drawIndexThisFrame;
    g_drawIndexThisFrame = 0;
    if (g_frameCameraCount > g_maxFrameCameras) g_maxFrameCameras = g_frameCameraCount;
    g_frameCameraCount = 0;
    if (g_frames % 600 == 0) InstallCrashFilter();
    ShadowInvalidate();
    g_haveAppliedWorld = g_haveAppliedView = g_haveAppliedProj = false;
    g_appliedU = g_appliedV = 0.0f;   // 2026-09-18 audit fix: see Hook_SetTransform's D3DTS_TEXTURE0 case
    g_lightingSetUp = false;
    DisableUnusedLightSlots(dev);

    // Split our own end-of-frame block from Remix's. Both land in the NEXT interval, which is
    // why they are carried in "Last" variables rather than accumulated here.
    g_ourPresentMsLast = MsSince(tPresentEntry);
    const LONGLONG tRealPresent = Now();
    const HRESULT hr = g_origPresent(dev, src, dst, window, dirty);
    g_presentMsLast = MsSince(tRealPresent);
    FrameStepHold();   // frozen: waits here with the frame on screen
    return hr;
}


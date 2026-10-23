// parts/14_classify.cpp - render-command blocks and Classify (the per-draw disposition).
// NOT a standalone translation unit: #included, in order, by ../sr3rtx.cpp inside its anonymous
// namespace. Originally sr3rtx.cpp lines 9245-10102 (old file:line citations map here).
// ---------------------------------------------------------------------------------------------
// Walking SR3's command block - the one thing the engine offers that D3D9 does not
//
// The block bounded by [0x02E5D648, 0x02E5D64C) is ALREADY BUILT when the render thread starts
// executing it. So at any draw the rest of the block can be read: what passes remain, and - the
// part that matters - whether the engine is going to READ BACK what it is drawing into.
//
// That is rule 1 stated by the engine instead of guessed at:
//
//   "A draw whose RESULT the engine reads can never be SKIPPED, only hidden or passed through."
//
// Three separate rules have blacked out the world by getting that wrong from D3D9-level
// inference. `GetRenderTargetData` is opcodes 55/56/72; if a block contains one, everything drawn
// in that block feeds a readback and must not be removed.
//
// Sizes were extracted statically from each handler's own advance of the read pointer
// (docs/engine-map.md). Most are constant; the payload-carrying commands compute theirs from a
// count inside the command, and those formulas are transcribed here verbatim:
//
//   op 24/27  SetVertex/PixelShaderConstantF   0x0C + count*16,  count at +8
//   op 26/29  SetVertex/PixelShaderConstantB   0x0C + count*4,   count at +8
//   op 20     0x18 + [+0x14]      op 41  0x14 + [+0x10]
//   op 61     0x08 + [+4]         op 67  0x18 + [+0x10]
//
// An opcode whose size is not known stops the walk. That is deliberate: guessing a size would
// desynchronise the walk and every command after it would be garbage, which is a far worse
// failure than a short answer.
unsigned CommandSize(unsigned op, const unsigned char* cmd) {
    auto u32 = [cmd](unsigned off) {
        return *reinterpret_cast<const volatile unsigned*>(cmd + off);
    };
    switch (op) {
        case 0: case 1: case 2: case 5: case 6:            return 4;
        case 3: case 4: case 8:                            return 8;
        case 9:                                            return 0x0C;  // SetRenderTarget
        case 10:                                           return 8;
        case 11:                                           return 0x14;
        case 12:                                           return 0x20;
        case 14: case 17:                                  return 0x14;
        case 15:                                           return 8;
        case 16: case 18:                                  return 0x0C;
        case 19:                                           return 0x10;
        case 21: case 31: case 32: case 33: case 34:       return 8;
        case 22: case 23:                                  return 0x0C;
        case 25: case 28:                                  return 0x10;
        case 35:                                           return 0x18;  // SetStreamSource
        case 36:                                           return 0x10;  // SetTexture
        case 37:                                           return 0x0C;  // DrawPrimitive
        case 38:                                           return 4;
        case 40:                                           return 0x10;
        case 42:                                           return 0x1C;  // DrawIndexedPrimitive
        case 50:                                           return 0x20;
        case 51:                                           return 0x34;
        case 52: case 53:                                  return 8;
        case 54: case 57: case 66: case 73:                return 0x0C;
        case 55:                                           return 0x10;  // GetRenderTargetData
        case 58:                                           return 0x604;
        case 62:                                           return 4;
        case 68:                                           return 0x1C;
        // payload-carrying: size computed from the command itself
        case 20:                                           return 0x18 + u32(0x14);
        case 24: case 27:                                  return 0x0C + u32(8) * 16;
        case 26: case 29:                                  return 0x0C + u32(8) * 4;
        case 41:                                           return 0x14 + u32(0x10);
        case 61:                                           return 8 + u32(4);
        case 67:                                           return 0x18 + u32(0x10);
        default:                                           return 0;     // unknown - stop
    }
}

unsigned g_scanBlocks = 0, g_scanCmds = 0, g_scanStopped = 0;
unsigned g_scanDraws = 0, g_scanTargets = 0, g_scanReadbacks = 0;
unsigned g_blocksWithReadback = 0;
unsigned g_scanLastBlock = 0;

// Walk one command block, once, the first time a draw inside it is seen.
void ScanCommandBlock() {
    if (!CommandBlocksAvailable() || !g_settings.scanCommandBlocks) return;
    const unsigned blockStart = *reinterpret_cast<const volatile unsigned*>(kAddrBlockStart);
    if (!blockStart || blockStart == g_scanLastBlock) return;
    g_scanLastBlock = blockStart;
    const unsigned blockEnd = *reinterpret_cast<const volatile unsigned*>(kAddrBlockEnd);
    if (blockEnd <= blockStart || blockEnd - blockStart > (1u << 20)) return;

    ++g_scanBlocks;
    unsigned draws = 0, targets = 0, readbacks = 0, cmds = 0;
    unsigned p = blockStart;
    // Hard bound as well as the end pointer: a size table that is wrong anywhere must not be able
    // to spin. 4096 commands is far more than a 16 KB block can hold.
    for (unsigned guard = 0; guard < 4096 && p + 4 <= blockEnd; ++guard) {
        const unsigned op = *reinterpret_cast<const volatile unsigned*>(p);
        if (op >= 74) { ++g_scanStopped; break; }
        const unsigned sz = CommandSize(op, reinterpret_cast<const unsigned char*>(p));
        if (!sz || p + sz > blockEnd) { ++g_scanStopped; break; }
        ++cmds;
        if (op == 37 || op == 40 || op == 41 || op == 42 || op == 43) ++draws;
        else if (op == 9) ++targets;
        else if (op == 55 || op == 56 || op == 72) ++readbacks;
        p += sz;
    }
    g_scanCmds += cmds;
    g_scanDraws += draws;
    g_scanTargets += targets;
    g_scanReadbacks += readbacks;
    if (readbacks) ++g_blocksWithReadback;
}

// Cleared on the first draw of a frame, NOT in Present.
//
// The first version called ResetCommandBlocks() near the top of Hook_Present - and the frame
// report runs LATER in that same function, so the listing was wiped before it printed. Every
// report read "this frame: 0 blocks" while the game was submitting 2,320 draws a frame. Same
// class as the destructive bucket-pop caught in the DIFFUSE_COLOR report before shipping; this
// one shipped.
unsigned g_cbLastFrameSeen = 0xFFFFFFFF;

void NoteCommandBlock() {
    if (!CommandBlocksAvailable()) return;
    if (g_cbLastFrameSeen != g_frames) {
        g_cbLastFrameSeen = g_frames;
        if (g_cmdBlockCount) { g_cbBlocksAccum += g_cmdBlockCount; ++g_cbFramesSeen; }
        g_cmdBlockCount = 0;
    }
    g_curBlockStart = *reinterpret_cast<const volatile unsigned*>(kAddrBlockStart);
    g_curOpcode = *reinterpret_cast<const volatile unsigned*>(kAddrLastOpcode);
    if (*reinterpret_cast<const volatile unsigned char*>(kAddrDrawDisabled)) ++g_cbKillSwitchSeen;
    ++g_cbDrawsAttributed;
    ScanCommandBlock();
    for (unsigned i = 0; i < g_cmdBlockCount; ++i) {
        if (g_cmdBlocks[i].start == g_curBlockStart) {
            ++g_cmdBlocks[i].draws;
            if (g_curOpcode < g_cmdBlocks[i].firstOp) g_cmdBlocks[i].firstOp = g_curOpcode;
            if (g_curOpcode > g_cmdBlocks[i].lastOp) g_cmdBlocks[i].lastOp = g_curOpcode;
            return;
        }
    }
    if (g_cmdBlockCount >= kMaxCmdBlocks) { ++g_cmdBlockOverflow; return; }
    CmdBlock& b = g_cmdBlocks[g_cmdBlockCount++];
    b.start = g_curBlockStart;
    b.draws = 1;
    b.firstOp = b.lastOp = g_curOpcode;
}

// Per FRAME, so the listing describes one frame rather than a running total.
void ResetCommandBlocks() {
    if (g_cmdBlockCount) { g_cbBlocksAccum += g_cmdBlockCount; ++g_cbFramesSeen; }
    g_cmdBlockCount = 0;
}

// HIDE-BY-SHADER-NAME counters (hideShaderContains) - see that setting's own comment in
// struct Settings, and the check inside Classify, for what this exists to answer. Never
// touched while the list is empty, so a run that is not using this diagnostic reports 0
// here and prints no report line for it (see the periodic report in Hook_Present).
unsigned long long g_hideShaderContainsHidden = 0;                                    // total, all terms combined
unsigned long long g_hideShaderContainsHitCount[Settings::kMaxHideShaderTerms] = {};   // per matched substring
// Which FIELD matched, per term - so a bisection result says whether a family was caught by
// its albedo sampler, its first sampler, or both. See the comment on the check in Classify().
unsigned long long g_hideShaderContainsByAlbedo[Settings::kMaxHideShaderTerms] = {};
unsigned long long g_hideShaderContainsByFirst[Settings::kMaxHideShaderTerms] = {};

Disp Classify(IDirect3DDevice9* dev) {
    g_dispReason = "converted";
    g_markClearAllStages = false;
    g_isHudDraw = false;
    if (!g_settings.ffp) return Because("ffp=0, shim inert", Disp::PassThrough);
    if (!g_viewProjValid) { ++g_skipNoVP; return Because("no view/proj yet", Disp::PassThrough); }

    // Step 1b. Ahead of everything, and a no-op unless remixApiCamera is on AND the device
    // registered - so it cannot affect a run that is only testing initialization.
    RemixApiCameraTick();

    // CAMERA ONLY. Ahead of every rule that can skip, hide or convert - so none of them run.
    if (g_settings.cameraOnly) {
        ApplyCameraOnly(dev);
        return Because("cameraOnly: camera handed to Remix, draw left untouched",
                       Disp::PassThrough);
    }

    // HIDE-BY-SHADER-NAME (hideShaderContains): bisection-by-elimination for the transient
    // large flat polygon this project is hunting (see the setting's own comment in struct
    // Settings). Placed here, immediately after cameraOnly - early enough that a hidden draw
    // does none of the classification work below (sky, screen-space, skinning, MRT, ...), but
    // only AFTER cameraOnly's own return, because cameraOnly's whole contract is "nothing
    // skipped, nothing hidden, nothing replaced" (see its comment above BeginFFP) and this
    // diagnostic must not be the thing that breaks that guarantee. g_curPS.albedoSampler is
    // already fully resolved by this point: it is set from the shader's CTAB when
    // Hook_SetPixelShader binds the shader, not per draw, so it is exactly as valid here at
    // Classify's entry as it is anywhere else in this function.
    //
    // CASE-SENSITIVE ON PURPOSE - strstr, not StrStrIA. Diffuse_mapSampler and
    // Diffuse_MapSampler differ by exactly one letter's case and are DIFFERENT shader
    // families (ring-record analysis narrowed the artifact to exactly this trio); the whole
    // point of this tool is to tell them apart, so a case-insensitive match would defeat it
    // outright. Plain substring matching is safe with this list: Diffuse_MapSampler is not a
    // substring of anything else this project has named, and Diffuse_Map_1Sampler does NOT
    // contain Diffuse_MapSampler - the "_1" sits between "Map" and "Sampler" so the
    // characters differ - so the short name can never silently swallow the long one. Do not
    // loosen this to a case-insensitive or fuzzier match without re-checking that property.
    //
    // MATCH BOTH firstSampler AND albedoSampler. Measured 2026-09-25, the hard way: the rolling
    // draw record's ps= field is g_curPS.firstSampler (see RingRecordDraw's own strncpy_s), NOT
    // albedoSampler, so every candidate name read off a ring record is a FIRST-sampler name. For
    // most families the two coincide and matching either works, but they genuinely differ for the
    // deferred G-buffer family: a term of "IR_GBuffer_DSF_Data" taken from a ring record matched
    // 0.00 draws/frame against albedoSampler alone, silently producing an empty test that looked
    // like a clean exclusion. Testing both fields is what makes a name copied out of a ring record
    // mean what the reader thinks it means. Also note the ring truncates ps= to 19 characters
    // (char ps[20]), so a name read from there may be a prefix - substring matching handles that,
    // which is another reason not to tighten this into an exact compare.
    if (g_settings.hideShaderTermCount && g_curPS.isPixelShader) {
        for (int i = 0; i < g_settings.hideShaderTermCount; ++i) {
            const char* term = g_settings.hideShaderTerms[i];
            const bool byAlbedo = g_curPS.albedoSampler[0] && strstr(g_curPS.albedoSampler, term);
            const bool byFirst  = g_curPS.firstSampler[0] && strstr(g_curPS.firstSampler, term);
            if (byAlbedo || byFirst) {
                ++g_hideShaderContainsHidden;
                ++g_hideShaderContainsHitCount[i];
                if (byAlbedo) ++g_hideShaderContainsByAlbedo[i];
                if (byFirst)  ++g_hideShaderContainsByFirst[i];
                return Because(byAlbedo
                                   ? "hideShaderContains: albedo sampler matched a bisection term"
                                   : "hideShaderContains: first sampler matched a bisection term",
                               Disp::Hide);
            }
        }
    }

    // ---------------------------------------------------------------- the rasterized-frame invariant
    //
    // Ahead of every rule below that can Convert, Hide or Mark a draw, so nothing downstream can
    // hand Remix a draw this test would have refused - see CompositesEngineOutputToScreen, above,
    // for the invariant itself and why it exists. What happens when it fires is the trap this
    // project has hit before: a draw whose GetVertexShader is NULL is already fixed function and
    // Remix accepts it whatever rtx.useVertexCapture says, so passing it through untouched would
    // still paint it - it must not reach the device at all. A draw that still carries a vertex
    // shader is safe to pass through exactly as it is: Remix declines a shader draw with vertex
    // capture off, which is the whole reason g_lastVS-bound draws are safe to forward everywhere
    // else in this file. g_lastVS is read rather than a fresh GetVertexShader() bridge call
    // because Hook_SetVertexShader keeps it in sync with the device's real binding for exactly
    // this reason - SyncBridgeVS trusts the same shadow for every PassThrough draw already.
    if (g_settings.blockEngineOutputToScreen) {
        int rtStage = -1;
        UINT rtW = 0, rtH = 0;
        if (CompositesEngineOutputToScreen(dev, &rtStage, &rtW, &rtH)) {
            const bool hasVS = g_lastVS != nullptr;
            LogRtInvariantBlock(g_curDrawIndexed ? "Classify(indexed)" : "Classify(non-indexed)",
                               rtStage, rtW, rtH, g_curDrawPrimCount, hasVS);
            if (hasVS) {
                if (g_curDrawIndexed) ++g_rtInvariantIndexedPass; else ++g_rtInvariantDrawPass;
                return Because("BLOCKED: samples a render target and writes the screen - vertex "
                               "shader is bound, passed through untouched so Remix (capture off) "
                               "declines it", Disp::PassThrough);
            }
            if (g_curDrawIndexed) ++g_rtInvariantIndexedSkip; else ++g_rtInvariantDrawSkip;
            return Because("BLOCKED: samples a render target and writes the screen - already "
                           "fixed function, skipped so it never reaches the device", Disp::Skip);
        }
    }

    // THE SKY IS NOT SCREEN SPACE, even though it looks like it from here.
    //
    // Measured 2026-08-18 with ProbeSkyDraw, which is the only reason this was found: SR3's
    // skybox is a 343-vertex dome, 36x9x36 units, sitting ONE UNIT from the camera, and its
    // vertex shader never references projTM. So the screen-space test claimed it, the HUD rule
    // below demoted it to UI, and the sky vanished from the path tracer while the game's own
    // raster still drew it. That is the black sky, and it was my own rule that caused it.
    //
    // A skybox is world content, not an overlay. It passes through so Remix's vertex capture
    // reconstructs it with its real Diffuse_Map bound, which is what lets the sky be TAGGED:
    // rtx.skyBoxTextures is the mechanism Remix provides, and it needs to see the game's own
    // texture on the draw. Marking it would hand Remix our marker instead and tagging could
    // never work.
    //
    // Converting is not an option here: with no projTM there is no projection to rebuild, and
    // fixed function would need one. Pass-through is also how the sky behaved before the HUD
    // rule existed, so this restores a known-good state rather than inventing a new one.
    if (g_curPS.skyShader) {
        ++g_skyDraws;
        return Because("sky (rfg-skybox family) - left for Remix to capture and tag",
                       Disp::PassThrough);
    }

    // SCREEN SPACE IS TESTED FIRST, and the order is the point.
    //
    // Measured 2026-08-18: 21 fullscreen quads a frame sampling IR_GBuffer_Depth and
    // IR_GBuffer_Lighting were still being passed through and rasterised over the camera. They
    // never reached the screen-space test at all - they carry IR_Light_* constants, and the
    // light-volume test used to run first, claim them, and refuse to mark them.
    //
    // A fullscreen quad that RESOLVES lighting is not a light volume. The volume test is about a
    // 3D hull whose shader carries a light's parameters; a screen-space draw is part of the
    // composite chain whatever its constants say. Testing shape before contents fixes it.
    //
    // Light injection is unaffected: EmitLight runs in the draw hook before Classify is called,
    // so it has already harvested these constants either way.
    // A shader that never references projTM emits clip-space positions itself: a fullscreen or
    // HUD quad. There is no world transform to build for it, and it is not scene geometry.
    if (!g_curVS.usesProjTM) {
        ++g_skipScreenSpace;
        // Not all screen-space quads are equal, and conflating them is what put a rasterised
        // image back on the camera.
        //
        // The composite pass draws the engine's FINISHED RASTERISED FRAME as a fullscreen quad
        // sampling the HDR scene render target. Demoting that to UI does not hide it - Remix
        // rasterises UI as a 2D overlay, so the game's own rendered image gets painted over the
        // path-traced world and follows the camera even in free cam. (Recorded in the worklog at
        // session 8 and repeated here; it has to be dropped, not demoted.)
        //
        // The HUD, by contrast, samples authored textures and must still be demoted rather than
        // dropped - dropping every screen-space draw froze the image in an earlier session.
        //
        // The separator is what the quad SAMPLES: a render-target texture means it is
        // compositing the engine's own output, which is exactly what we are replacing.
        // Measured 2026-08-18 from a complete frame dump: 49 screen-space draws, EVERY ONE of
        // them passed through, and they are the whole deferred-resolve and post chain -
        // IR_GBuffer_Lighting and IR_GBuffer_Depth resolves, the 512x512 blur, the auto-exposure
        // reduction (320x180 -> 64x64 -> 16x16 -> 4x4 -> 1x1), the colour LUT, and the two draws
        // that write the back buffer. Remix reconstructs all of them and rasterises them over the
        // path-traced world. THAT is "a partial rasterised image overlays the path-traced world".
        //
        // The separator is what the quad SAMPLES, which is the test this comment has described
        // since the fork without the code ever implementing it: a render-target texture means the
        // quad is compositing the engine's own output, which is exactly what we are replacing, so
        // marking it is safe by design. An authored texture means the HUD, a light cookie or the
        // LUT - real 2D content that must still be drawn, and which Remix's own uiTextures list
        // is the right mechanism for.
        //
        // A null stage 0 is grouped with the render targets: the IR_GBuffer_Depth resolves bind
        // nothing at all, so there is no game texture to preserve and nothing for Remix to hash
        // except what we give it.
        switch (g_settings.screenSpaceMode) {
            case 0: return Because("screen-space", Disp::PassThrough);
            case 1: return Because("screen-space, ortho demote", Disp::Hide);
            default:
                // Three ways to recognise "this quad is compositing the engine's own output":
                //
                //   * stage 0 is a render target we watched the game create;
                //   * stage 0 is null - the IR_GBuffer_Depth resolves bind nothing at all, so
                //     there is no game texture to preserve;
                //   * the shader names NO colour sampler. This catches the depth resolve, whose
                //     source is made with CreateDepthStencilSurface and so never passes through
                //     our CreateTexture hook - g_rtTextures cannot know about it.
                //
                // A quad that DOES name a colour sampler is left alone: that is the HUD, a light
                // cookie (Diffuse_Map_1 into the 400x288 light buffers) or something else with
                // real authored 2D content, and rtx.uiTextures is the mechanism for those.
                if (!g_curTexture[0] || g_rtTextures.count(g_curTexture[0]) ||
                    (g_curPS.isPixelShader && g_curPS.albedoRank == 0)) {
                    ++g_screenSpaceMarked;
                    // ---------------------------------------------------------------------
                    // A composite into an OFF-SCREEN TEXTURE is an engine INPUT, not output.
                    //
                    // SR3 bakes each character - skin, tattoos, clothing - into one 2048x1024
                    // render target and then samples it as that character's albedo. The frame
                    // dump shows the player's 36 body/head draws all bound to one such atlas.
                    // Those composites are screen-space quads with no projTM, and their source
                    // is often rank-0 (Normal_Map_A and friends), so they land in this branch
                    // and `hiddenPassMode=2` turns them into SKIP. Skipped, the atlas is never
                    // written and the character is drawn with a BLACK texture.
                    //
                    // Evidence, from Remix's own captures (external, not inference):
                    //   captures 08-13..08-18  atlas 8E4C8047F62D947A  mean 182.7 - a real face
                    //   capture  08-23         atlas E881A25E37E37B19  mean   0.0 - pure black
                    // and in the 08-23 capture 72 character sub-meshes are bound to a material
                    // whose diffuse_texture IS that black file. The split lands exactly on the
                    // change to hiddenPassMode=2.
                    //
                    // This is rule 1 of this project, for the fourth time: A DRAW WHOSE RESULT
                    // THE ENGINE READS CAN NEVER BE SKIPPED. The engine reads this one as a
                    // texture.
                    //
                    // The test is the TARGET, not the source: a quad drawing into a surface
                    // that is not the size of the screen is not compositing the screen. That
                    // keeps the back buffer and the 2560x1440 deferred-resolve chain on exactly
                    // the path they are on today - "skipping the composite quad freezes the
                    // image" is a recorded dead end and this must not reopen it.
                    //
                    // Measured on the 2026-08-28 frame dump BEFORE shipping, which is the step
                    // this project keeps missing: of 398 composite quads in that frame, 357
                    // target 2560x1440 surfaces and are untouched here; 41 target smaller
                    // surfaces and are what this rule changes, from SKIP to PASS. Pass-through
                    // submits the draw exactly as the game wrote it, and with
                    // `rtx.useVertexCapture = False` Remix refuses shader-driven draws outright,
                    // so nothing new reaches the path tracer - the same reasoning
                    // skipDeferredGBuffer already rests on.
                    if (g_settings.compositeToTexturePass && g_backBufferW && g_rt0Width &&
                        (g_rt0Width != g_backBufferW || g_rt0Height != g_backBufferH)) {
                        ++g_compositeToTexture;
                        return Because("composite into an off-screen texture - the engine reads "
                                       "this back as a texture", Disp::PassThrough);
                    }
                    g_dispReason = "post/composite quad";
                    // Leftovers on stages 1-7 include UI-tagged textures, and one of those makes
                    // Remix treat the whole draw as UI - which it rasterises as an overlay.
                    g_markClearAllStages = true;
                    return HiddenDisp(true);
                }
                // An authored-texture screen-space quad is the HUD. It must still be DRAWN, and
                // drawn flat on the screen rather than rebuilt as a plane floating in the world,
                // which is what Remix's vertex capture does with it when nothing tells Remix it
                // is UI.
                //
                // Ortho demote is the right tool, and it is the same mechanism session 14
                // recorded as a FAILURE - "Remix classifies it UI, and UI is rasterised as a 2D
                // overlay, so the shape stays visible". That was a failure when the goal was to
                // HIDE world geometry. For the HUD a 2D overlay is precisely the goal, so the
                // same measured behaviour is now the feature.
                //
                // It also needs no texture hashes, which is the point. The user reports that
                // tagging anything into rtx.uiTextures turns the screen magenta: a UI-tagged
                // draw is rasterised from the game's own output, and we have deliberately filled
                // that output with the marker. A mechanism that depends on no hash list cannot
                // be caught by that interaction.
                //
                // Requires rtx.orthographicIsUI = True.
                ++g_hudDemoted;
                g_isHudDraw = true;
                return Because("screen-space HUD, demoted to UI", Disp::Hide);
        }
    }

    // Light volumes: EmitLight has already turned this one into a real Remix light, so the
    // volume mesh is redundant - hide it rather than let it be captured as a shell.
    if (g_curPS.light.kind != LightShader::NotLight) {
        ++g_skipNotEligible;
        // Routed through the same policy as the other hidden passes, NOT Disp::Hide. An
        // orthographic demote makes Remix rasterise the volume as a 2D overlay, which for a 3D
        // hull means it stays visible - the trap that produced "the shapes changed transform".
        //
        // Skipping is safe for OUR lighting: EmitLight runs before this in the draw hook and
        // reads the light's parameters out of the shadowed pixel-shader constants, so the light
        // is already injected whether or not the volume mesh is drawn. Only the engine's own
        // light buffer loses it, which matters solely to pass-through draws that sample it.
        // markSafe = false: a light volume's pixel shader samples the G-buffer at stage 0, so
        // rebinding it would change the engine's own light buffer. Revisit once the composite
        // chain is hidden and the game's raster no longer reaches the screen.
        g_dispReason = "light volume (its light is already injected separately)";
        return g_settings.hideLightVolumes ? HiddenDisp(false) : Disp::PassThrough;
    }

    // Skinned meshes transform through the c52 bone palette, so objTM means nothing for them.
    // These are REAL scene geometry we cannot convert yet (CPU skinning is the next phase), so
    // they pass through and Remix's vertex capture places them as best it can. Hiding them
    // would delete every character from the scene.
    if (g_curVS.skinned && !g_settings.convertSkinned) {
        ++g_skipSkinned;
        return Because("skinned (c52 bone palette), awaiting the skinning port", Disp::PassThrough);
    }

    // Instanced draws: fixed function has no per-instance transform, so every copy would land
    // on one matrix. Measured 2026-08-16 at ~1,000 draws/frame - the bulk of the city - so
    // converting these properly (one draw per instance, transform read from the instance
    // stream) is the next substantial piece of work, not a corner case.
    // Instanced draws are convertible when they carry exactly one instance and declare an
    // instance transform - which measured as universally true in SR3. More than one instance
    // genuinely cannot be represented: fixed function has a single world matrix, so every copy
    // would land on top of the first.
    if (g_instancedDraw) {
        if (g_instanceCount > g_maxInstanceCount) g_maxInstanceCount = g_instanceCount;
        g_instanceTotal += g_instanceCount;
        if (g_instanceCount > 1 || !g_curLayout.hasInstanceTransform) {
            ++g_skipInstanced;
            DumpInstancedDeclaration();
            // real geometry we cannot place; let capture try
            return Because("multi-instance draw, one world matrix cannot place it",
                           Disp::PassThrough);
        }
    }

    // Fixed function transforms the POSITION element itself, so that element has to be
    // something it can read: an uncompressed float3/float4 in stream 0. SR3 compresses most
    // vertex fields, and a compressed or non-stream-0 position handed to FFP is read as raw
    // integers - the mesh lands at coordinates in the tens of thousands and stretches across
    // the map. Like the skinned exclusion this is a representability precondition, not a
    // heuristic: there is no correct way to convert such a draw without expanding its vertices.
    if (!g_curLayout.parsed || g_curLayout.posStream != 0 ||
        (g_curLayout.posType != D3DDECLTYPE_FLOAT3 &&
         g_curLayout.posType != D3DDECLTYPE_FLOAT4)) {
        ++g_skipVertexFormat;
        // real geometry, just not in a format FFP can read
        return Because("position not float3/float4 in stream 0", Disp::PassThrough);
    }

    // A shader that names no colour texture at all is not the pass that defines surface colour.
    // Measured 2026-08-17: the untextured materials' samplers are Normal_Map, Detail_Normal_Map,
    // Damage_Normal_Map, IR_Stipple_Pattern_2D, IR_GBuffer_DSF_Data and Dual_Paraboloid_Map -
    // every one a normal, dither or deferred-data pass. SR3 draws the world through several
    // passes; converting the normal pass and painting it WHITE puts an untextured surface
    // coincident with the real textured one, which is why the world reads as untextured even
    // though 646 draws a frame do carry a proper albedo.
    //
    // HIDE, not merely skip. Measured 2026-08-17 with the state probe: the identical mesh is
    // submitted to render target 14EDE580 with a pixel shader that samples NOTHING, and again to
    // 14EE44D8 with Diffuse_MapSampler. The first is the DSF/geometry prepass. Skipping only its
    // conversion left Remix free to capture it from vertex-shader output and draw it untextured
    // over the real material pass - the shells around the player.
    // The prepass signature is a pixel shader that samples NOTHING AT ALL - firstSampler empty.
    // That is what the state probe measured: the same mesh submitted once with ps='' and once
    // with ps='Diffuse_MapSampler'.
    //
    // The previous version keyed on albedoRank == 0 instead, which is a much weaker condition:
    // it means "no COLOUR sampler", and a material whose only texture is Normal_MapSampler
    // satisfies it while being a perfectly real surface. That skipped ~2,400 draws a frame of
    // genuine world geometry - 17,601-vertex terrain and building meshes among them - which is
    // why the 3D world became hard to see. rank == 0 is not evidence of a prepass.
    // Counted separately: index 0 cannot hold colour but another BOUND slot can. That is the
    // MRT G-buffer pass, and it is exactly the population the first version of this rule
    // destroyed. If this number is large the multi-target reading is right.
    if (g_rtChannels[0] < 3 && AnyColourTarget()) ++g_mrtColourElsewhere;

    // A target with fewer than three colour channels cannot hold an albedo, so this draw is not
    // the pass that defines surface colour - whatever its pixel shader samples.
    //
    // This is the rule the sampler-based prepass tests could not express. Measured 2026-08-27:
    // of 467 draws into SR3's 2560x1440 G16R16 target, 463 were already being skipped for
    // sampling nothing, and the 4 that survived were whole characters carrying
    // Diffuse_MapSampler at rank 100 - the same vertex buffers, at the same objTM, as draws in
    // the A16B16G16R16F material pass later in the frame. Remix path-traced both copies.
    //
    // Safe to SKIP rather than hide, on this game's own terms: 463 of its siblings already are,
    // and the depth readback the engine does care about is answered by the occlusion-query
    // hook, not by this surface.
    if (g_settings.skipNonColourTargets && !AnyColourTarget()) {
        ++g_skipNonColourTarget;
        if (g_curLayout.skinned) g_markClearAllStages = true;
        g_dispReason = "render target cannot hold colour (fewer than 3 channels)";
        return HiddenDisp(true);
    }

    if (g_settings.skipUntextured && g_curPS.isPixelShader && !g_curPS.firstSampler[0]) {
        ++g_skipUntextured;
        // The wide clear belongs HERE, not only on the stipple rule below.
        //
        // Run 58 put it on that rule after the magenta came back, and the frame dump then showed
        // the patch had missed almost everything it was aimed at: of 86 marked skinned prepass
        // draws, 85 arrive through THIS test - their pixel shader declares no sampler at all -
        // and exactly one through the stipple one. The character prepass is the sampler-less
        // prepass.
        //
        // Why the clear matters: the marker's hash IS in rtx.ignoreTextures and Remix logs it as
        // loaded, so stage 0 is ignored - but Remix categorises a draw from ANY stage carrying a
        // texture, and a prepass arrives with whatever the previous material draw left on stages
        // 1-7. Clearing only stage 0 leaves those behind.
        //
        // Skinned only, so the cost lands where the evidence is: this rule fires on ~1,400 draws
        // a frame across the whole world, and at two bridge calls per stage the wide clear on all
        // of them is the expense that had it removed on 2026-08-18. The skinned share is ~85.
        if (g_curLayout.skinned) g_markClearAllStages = true;
        g_dispReason = "prepass: shader samples nothing";
        return HiddenDisp(true);
    }

    // THE SECOND PREPASS SIGNATURE, found 2026-08-18 by disassembly rather than by observation.
    //
    // "Samples nothing" is not the only shape SR3's prepass takes. ir_bb_tod_window_bs.fxo_pc
    // shader [6] samples Normal_Map at s0 and IR_Stipple_Pattern_2D at s11, decodes a tangent
    // space normal (*2-1, normalise) and writes specular power - no colour anywhere. Shader [8]
    // of the SAME FILE is the material pass, carrying Diffuse_Map, Decal_Map, Specular_Map,
    // IR_LBuffer and Tint_color. 415 pixel shaders match [6]: 236 with the stipple alone, 179
    // with stipple + Normal_Map. The stipple is inferred lighting's dithered-rejection pattern,
    // and a shader that samples it while naming no colour map at all is writing the G-buffer.
    //
    // These were being CONVERTED with stage 0 unbound, which is where "most surfaces are white"
    // came from: a white copy of the world, coincident with the correctly textured material pass.
    // Every rank-0 converted draw in the frame dump was one of these - 24 of 94.
    //
    // markSafe = true, and specifically because of the register assignment: stage 0 holds the
    // NORMAL MAP, while the stipple that drives the dithered discard sits at s11 and is left
    // alone. So the depth this pass writes - and therefore the engine's occlusion culling, which
    // reads it back - is unaffected. Only the G-buffer normal changes, and that feeds the light
    // buffer and the composite, both of which are hidden.
    // Widened 2026-08-18 after the first run: the stipple was only 19 draws a frame, while 83
    // more were still converting white with Normal_Map as their only sampler and no stipple at
    // all. Both are the same thing - a pass that writes G-buffer normals - so the test is now
    // "every sampler this shader declares is a prepass utility", which covers the stipple, the
    // normal maps, and depth/shadow maps.
    //
    // Verified before widening rather than after: all 83 went to the G16R16 prepass target, and
    // every one shared its exact vertex and primitive counts with a properly coloured converted
    // draw. They are duplicates of meshes we already convert, so hiding them removes a copy
    // rather than a surface.
    if (g_settings.skipUntextured && g_curPS.isPixelShader && g_curPS.prepassSamplersOnly &&
        g_curPS.albedoRank == 0) {
        ++g_skipStipplePrepass;
        // CLEAR EVERY STAGE on a skinned prepass, not just stage 0.
        //
        // BeginMark's own note said how this failure would announce itself: "if Remix did need the
        // other stages cleared, the failure announces itself: the magenta comes back." It came
        // back - a Remix capture shows the player character rendered in the marker's magenta, and
        // the character's prepass draws are exactly these, sharing vertex and primitive counts
        // with the converted copy beside them.
        //
        // The marker's hash IS in rtx.ignoreTextures and Remix logs it as loaded, so stage 0 is
        // being ignored. What is not ignored is the draw, because Remix categorises a draw from
        // ANY stage it finds a texture on, and a prepass arrives carrying whatever the previous
        // material draw left on stages 1-7. That is the same mechanism that made the composite
        // quads paint magenta over the world in run 42, fixed there by the same wide clear.
        //
        // Restricted to skinned draws so the cost stays where the evidence is: ~2,000 marked draws
        // a frame at 2 bridge calls per stage would be the expense that made the first wide clear
        // not worth keeping, while the skinned prepass is around a hundred.
        if (g_curLayout.skinned) g_markClearAllStages = true;
        g_dispReason = "prepass: only normal/stipple/depth samplers";
        return HiddenDisp(true);
    }

    // The same conclusion, reached by a property instead of by a list of sampler names.
    //
    // The rule above needs every sampler to be one this shim happens to recognise, so a prepass
    // that samples anything unlisted falls through and converts WHITE. Measured 2026-08-19:
    // ir_sr3pchair_mc[5] samples only Dob_MapSampler and cust_normal_map_blend_mc[2] samples
    // baseSampler/body_age/muscle - none of which are on the utility list, and 8-20 draws a
    // frame were rendering white because of it.
    //
    // SR3 uses inferred lighting, so a pass that produces visible colour MUST read the L-buffer
    // to apply that lighting to itself. A pass with no colour map, no colour constant, and no
    // L-buffer read produces no colour at all - it is filling the G-buffer.
    //
    // Checked against the whole shader database before being written, not after, because
    // hiding a real surface makes it invisible:
    //
    //     no albedo map, HAS constant, reads L-buffer :  117   material passes - untouched here
    //     no albedo map, NO constant,  reads L-buffer :    0   <- nothing can be wrongly hidden
    //     no albedo map, NO constant,  no L-buffer    :  962   prepasses - this rule
    //
    // The middle row being zero is what makes this safe: there is no shader in the game that
    // this rule could hide and that could also have produced colour.
    // Screen-space passes: geometry that is a PROXY for a region of the screen, not a surface.
    //
    // "if i shoot the ground, a plane fullscreens my camera." A screen-space decal is drawn as a
    // quad or box that projects its texture onto whatever the G-buffer already holds; it is
    // clipped to real geometry by the projection, so its own extent may be enormous. Converted to
    // fixed function that proxy becomes an actual surface in the world, and standing inside one
    // fills the view - which is exactly the report.
    //
    // The test is that the shader samples the G-buffer's NORMALS. Reading the orientation of
    // geometry already on screen is something only a screen-space pass needs; a real surface
    // carries its own. Checked across every pixel shader in the game before writing it:
    //
    //     30 shaders sample IR_GBuffer_Normals:
    //        22  ir_light_*                    light volumes - already hidden separately
    //         4  rl_ssao_* / rl_rao_*          ambient occlusion passes
    //         1  ir_decal_screenspace          bullet holes and the like
    //         1  ir_blood_pool_screenspace     blood splatters
    //     0 real surfaces.
    //
    // Water was the danger here and is NOT in that list: it samples the depth buffer (for soft
    // edges) but never the normals, so the broader "samples any depth buffer" rule - which would
    // have hidden all five water shaders - was rejected in favour of this one.
    //
    // These decals are LOST from the path-traced scene rather than corrected. That is the honest
    // trade: fixed function cannot express a projection onto the depth buffer, so the choice is
    // between not seeing a bullet hole and having it fill the screen.
    // Particle billboards. The `rl_particle_*` vertex shaders BUILD their quad from camera basis
    // vectors and per-particle data - the vertex buffer holds corner offsets, not positions. Fixed
    // function has no way to reproduce that, so converting one submits the raw corner data as
    // geometry and the result can land anywhere at any size. Probe lines #6 and #7 of run 38 are
    // two of them, four vertices each, one carrying an objTM translation of (0, 0, -1024).
    //
    // The test is the sampler SPELLING, and it is exact rather than approximate:
    //     Depth_bufferSampler      29 shaders, every one rl_particle_*, none reading the L-buffer
    //     IR_GBuffer_DepthSampler  40 shaders - this is what WATER uses
    //     Depth_mapSampler          5 shaders - projectors
    // So this catches the particle system and cannot catch water, which is the mistake the
    // broader "samples any depth buffer" rule would have made.
    if (g_curPS.isPixelShader && g_curPS.samplesParticleDepth) {
        ++g_skipParticlePass;
        g_dispReason = "particle billboard: built in the vertex shader, so its vertex buffer is "
                       "corner offsets rather than positions";
        return HiddenDisp(true);
    }

    if (g_curPS.isPixelShader && g_curPS.samplesGBufferNormals) {
        ++g_skipScreenSpacePass;
        g_dispReason = "screen-space pass: samples the G-buffer normals, so its geometry is a "
                       "proxy for the screen rather than a surface";
        return HiddenDisp(true);
    }

    if (g_settings.skipUntextured && g_curPS.isPixelShader && g_curPS.anySampler &&
        g_curPS.albedoRank == 0 && g_curPS.colourConstReg < 0 && !g_curPS.samplesLBuffer) {
        ++g_skipNoColourPass;
        g_dispReason = "prepass: no colour map, no colour constant, never reads the L-buffer";
        return HiddenDisp(true);
    }

    // A real material with no colour map (typically only Normal_MapSampler). It IS scene
    // geometry and must stay visible - but passing it through to Remix is the worst option of
    // the three, and that is measured, not reasoned:
    //
    //   pass through -> Remix reads texture stage 0 as albedo, and stage 0 holds the NORMAL MAP.
    //                   ~900 materials render as tangent-space green/orange. The user identified
    //                   this artefact long before we understood it.
    //   convert as-is -> identical result; the normal map is still what is bound.
    //   convert blank -> untextured white. Wrong, but honestly wrong: it does not masquerade as
    //                   surface detail, and it is a flat colour rather than a misleading pattern.
    //
    // Converting also means Remix sees OUR draw instead of capturing the shader-driven one, so
    // this removes a duplicate surface as well as the false colour. EffectiveAlbedo() unbinds
    // stage 0 for these, which is where the blanking actually happens.
    // Counted so the size of this population stays visible - but only for the materials it names.
    // It used to increment for EVERY draw reaching the end of Classify, so the log line read
    // 1,403/frame against 843 converted draws: it was reporting converted + other-camera under a
    // label that says "materials with no colour map". A counter that does not count what its name
    // says is worse than no counter, and this file has already lost a session to one.
    if (g_curPS.isPixelShader && g_curPS.albedoRank == 0) ++g_skipNoAlbedo;

    // There is deliberately no COLORWRITEENABLE == 0 test here. It never fired - a complete frame
    // dump contains zero such draws - and it read that state through ShadowGetRS, which returns 0
    // when the query fails. The only way it could ever have triggered was a false positive that
    // would have skipped the entire world.

    // SR3's deferred G-BUFFER pass: a pixel shader that writes more than one render target.
    // Disassembled 2026-08-28, ir_sr3npcskinfull_mc shader[6] - the head: oC0.xy is the normal,
    // oC1/oC2 are data, and its one colour-ranked sampler (Blend_Map) contributes a single scalar
    // to a fresnel term. No colour anywhere. The albedo ranker scored that mask 70 and handed it
    // to Remix as the head's base colour, on top of the correct material copy - the wrong texture
    // and the z-fighting, one cause.
    //
    // PASS-THROUGH, and this placement is the whole point. The first version returned
    // HiddenDisp() from the TOP of Classify and hid 693 draws a frame: with hiddenPassMode=2 that
    // is SKIP, the G-buffer never reached the device, and the material pass reads it back through
    // IR_GBuffer_DSF_DataSampler and IR_LBufferSampler - so the world rendered black except where
    // nothing deferred was on screen. **A draw whose RESULT the engine reads can never be
    // skipped, only hidden.** That rule is written above hiddenPassMode in this file and I broke
    // it anyway.
    //
    // Here, at the end, every earlier rule has already had its say: draws those rules skip are
    // still skipped, and only the handful that would otherwise have been CONVERTED land here.
    // Pass-through submits them to the device exactly as before, so the engine's own buffers are
    // untouched, and with vertex capture off Remix never sees them.
    // ...but ONLY for SKINNED draws. Corrected 2026-08-28 after the unconditional version turned
    // the world black a second time.
    //
    // Terrain's G-buffer shader (ir_bbterrain1_s[5]) is structurally identical to the head's -
    // writes normals to oC0.xy, data to oC1/oC2, and its Blend_Map sample is dead. So "this pass
    // produces no colour" is true of BOTH and does not separate them. What separates them is what
    // the game BINDS: on a world draw the texture at that stage is the object's real colour map,
    // which is why converting terrain from this pass looks right; on a character draw it is a
    // specular mask. And characters have a material-pass copy of the same geometry, while the
    // terrain's only converted copy is this one - the frame dump caught the unconditional rule
    // passing through `v=17601 Diffuse_MapSampler` terrain and every large instanced mesh.
    // CHANGE 1, 2026-09-18: THE STRETCHED POLYGON - VFX mask materials render as solid
    // slabs. A multi-agent investigation, verified independently against the frame dump
    // (Saints Row 3\sr3-rtx-frame-1.log:5637), found a four-vertex quad 46 units wide and
    // 785 units tall, ps='Alpha_MaskSampler', blend=1(5/2) (SRCALPHA/ONE, additive) - a
    // searchlight beam or light shaft, not a decal, whose entire SHAPE lives in
    // Alpha_MaskSampler at sampler register 3 (re\shader_constants.csv:
    // ir_vfxmask_bs.fxo_pc,5,ps_3_0,Alpha_MaskSampler,sampler,3,1, alongside
    // Alpha_Falloff_Amount/Power and Soft_Fade_Alpha).
    //
    // The failure chain, all verified in code: AlbedoRank scores Diffuse* 100 and Alpha_Mask
    // 0, so stage 0 gets the Diffuse map; SetupTextureStages then disables stages 1-7, so
    // Alpha_MaskSampler - the ONLY sampler that carries this material's actual shape - is
    // never bound at all. These shaders declare no Alpha_Threshold, so shaderCutout is
    // false; the chosen albedo is not Decal-named, so decalAlphaBlend is false too; ALPHAARG1
    // therefore falls back to D3DTA_TFACTOR, and TFACTOR is pinned to 0xFFFFFFFF. Alpha stuck
    // at 1.0 under additive blending hands Remix the FULL diffuse texture, so a soft cone of
    // light becomes an opaque slab covering about 168% of screen height.
    //
    // Checked for over-matching before this shipped: grepping re\shader_constants.csv for
    // every shader that names a sampler containing "Alpha_Mask" turns up exactly four files -
    // ir_vfxmask_bs.fxo_pc, ir_vfxmask_s.fxo_pc, ir_vfxmasktod_bs.fxo_pc,
    // ir_vfxmasktod_s.fxo_pc - all one family, the VFX mask material itself. Nothing that
    // names a character, a vehicle or a wall shader ever names Alpha_Mask, so this test
    // cannot reach them.
    //
    // The test is a real pixel shader, genuinely blended by its FACTORS rather than merely
    // having the ALPHABLENDENABLE flag set (SR3 leaves that on for ordinary opaque draws
    // too - trulyBlended below is decalAlphaBlend's own test, reproduced here because
    // Classify runs before SetupTextureStages computes its copy and that local has gone out
    // of scope by the time this function is reached), that names an Alpha_Mask sampler
    // somewhere OTHER than the stage AlbedoRank actually chose as albedo. Excluding the
    // chosen stage is what keeps this from ever firing on a shader that names Alpha_Mask AS
    // its own albedo (none currently do, but the exclusion costs nothing and removes the
    // possibility outright).
    //
    // Routed through HiddenDisp rather than a bare Disp::Skip, so the mode switch governs
    // this exactly like every other hidden pass and the disposition is whatever that policy
    // resolves to, never a second policy disagreeing with the first. Under the deployed
    // hiddenPassMode=2, HiddenDisp unconditionally returns Disp::Skip regardless of the
    // markSafe argument (mode 2's switch case never looks at it) - the draw never reaches
    // the device. That is safe here specifically because the shape it would otherwise
    // contribute is already the wrong one (a solid slab standing in for a light shaft), so
    // removing it cannot punch a hole in geometry that was ever correct - unlike the geometry
    // prepass, whose skip broke GPU occlusion culling because the ENGINE reads that pass's
    // own result back.
    if (g_settings.skipMaskOnlyVfx && g_curPS.isPixelShader) {
        const bool trulyBlended = ShadowGetRS(dev, D3DRS_ALPHABLENDENABLE) != 0 &&
                                  !(ShadowGetRS(dev, D3DRS_SRCBLEND) == D3DBLEND_ONE &&
                                    ShadowGetRS(dev, D3DRS_DESTBLEND) == D3DBLEND_ZERO);
        if (trulyBlended) {
            bool namesAlphaMaskElsewhere = false;
            for (int st = 0; st < 8; ++st) {
                if (st == g_curPS.albedoStage) continue;
                if (g_curPS.samplerName[st][0] && StrStrIA(g_curPS.samplerName[st], "Alpha_Mask")) {
                    namesAlphaMaskElsewhere = true;
                    break;
                }
            }
            if (namesAlphaMaskElsewhere) {
                ++g_vfxMaskHidden;
                if (g_lastPS) g_vfxMaskShaders.insert(g_lastPS);
                return Because("VFX mask material: its opacity lives in a sampler fixed function cannot bind",
                               HiddenDisp(true));
            }
        }
    }

    // RESTORED 2026-09-19. This rule was silently DELETED on 2026-09-18 when the VFX mask rule
    // above was patched in: the exact-match anchor used for that insertion spanned this whole
    // block plus the return below, and the replacement text omitted it. The anchor matched once,
    // the script reported success and the build compiled clean, so nothing caught it until the
    // user reported NPC heads z-fighting and white underwear - which are ONE fault, this one:
    // the character G-buffer copy converted with Blend_Map, a specular mask, as its albedo, on
    // top of the correct material copy. A specular mask read as colour is mostly white.
    // The lesson: when an anchor spans code you mean to KEEP, the replacement must repeat it
    // verbatim, and a counter that reads 0.0/frame is worth as much attention as one that spikes.
    // SR3's deferred G-BUFFER pass: a pixel shader that writes more than one render target.
    // Disassembled 2026-08-28, ir_sr3npcskinfull_mc shader[6] - the head: oC0.xy is the normal,
    // oC1/oC2 are data, and its one colour-ranked sampler (Blend_Map) contributes a single scalar
    // to a fresnel term. No colour anywhere. The albedo ranker scored that mask 70 and handed it
    // to Remix as the head's base colour, on top of the correct material copy - the wrong texture
    // and the z-fighting, one cause.
    //
    // PASS-THROUGH, and this placement is the whole point. The first version returned
    // HiddenDisp() from the TOP of Classify and hid 693 draws a frame: with hiddenPassMode=2 that
    // is SKIP, the G-buffer never reached the device, and the material pass reads it back through
    // IR_GBuffer_DSF_DataSampler and IR_LBufferSampler - so the world rendered black except where
    // nothing deferred was on screen. **A draw whose RESULT the engine reads can never be
    // skipped, only hidden.** That rule is written above hiddenPassMode in this file and I broke
    // it anyway.
    //
    // Here, at the end, every earlier rule has already had its say: draws those rules skip are
    // still skipped, and only the handful that would otherwise have been CONVERTED land here.
    // Pass-through submits them to the device exactly as before, so the engine's own buffers are
    // untouched, and with vertex capture off Remix never sees them.
    // ...but ONLY for SKINNED draws. Corrected 2026-08-28 after the unconditional version turned
    // the world black a second time.
    //
    // Terrain's G-buffer shader (ir_bbterrain1_s[5]) is structurally identical to the head's -
    // writes normals to oC0.xy, data to oC1/oC2, and its Blend_Map sample is dead. So "this pass
    // produces no colour" is true of BOTH and does not separate them. What separates them is what
    // the game BINDS: on a world draw the texture at that stage is the object's real colour map,
    // which is why converting terrain from this pass looks right; on a character draw it is a
    // specular mask. And characters have a material-pass copy of the same geometry, while the
    // terrain's only converted copy is this one - the frame dump caught the unconditional rule
    // passing through `v=17601 Diffuse_MapSampler` terrain and every large instanced mesh.
    if (g_settings.skipDeferredGBuffer && g_curPS.isPixelShader && g_curPS.rtCount > 1 &&
        g_curLayout.skinned) {
        ++g_gbufferHidden;
        return Because("deferred G-buffer pass: writes >1 render target, so it carries no colour",
                       Disp::PassThrough);
    }

    return Disp::Convert;
}


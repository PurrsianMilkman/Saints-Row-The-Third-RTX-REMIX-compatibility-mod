// parts/11_texture_stages.cpp - SetupTextureStages, declaration dumps, hidden-pass dispositions.
// NOT a standalone translation unit: #included, in order, by ../sr3rtx.cpp inside its anonymous
// namespace. Originally sr3rtx.cpp lines 7727-8266 (old file:line citations map here).
// ---------------------------------------------------------------------------------------------
// Hair: its colour is a per-texel recipe over constants that have never been captured
//
// `ir_sr3pchair_c` shader[8] is the hair material pass. Its constant table:
//
//     Hair_Parameters   c1     Hair_Spec_Alpha  c2
//     Hair_Spec_Color1  c3     Hair_Spec_Color2 c4
//     Dob_Map           s0     Diffuse_Map      s1
//
// and it ends by building a colour through a chain of `cmp` selects over those constants before
// the usual fog lerp and `mul oC0, r6, c37`. That is a per-texel computation of the same shape as
// the clothing recipe - fixed function cannot express it, and the honest answer is a generated
// texture, as it was for cloth.
//
// None of that can be designed without the runtime VALUES, and this project has been trying to
// read them since 2026-08-19 with the note "Hair_Spec_Color1/2 never captured - the probe's slots
// fill first". ProbeCharacterConstants reports once per distinct first-sampler name and stops at
// 12, and hair loses the race every time. So this is a probe of its own, keyed on the SHADER
// declaring Hair_Spec_Color1 rather than on a sampler name, which cannot be crowded out.
//
// Read-only. It logs and binds nothing.
unsigned g_hairReports = 0;
const void* g_hairSeen[6]{};

void ProbeHairMaterial() {
    if (!g_settings.clothProbe || g_hairReports >= 6) return;
    const ShaderInfo& ps = g_curPS;
    if (!ps.isPixelShader || ps.hairColorReg[0] < 0) return;   // not a hair shader
    const void* key = g_curTexture[ps.albedoStage >= 0 && ps.albedoStage < 8
                                       ? ps.albedoStage : 0];
    for (unsigned i = 0; i < g_hairReports; ++i)
        if (g_hairSeen[i] == key) return;
    g_hairSeen[g_hairReports] = key;
    ++g_hairReports;

    auto c = [](int reg, float* out) {
        if (reg >= 0 && reg < static_cast<int>(kMaxPsConst)) memcpy(out, g_psConst[reg], 16);
    };
    float h1[4]{}, h2[4]{};
    c(ps.hairColorReg[0], h1);
    c(ps.hairColorReg[1], h2);
    Log("HAIR #%u: albedoStage=%d rank=%d first='%s'", g_hairReports, ps.albedoStage,
        ps.albedoRank, ps.firstSampler);
    Log("      Hair_Spec_Color1(c%d) = (%.3f %.3f %.3f %.3f)", ps.hairColorReg[0],
        h1[0], h1[1], h1[2], h1[3]);
    Log("      Hair_Spec_Color2(c%d) = (%.3f %.3f %.3f %.3f)", ps.hairColorReg[1],
        h2[0], h2[1], h2[2], h2[3]);
    // c1/c2 are Hair_Parameters and Hair_Spec_Alpha in every hair shader in the game. Dumped raw
    // rather than reflected, because naming them costs a reflection change and the registers are
    // fixed across both hair files.
    for (int reg = 0; reg <= 4; ++reg) {
        if (reg >= static_cast<int>(kMaxPsConst)) break;
        const float* v = g_psConst[reg];
        Log("      c%-2d = (%.3f %.3f %.3f %.3f)", reg, v[0], v[1], v[2], v[3]);
    }
    for (int st = 0; st < 4; ++st) {
        IDirect3DBaseTexture9* t = g_curTexture[st];
        UINT w = 0, h = 0; int fmt = 0, lv = 0;
        if (t && t->GetType() == D3DRTYPE_TEXTURE) {
            D3DSURFACE_DESC sd{};
            if (SUCCEEDED(static_cast<IDirect3DTexture9*>(t)->GetLevelDesc(0, &sd))) {
                w = sd.Width; h = sd.Height; fmt = static_cast<int>(sd.Format);
                lv = static_cast<int>(static_cast<IDirect3DTexture9*>(t)->GetLevelCount());
            }
        }
        Log("      s%d %-20s %p %ux%u fmt=%d levels=%d%s", st,
            ps.samplerName[st][0] ? ps.samplerName[st] : "(unnamed)",
            static_cast<void*>(t), w, h, fmt, lv,
            st == ps.albedoStage ? "   <- bound as albedo" : "");
    }
}

void SetupTextureStages(IDirect3DDevice9* dev) {
    ProbeClothMaterial(dev);
    ProbeCharacterConstants();
    ProbeCharacterTextures();
    ProbeDiffuseColor();
    ProbeHairMaterial();
    // SELECTARG1 from the texture, NOT modulate against the vertex colour. Remix wants the
    // material's own base colour; multiplying in a diffuse term the game meant for its own
    // shading darkens everything and bakes lighting into the albedo.
    // A material with no colour map takes its base colour from a shader CONSTANT instead of a
    // texture. ir_bbsimple2_nodiffmap_bs.fxo_pc shader [8] ends with
    //     mul_pp oC0, r1, c37        ; r1 = lighting, c37 = Tint_color
    // so the constant IS the albedo. 150 draws a frame were rendering WHITE for want of reading
    // it. Selecting TFACTOR instead of TEXTURE is the fixed-function equivalent of that mul.
    D3DCOLOR constColour = 0;
    const bool haveConstant = ConstantAlbedo(constColour);
    const bool useConstant = (g_curPS.albedoRank == 0) && haveConstant;

    // A FALLBACK map is not albedo on its own. Measured 2026-08-19 on the player character: 18
    // draws found a real Diffuse_Map, but 13 fell back to Pattern_Map (rank 80) or Blend_Map
    // (rank 70) - and those materials have no diffuse map BY DESIGN. They are SR3's clothing
    // customisation shaders, which carry Diffuse_Color_a/b/c and Tint_color and build their
    // colour from constants. ir_sr3npcclothfull_c.fxo_pc shader [8]:
    //     texld_pp r3, v0, s0            ; Pattern_Map
    //     mad_pp   r3.xyz, r3.x, c3, ... ; combined with Diffuse_Color_c
    //     mul_pp   oC0, r1, c37          ; and the whole result * Tint_color
    // Binding the pattern raw drops that final multiply entirely, which is why parts of the
    // character read as untextured while the diffuse-mapped parts look right. MODULATE against
    // TFACTOR restores the multiply. It is an approximation - the real shader masks three
    // colours through separate channels and fixed function cannot express that - but a tinted
    // pattern is far closer than an untinted one.
    //
    // MEASURED WRONG, 2026-08-19, and off by default because of it. With this on, the user
    // reported "my character clothes are dark, NPC clothes and body is dark" and the tinted
    // population had doubled to 43 draws/frame. The reasoning was sound and the result was not:
    // the real shader is
    //     (pattern.r * Diffuse_Color_c + pattern.gba) * Tint_color
    // where the pattern's channels are MASKS selecting between three customisation colours.
    // Modulating the whole pattern texture by Tint_color multiplies two things that were never
    // multiplied - a mask by a tint - and the product is darker than either. "Safe by
    // construction because a white tint is a no-op" was true and irrelevant: these tints are not
    // white.
    //
    // Kept as a toggle rather than deleted, because the finding it rests on is still correct -
    // these materials DO take their colour from constants and binding the pattern raw is also
    // wrong. Getting it right needs the three-colour mask evaluated per texel, which fixed
    // function cannot do; the honest options are a generated texture or leaving it alone.
    const bool tintFallback = g_settings.tintFallbackAlbedo && g_curPS.albedoRank > 0 &&
                              g_curPS.albedoRank < 100 && haveConstant;

    if (useConstant) {
        ShadowSetTSS(dev, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        ShadowSetRS(dev, D3DRS_TEXTUREFACTOR, constColour);
        ShadowSetTSS(dev, 0, D3DTSS_COLORARG1, D3DTA_TFACTOR);
        ++g_constantAlbedo;
    } else if (tintFallback) {
        ShadowSetTSS(dev, 0, D3DTSS_COLOROP, D3DTOP_MODULATE);
        ShadowSetRS(dev, D3DRS_TEXTUREFACTOR, constColour);
        ShadowSetTSS(dev, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        ShadowSetTSS(dev, 0, D3DTSS_COLORARG2, D3DTA_TFACTOR);
        ++g_tintedAlbedo;
    } else {
        ShadowSetTSS(dev, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        // Honour this function's own "one register, one owner" contract for the ordinary textured
        // draw too - see resetTFactorOnTexturedDraws' comment in Settings for why a stale value
        // here tints a surface that never asked for it, and why the tint tracks camera movement.
        if (g_settings.resetTFactorOnTexturedDraws) {
            ShadowSetRS(dev, D3DRS_TEXTUREFACTOR, 0xFFFFFFFF);
            ++g_tfactorReset;
        }
        // Which texture is about to carry colour: the same rule EffectiveAlbedo() applies for
        // a real texture pick (named colour sampler by rank, else raw stage 0 with no shader
        // reflection to guide it), duplicated narrowly rather than called, because
        // EffectiveAlbedo() itself does not run until AFTER this function returns - it needs
        // the su/sv and transformflags SetupTextureStages is still computing below, published
        // for ProbeDecalDraw - and calling it here too would double every counter it keeps
        // (g_albedoMoved, g_albedoNullRanked, g_albedoBlanked, g_albedoStage0Raw). Its
        // render-target exclusion is not reproduced: that only ever swaps one texture for
        // another when the FIRST pick IS a render target, and every render target this game
        // creates is A8R8G8B8 or X8R8G8B8 - never colourless - so skipping that step cannot
        // change the answer this check needs. Gated on the setting itself so OFF costs
        // exactly what today's build costs: no lookup, nothing beyond the one flag test.
        bool tookColourlessTexture = false;
        if (g_settings.albedoRejectColourlessTexture) {
            IDirect3DBaseTexture9* prospectiveAlbedo =
                (g_settings.rankAlbedo && g_curPS.albedoRank > 0 &&
                 g_curPS.albedoStage >= 0 && g_curPS.albedoStage < 8)
                    ? g_curTexture[g_curPS.albedoStage]
                    : (g_curPS.albedoRank == 0 && g_curPS.isPixelShader) ? nullptr
                                                                          : g_curTexture[0];
            // fmt=28 (D3DFMT_A8) is alpha-only: no RGB channels exist, so
            // SELECTARG1/D3DTA_TEXTURE reads back RGB=0 in fixed function and the surface
            // renders solid BLACK under its own alpha mask. Measured 2026-09-18 on the decal
            // probe (albedoSampler='Decal_MapSampler', stage=2, a static world decal, not a
            // particle) - the same shape of defect already recorded for the distfield family,
            // whose shader does smoothstep on this exact alpha and takes colour from a
            // constant (Decal_Map_Color) instead. TFACTOR in THIS branch is forced to opaque
            // white a few lines below regardless of this choice (no shader constant, no tint
            // fallback), so redirecting COLORARG1 here is not a guess at the real per-family
            // colour - it is the same white this branch already renders wherever a texture is
            // missing outright, now applied through the mask instead of ignoring it. Resolving
            // the real constant per decal family is future work; this only stops a format
            // with no colour from being read as though it had one.
            if (prospectiveAlbedo && AlbedoFormatFor(prospectiveAlbedo).channels < 3) {
                ShadowSetTSS(dev, 0, D3DTSS_COLORARG1, D3DTA_TFACTOR);
                ++g_albedoColourlessRejected;
                g_albedoColourlessTextures.insert(prospectiveAlbedo);
                tookColourlessTexture = true;
            }
        }
        if (!tookColourlessTexture) {
            ShadowSetTSS(dev, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        }
        // Genuinely white: no colour map AND no constant to stand in for one. This is the only
        // population that actually renders untextured, and it is counted separately because
        // g_skipNoAlbedo counts every rank-0 draw INCLUDING the ones the constant rule rescues.
        // Reporting that number as "still blank" would overstate the problem by the whole size
        // of the fix - the exact error the note at g_skipNoAlbedo warns about.
        // These materials are already named, once per distinct first-sampler, by the
        // "untextured material #N" report in AlbedoForDraw. Only the count is added here.
        if (g_curPS.isPixelShader && g_curPS.albedoRank == 0) ++g_blankAlbedo;
    }

    // Alpha needs care. Saints Row leaves alpha test enabled with a reference of 0 on ordinary
    // opaque world draws, so taking alpha from the texture would hand Remix the sub-1.0 alpha
    // channel these materials carry for their own purposes and every wall would turn
    // translucent. Only a genuine cutout - alpha test on AND a non-zero reference - gets the
    // texture's alpha; everything else stays opaque via TFACTOR.
    const DWORD alphaTest = ShadowGetRS(dev, D3DRS_ALPHATESTENABLE);
    const DWORD alphaRef = ShadowGetRS(dev, D3DRS_ALPHAREF);

    // A shader-side cutout counts as a cutout. Measured 2026-08-19 from tree_s.fxo_pc:
    //
    //     float Alpha_Threshold;   // c41
    //     texkill r0
    //
    // The pixel shader kills the fragment itself, so D3DRS_ALPHATESTENABLE is never involved and
    // the "alpha test on AND ref > 0" rule below cannot see it. 403 pixel shaders do this - the
    // whole `ir_at_*` family (at = alpha test), foliage, decals, windows and cloth - and every
    // one of them was being handed opaque alpha, which is why "tree leaves texture is not working
    // correctly": the leaf card renders as a solid rectangle with the leaf shape thrown away.
    const bool shaderCutout = g_curPS.isPixelShader && g_curPS.alphaThresholdReg >= 0;
    const bool cutout = (alphaTest && alphaRef > 0) || shaderCutout;

    // FIX B: three decal families do NOT declare Alpha_Threshold and are blended by the game
    // instead - their pixel shader computes alpha = decal.a * Decal_Map_Opacity * Tint_color.a
    // and the draw is submitted with real SRCALPHA/INVSRCALPHA factors (the frame dump shows
    // blend=1(5/6) on them). Under the plain cutout rule above those draws take ALPHAARG1 from
    // TFACTOR - alpha 1.0 - so the decal covers the surface beneath it as an OPAQUE rectangle
    // instead of blending onto it.
    //
    // trulyBlended reuses, verbatim, the test the decal-offset code already computes elsewhere in
    // this file: ALPHABLENDENABLE is on and the factor pair is not the ONE/ZERO SR3 leaves set on
    // its own opaque draws. Recomputed here through the shadow accessor because that code runs in
    // a different function and its own local goes out of scope before SetupTextureStages is called.
    const bool trulyBlended = ShadowGetRS(dev, D3DRS_ALPHABLENDENABLE) != 0 &&
                              !(ShadowGetRS(dev, D3DRS_SRCBLEND) == D3DBLEND_ONE &&
                                ShadowGetRS(dev, D3DRS_DESTBLEND) == D3DBLEND_ZERO);
    const bool decalAlphaBlend = g_settings.decalAlphaFromTexture && trulyBlended &&
                                 g_curPS.isDecalAlbedo;
    if (decalAlphaBlend) ++g_decalAlphaFromTexture;

    // Fixed function has no texkill, so the threshold becomes a real alpha test - which is also
    // the signal Remix needs to treat the surface as a cutout rather than as glass. The previous
    // values are saved and put back in EndFFP: EndFFP restores textures and shaders but NOT
    // render states, so anything left set here leaks into the engine's own draws, and alpha test
    // is a state the engine actively relies on.
    g_alphaStateOverridden = false;
    if (shaderCutout) {
        const int reg = g_curPS.alphaThresholdReg;
        float threshold = 0.5f;
        if (reg >= 0 && reg < static_cast<int>(kMaxPsConst)) {
            const float t = g_psConst[reg][0];
            if (std::isfinite(t) && t > 0.0f && t <= 1.0f) threshold = t;
        }
        g_savedAlphaTest = alphaTest;
        g_savedAlphaRef = alphaRef;
        g_savedAlphaFunc = ShadowGetRS(dev, D3DRS_ALPHAFUNC);
        g_alphaStateOverridden = true;
        ShadowSetRS(dev, D3DRS_ALPHATESTENABLE, TRUE);
        ShadowSetRS(dev, D3DRS_ALPHAREF, static_cast<DWORD>(threshold * 255.0f + 0.5f));
        ShadowSetRS(dev, D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);
    }
    // TFACTOR carries the opaque alpha for non-cutouts AND, above, the constant base colour.
    // ConstantAlbedo forces alpha to 255 precisely so one register can serve both without the
    // colour path making every constant-coloured surface translucent.
    if (!useConstant && !tintFallback) ShadowSetRS(dev, D3DRS_TEXTUREFACTOR, 0xFFFFFFFF);
    ShadowSetTSS(dev, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    ShadowSetTSS(dev, 0, D3DTSS_ALPHAARG1, (cutout || decalAlphaBlend) ? D3DTA_TEXTURE : D3DTA_TFACTOR);
    ShadowSetTSS(dev, 0, D3DTSS_TEXCOORDINDEX, 0);

    // SAMPLER state for stage 0. Nothing set this before, so a converted draw sampled with
    // whatever the engine had last configured for one of ITS passes - and the engine's pixel
    // shaders compute their own coordinates, so it has no reason to leave a mode that suits us.
    //
    // The user's own observation is what pins this down: "trees have albedo texture" while the
    // rest of the world shows only flat colour. Foliage UVs sit inside a single atlas cell, so
    // they land in 0..1 and CLAMP does nothing to them. A tiled world surface has UVs well past
    // 1 by design, and under CLAMP every pixel of it samples the same edge texel - which is
    // precisely "I only see basic material color". One inherited state, and exactly the split
    // between what works and what does not.
    //
    // WRAP is what the game's own world materials use; the tiling registers this shim already
    // reads exist only because these surfaces repeat. Filtering is set for the same reason: the
    // textures arrive with full mip chains (measured: 8-10 levels), which are wasted if the
    // inherited MIPFILTER is NONE.
    // Record what we INHERITED, a few times, so the next run either confirms the theory above or
    // kills it. If these read CLAMP (3) the diagnosis is right; if they already read WRAP (1)
    // then sampler state was never the problem and this fix is inert - which is worth knowing
    // before any more of the flat-colour work is built on it.
    if (g_samplerProbeReports < 6) {
        ++g_samplerProbeReports;
        DWORD au = 0, av = 0, mip = 0, mag = 0;
        dev->GetSamplerState(0, D3DSAMP_ADDRESSU, &au);
        dev->GetSamplerState(0, D3DSAMP_ADDRESSV, &av);
        dev->GetSamplerState(0, D3DSAMP_MIPFILTER, &mip);
        dev->GetSamplerState(0, D3DSAMP_MAGFILTER, &mag);
        Log("INHERITED sampler0 before conversion #%u: addressU=%lu addressV=%lu mipfilter=%lu "
            "magfilter=%lu (1=WRAP 2=MIRROR 3=CLAMP | filter 0=NONE 1=POINT 2=LINEAR) | ps='%s'",
            g_samplerProbeReports, static_cast<unsigned long>(au), static_cast<unsigned long>(av),
            static_cast<unsigned long>(mip), static_cast<unsigned long>(mag),
            g_curPS.firstSampler[0] ? g_curPS.firstSampler : "(none)");
    }

    // The population above measured WRONG on 2026-09-18: all six reports read addressU=1
    // addressV=1 (WRAP) - which says sampler state was never the problem - but all six were
    // ps='Diffuse_MapSampler', ordinary wall materials, and none were a decal. That is the
    // wrong population to answer the CLAMP-versus-WRAP question from: a decal's measured UVs
    // run from -0.95 to +1.24 (see ProbeDecalDraw), well outside 0..1, so it is the one family
    // that would actually look different under the two modes. This second, separate set of
    // slots is gated on the CHOSEN albedo sampler naming a Decal map - the same test
    // ProbeDecalDraw uses - so the next run reports the inherited address mode for THAT
    // population specifically, without disturbing the diffuse slots above.
    if (g_samplerProbeDecalReports < 6 && g_curPS.albedoSampler[0] &&
        StrStrIA(g_curPS.albedoSampler, "Decal")) {
        ++g_samplerProbeDecalReports;
        DWORD dau = 0, dav = 0, dmip = 0, dmag = 0;
        dev->GetSamplerState(0, D3DSAMP_ADDRESSU, &dau);
        dev->GetSamplerState(0, D3DSAMP_ADDRESSV, &dav);
        dev->GetSamplerState(0, D3DSAMP_MIPFILTER, &dmip);
        dev->GetSamplerState(0, D3DSAMP_MAGFILTER, &dmag);
        Log("INHERITED sampler0 before conversion, DECAL #%u: addressU=%lu addressV=%lu "
            "mipfilter=%lu magfilter=%lu (1=WRAP 2=MIRROR 3=CLAMP | filter 0=NONE 1=POINT "
            "2=LINEAR) | albedoSampler='%s'",
            g_samplerProbeDecalReports, static_cast<unsigned long>(dau),
            static_cast<unsigned long>(dav), static_cast<unsigned long>(dmip),
            static_cast<unsigned long>(dmag), g_curPS.albedoSampler);
    }

    ShadowSetSS(dev, 0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
    ShadowSetSS(dev, 0, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
    ShadowSetSS(dev, 0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    ShadowSetSS(dev, 0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    ShadowSetSS(dev, 0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);

    // Reproduce the shader's UV maths with a texture matrix: uv = raw * tiling / 1024.
    // The 1/1024 applies only to short2 coordinates (fixed function reads them as raw
    // integers); the per-material tiling applies whatever the storage format, because the
    // shader multiplies by it either way.
    float su = 1.0f, sv = 1.0f;
    // 2026-09-18 AUDIT FIX: the scale must come from the SET the conversion actually used, not
    // always set 0. FIX A (DecalUvSourceSet) sources this draw's uv from TEXCOORD1 when the
    // vertex shader declares no usage-index-0 input at all - exactly the decalonly/fauxinterior
    // families - and UvBufferFor then writes the RAW short values widened to float, deliberately
    // unscaled, on the understanding that THIS texture matrix supplies the missing 1/1024.
    // g_curLayout.texcoordType names set 0 only, which ParseDeclaration leaves at -1 for these
    // declarations (no stream-0 TEXCOORD0 exists to set it), so su/sv silently fell back to 1.0
    // and the D3DTTFF_DISABLE branch below let 1024 raw units reach the sampler as 1024 UV -
    // about a thousand texture repeats, which is the fine stripe pattern standing in for
    // ir_sr3fauxinterior's (fake window interior) windows in the user's screenshot.
    const int uvSrcType = (g_settings.decalUvScaleFromSet && DecalUvSourceSet() == 1)
                              ? g_curLayout.texcoord1Type : g_curLayout.texcoordType;
    if (uvSrcType == D3DDECLTYPE_SHORT2) su = sv = CurrentShortUVScale();
    // Measured apart from the FIX A uv-stream counters near UvBufferFor: how often taking the
    // set-1 type actually changes the SHORT2 verdict a plain texcoordType test would have given -
    // whether this population exists at all, printed beside those counters on the SHORT2 report
    // line.
    if (g_settings.decalUvScaleFromSet &&
        (uvSrcType == D3DDECLTYPE_SHORT2) != (g_curLayout.texcoordType == D3DDECLTYPE_SHORT2))
        ++g_decalUvScaleFixChanged;
    int tilingU = -1, tilingV = -1;
    TilingForAlbedo(tilingU, tilingV);
    if (tilingU >= 0 && tilingU < static_cast<int>(kMaxVsConst)) su *= g_vsConst[tilingU][0];
    if (tilingV >= 0 && tilingV < static_cast<int>(kMaxVsConst)) sv *= g_vsConst[tilingV][0];

    // Published for ProbeDecalDraw: the su/sv THIS draw's texture matrix is about to be set
    // to, and (below) whether the transform ends up COUNT2 or DISABLE. Read-only elsewhere -
    // nothing here changes what su/sv or the transform flag actually get set to.
    g_decalProbeSu = su;
    g_decalProbeSv = sv;

    if (su != 1.0f || sv != 1.0f) {
        // U and V tile independently, so a wrong pair skews the texture rather than merely
        // mis-scaling it - which is what "seams look misoriented" looks like.
        if (su != g_appliedU || sv != g_appliedV) {
            D3DMATRIX uv = kIdentity;
            uv._11 = su;
            uv._22 = sv;
            g_origSetTransform(dev, D3DTS_TEXTURE0, &uv);
            g_appliedU = su;
            g_appliedV = sv;
            ++g_uvMatrixWrites;
        }
        ShadowSetTSS(dev, 0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT2);
        g_decalProbeCount2 = true;
    } else {
        ShadowSetTSS(dev, 0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
        g_decalProbeCount2 = false;
    }

    // The game binds shadow maps, lookup tables and normal maps on higher stages for its pixel
    // shaders. Under FFP those become active and would be blended into the output.
    for (DWORD s = 1; s < 8; ++s) {
        ShadowSetTSS(dev, s, D3DTSS_COLOROP, D3DTOP_DISABLE);
        ShadowSetTSS(dev, s, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    }
}

// SR3 draws ~860 objects per frame through D3D9 instancing with an instance count of ONE, which
// means the object's transform is not in objTM but in the per-instance vertex stream. Those
// draws are convertible - the transform just has to be read from the right place. Dump the full
// declaration of the first few so the instance layout is known rather than assumed.
const char* DeclTypeName(int t);   // defined with the declaration parsing, further down

const char* UsageName(int u) {
    switch (u) {
        case D3DDECLUSAGE_POSITION: return "POSITION";
        case D3DDECLUSAGE_BLENDWEIGHT: return "BLENDWEIGHT";
        case D3DDECLUSAGE_BLENDINDICES: return "BLENDINDICES";
        case D3DDECLUSAGE_NORMAL: return "NORMAL";
        case D3DDECLUSAGE_TEXCOORD: return "TEXCOORD";
        case D3DDECLUSAGE_TANGENT: return "TANGENT";
        case D3DDECLUSAGE_BINORMAL: return "BINORMAL";
        case D3DDECLUSAGE_COLOR: return "COLOR";
        case D3DDECLUSAGE_POSITIONT: return "POSITIONT";
        default: return "?";
    }
}

unsigned g_instDeclReports = 0;
unsigned g_skinnedDeclDumps = 0;
void DumpInstancedDeclaration() {
    if (g_instDeclReports >= 3 || !g_curDecl || !g_haveCamera) return;
    D3DVERTEXELEMENT9 elems[64]{};
    UINT count = 0;
    if (FAILED(g_curDecl->GetDeclaration(elems, &count))) return;
    ++g_instDeclReports;
    Log("instanced draw declaration #%u (instances=%u) streams: "
        "0 stride=%u  1 stride=%u  2 stride=%u  3 stride=%u",
        g_instDeclReports, g_instanceCount, g_streamStride[0], g_streamStride[1],
        g_streamStride[2], g_streamStride[3]);
    for (UINT i = 0; i < count; ++i) {
        const D3DVERTEXELEMENT9& e = elems[i];
        if (e.Stream == 0xFF) break;
        Log("    stream=%u offset=%2u type=%-8s usage=%-12s index=%u",
            e.Stream, e.Offset, DeclTypeName(e.Type), UsageName(e.Usage), e.UsageIndex);
    }
}

// What should happen to a draw. The three-way split matters, and conflating two of them was a
// long-standing bug in this shim.
//
//   Convert      - re-issue as fixed function; this is what Remix path-traces.
//   Hide         - the draw must still EXECUTE (the engine depends on its output) but Remix
//                  must not see it. Anything else and Remix's vertex capture renders it anyway.
//   PassThrough  - leave it entirely alone and let Remix's vertex capture do what it can. Only
//                  for genuine scene geometry we cannot convert yet, such as skinned meshes.
//
// The distinction Hide/PassThrough is the one that was missing. Refusing to CONVERT a draw does
// nothing to stop Remix capturing it from vertex-shader output. Measured 2026-08-17: SR3 submits
// every piece of world geometry TWICE - once into the DSF/geometry prepass (its pixel shader
// samples nothing at all) and once into the material pass (carrying the real diffuse map). We
// were converting the material pass correctly and merely *not converting* the prepass, so Remix
// captured the prepass copy and drew it untextured on top of the real one. That is the origin of
// the shells and shapes around the player - not light volumes, and not a texture problem, which
// is why removing textures only ever chipped pieces off them.
// A fourth disposition, added 2026-08-17 after the user observed that "hiding" the shapes
// changed their TRANSFORM rather than removing them.
//
// That observation exposed a wrong assumption. `rtx.orthographicIsUI = True` is indeed set, so
// the mechanism works - but a UI-classified draw is not discarded by Remix, it is RASTERISED AS
// A 2D OVERLAY. Correct for a HUD; for a 3D prepass shell it means the geometry is still drawn,
// now under an identity projection, which is precisely a shape whose transform has changed.
//
// So orthographic demotion is only right for draws that genuinely ARE screen-space and must
// still execute (the composite/post chain - dropping those froze the image in an earlier
// session). For the geometry prepass the correct action is to not issue the draw at all:
//
//   Skip - the draw never reaches the device. Remix cannot see what was never submitted.
//
// The prepass exists to feed the engine's own light buffer, and for every draw we CONVERT we
// null the pixel shader, so that lighting is never sampled. The cost is that PASS-THROUGH draws
// (skinned characters) do still sample it, so their in-game shading may degrade - which is a
// real trade to measure, not a certainty to assume.
enum class Disp { Convert, Hide, Skip, PassThrough, Mark, Count };

// What to do with a pass that is real geometry but must not reach the path tracer: the geometry
// prepass, the depth-only pass, and the auxiliary cameras (shadow, reflection, paraboloid).
//
// The first three were each measured, and each is wrong in its own way:
//   0 pass  - Remix captures it and the world appears duplicated (runs 4-5). This IS the
//             z-fighting: run 17 counted 877 prepass draws a frame against 314 converted ones,
//             so a complete second copy of the world lands on top of the material pass.
//   1 ortho - Remix classifies it UI and rasterises it as a 2D overlay, so the shape is still
//             visible with a changed transform (what the user observed).
//   2 skip  - the draw never happens, so Remix cannot see it. MEASURED TO BREAK THE ENGINE: its
//             own submission collapsed from 2,615 to 593 draws a frame and the image froze,
//             because it reads the prepass back - GPU occlusion culling. The general rule that
//             came out of it: a draw whose RESULT the engine reads can never be skipped, only
//             hidden.
//   3 mark  - bind OUR OWN 4x4 texture to stage 0 for the duration of the draw, and otherwise
//             let the draw proceed untouched. Remix reads stage 0 as albedo, so a single hash
//             in rtx.ignoreTextures (or rtx.hideInstanceTextures) then names exactly this pass.
//
// Mode 3 is the only one that satisfies every constraint at once, and the reason it is safe is
// measured rather than hoped for: the prepass pixel shaders sample NOTHING. The state probe
// caught the identical mesh submitted once with ps='' and once with ps='Diffuse_MapSampler', so
// nothing bound to a texture stage can reach the prepass's output. The game renders bit-
// identically, its readback is untouched, and Remix gets a handle on the pass that is OUR
// texture and no game asset.
//
// This is not the use of rtx.ignoreTextures the user has objected to. That objection is about
// ignoring GAME textures to make shapes go away - which removes the texture everywhere it is
// used, breaks whenever a hash changes, and treats a geometry-pass problem as a material one.
// Ignoring a marker we created ourselves can affect nothing else in the world by construction.
//
// The safety precondition is CHECKED here, not assumed. HiddenDisp is reached from five call
// sites - the prepass, light volumes, orthographic passes, auxiliary cameras and mirrored
// passes - and only the prepass has a shader that samples nothing. For the other four,
// rebinding stage 0 would change what the game draws, so they fall back to pass-through and are
// counted separately.
// Why the current draw got its disposition. Recorded rather than re-derived, so the frame dump
// can say WHICH test fired - the difference between "1,302 draws passed through" and knowing they
// were auxiliary-camera draws. Set at every return; read only by the dump.
const char* g_dispReason = "";
// Set by Classify for the draw classes whose leftover texture bindings would otherwise decide
// their category in Remix. Only the screen-space post chain needs it; see BeginMark.
bool g_markClearAllStages = false;
bool g_auxCameraReported = false;
inline Disp Because(const char* why, Disp d) { g_dispReason = why; return d; }

// `markSafe` is asserted BY THE CALLER, because only the call site knows why binding our marker
// to stage 0 cannot be observed by the game. Deriving it here would be a second classifier
// disagreeing with the first, which is the bug that turned real geometry into UI overlays in
// session 14. The three safe cases, each with its own reason:
//
//   * the sampler-less prepass  - the shader declares no sampler, so no stage can be read;
//   * the stipple prepass       - stage 0 holds the NORMAL MAP and the stipple pattern lives at
//                                 s11, so the dithered discard that shapes depth output is
//                                 untouched and only the G-buffer normal changes;
//   * the composite/post chain  - it samples the engine's own render targets, which is precisely
//                                 the output we are replacing.
//
// Light volumes, auxiliary cameras and mirrored passes are NOT safe: they sample real data at
// stage 0 and the game's own rendering would change. They stay pass-through and are counted.
Disp HiddenDisp(bool markSafe) {
    switch (g_settings.hiddenPassMode) {
        case 0: return Disp::PassThrough;
        case 1: return Disp::Hide;
        case 2: return Disp::Skip;
        default:
            if (g_marker && markSafe) return Disp::Mark;
            ++g_markRefused;
            return Disp::PassThrough;
    }
}



// parts/10_albedo_materials.cpp - albedo selection, lighting setup, constant colours, tiling, material probes.
// NOT a standalone translation unit: #included, in order, by ../sr3rtx.cpp inside its anonymous
// namespace. Originally sr3rtx.cpp lines 6815-7726 (old file:line citations map here).
// ---------------------------------------------------------------------------------------------
// Render-target albedo: SR3 bakes each character into one, and Remix cannot read it
//
// SR3 composites every character - skin, tattoos, clothing - into ONE 2048x1024 texture and
// binds that as the character's diffuse map. The player's 36 body/head draws all share it:
//
//     s0 Diffuse_MapSampler 3B3F5DD8 2048x1024 fmt=22 levels=9   <- bound as albedo
//     ALBEDO BOUND #5: ... 2048x1024 fmt=22 mips=9 pool=0 usage=0x200
//
// CORRECTED 2026-08-28: usage 0x200 is D3DUSAGE_DYNAMIC, NOT D3DUSAGE_RENDERTARGET (which is
// 0x1). The first version of this tested for RENDERTARGET, matched nothing, and its own
// falsifier said so - "no render-target albedo has been seen", 0 copies, 0 draws bound. The
// atlas is a DYNAMIC, CPU-written texture.
//
// D3DPOOL_DEFAULT + D3DUSAGE_DYNAMIC does not separate it either: EVERY albedo in this game is
// pool=0 usage=0x200, and the world is textured correctly. What separates it is the FORMAT.
// Every streamed art texture is DXT1/DXT5; the character atlas is the only UNCOMPRESSED one,
// because it is composited at runtime rather than loaded from disk:
//
//     1024x1024 DXT1  | 1024x512 DXT5 | 512x512 DXT5 | 256x128 DXT1 | 128x256 DXT5   <- art
//     2048x1024 fmt=22 (X8R8G8B8) mips=9                                             <- the atlas
//
// Remix hashes a texture from the content the game UPLOADS - the note above the cloth generator
// states it exactly: "UpdateTexture is also the upload Remix hashes". A render target is written
// by the GPU and never uploaded, so Remix has no pixels for it and the material it builds is an
// ALL-ZERO texture. Measured in Remix's own capture, taken by the user on 2026-08-28 19:13:
//
//     mat_E881A25E37E37B19   diffuse_texture 2048x1024   mean 0.0   100% near-black
//
// That is the reported "my player skin is black and has none of the tattoos", and the hash is
// the same one seen on 08-23 because an all-zero image always hashes the same.
//
// The fix is to hand Remix the same pixels through the path it DOES hash: read the render target
// back with GetRenderTargetData and re-upload it with UpdateTexture, then bind that copy.
//
// This was not the first theory, and the previous one is why the probe below reports a MEAN.
// `compositeToTexturePass` assumed the game never FILLED the atlas because we skipped the quads
// that composite it. It fired on exactly the population measured - 35.9 draws/frame against a
// prediction of 41 - and changed nothing, because the atlas is composited at SPAWN and appears in
// no steady-state frame. So the two causes must be told apart by measurement, not by argument:
//
//     mean > 0   the pixels exist on the GPU and only Remix was missing them - this fix is right
//     mean = 0   the game really never wrote the atlas, and the composite hunt resumes
//
// Cost control, because this project has already killed the game once by allocating per draw:
//   * ONE readback per distinct texture, cached by pointer, and at most one per FRAME - a
//     GetRenderTargetData of 8 MB is a full GPU sync and must never sit on a per-draw path;
//   * the source is AddRef'd, because a cache holding a D3D pointer without one is the exact
//     use-after-free this project fixed in 2026-08-18;
//   * a 64 MB ceiling with refusals counted, and every allocation in try/catch - a shim must
//     degrade, not abort its host.
struct RtAlbedoCopy {
    IDirect3DTexture9* copy = nullptr;   // what we bind instead of the render target
    bool isRT = false;                   // false = an ordinary texture, never look again
    bool settled = false;                // got real pixels; stop reading back
    unsigned attempts = 0;               // readbacks tried while it still came back blank
};
std::unordered_map<IDirect3DBaseTexture9*, RtAlbedoCopy> g_rtCopies;
unsigned g_rtCopyMatched = 0;     // distinct textures the rule claimed - the population
unsigned g_rtCopyStretchFailed = 0;  // the GPU refused to copy the source at all
unsigned g_rtCopyMade = 0;        // copies successfully built
unsigned g_rtCopyFailed = 0;      // readback or upload refused
unsigned g_rtCopyBlank = 0;       // readbacks that came back all zero
unsigned g_rtCopyBound = 0;       // draws/frame that bound a copy instead of the render target
unsigned g_rtCopyBudget = 0;      // copies refused on the memory ceiling
size_t   g_rtCopyBytes = 0;
double   g_rtCopyLastMean = -1.0; // mean of the most recent readback, the falsifier
UINT     g_rtCopyLastFrame = 0;
constexpr size_t kMaxRtCopyBytes = 64u << 20;

// Read one render-target texture back and re-upload it so Remix hashes real pixels.
// Returns the copy, or nullptr if it could not be made this frame.
IDirect3DTexture9* BuildRtAlbedoCopy(IDirect3DDevice9* dev, IDirect3DTexture9* src,
                                     const D3DSURFACE_DESC& d, RtAlbedoCopy& e) {
    IDirect3DSurface9* srcSurf = nullptr;
    IDirect3DSurface9* sysSurf = nullptr;
    IDirect3DTexture9* scratch = nullptr;
    IDirect3DSurface9* scratchSurf = nullptr;
    IDirect3DTexture9* staging = nullptr;
    IDirect3DTexture9* made = nullptr;
    const bool wasInternal = g_internal;
    g_internal = true;
    g_internalThread = GetCurrentThreadId();
    try {
        D3DLOCKED_RECT lr{};
        // THE READ GOES THROUGH THE GPU, and it must.
        //
        // The first version locked the source directly. Measured 2026-08-29 with charTexDump,
        // which decodes every character stage through that same LockRect(READONLY):
        //
        //     Pattern_Map 32x32 DXT1     mean  85.0   content
        //     Sphere_Map 128x128 DXT1    mean 202.7   content
        //     Dob_Map 512x512 DXT1       mean 132.2   content
        //     Diffuse_Map 2048x1024 fmt=22   mean 0.0   BLACK   <- the character atlas
        //     Normal_Map  1024x512 fmt=21    mean 0.0   BLACK   <- its normal map
        //
        // Every DXT texture in the same draws reads back correctly and both UNCOMPRESSED ones
        // read as zeros. A character's normal map is not empty - the game shades characters with
        // it - so this is a blind READ, not an empty texture: D3D9 leaves the contents of a
        // D3DPOOL_DEFAULT + D3DUSAGE_DYNAMIC surface UNDEFINED under a READONLY lock, because it
        // is write-only memory from the CPU side.
        //
        // StretchRect asks the GPU to copy it into a render target we own, and
        // GetRenderTargetData resolves THAT - a path with defined contents. If the pixels exist
        // on the GPU, this sees them; the previous path could not have, whatever was there.
        if (FAILED(src->GetSurfaceLevel(0, &srcSurf)) || !srcSurf) { ++g_rtCopyFailed; goto done; }
        if (FAILED(dev->CreateTexture(d.Width, d.Height, 1, D3DUSAGE_RENDERTARGET,
                                      D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &scratch, nullptr))
            || !scratch) { ++g_rtCopyFailed; goto done; }
        if (FAILED(scratch->GetSurfaceLevel(0, &scratchSurf)) || !scratchSurf) {
            ++g_rtCopyFailed; goto done;
        }
        if (FAILED(dev->StretchRect(srcSurf, nullptr, scratchSurf, nullptr, D3DTEXF_NONE))) {
            ++g_rtCopyStretchFailed; goto done;
        }
        if (FAILED(dev->CreateOffscreenPlainSurface(d.Width, d.Height, D3DFMT_A8R8G8B8,
                                                    D3DPOOL_SYSTEMMEM, &sysSurf, nullptr))
            || !sysSurf) { ++g_rtCopyFailed; goto done; }
        if (FAILED(dev->GetRenderTargetData(scratchSurf, sysSurf))) { ++g_rtCopyFailed; goto done; }
        if (FAILED(sysSurf->LockRect(&lr, nullptr, D3DLOCK_READONLY)) || !lr.pBits) {
            ++g_rtCopyFailed; goto done;
        }
        // Mean over a coarse grid - the falsifier that separates "Remix could not read it" from
        // "the game never wrote it". Sampled, not summed: this is 8 MB and runs on the render
        // thread.
        double acc = 0.0; unsigned n = 0;
        for (UINT y = 0; y < d.Height; y += 8) {
            const unsigned char* row = static_cast<const unsigned char*>(lr.pBits) +
                                       static_cast<size_t>(y) * lr.Pitch;
            for (UINT x = 0; x < d.Width; x += 8) {
                acc += row[x * 4 + 0] + row[x * 4 + 1] + row[x * 4 + 2];
                ++n;
            }
        }
        const double mean = n ? acc / (3.0 * n) : 0.0;
        g_rtCopyLastMean = mean;

        // Still blank: the game composites a character's atlas when it SPAWNS, so the first
        // sight of one can legitimately precede its content. Refuse to cache a black copy and
        // let a later frame try again - caching it would freeze the bug in place.
        if (mean <= 0.05) {
            sysSurf->UnlockRect();
            ++g_rtCopyBlank;
            goto done;
        }

        const size_t bytes = static_cast<size_t>(d.Width) * d.Height * 4;
        if (g_rtCopyBytes + bytes > kMaxRtCopyBytes) {
            sysSurf->UnlockRect();
            ++g_rtCopyBudget;
            e.settled = true;              // stop retrying; we will never fit
            goto done;
        }

        // Staged SYSTEMMEM -> DEFAULT through UpdateTexture, which is the upload Remix hashes.
        // A8R8G8B8 regardless of the source's X8R8G8B8: alpha is forced opaque so Remix cannot
        // read an undefined X channel as transparency.
        if (SUCCEEDED(dev->CreateTexture(d.Width, d.Height, 1, 0, D3DFMT_A8R8G8B8,
                                         D3DPOOL_SYSTEMMEM, &staging, nullptr)) && staging) {
            D3DLOCKED_RECT dst{};
            if (SUCCEEDED(staging->LockRect(0, &dst, nullptr, 0)) && dst.pBits) {
                for (UINT y = 0; y < d.Height; ++y) {
                    const unsigned char* srow = static_cast<const unsigned char*>(lr.pBits) +
                                                static_cast<size_t>(y) * lr.Pitch;
                    unsigned char* drow = static_cast<unsigned char*>(dst.pBits) +
                                          static_cast<size_t>(y) * dst.Pitch;
                    for (UINT x = 0; x < d.Width; ++x) {
                        drow[x * 4 + 0] = srow[x * 4 + 0];
                        drow[x * 4 + 1] = srow[x * 4 + 1];
                        drow[x * 4 + 2] = srow[x * 4 + 2];
                        drow[x * 4 + 3] = 255;
                    }
                }
                staging->UnlockRect(0);
            }
            if (SUCCEEDED(dev->CreateTexture(d.Width, d.Height, 1, 0, D3DFMT_A8R8G8B8,
                                             D3DPOOL_DEFAULT, &made, nullptr)) && made) {
                if (FAILED(dev->UpdateTexture(staging, made))) {
                    made->Release();
                    made = nullptr;
                    ++g_rtCopyFailed;
                } else {
                    g_rtCopyBytes += bytes;
                    ++g_rtCopyMade;
                }
            } else {
                ++g_rtCopyFailed;
            }
        } else {
            ++g_rtCopyFailed;
        }
        sysSurf->UnlockRect();
    } catch (...) {
        ++g_rtCopyFailed;
    }
done:
    if (staging) staging->Release();
    if (sysSurf) sysSurf->Release();
    if (scratchSurf) scratchSurf->Release();
    if (scratch) scratch->Release();
    if (srcSurf) srcSurf->Release();
    g_internal = wasInternal;
    return made;
}

IDirect3DBaseTexture9* RemixReadableAlbedo(IDirect3DDevice9* dev, IDirect3DBaseTexture9* albedo) {
    if (!g_settings.rtAlbedoCopy || !albedo || !dev) return albedo;

    // The snooped character atlas wins outright. Its pixels came from the game's own write, at
    // the CPU boundary where they demonstrably exist, and were re-uploaded through UpdateTexture
    // so Remix hashes real content instead of the all-zero image it builds today.
    if (g_settings.atlasSnoop) {
        if (IDirect3DTexture9* snooped = AtlasCopyFor(dev, albedo)) {
            ++g_atlasBound;
            return snooped;
        }
    }

    // Cached by POINTER, so the GetLevelDesc below costs one bridge round trip per distinct
    // texture and never one per draw - the player alone binds the same atlas 36 times a frame.
    auto it = g_rtCopies.find(albedo);
    if (it == g_rtCopies.end()) {
        RtAlbedoCopy e;
        if (albedo->GetType() == D3DRTYPE_TEXTURE) {
            D3DSURFACE_DESC d{};
            if (SUCCEEDED(static_cast<IDirect3DTexture9*>(albedo)->GetLevelDesc(0, &d))) {
                // UNCOMPRESSED + DYNAMIC + DEFAULT, and big enough to be an atlas. That is the
                // runtime-composited character texture and, measured across every albedo this
                // game binds, nothing else: the art is all DXT1/DXT5, and our own generated
                // cloth textures carry usage 0 and one mip. A rule that matched every
                // DEFAULT+DYNAMIC texture would try to copy the entire streamed world.
                const bool uncompressed = (d.Format == D3DFMT_X8R8G8B8 ||
                                           d.Format == D3DFMT_A8R8G8B8);
                e.isRT = d.Pool == D3DPOOL_DEFAULT && uncompressed &&
                         (d.Usage & (D3DUSAGE_DYNAMIC | D3DUSAGE_RENDERTARGET)) != 0 &&
                         d.Width >= 256 && d.Height >= 256;
                if (e.isRT) ++g_rtCopyMatched;
            }
        }
        // AddRef only what we intend to keep. A cache holding a D3D pointer without one is the
        // use-after-free this project fixed on 2026-08-18.
        // Tracking and hooking now happen at CreateTexture, which is before anything can fill
        // a texture. This only pins the albedo against pointer reuse.
        if (e.isRT) albedo->AddRef();
        it = g_rtCopies.emplace(albedo, e).first;
    }
    RtAlbedoCopy& e = it->second;
    if (e.isRT) SelfTestCopyPath(dev);   // once, and only when there is something to test
    if (!e.isRT) return albedo;
    if (e.copy) { ++g_rtCopyBound; return e.copy; }
    if (e.settled) return albedo;

    // At most ONE readback per frame across all textures: GetRenderTargetData is a full GPU
    // sync and several of them in one frame is a stall, not a copy.
    if (g_rtCopyLastFrame == g_frames) return albedo;
    if (e.attempts >= static_cast<unsigned>(g_settings.rtAlbedoRetries)) { e.settled = true; return albedo; }
    g_rtCopyLastFrame = g_frames;
    ++e.attempts;

    D3DSURFACE_DESC d{};
    if (FAILED(static_cast<IDirect3DTexture9*>(albedo)->GetLevelDesc(0, &d))) {
        e.settled = true;
        return albedo;
    }
    if (IDirect3DTexture9* made = BuildRtAlbedoCopy(dev, static_cast<IDirect3DTexture9*>(albedo), d, e)) {
        e.copy = made;
        e.settled = true;
        ++g_rtCopyBound;
        return made;
    }
    return albedo;
}



IDirect3DBaseTexture9* EffectiveAlbedo() {
    IDirect3DBaseTexture9* albedo = nullptr;
    int stage = -1;   // which g_curTexture[] slot `albedo` came from; -1 until one is chosen

    // First choice: the sampler the shader itself names as a colour map. This is SR3-specific
    // and stronger than anything geometry can tell us - the CTAB says which map is which.
    if (g_settings.rankAlbedo && g_curPS.albedoRank > 0 && g_curPS.albedoStage >= 0 &&
        g_curPS.albedoStage < 8) {
        albedo = g_curTexture[g_curPS.albedoStage];
        stage = g_curPS.albedoStage;
        if (albedo && g_curPS.albedoStage != 0) ++g_albedoMoved;
        // The shader NAMES a colour map but nothing is bound where it says. This renders white
        // with no counter anywhere - the "untextured material" report above only covers rank 0,
        // so a surface failing here has been invisible to every number in the log. Named once
        // per distinct sampler, like the rank-0 case, because the name is what identifies which
        // material family is losing its texture.
        if (!albedo) {
            ++g_albedoNullRanked;
            if (g_nullRankedReports < 12 && g_curPS.firstSampler[0]) {
                bool seen = false;
                for (unsigned i = 0; i < g_nullRankedReports; ++i)
                    if (!strcmp(g_nullRankedNames[i], g_curPS.firstSampler)) { seen = true; break; }
                if (!seen) {
                    strncpy_s(g_nullRankedNames[g_nullRankedReports], g_curPS.firstSampler, 27);
                    Log("albedo named but NOT BOUND #%u: sampler '%s' rank=%d expected stage %d "
                        "(stages bound: %d%d%d%d%d%d%d%d)",
                        g_nullRankedReports + 1, g_curPS.firstSampler, g_curPS.albedoRank,
                        g_curPS.albedoStage,
                        g_curTexture[0] ? 1 : 0, g_curTexture[1] ? 1 : 0, g_curTexture[2] ? 1 : 0,
                        g_curTexture[3] ? 1 : 0, g_curTexture[4] ? 1 : 0, g_curTexture[5] ? 1 : 0,
                        g_curTexture[6] ? 1 : 0, g_curTexture[7] ? 1 : 0);
                    ++g_nullRankedReports;
                }
            }
        }
    } else if (g_curPS.albedoRank == 0 && g_curPS.isPixelShader) {
        // Record what these shaders actually sample, once per distinct first-sampler name.
        if (g_blankReports < 16 && g_curPS.firstSampler[0]) {
            bool seen = false;
            for (unsigned i = 0; i < g_blankReports; ++i)
                if (!strcmp(g_blankNames[i], g_curPS.firstSampler)) { seen = true; break; }
            if (!seen) {
                strncpy_s(g_blankNames[g_blankReports], g_curPS.firstSampler, 27);
                Log("untextured material #%u: first sampler is '%s'", g_blankReports + 1,
                    g_curPS.firstSampler);
                ++g_blankReports;
            }
        }
        // The shader binds no colour map at all. Whatever sits in stage 0 is a normal or
        // specular map, so show nothing rather than show that - untextured white is wrong, but
        // it is honestly wrong, and it will not masquerade as surface detail.
        ++g_albedoBlanked;
        g_lastEffectiveAlbedo = nullptr;
        g_lastEffectiveAlbedoStage = -1;
        return nullptr;
    } else {
        // No usable shader reflection: stage 0 is taken raw, whatever it happens to hold. On a
        // material whose stage 0 is a normal map that binds a tangent-space normal map AS base
        // colour, which renders as the wrong colour with surface detail visibly present - the
        // "shoes have normals but they are the wrong colour" report. Counted because until now
        // nothing distinguished this path from a legitimate stage-0 albedo.
        albedo = g_curTexture[0];
        stage = 0;
        if (!g_curPS.isPixelShader) ++g_albedoStage0Raw;
    }

    // A render-target texture is generated data - the light buffer, the HDR scene, a shadow
    // map - never surface colour. Binding one as albedo is what makes surfaces flicker through
    // unrelated images as the frame's targets are rewritten.
    if (g_settings.excludeRTAlbedo && albedo && g_rtTextures.count(albedo)) {
        IDirect3DBaseTexture9* best = nullptr;
        UINT bestArea = 0;
        int bestStage = -1;
        for (int s = 0; s < 8; ++s) {
            IDirect3DBaseTexture9* t = g_curTexture[s];
            if (!t || g_rtTextures.count(t)) continue;
            UINT area = 0;
            if (t->GetType() == D3DRTYPE_TEXTURE) {
                D3DSURFACE_DESC d{};
                if (SUCCEEDED(static_cast<IDirect3DTexture9*>(t)->GetLevelDesc(0, &d)))
                    area = d.Width * d.Height;
            }
            // Largest, not first: the first non-RT bound is often a tiny detail or ramp map,
            // and a featureless albedo makes Remix render the surface as a near-mirror.
            if (!best || area > bestArea) { best = t; bestArea = area; bestStage = s; }
        }
        // Honest, but not free: a null here renders white just the same, and until now nothing
        // counted it. If this number is large the RT exclusion is removing real surfaces rather
        // than protecting them.
        if (!best) ++g_albedoNullAfterRT;
        albedo = best;   // may be null, which is the honest answer
        stage = bestStage;
    }
    g_lastEffectiveAlbedo = albedo;
    g_lastEffectiveAlbedoStage = albedo ? stage : -1;
    return albedo;
}

void SetupLighting(IDirect3DDevice9* dev) {
    // FFP lighting is switched off deliberately. Remix path-traces from the geometry and the
    // lights we inject separately; letting the fixed-function pipeline also shade the surface
    // would bake a second, wrong lighting term into the albedo Remix reads.
    ShadowSetRS(dev, D3DRS_LIGHTING, FALSE);
    ShadowSetRS(dev, D3DRS_AMBIENT, D3DCOLOR_XRGB(255, 255, 255));
    ShadowSetRS(dev, D3DRS_COLORVERTEX, FALSE);
    ShadowSetRS(dev, D3DRS_SPECULARENABLE, FALSE);
    // TEXTUREFACTOR is NOT set here any more. SetupTextureStages owns it: it holds either the
    // opaque alpha for a normal draw or the base colour of a constant-coloured material, and it
    // is written per converted draw. This function runs once a frame, AFTER SetupTextureStages,
    // so leaving the write here made the first converted draw of every frame lose its constant
    // colour and render white. One register, one owner.

    D3DMATERIAL9 mat{};
    mat.Diffuse = {1.0f, 1.0f, 1.0f, 1.0f};
    mat.Ambient = {1.0f, 1.0f, 1.0f, 1.0f};
    if (g_origSetMaterial) g_origSetMaterial(dev, &mat);
}

// The base colour of a no-diffuse-map material, read out of the pixel shader's constants.
//
// Returns false when this shader has no colour constant, in which case stage 0 stays unbound and
// the surface renders white as before - 4 of the 121 DSF materials are in that position.
//
// The constant is linear float RGBA in the shader; D3DCOLOR is 8-bit. Values above 1.0 are real
// (these are HDR tints) and are clamped rather than scaled, because Remix reads TFACTOR as an
// ordinary base colour and an over-bright albedo would make the surface glow.
bool ConstantAlbedo(D3DCOLOR& out) {
    const int reg = g_curPS.colourConstReg;
    if (!g_curPS.isPixelShader || reg < 0 || reg >= static_cast<int>(kMaxPsConst)) return false;
    const float* c = g_psConst[reg];
    // These constants are LINEAR, and TEXTUREFACTOR is an 8-bit colour Remix reads as sRGB - so
    // writing them raw applies gamma a second time on the way in. Measured 2026-08-20 on vehicle
    // paint: Base_Paint_Color = (0.041, 0.008, 0.006), which raw becomes byte (10, 2, 2), and
    // reading THAT as sRGB gives about 0.0012 linear - roughly thirty times too dark. "Cars are
    // black" is that arithmetic, not a missing texture.
    //
    // Encoded, the same constant becomes (58, 29, 26): a dark red car. This is the identical
    // mistake the clothing generator made in run 51, in the one other place this shim hands Remix
    // a colour it computed itself.
    // A constant at or above 1.0 in every channel carries no colour, only scale - and `byte`
    // clamps it to 255, so the material renders PURE WHITE while being counted as
    // "constant-colour". Measured 2026-08-20: Tint_color reads (5.0, 5.0, 5.0) on character
    // materials, so any material whose best colour constant is Tint_color was being painted
    // white and reported as though it had been rescued.
    //
    // CORRECTED 2026-08-28. That measurement was written down as "every character material in
    // the game"; it was four draws in one frame, because CHAR CONST reports once per distinct
    // first-sampler name and stops at 12. The shim's own log disproves the "everywhere": the
    // vehicle materials in the same report read Tint_color = (0.0, 0.0, 0.0). The rejection
    // below is still right, and for a better reason than its value - Tint_color is applied
    // AFTER the fog lerp, at the very end of the shader (1,681 of 1,944 declaring entries use
    // it in the final instruction, and 945 uses are `rcp`, dividing BY it to undo it). It is a
    // terminal exposure scale, never an albedo. The per-material albedo tint is a DIFFERENT
    // constant - Diffuse_Color, which scales the diffuse sample in 215 entries - and this shim
    // drops it whenever a map is bound. See ProbeDiffuseColor.
    //
    // Rejected rather than clamped, so these fall to the honest blank count instead of
    // masquerading. World materials whose Tint_color IS a real colour are unaffected: they only
    // reach here when at least one channel is below 1.
    if (c[0] >= 1.0f && c[1] >= 1.0f && c[2] >= 1.0f) { ++g_constantAlbedoRejected; return false; }
    auto byte = [](float v) -> DWORD {
        if (v <= 0.0f) return 0;
        if (v >= 1.0f) return 255;
        return static_cast<DWORD>(powf(v, 1.0f / 2.2f) * 255.0f + 0.5f);
    };
    // Alpha deliberately forced opaque. Several of these carry Opacity_fade in the alpha channel
    // and Remix would read a sub-1.0 alpha as translucency - the same trap that turned every wall
    // to X-ray when texture alpha was taken unconditionally.
    out = D3DCOLOR_ARGB(255, byte(c[0]), byte(c[1]), byte(c[2]));
    return true;
}

// Which of the vertex shader's tiling pairs applies to the texture being bound as albedo?
//
// The pair is named after its map - Diffuse_Map_TilingU, Decal_Map_TilingV, Normal_Map_TilingU -
// and the albedo sampler is named after the same map, so the two are matched by name. A shader
// carrying only a Normal_Map pair has no tiling for its diffuse map, and the answer is then
// NO TILING rather than the nearest available pair; scaling a texture by a factor belonging to a
// different map is what made roads too dense.
unsigned g_tilingMatched = 0, g_tilingUnmatched = 0;

// Memoised on the (vertex shader, pixel shader) pair: the answer depends only on
// g_curVS.tiling/tilingCount and g_curPS.albedoSampler, both fixed properties of whichever
// shaders are bound (set once by ReflectShader, never touched per draw), and g_lastVS/g_lastPS
// are exactly what selected g_curVS/g_curPS in the first place (Hook_SetVertexShader and
// Hook_SetPixelShader set the pair together). So the _stricmp/_strnicmp chain below computed
// the identical answer every time two shaders already seen together were rebound - measured at
// part of the ~0.09 ms/frame this audit is recovering. Safe to key on the raw pointers with no
// pinning of its own: a shader only reaches g_lastVS/g_lastPS after Hook_CreateVertexShader or
// Hook_CreatePixelShader has already inserted it into g_shaders, which AddRef's it and never
// erases the entry - so by the time this cache could see a (vs, ps) pair, both pointers are
// already pinned for the process lifetime, exactly like g_shaders' own keys.
struct TilingForAlbedoKey {
    IDirect3DVertexShader9* vs;
    IDirect3DPixelShader9* ps;
    bool operator==(const TilingForAlbedoKey& o) const { return vs == o.vs && ps == o.ps; }
};
struct TilingForAlbedoKeyHash {
    size_t operator()(const TilingForAlbedoKey& k) const {
        size_t h = reinterpret_cast<size_t>(k.vs) * 0x9E3779B1u;
        h ^= reinterpret_cast<size_t>(k.ps) * 0xC2B2AE35u + 0x7F4A7C15u + (h << 6) + (h >> 2);
        return h;
    }
};
// matched mirrors the old "best != nullptr" test exactly - u/v can each still be -1 inside a
// matched pair (a map with only a TilingU constant and no TilingV), so matched is not simply
// "u >= 0 || v >= 0" and has to be stored in its own right.
struct TilingForAlbedoResult { int u, v; bool matched; };
std::unordered_map<TilingForAlbedoKey, TilingForAlbedoResult, TilingForAlbedoKeyHash> g_tilingForAlbedoCache;

void TilingForAlbedo(int& uReg, int& vReg) {
    uReg = vReg = -1;
    if (g_curVS.tilingCount == 0 || !g_curPS.albedoSampler[0]) return;

    const TilingForAlbedoKey key{g_lastVS, g_lastPS};
    auto it = g_tilingForAlbedoCache.find(key);
    if (it == g_tilingForAlbedoCache.end()) {
        // "Diffuse_MapSampler" names the map "Diffuse_Map".
        char base[28] = {};
        strncpy_s(base, g_curPS.albedoSampler, sizeof(base) - 1);
        const size_t len = strlen(base);
        if (len > 7 && !_stricmp(base + len - 7, "Sampler")) base[len - 7] = 0;

        const ShaderInfo::TilingPair* best = nullptr;
        for (int t = 0; t < g_curVS.tilingCount; ++t) {
            const ShaderInfo::TilingPair& p = g_curVS.tiling[t];
            if (!p.base[0]) continue;
            if (!_stricmp(p.base, base)) { best = &p; break; }   // exact: Diffuse_Map == Diffuse_Map
            // A prefix also belongs to this map: "Diffuse_TilingU" against "Diffuse_MapSampler".
            const size_t pl = strlen(p.base);
            if (pl && !_strnicmp(base, p.base, pl) && !best) best = &p;
        }
        TilingForAlbedoResult r{-1, -1, best != nullptr};
        if (best) { r.u = best->uReg; r.v = best->vReg; }
        try { it = g_tilingForAlbedoCache.emplace(key, r).first; }
        catch (...) {
            // Allocation failure: behave exactly as an uncached call would, just do not cache it.
            if (best) { ++g_tilingMatched; uReg = best->uReg; vReg = best->vReg; } else ++g_tilingUnmatched;
            return;
        }
    }
    const TilingForAlbedoResult& r = it->second;
    if (r.matched) ++g_tilingMatched; else ++g_tilingUnmatched;
    uReg = r.u;
    vReg = r.v;
}

// Everything the clothing texture generator needs to know before it can be written, and two
// things that decide whether it can be written at all.
//
//   1. Can the Pattern_Map be READ? SR3's textures are D3DPOOL_DEFAULT (pool=0 in the ALBEDO
//      BOUND probe), and D3D9 does not promise a lock on those. The bind-pose decode read a
//      DEFAULT vertex buffer successfully under DXVK, so it is worth asking rather than
//      assuming - but a texture is not a vertex buffer and the answer may differ. If the lock
//      fails, the source has to come from snooping UpdateTexture at upload time instead.
//   2. How many DISTINCT colour combinations appear? The generator caches one texture per
//      (pattern, a, b, c, tint), and that cache is only affordable if the count is small. A
//      handful per character is fine; hundreds is not.
//
// Neither can be answered by reading the shaders, and building a DXT decoder before knowing
// them would be building on a guess.
const char* DeclTypeName(int t);   // defined with the declaration parsing, further down

unsigned g_clothProbeReports = 0;
unsigned g_clothDraws = 0;
unsigned g_clothLockOk = 0, g_clothLockFail = 0;
unsigned long long g_clothCombos[64]{};
unsigned g_clothComboCount = 0;

void ProbeClothMaterial(IDirect3DDevice9* dev) {
    if (!g_settings.clothProbe) return;
    const ShaderInfo& ps = g_curPS;
    if (ps.patternStage < 0 || ps.patternStage >= 8 || ps.tintColorReg < 0) return;
    if (ps.diffuseColorReg[0] < 0 || ps.diffuseColorReg[1] < 0 || ps.diffuseColorReg[2] < 0)
        return;
    ++g_clothDraws;

    float col[4][4]{};
    for (int i = 0; i < 3; ++i) {
        const int r = ps.diffuseColorReg[i];
        if (r >= 0 && r < static_cast<int>(kMaxPsConst)) memcpy(col[i], g_psConst[r], 16);
    }
    if (ps.tintColorReg < static_cast<int>(kMaxPsConst))
        memcpy(col[3], g_psConst[ps.tintColorReg], 16);

    // Quantised to 8 bits a channel, which is the precision the generated texture would hold
    // anyway - two combinations closer than that would produce the same image.
    unsigned long long key = 1469598103934665603ull;
    for (int c = 0; c < 4; ++c)
        for (int k = 0; k < 4; ++k) {
            float v = col[c][k];
            if (!std::isfinite(v)) v = 0.0f;
            const unsigned q = static_cast<unsigned>(max(0.0f, min(1.0f, v)) * 255.0f + 0.5f);
            key ^= q; key *= 1099511628211ull;
        }
    IDirect3DBaseTexture9* pattern = g_curTexture[ps.patternStage];
    key ^= reinterpret_cast<uintptr_t>(pattern);
    key *= 1099511628211ull;

    bool seen = false;
    for (unsigned i = 0; i < g_clothComboCount; ++i)
        if (g_clothCombos[i] == key) { seen = true; break; }
    if (!seen && g_clothComboCount < 64) g_clothCombos[g_clothComboCount++] = key;
    if (seen || g_clothProbeReports >= 8) return;
    ++g_clothProbeReports;

    Log("CLOTH #%u: ps='%s' patternStage=%d | a(%.3f %.3f %.3f) b(%.3f %.3f %.3f) "
        "c(%.3f %.3f %.3f) tint(%.3f %.3f %.3f)",
        g_clothProbeReports, ps.firstSampler, ps.patternStage,
        col[0][0], col[0][1], col[0][2], col[1][0], col[1][1], col[1][2],
        col[2][0], col[2][1], col[2][2], col[3][0], col[3][1], col[3][2]);

    // The PLAYER's clothing shaders are a different recipe, and it is the one that matters most
    // to the stated goal. From ir_at_sr3pccloth_c.fxo_pc shader [8]:
    //
    //     layer  = lerp(lerp(lerp(1, Diffuse_Color_c^2.2, p.b), Diffuse_Color_b^2.2, p.g),
    //                                                           Diffuse_Color_a^2.2, p.r)
    //     albedo = Diffuse_Map * Diffuse_Color * layer
    //
    // A layered mask rather than the NPC family's weighted sum, and it multiplies a full
    // resolution Diffuse_Map - but the pattern is sampled from a SECOND texture coordinate set
    // (v1, with ClampU1/ClampV1), so the two are not in the same UV space. Folding them into one
    // texture is only possible if TEXCOORD1 is a fixed transform of TEXCOORD0; if it is an
    // independent mapping, it is not, and the colours have to reach Remix another way.
    //
    // That is a question about the vertex data, not the shaders, so it is measured here.
    if (ps.albedoStage != ps.patternStage && g_curDecl) {
        D3DVERTEXELEMENT9 elems[64]{};
        UINT n = 0;
        int uv0 = -1, uv1 = -1;
        if (SUCCEEDED(g_curDecl->GetDeclaration(elems, &n))) {
            for (UINT i = 0; i < n; ++i) {
                if (elems[i].Stream == 0xFF) break;
                if (elems[i].Usage != D3DDECLUSAGE_TEXCOORD) continue;
                Log("    PLAYER cloth texcoord set %u: stream=%u offset=%u type=%s",
                    elems[i].UsageIndex, elems[i].Stream, elems[i].Offset,
                    DeclTypeName(elems[i].Type));
                if (elems[i].Stream == 0 && elems[i].Type == D3DDECLTYPE_SHORT2) {
                    if (elems[i].UsageIndex == 0) uv0 = elems[i].Offset;
                    else if (elems[i].UsageIndex == 1) uv1 = elems[i].Offset;
                }
            }
        }

        // CAN the player's two coordinate sets be reconciled?
        //
        // The player recipe is  albedo = Diffuse_Map * Diffuse_Color * layer(pattern),  with the
        // diffuse on TEXCOORD0 and the pattern on TEXCOORD1. Folding them into one generated
        // texture requires expressing the pattern's coordinates in the diffuse's space. If UV1 is
        // a fixed affine transform of UV0 - the same unwrap at a different scale and offset -
        // that is a per-mesh constant and the fold is possible. If the two are independent
        // unwraps, it is not, and the colours have to reach Remix some other way.
        //
        // Fitted from the first and last sampled vertex and CHECKED against the ones between, so
        // a coincidence in two points cannot pass for a relationship.
        if (uv0 >= 0 && uv1 >= 0 && g_stream0 && g_stream0Stride >= 36 && g_curDrawVertexCount >= 8) {
            D3DVERTEXBUFFER_DESC vbd{};
            void* mapped = nullptr;
            if (SUCCEEDED(g_stream0->GetDesc(&vbd)) && !(vbd.Usage & D3DUSAGE_DYNAMIC) &&
                VertexRangeFits(g_stream0, g_stream0Offset, g_curDrawFirstVertex,
                                g_curDrawVertexCount, g_stream0Stride) &&
                SUCCEEDED(g_stream0->Lock(
                    g_stream0Offset + g_curDrawFirstVertex * g_stream0Stride,
                    g_curDrawVertexCount * g_stream0Stride, &mapped,
                    D3DLOCK_READONLY | D3DLOCK_NOSYSLOCK)) && mapped) {
                const unsigned char* base = static_cast<const unsigned char*>(mapped);
                const UINT step = g_curDrawVertexCount / 8;
                auto uvAt = [&](UINT i, int off, int c) -> float {
                    return static_cast<float>(
                        reinterpret_cast<const short*>(base + i * g_stream0Stride + off)[c]);
                };
                const UINT ia = 0, ib = step * 7;
                bool affine = true;
                float su = 0.0f, ou = 0.0f, sv = 0.0f, ov = 0.0f;
                const float d0u = uvAt(ib, uv0, 0) - uvAt(ia, uv0, 0);
                const float d0v = uvAt(ib, uv0, 1) - uvAt(ia, uv0, 1);
                if (fabsf(d0u) < 1.0f || fabsf(d0v) < 1.0f) {
                    affine = false;      // degenerate span, cannot fit
                } else {
                    su = (uvAt(ib, uv1, 0) - uvAt(ia, uv1, 0)) / d0u;
                    ou = uvAt(ia, uv1, 0) - su * uvAt(ia, uv0, 0);
                    sv = (uvAt(ib, uv1, 1) - uvAt(ia, uv1, 1)) / d0v;
                    ov = uvAt(ia, uv1, 1) - sv * uvAt(ia, uv0, 1);
                    for (UINT k = 1; k < 7 && affine; ++k) {
                        const UINT i = step * k;
                        if (fabsf(su * uvAt(i, uv0, 0) + ou - uvAt(i, uv1, 0)) > 2.0f) affine = false;
                        if (fabsf(sv * uvAt(i, uv0, 1) + ov - uvAt(i, uv1, 1)) > 2.0f) affine = false;
                    }
                }
                // "could not fit" is NOT "independent". The degenerate-span branch above
                // gives up without establishing anything, and this line then reported that as
                // INDEPENDENT UNWRAPS - a positive claim that folding is impossible. Same class
                // of error as the slot-overlap warning: a diagnostic asserting a conclusion it
                // never reached. A CONSTANT uv1 is the most foldable case there is - one pattern
                // texel for the entire garment - and it was being reported as the least.
                const bool fitFailed = (fabsf(d0u) < 1.0f || fabsf(d0v) < 1.0f);
                Log("    PLAYER cloth UV1 vs UV0: %s",
                    fitFailed ? "could not fit - the two sampled vertices share a uv0, so this "
                                "says nothing either way"
                              : (affine ? "AFFINE - the fold into one texture is possible"
                                        : "INDEPENDENT unwraps - genuinely different unwraps"));
                if (affine)
                    Log("        UV1 = UV0 * (%.4f, %.4f) + (%.1f, %.1f)", su, sv, ou, ov);
                for (UINT k = 0; k < 4; ++k) {
                    const UINT i = step * k;
                    Log("        vert %u: uv0(%.0f %.0f)  uv1(%.0f %.0f)", i,
                        uvAt(i, uv0, 0), uvAt(i, uv0, 1), uvAt(i, uv1, 0), uvAt(i, uv1, 1));
                }
                g_stream0->Unlock();
            }
        }
    }

    if (!pattern || pattern->GetType() != D3DRTYPE_TEXTURE) {
        Log("    no 2D pattern texture bound at that stage");
        return;
    }
    IDirect3DTexture9* tex = static_cast<IDirect3DTexture9*>(pattern);
    D3DSURFACE_DESC d{};
    if (FAILED(tex->GetLevelDesc(0, &d))) { Log("    GetLevelDesc failed"); return; }

    // The whole question: does a read-lock work on this pool?
    D3DLOCKED_RECT lr{};
    const HRESULT hr = tex->LockRect(0, &lr, nullptr, D3DLOCK_READONLY);
    if (SUCCEEDED(hr) && lr.pBits) {
        ++g_clothLockOk;
        const unsigned char* b = static_cast<const unsigned char*>(lr.pBits);
        Log("    pattern %ux%u fmt=%d levels=%u pool=%d usage=0x%lX pitch=%d | "
            "LOCK OK, first 8 bytes %02X %02X %02X %02X %02X %02X %02X %02X",
            d.Width, d.Height, static_cast<int>(d.Format), tex->GetLevelCount(),
            static_cast<int>(d.Pool), static_cast<unsigned long>(d.Usage), lr.Pitch,
            b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
        tex->UnlockRect(0);
    } else {
        ++g_clothLockFail;
        Log("    pattern %ux%u fmt=%d levels=%u pool=%d usage=0x%lX | LOCK FAILED 0x%08lX - the "
            "source must come from snooping UpdateTexture instead",
            d.Width, d.Height, static_cast<int>(d.Format), tex->GetLevelCount(),
            static_cast<int>(d.Pool), static_cast<unsigned long>(d.Usage), hr);
    }
}

// Hair renders WHITE and skin renders with the wrong tint, and the shader constants say why:
// ir_sr3pchair_c carries Tint_color and nothing else that could colour it, ending
// `mul_pp oC0, r6, c37`; ir_sr3npcskinfull_c carries Diffuse_Color. Both multiply their diffuse
// map by a constant, and this shim binds the raw map and discards the constant - so a greyscale
// hair mask stays greyscale and skin keeps whatever tone the map happens to hold.
//
// Fixed function CAN express that: MODULATE the texture against TFACTOR. What it cannot express is
// a multiplier ABOVE one, and Tint_color measured (5.0, 5.0, 5.0) on the clothing draws - an
// exposure factor, not a colour. Whether hair and skin see the same 5.0 or a real colour decides
// whether MODULATE is the answer or a trap, and it is a runtime value that no shader can be read
// for. tintFallbackAlbedo already darkened every garment by guessing at exactly this.
unsigned g_charConstReports = 0;
char g_charConstSeen[12][28]{};

void ProbeCharacterConstants() {
    if (!g_settings.clothProbe || !g_curLayout.skinned || g_charConstReports >= 12) return;
    const ShaderInfo& ps = g_curPS;
    if (!ps.isPixelShader || !ps.firstSampler[0]) return;
    if (ps.colourConstReg < 0 && ps.tintColorReg < 0) return;
    for (unsigned i = 0; i < g_charConstReports; ++i)
        if (!strcmp(g_charConstSeen[i], ps.firstSampler)) return;
    strncpy_s(g_charConstSeen[g_charConstReports], ps.firstSampler, 27);
    ++g_charConstReports;

    float cc[4]{}, tc[4]{};
    if (ps.colourConstReg >= 0 && ps.colourConstReg < static_cast<int>(kMaxPsConst))
        memcpy(cc, g_psConst[ps.colourConstReg], 16);
    if (ps.tintColorReg >= 0 && ps.tintColorReg < static_cast<int>(kMaxPsConst))
        memcpy(tc, g_psConst[ps.tintColorReg], 16);
    Log("CHAR CONST #%u: first='%s' albedoRank=%d | %s(c%d) = (%.3f %.3f %.3f %.3f) | "
        "Tint_color(c%d) = (%.3f %.3f %.3f %.3f)",
        g_charConstReports, ps.firstSampler, ps.albedoRank,
        ps.colourConstName[0] ? ps.colourConstName : "(none)", ps.colourConstReg,
        cc[0], cc[1], cc[2], cc[3], ps.tintColorReg, tc[0], tc[1], tc[2], tc[3]);
    for (int i = 0; i < 2; ++i) {
        const int r = ps.hairColorReg[i];
        if (r < 0 || r >= static_cast<int>(kMaxPsConst)) continue;
        const float* h = g_psConst[r];
        Log("        Hair_Spec_Color%d(c%d) = (%.3f %.3f %.3f %.3f)", i + 1, r,
            h[0], h[1], h[2], h[3]);
    }
}

// Hair renders white and skin has the wrong tint, and the constants have now ruled themselves
// out: every character colour constant measured (1.0, 1.0, 1.0) and Tint_color measured
// (5.0, 5.0, 5.0) everywhere - an exposure factor carrying no colour at all. So the colour is in a
// TEXTURE, and the question is which one, because this shim is picking Diffuse_Map by rank and
// getting something greyscale.
//
// ir_sr3pchair_c samples Dob_Map (s0) and Diffuse_Map (s1); ir_sr3npcskinfull_c samples Blend_Map,
// Diffuse_Map, Normal_Map and two Sphere_Maps. Which of those actually holds the colour is a
// runtime fact about what the game binds, not something the sampler names settle.
unsigned g_charTexReports = 0;
const void* g_charTexSeen[8]{};

void ProbeCharacterTextures() {
    if (!g_settings.clothProbe || !g_curLayout.skinned || g_charTexReports >= 8) return;
    if (!g_curPS.isPixelShader || !g_lastPS) return;
    for (unsigned i = 0; i < g_charTexReports; ++i)
        if (g_charTexSeen[i] == g_lastPS) return;
    g_charTexSeen[g_charTexReports++] = g_lastPS;

    Log("CHAR TEX #%u: first='%s' albedoStage=%d rank=%d", g_charTexReports,
        g_curPS.firstSampler, g_curPS.albedoStage, g_curPS.albedoRank);
    for (int st = 0; st < 8; ++st) {
        IDirect3DBaseTexture9* t = g_curTexture[st];
        if (!t && !g_curPS.samplerName[st][0]) continue;
        UINT w = 0, h = 0;
        int fmt = 0, levels = 0;
        if (t && t->GetType() == D3DRTYPE_TEXTURE) {
            D3DSURFACE_DESC sd{};
            if (SUCCEEDED(static_cast<IDirect3DTexture9*>(t)->GetLevelDesc(0, &sd))) {
                w = sd.Width; h = sd.Height; fmt = static_cast<int>(sd.Format);
                levels = static_cast<int>(static_cast<IDirect3DTexture9*>(t)->GetLevelCount());
            }
        }
        Log("    s%d %-20s %p %ux%u fmt=%d levels=%d%s", st,
            g_curPS.samplerName[st][0] ? g_curPS.samplerName[st] : "(unnamed)",
            static_cast<void*>(t), w, h, fmt, levels,
            st == g_curPS.albedoStage ? "   <- bound as albedo" : "");

        // Which texture actually holds hair and skin colour is a question about pixels, and the
        // sampler names have already been shown not to settle it - Dob_Map and Diffuse_Map are
        // both plausible from their names alone. Dumping the stages answers it by looking.
        //
        // EVERY stage, named or not, and through the DDS writer rather than DecodeTextureRGB.
        //
        // Two things the previous version could not have found. It skipped UNNAMED stages, and
        // the player's hair binds five textures of which three are unnamed - two of them 512x512,
        // the largest maps in the material and the obvious place for strand detail. And it
        // decoded to RGB, discarding ALPHA, which is where a hair card's strand cut-out lives in
        // most engines; "the hair has strands" cannot be answered from RGB alone.
        //
        // The DDS path copies compressed data through untouched, so what lands on disk is what
        // the game gave the GPU - no decode of ours between the evidence and the eye.
        if (g_settings.charTexDump && t) {
            const char* dumped = nullptr;
            if (DumpTextureDds(t, &dumped) && dumped)
                Log("      s%d %-20s -> %s", st,
                    g_curPS.samplerName[st][0] ? g_curPS.samplerName[st] : "(unnamed)", dumped);
        }
    }
}


// ---------------------------------------------------------------------------------------------
// Diffuse_Color: the albedo multiply this shim drops
//
// SR3's material pass samples its colour map and immediately scales it by a per-material
// constant. `ir_sr3npcskinfull_mc` shader[8] - the character material pass:
//
//     texld_pp r2, v4, s0        ; Diffuse_Map                        <- the albedo
//     mul_pp   r2, r2, c0        ; * Diffuse_Color                    <- dropped by this shim
//     ...
//     lrp_pp   r2.xyz, v3.w, c38, r0    ; fog
//     mul_pp   oC0, r2, c37             ; * Tint_color                <- AFTER fog
//
// SetupTextureStages binds the map with SELECTARG1/TEXTURE whenever one exists, so the
// `mul r2, r2, c0` never happens for a converted draw.
//
// Swept 2026-08-28 over all 846 disassembled files, because a rule derived from one shader is
// what has cost this project its last three regressions:
//
//   - Diffuse_Color scales a diffuse/blend/pattern sample in 215 pixel-shader entries;
//     Base_Color in 12; Tint_color in 1 of the 1,944 entries that declare it.
//   - Tint_color is otherwise TERMINAL: 1,681 of 1,944 use it in the shader's very last
//     instruction and 1,927 within three of the end. Its only consumers are `mul` (2,025) and
//     `rcp` (945) - shaders divide BY it to undo it on a value read back from a buffer, which
//     is something done to an exposure scale and never to a material colour. The 147 remaining
//     uses are all `.w`, car-glass opacity.
//
// So discarding Tint_color is correct, and Diffuse_Color is the only candidate for the missing
// per-object albedo tint. Whether it MATTERS is a runtime question: it was observed twice, both
// times (1,1,1,1), by a probe that reports once per distinct first-sampler name and stops at 12.
// That is how "(5.0,5.0,5.0) on every character material in the game" came to be written down
// from four draws in one frame, and it is the reason this probe measures a DISTRIBUTION.
//
// Read-only. It changes no disposition and binds nothing.
struct DiffColBucket { DWORD key; unsigned all; unsigned skinned; };
DiffColBucket g_diffCol[64]{};
unsigned g_diffColUsed = 0;          // distinct quantised values seen
unsigned g_diffColOverflow = 0;      // values that did not fit the table
unsigned g_diffColDraws = 0;         // converted draws carrying Diffuse_Color WITH a map bound
unsigned g_diffColSkinned = 0;       // ...of which skinned
unsigned g_diffColNonWhite = 0;      // ...whose constant is not (1,1,1)
unsigned g_diffColNonWhiteSkinned = 0;

void ProbeDiffuseColor() {
    if (!g_settings.diffuseColorProbe) return;
    const ShaderInfo& ps = g_curPS;
    if (!ps.isPixelShader) return;
    // ONLY the population where the constant is dropped: a real map is bound, so the constant
    // path in SetupTextureStages never runs. Rank-0 draws already take the constant AS their
    // albedo via ConstantAlbedo and are not part of this question.
    if (ps.albedoRank == 0) return;
    if (ps.colourConstReg < 0 || ps.colourConstReg >= static_cast<int>(kMaxPsConst)) return;
    // By NAME, not by rank. The ranker falls through to Tint_color at 50 when nothing better is
    // declared, and Tint_color is exactly what this probe must not be counting.
    if (_stricmp(ps.colourConstName, "Diffuse_Color") != 0) return;

    const float* c = g_psConst[ps.colourConstReg];
    if (!std::isfinite(c[0]) || !std::isfinite(c[1]) || !std::isfinite(c[2])) return;

    const bool skinned = g_curLayout.skinned;
    ++g_diffColDraws;
    if (skinned) ++g_diffColSkinned;

    // Quantised to 1/64 so float noise cannot split one colour across buckets, and clamped so a
    // large value cannot alias onto white.
    auto q = [](float v) -> DWORD {
        if (!(v > 0.0f)) return 0;
        const float t = v * 64.0f + 0.5f;
        return (t >= 255.0f) ? 255u : static_cast<DWORD>(t);
    };
    const DWORD key = (q(c[0]) << 16) | (q(c[1]) << 8) | q(c[2]);
    const DWORD kWhite = (64u << 16) | (64u << 8) | 64u;
    if (key != kWhite) {
        ++g_diffColNonWhite;
        if (skinned) ++g_diffColNonWhiteSkinned;
    }
    for (unsigned i = 0; i < g_diffColUsed; ++i) {
        if (g_diffCol[i].key == key) {
            ++g_diffCol[i].all;
            if (skinned) ++g_diffCol[i].skinned;
            return;
        }
    }
    if (g_diffColUsed >= 64) { ++g_diffColOverflow; return; }
    g_diffCol[g_diffColUsed].key = key;
    g_diffCol[g_diffColUsed].all = 1;
    g_diffCol[g_diffColUsed].skinned = skinned ? 1u : 0u;
    ++g_diffColUsed;
}



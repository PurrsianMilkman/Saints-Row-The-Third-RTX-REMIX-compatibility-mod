// parts/08_hair.cpp - hair for the real character.
// NOT a standalone translation unit: #included, in order, by ../sr3rtx.cpp inside its anonymous
// namespace. Originally sr3rtx.cpp lines 4411-5933 (old file:line citations map here).
// ------------------------------------------------------------------ hair, for the REAL character
//
// The strand generator built earlier writes a DDS for the Remix API path - and that path turned
// out not to be drawing at all. The fix belonged on the shared path from the start: this is the
// same arithmetic, producing a D3D texture the fixed-function conversion can bind, so the strands
// land on the character the game itself draws.
//
//     albedo = chosen hair colour * (Dob_Map.R / 255)
//
// Measured from the shipped asset: R carries the strands (16..222), G is flat at 247, B mirrors
// R. Nothing here is lit - the mask is authored - so Remix still does all the lighting, which is
// the objection the older note in ApiSlot raises against baking the hair SHADER. Different thing.
unsigned g_hairAlbedoGen = 0, g_hairAlbedoFail = 0;

// ------------------------------------------------- hair and skin, AS THEIR SHADERS COMPUTE THEM
//
// Read from the disassembly 2026-10-22 (re/shaders/DX9_disasm), after the user reported NPC heads
// off-colour, body skin lighter than the head, and every head carrying WHITE hair.
//
// HAIR - ir_sr3pchair_c ps[8], the forward pass:
//     b = Dob.B;  S = lerp(c1.z, c1.y, b);  V = b*c1.w + c2.y;  H = c1.x (hue in sectors, 0..6)
//     albedo = HSV(H, S, V)            the six-sector cmp chain, standard HSV
//     out    = (L*albedo + spec) -> fog -> * Tint_color        alpha = Dob.G
// Hair_Parameters c1 and Hair_Spec_Alpha c2 match player_creation_hair_color.xtbl exactly (the
// logged Brunette: Hue 8 -> 8/60 = 0.133, Saturation .8/.8, Brightness .07, bias .01). c3/c4,
// which HairAlbedo coloured the strands with, only weight the SPECULAR terms.
//
// SKIN - ir_sr3npcskinfull_c ps[8] (ir_sr3pcskinfull has the same colour code):
//     uv  = view-space normal mapped to (n.x*.5+.5, 1-(n.y*.5+.5))      a MATCAP lookup
//     S   = lerp(Sphere_Map_1[s3](uv), Sphere_Map_2[s2](uv), Blend_Map[s4].r)
//     out = (L * Diffuse[s0]*Diffuse_Color * S * 2 + spec + ...) -> fog -> * Tint_color
// The skin TONE lives in the two matcaps, and Blend_Map.r picks between them - which is how a
// head's painted-on hair gets its colour from the same draw. The shim bound only Diffuse_Map, a
// greyscale map: no tone, and that hair white. The matcap is view-dependent lighting as well as
// colour, so it is averaged over its disc - every disc texel stands for an equal share of what a
// viewer sees of a sphere - which keeps the colour and leaves the lighting to Remix.
//
// Both go through the cloth exposure curve (Reinhard on Tint*colour, clothColourCurve) so hair,
// skin and clothes sit on one scale, and share the cloth cache, its flushes and its orphan sweep.
unsigned g_hairDraws = 0, g_hairHsvGen = 0, g_hairNull[5] = {};
const char* const kHairNull[5] = {"no Dob_Map bound", "decode failed", "create failed",
                                  "cached failure", "Hair_Parameters out of range"};
unsigned g_skinDraws = 0, g_skinGen = 0, g_skinDeferred = 0, g_skinNull[5] = {};
const char* const kSkinNull[5] = {"a matcap/blend/diffuse texture missing",
                                  "dynamic or render-target diffuse (the player's atlas)",
                                  "decode failed", "create failed", "cached failure"};
unsigned g_skinGenFrame = 0, g_skinGenThisFrame = 0;
unsigned g_hairShaderSeen = 0, g_hairShaderConverted = 0, g_skinShaderSeen = 0, g_skinShaderConverted = 0;

// HSV with the hue in SECTORS, as Hair_Parameters.x carries it. Sector 6 wraps to 0 with the same
// fraction, which is what the shader's (i==0 || i==6) default does.
void HsvSectorToRgb(float h, float s, float v, float out[3]) {
    const float fi = floorf(h);
    const float f = h - fi;
    int sec = static_cast<int>(fi) % 6;
    if (sec < 0) sec += 6;
    const float p = v * (1.0f - s), q = v * (1.0f - s * f), t = v * (1.0f - s * (1.0f - f));
    switch (sec) {
        case 0:  out[0] = v; out[1] = t; out[2] = p; break;
        case 1:  out[0] = q; out[1] = v; out[2] = p; break;
        case 2:  out[0] = p; out[1] = v; out[2] = t; break;
        case 3:  out[0] = p; out[1] = q; out[2] = v; break;
        case 4:  out[0] = t; out[1] = p; out[2] = v; break;
        default: out[0] = v; out[1] = p; out[2] = q; break;
    }
}

// Tint_color from the bound pixel shader, the way ClothAlbedoUniform reads it.
float CurrentTintScalar() {
    float tintC = 5.0f;
    if (g_curPS.tintColorReg >= 0 && g_curPS.tintColorReg < static_cast<int>(kMaxPsConst)) {
        const float t = g_psConst[g_curPS.tintColorReg][0];
        if (std::isfinite(t) && t > 0.0f && t < 50.0f) tintC = t;
    }
    return tintC;
}

// The cloth exposure curve on one linear channel.
inline float ExposureCurve(float c, float tintC) {
    if (!g_settings.clothColourCurve) return c * g_settings.clothTintScale;
    const float x = max(0.0f, c) * tintC;
    return x / (1.0f + x);
}

// BGRA pixels -> a DEFAULT-pool texture through a SYSTEMMEM staging copy (UpdateTexture is the
// upload Remix hashes). Null on any failure.
IDirect3DTexture9* UploadGeneratedBgra(IDirect3DDevice9* dev, const std::vector<unsigned>& pixels,
                                       UINT w, UINT h) {
    IDirect3DTexture9* staging = nullptr;
    IDirect3DTexture9* generated = nullptr;
    const bool wasInternal = g_internal;
    g_internal = true;
    g_internalThread = GetCurrentThreadId();
    if (SUCCEEDED(dev->CreateTexture(w, h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM,
                                     &staging, nullptr)) && staging) {
        D3DLOCKED_RECT dst{};
        bool filled = false;
        if (SUCCEEDED(staging->LockRect(0, &dst, nullptr, 0)) && dst.pBits) {
            for (UINT y = 0; y < h; ++y)
                memcpy(static_cast<unsigned char*>(dst.pBits) + y * dst.Pitch,
                       &pixels[static_cast<size_t>(y) * w], static_cast<size_t>(w) * 4);
            staging->UnlockRect(0);
            filled = true;
        }
        if (filled &&
            SUCCEEDED(dev->CreateTexture(w, h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                                         &generated, nullptr)) && generated &&
            FAILED(dev->UpdateTexture(staging, generated))) {
            generated->Release();
            generated = nullptr;
        }
        staging->Release();
    }
    g_internal = wasInternal;
    return generated;
}

IDirect3DBaseTexture9* HairAlbedoHsv(IDirect3DDevice9* dev) {
    const ShaderInfo& ps = g_curPS;
    if (ps.hairParamsReg < 0) return nullptr;   // not the hair shader: not counted
    ++g_hairDraws;
    if (ps.hairParamsReg >= static_cast<int>(kMaxPsConst)) { ++g_hairNull[4]; return nullptr; }

    IDirect3DBaseTexture9* dobTex = nullptr;
    for (int st = 0; st < 8; ++st)
        if (g_curTexture[st] && ps.samplerName[st][0] && StrStrIA(ps.samplerName[st], "dob")) {
            dobTex = g_curTexture[st];
            break;
        }
    if (!dobTex) { ++g_hairNull[0]; return nullptr; }

    float c1[4]{}, c2[4]{};
    memcpy(c1, g_psConst[ps.hairParamsReg], 16);
    if (ps.hairSpecAlphaReg >= 0 && ps.hairSpecAlphaReg < static_cast<int>(kMaxPsConst))
        memcpy(c2, g_psConst[ps.hairSpecAlphaReg], 16);
    const float tintC = CurrentTintScalar();

    // ClothKey quantises each value to 0..1 and reads xyz only, so the hue (0..6) is scaled into
    // range and the brightness/bias get a row of their own - or every hue past red, and every
    // brightness, would share one texture.
    float col[3][4]{};
    col[0][0] = c1[0] / 6.0f; col[0][1] = c1[1]; col[0][2] = c1[2];
    col[1][0] = c1[3];        col[1][1] = c2[1]; col[1][2] = tintC / 50.0f;
    col[2][0] = g_settings.clothColourCurve ? 1.0f : 0.0f;
    const unsigned long long key = ClothKey(dobTex, col) ^ 0xC2B2AE3D27D4EB4Full;
    const auto it = g_clothCache.find(key);
    if (it != g_clothCache.end()) {
        if (it->second.generated) ++g_clothBound; else ++g_hairNull[3];
        return it->second.generated;
    }
    if (g_clothCache.size() >= kMaxClothTextures) { ReleaseClothCache(); ++g_clothCacheFlushes; }

    std::vector<unsigned char> dob;
    UINT w = 0, h = 0;
    if (!TextureToBgra(dobTex, dob, w, h) || !w || !h || w > 2048 || h > 2048 ||
        dob.size() < static_cast<size_t>(w) * h * 4) {
        ++g_hairNull[1];
        g_clothCache[key] = ClothTex{nullptr, nullptr};
        return nullptr;
    }
    std::vector<unsigned> pixels;
    try { pixels.assign(static_cast<size_t>(w) * h, 0xFF000000u); }
    catch (...) { ++g_hairNull[2]; return nullptr; }
    // The Dob_Map is sampled without sRGB decode (its stage reads srgb=0 in the HEAD/BODY probe),
    // so B is used raw. The HSV result is LINEAR shader arithmetic, encoded once on the way out.
    double acc[3] = {};
    size_t cut = 0;
    for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
        const float b = dob[i * 4 + 0] / 255.0f;            // BGRA: B is the colour mask
        const float S = c1[2] + (c1[1] - c1[2]) * b;        // lrp b, c1.y, c1.z
        const float V = b * c1[3] + c2[1];
        float rgb[3];
        HsvSectorToRgb(c1[0], S, V, rgb);
        unsigned o[3];
        for (int k = 0; k < 3; ++k) {
            o[k] = static_cast<unsigned>(LinearToSrgb(ExposureCurve(rgb[k], tintC)) * 255.0f + 0.5f);
            acc[k] += o[k];
        }
        // Dob.G is the shader's alpha. Kept binary: a fractional alpha on a blended draw would
        // read to Remix as translucency, and the strands are a cutout, not glass.
        const bool solid = dob[i * 4 + 1] >= 128;
        if (!solid) ++cut;
        pixels[i] = (solid ? 0xFF000000u : 0u) | (o[0] << 16) | (o[1] << 8) | o[2];
    }

    IDirect3DTexture9* generated = UploadGeneratedBgra(dev, pixels, w, h);
    if (!generated) {
        ++g_hairNull[2];
        g_clothCache[key] = ClothTex{nullptr, nullptr};
        return nullptr;
    }
    ++g_hairHsvGen;
    if (g_hairHsvGen <= 12) {
        float top[3];
        HsvSectorToRgb(c1[0], c1[1], c1[3] + c2[1], top);
        const double n = static_cast<double>(w) * h;
        Log("HAIR HSV #%u: %ux%u | Hair_Parameters (%.3f %.3f %.3f %.3f) bias %.3f tint %.2f | "
            "strand colour at full mask: linear (%.3f %.3f %.3f) -> sRGB (%.0f %.0f %.0f) | "
            "texture mean sRGB (%.1f %.1f %.1f) | cut out %.1f%%",
            g_hairHsvGen, w, h, c1[0], c1[1], c1[2], c1[3], c2[1], tintC, top[0], top[1], top[2],
            LinearToSrgb(ExposureCurve(top[0], tintC)) * 255.0f,
            LinearToSrgb(ExposureCurve(top[1], tintC)) * 255.0f,
            LinearToSrgb(ExposureCurve(top[2], tintC)) * 255.0f,
            acc[0] / n, acc[1] / n, acc[2] / n, 100.0 * cut / n);
    }
    dobTex->AddRef();
    g_clothCache[key] = ClothTex{dobTex, generated};
    if (cut && g_settings.clothCutout) g_clothCutoutTex.insert(generated);
    ++g_clothBound;
    return generated;
}

// Mean LINEAR colour of a matcap over its inscribed disc (gammaLUT, as the cloth maps are read).
bool MatcapDiscMean(const std::vector<unsigned char>& m, UINT w, UINT h, float out[3]) {
    double acc[3] = {};
    double n = 0.0;
    for (UINT y = 0; y < h; ++y) {
        const float ny = 1.0f - 2.0f * (y + 0.5f) / h;
        for (UINT x = 0; x < w; ++x) {
            const float nx = 2.0f * (x + 0.5f) / w - 1.0f;
            if (nx * nx + ny * ny >= 1.0f) continue;
            const unsigned char* p = &m[(static_cast<size_t>(y) * w + x) * 4];
            acc[0] += g_gammaLUT[p[2]]; acc[1] += g_gammaLUT[p[1]]; acc[2] += g_gammaLUT[p[0]];
            n += 1.0;
        }
    }
    if (n < 1.0) return false;
    for (int k = 0; k < 3; ++k) out[k] = static_cast<float>(acc[k] / n);
    return true;
}

IDirect3DBaseTexture9* SkinAlbedo(IDirect3DDevice9* dev) {
    if (!g_settings.skinMatcapAlbedo) return nullptr;
    const ShaderInfo& ps = g_curPS;
    int sDif = -1, sS1 = -1, sS2 = -1, sBl = -1;
    for (int st = 0; st < 8; ++st) {
        const char* nm = ps.samplerName[st];
        if (!nm[0]) continue;
        if (StrStrIA(nm, "Sphere_Map_1")) sS1 = st;
        else if (StrStrIA(nm, "Sphere_Map_2")) sS2 = st;
        else if (StrStrIA(nm, "Blend_Map")) sBl = st;
        else if (StrStrIA(nm, "Diffuse_Map")) sDif = st;
    }
    if (sDif < 0 || sS1 < 0 || sS2 < 0 || sBl < 0) return nullptr;   // not the skin shader
    ++g_skinDraws;
    IDirect3DBaseTexture9* const dif = g_curTexture[sDif];
    IDirect3DBaseTexture9* const s1 = g_curTexture[sS1];
    IDirect3DBaseTexture9* const s2 = g_curTexture[sS2];
    IDirect3DBaseTexture9* const bl = g_curTexture[sBl];
    if (!dif || !s1 || !s2 || !bl || dif->GetType() != D3DRTYPE_TEXTURE) {
        ++g_skinNull[0];
        return nullptr;
    }
    // The player's skin is the composited atlas - a dynamic texture the game rewrites, already
    // handled by the atlas path. Baking it here would freeze one moment of it.
    {
        D3DSURFACE_DESC sd{};
        if (g_rtTextures.count(dif) ||
            (SUCCEEDED(static_cast<IDirect3DTexture9*>(dif)->GetLevelDesc(0, &sd)) &&
             (sd.Usage & (D3DUSAGE_DYNAMIC | D3DUSAGE_RENDERTARGET)))) {
            ++g_skinNull[1];
            return nullptr;
        }
    }

    float c0[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    if (ps.diffuseColorMulReg >= 0 && ps.diffuseColorMulReg < static_cast<int>(kMaxPsConst))
        memcpy(c0, g_psConst[ps.diffuseColorMulReg], 16);
    const float tintC = CurrentTintScalar();

    float col[3][4]{};
    col[0][0] = c0[0]; col[0][1] = c0[1]; col[0][2] = c0[2];
    col[1][0] = tintC / 50.0f; col[1][1] = g_settings.clothColourCurve ? 1.0f : 0.0f;
    unsigned long long key = ClothKey(dif, col) ^ 0x165667B19E3779F9ull;
    for (IDirect3DBaseTexture9* t : {s1, s2, bl}) {
        key ^= reinterpret_cast<uintptr_t>(t);
        key *= 1099511628211ull;
    }
    const auto it = g_clothCache.find(key);
    if (it != g_clothCache.end()) {
        if (it->second.generated) ++g_clothBound; else ++g_skinNull[4];
        return it->second.generated;
    }
    // A new head: built on the draw thread like the clothes, so capped per frame. Over the cap
    // the draw keeps its raw diffuse for this frame and is built on a later one.
    if (g_skinGenFrame != g_frames) { g_skinGenFrame = g_frames; g_skinGenThisFrame = 0; }
    if (g_settings.skinGenPerFrame > 0 &&
        g_skinGenThisFrame >= static_cast<unsigned>(g_settings.skinGenPerFrame)) {
        ++g_skinDeferred;
        return nullptr;
    }
    ++g_skinGenThisFrame;
    if (g_clothCache.size() >= kMaxClothTextures) { ReleaseClothCache(); ++g_clothCacheFlushes; }
    BuildClothLUTs();

    std::vector<unsigned char> D, M1, M2, B;
    UINT dw = 0, dh = 0, w1 = 0, h1 = 0, w2 = 0, h2 = 0, bw = 0, bh = 0;
    if (!TextureToBgra(dif, D, dw, dh) || !dw || !dh || dw > 2048 || dh > 2048 ||
        D.size() < static_cast<size_t>(dw) * dh * 4 ||
        !TextureToBgra(s1, M1, w1, h1) || M1.size() < static_cast<size_t>(w1) * h1 * 4 ||
        !TextureToBgra(s2, M2, w2, h2) || M2.size() < static_cast<size_t>(w2) * h2 * 4 ||
        !TextureToBgra(bl, B, bw, bh) || !bw || !bh || B.size() < static_cast<size_t>(bw) * bh * 4) {
        ++g_skinNull[2];
        g_clothCache[key] = ClothTex{nullptr, nullptr};
        return nullptr;
    }
    float m1[3], m2[3];
    if (!MatcapDiscMean(M1, w1, h1, m1) || !MatcapDiscMean(M2, w2, h2, m2)) {
        ++g_skinNull[2];
        g_clothCache[key] = ClothTex{nullptr, nullptr};
        return nullptr;
    }
    std::vector<unsigned> pixels;
    try { pixels.assign(static_cast<size_t>(dw) * dh, 0xFF000000u); }
    catch (...) { ++g_skinNull[3]; return nullptr; }
    // Diffuse and Blend_Map share the shader's uv (v4.xy), so the blend is read at the same
    // normalised position, nearest texel when the sizes differ. Blend.r is a weight, used raw.
    double acc = 0.0, blendAcc = 0.0;
    for (UINT y = 0; y < dh; ++y) {
        const UINT by = static_cast<UINT>((static_cast<unsigned long long>(y) * bh) / dh);
        for (UINT x = 0; x < dw; ++x) {
            const UINT bx = static_cast<UINT>((static_cast<unsigned long long>(x) * bw) / dw);
            const float wgt = B[(static_cast<size_t>(by) * bw + bx) * 4 + 2] / 255.0f;
            blendAcc += wgt;
            const size_t i = static_cast<size_t>(y) * dw + x;
            const unsigned char* d = &D[i * 4];
            unsigned o[3];
            for (int k = 0; k < 3; ++k) {
                const float m = m1[k] + (m2[k] - m1[k]) * wgt;    // lrp Blend.r, Sphere2, Sphere1
                const float tone = g_settings.clothColourCurve ? c0[k] * m * 2.0f : c0[k] * m;
                const float v = LinearToSrgb(g_gammaLUT[d[2 - k]] * ExposureCurve(tone, tintC));
                o[k] = static_cast<unsigned>(v * 255.0f + 0.5f);
                acc += o[k];
            }
            pixels[i] = (static_cast<unsigned>(d[3]) << 24) | (o[0] << 16) | (o[1] << 8) | o[2];
        }
    }
    IDirect3DTexture9* generated = UploadGeneratedBgra(dev, pixels, dw, dh);
    if (!generated) {
        ++g_skinNull[3];
        g_clothCache[key] = ClothTex{nullptr, nullptr};
        return nullptr;
    }
    ++g_skinGen;
    if (g_skinGen <= 16) {
        const double n = static_cast<double>(dw) * dh;
        Log("SKIN #%u: diffuse %ux%u, matcaps %ux%u / %ux%u, blend %ux%u | Diffuse_Color (%.3f %.3f "
            "%.3f) tint %.2f | Sphere_Map_1 disc mean linear (%.3f %.3f %.3f) = sRGB (%.0f %.0f %.0f), "
            "Sphere_Map_2 (%.3f %.3f %.3f) = sRGB (%.0f %.0f %.0f) | Blend.r mean %.3f | result mean "
            "%.1f of 255",
            g_skinGen, dw, dh, w1, h1, w2, h2, bw, bh, c0[0], c0[1], c0[2], tintC,
            m1[0], m1[1], m1[2], LinearToSrgb(m1[0]) * 255.0f, LinearToSrgb(m1[1]) * 255.0f,
            LinearToSrgb(m1[2]) * 255.0f, m2[0], m2[1], m2[2], LinearToSrgb(m2[0]) * 255.0f,
            LinearToSrgb(m2[1]) * 255.0f, LinearToSrgb(m2[2]) * 255.0f, blendAcc / n,
            acc / (3.0 * n));
    }
    dif->AddRef();
    g_clothCache[key] = ClothTex{dif, generated};
    ++g_clothBound;
    return generated;
}

IDirect3DBaseTexture9* HairAlbedo(IDirect3DDevice9* dev) {
    if (g_settings.hairHsvAlbedo) return HairAlbedoHsv(dev);
    if (!g_settings.hairStrandsFromDob) return nullptr;
    const ShaderInfo& ps = g_curPS;
    const int reg = (ps.hairColorReg[1] >= 0) ? ps.hairColorReg[1] : ps.hairColorReg[0];
    if (reg < 0 || reg >= static_cast<int>(kMaxPsConst)) return nullptr;

    IDirect3DBaseTexture9* dobTex = nullptr;
    for (int st = 0; st < 8; ++st)
        if (g_curTexture[st] && ps.samplerName[st][0] && StrStrIA(ps.samplerName[st], "dob")) {
            dobTex = g_curTexture[st];
            break;
        }
    if (!dobTex) return nullptr;

    float col[3][4]{};
    memcpy(col[0], g_psConst[reg], 16);
    unsigned long long key = ClothKey(dobTex, col) ^ 0x9E3779B97F4A7C15ull;
    const auto it = g_clothCache.find(key);
    if (it != g_clothCache.end()) {
        if (it->second.generated) ++g_clothBound;
        return it->second.generated;
    }
    if (g_clothCache.size() >= kMaxClothTextures) { ReleaseClothCache(); ++g_clothCacheFlushes; }

    std::vector<unsigned char> dob;
    UINT w = 0, h = 0;
    if (!TextureToBgra(dobTex, dob, w, h) || !w || !h || w > 2048 || h > 2048 ||
        dob.size() < static_cast<size_t>(w) * h * 4) {
        ++g_hairAlbedoFail;
        g_clothCache[key] = ClothTex{nullptr, nullptr};
        return nullptr;
    }
    std::vector<unsigned> pixels;
    try { pixels.assign(static_cast<size_t>(w) * h, 0xFF000000u); }
    catch (...) { ++g_hairAlbedoFail; return nullptr; }
    double acc = 0.0;
    for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
        const float m = dob[i * 4 + 2] / 255.0f;         // BGRA: R is the strand channel
        unsigned char o[3];
        for (int k = 0; k < 3; ++k) {
            float v = col[0][k] * m * g_settings.clothTintScale;
            v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
            o[k] = static_cast<unsigned char>(v * 255.0f + 0.5f);
            acc += o[k];
        }
        pixels[i] = 0xFF000000u | (static_cast<unsigned>(o[0]) << 16) |
                    (static_cast<unsigned>(o[1]) << 8) | static_cast<unsigned>(o[2]);
    }

    IDirect3DTexture9* staging = nullptr;
    IDirect3DTexture9* generated = nullptr;
    const bool wasInternal = g_internal;
    g_internal = true;
    g_internalThread = GetCurrentThreadId();
    if (SUCCEEDED(dev->CreateTexture(w, h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM,
                                     &staging, nullptr)) && staging) {
        D3DLOCKED_RECT dst{};
        if (SUCCEEDED(staging->LockRect(0, &dst, nullptr, 0)) && dst.pBits) {
            for (UINT y = 0; y < h; ++y)
                memcpy(static_cast<unsigned char*>(dst.pBits) + y * dst.Pitch,
                       &pixels[static_cast<size_t>(y) * w], static_cast<size_t>(w) * 4);
            staging->UnlockRect(0);
        }
        if (SUCCEEDED(dev->CreateTexture(w, h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                                         &generated, nullptr)) && generated &&
            FAILED(dev->UpdateTexture(staging, generated))) {
            generated->Release();
            generated = nullptr;
        }
        staging->Release();
    }
    g_internal = wasInternal;
    if (!generated) {
        ++g_hairAlbedoFail;
        g_clothCache[key] = ClothTex{nullptr, nullptr};
        return nullptr;
    }
    ++g_hairAlbedoGen;
    if (g_hairAlbedoGen <= 8)
        Log("HAIR ALBEDO #%u (the GAME's own character): %ux%u = colour (%.3f %.3f %.3f) * the "
            "Dob_Map strand channel | mean %.1f of 255",
            g_hairAlbedoGen, w, h, col[0][0], col[0][1], col[0][2],
            acc / (3.0 * static_cast<double>(w) * h));
    dobTex->AddRef();
    g_clothCache[key] = ClothTex{dobTex, generated};
    ++g_clothBound;
    return generated;
}

// The generated albedo for this draw's outfit, or null to leave the albedo choice alone.

// The body of the declaration above. Placed here, after TextureToBgra, because it needs a decoder
// that handles every format the game uses rather than the DXT1-only path ClothAlbedo grew up on.
IDirect3DBaseTexture9* ClothAlbedoUniform(IDirect3DDevice9* dev, IDirect3DBaseTexture9* pattern,
                                          IDirect3DBaseTexture9* diffuse, const float col[3][4]) {
    // Cached on the pair AND the colours: the result depends on the diffuse map as well, so the
    // pattern-only key ClothAlbedo uses would collide across two garments sharing one pattern -
    // and a solid-blue pattern is shared by definition.
    unsigned long long key = ClothKey(pattern, col);
    key ^= reinterpret_cast<uintptr_t>(diffuse) * 1099511628211ull;
    const auto it = g_clothCache.find(key);
    if (it != g_clothCache.end()) {
        if (it->second.generated) ++g_clothBound;
        return it->second.generated;
    }
    if (g_clothCache.size() >= kMaxClothTextures) {
        ReleaseClothCache();
        ++g_clothCacheFlushes;
    }

    std::vector<unsigned char> pat, dif;
    UINT pw = 0, ph = 0, dw = 0, dh = 0;
    if (!TextureToBgra(pattern, pat, pw, ph) || pat.size() < 4) {
        ++g_clothGenFailed;
        g_clothCache[key] = ClothTex{nullptr, nullptr};
        return nullptr;
    }
    // UNIFORM? Every texel the same colour means the lerp chain has one answer for the whole
    // garment. Checked rather than assumed, and a varying pattern is declined here so the CPU
    // baker still owns that case.
    // THE DIFFUSE IS DECODED HERE, not after the uniformity check.
    //
    // It used to be decoded further down, and the report above printed dw/dh before anything had
    // written them - so every garment was logged as "diffuse 0x0" and the input dump silently
    // wrote nothing. That is a measurement taken before the thing it measures exists, which is a
    // failure this file has recorded three times and still committed.
    if (!TextureToBgra(diffuse, dif, dw, dh) || dif.empty() || !dw || !dh ||
        dw > 2048 || dh > 2048) {
        ++g_clothUniformNoDiffuse;
        g_clothCache[key] = ClothTex{nullptr, nullptr};
        return nullptr;
    }

    unsigned char pick[4] = {pat[0], pat[1], pat[2], pat[3]};
    bool perTexelPattern = false;     // true = evaluate the pattern for EVERY diffuse texel
    std::vector<int> decalIdx;        // per albedo texel: the pattern texel the mesh puts there, or -1
    DecalBake decal;                  // the whole bake, including any further tiles
    // The two garments actually under investigation. A dump budget spent on whichever material
    // happened to be generated first told me nothing about the bra and the underwear, which is
    // what the last dump did - six slots, none of them these.
    bool hardClass = false;
    bool texelUniform = true;
    for (size_t q = 4; q < pat.size(); q += 4)
        if (pat[q] != pat[0] || pat[q + 1] != pat[1] || pat[q + 2] != pat[2]) {
            texelUniform = false; break;
        }
    if (!texelUniform) {
        // WHAT THE SHADER ACTUALLY DOES WITH THIS ALBEDO, reported here rather than in the API
        // character collection - the previous copy of this report sat behind remixApiCharacter,
        // which is now 0, so it could never fire. A diagnostic behind a closed gate is the same
        // mistake twice.
        // 16, and it now dumps what it was GIVEN. The user is deliberately reconfiguring the
        // character to wear a bra as well as the underwear, so this run is the one chance to see
        // BOTH members of the hard class side by side with their real inputs. Six reports and no
        // images would waste it.
        if (g_clampReports < 16) {
            ++g_clampReports;
            // WHICHEVER OF THE FOUR EXIST. The first version demanded all four before it
            // would print anything, so on a shader that declares only ClampU1 and ClampV1 - which
            // is what ir_sr3pccloth[6], the underwear's own shader, does - it reported "declares
            // no clamp constants" when two of them were sitting right there. An all-or-nothing
            // test turning a partial truth into a total absence is the same failure this file
            // keeps recording.
            float cl[4] = {0, 0, 0, 0};
            bool got[4] = {false, false, false, false};
            int haveCount = 0;
            for (int i = 0; i < 4; ++i) {
                const int r = g_curPS.clampReg[i];
                if (r >= 0 && r < static_cast<int>(kMaxPsConst)) {
                    cl[i] = g_psConst[r][0];
                    got[i] = true;
                    ++haveCount;
                }
            }
            const bool have = (haveCount > 0);
            char uvmap[200] = {0};
            int q = 0;
            for (int st = 0; st < 8 && q < 160; ++st)
                if (g_curPS.samplerName[st][0])
                    q += _snprintf_s(uvmap + q, sizeof(uvmap) - q, _TRUNCATE, "s%d=%s<-%s%d%s ",
                                     st, g_curPS.samplerName[st],
                                     g_curPS.samplerUv[st] < 0 ? "unknown" : "TEXCOORD",
                                     g_curPS.samplerUv[st] < 0 ? 0 : g_curPS.samplerUv[st],
                                     g_curPS.samplerUv[st] < 0 ? ""
                                         : (g_curPS.samplerUvDirect[st] ? "" : "*traced"));
            Log("CLOTH SHADER #%u: pattern %ux%u, diffuse %ux%u | %s",
                g_clampReports, pw, ph, dw, dh, uvmap);
            // The albedo's own uv range, which is what decides whether the clamp matters at all.
            // A garment living inside 0..1 is unaffected by pinning; one that leaves it - the
            // underwear spans -1.07..2.79 - is entirely shaped by it.
            // Declared out here, not inside the block below, because the DUMP further down
            // needs the overlay and the block below is where it gets filled in.
            unsigned long long coverTotal = 0, coverBlack = 0, coverLum = 0;
            std::vector<unsigned char> cover;
            // THE REFERENCED SET. Everything above walks [firstVertex, +numVertices), which is
            // the range the draw PROMISES to stay inside, not the vertices it uses. On a shared
            // character buffer that range can hold other garments' vertices, whose uv0 belongs
            // to other textures and lands on this one like noise - which is exactly what the
            // overlays showed. So walk the index buffer, mark only the vertices it names, and
            // measure those. The uv1-vs-uv0 relation is re-measured on the same set, because the
            // INDEPENDENT verdict came from the loose range too and may be an artefact of it.
            std::vector<unsigned char> coverRef;
            // THE DECAL'S SPACE. The pattern is sampled at TEXCOORD1 * Pattern_Map_Tiling / 1024
            // (vs[2]: mul o2.xy, r2, c1.x with r2 = c6/c7 * v4), wrapped. A texture baked in THAT
            // space would carry the decal exactly - if it can also carry the cutout, which lives
            // in uv0. It can only if no wrapped uv1 cell is shared by a VISIBLE surface point and a
            // CUT one. Count that, on the vertices the draw uses, before designing a bake on it.
            std::vector<unsigned char> uv1Over;     // pattern x4, visible=green, cut=red
            unsigned uv1Vis = 0, uv1Cut = 0, uv1CellsVis = 0, uv1CellsCut = 0, uv1CellsBoth = 0,
                     uv1VisInSharedCell = 0;
            float uv1Lo0 = 1e30f, uv1Hi0 = -1e30f, uv1Lo1 = 1e30f, uv1Hi1 = -1e30f;
            float patTileU = 1.0f, patTileV = 1.0f;
            bool patTileFound = false;
            for (int t = 0; t < g_curVS.tilingCount; ++t)
                if (!_stricmp(g_curVS.tiling[t].base, "Pattern_Map")) {
                    const int ur = g_curVS.tiling[t].uReg, vr = g_curVS.tiling[t].vReg;
                    if (ur >= 0 && ur < static_cast<int>(kMaxVsConst)) patTileU = g_vsConst[ur][0];
                    if (vr >= 0 && vr < static_cast<int>(kMaxVsConst)) patTileV = g_vsConst[vr][0];
                    patTileFound = true;
                    break;
                }
            std::vector<unsigned char> uv1CellVis, uv1CellCut;   // 64x64 occupancy, per class
            unsigned refIndices = 0, refDistinct = 0, refOutside = 0, refBlack = 0;
            unsigned refIdxMin = 0xFFFFFFFFu, refIdxMax = 0;
            unsigned long long refLum = 0;
            unsigned uvSame = 0, uvMaxDiff = 0;
            float fitS[2] = {0, 0}, fitO[2] = {0, 0}, fitWorst[2] = {0, 0};
            bool fitOk[2] = {false, false};
            bool refDone = false;
            // THE SCAN. TEXCOORD0 lands two thirds of the underwear's vertices in the black, so
            // the diffuse is fetched through some OTHER element of the vertex on this garment.
            // Rather than guess which - "it must be TEXCOORD1" is exactly the kind of guess that
            // has cost runs here - try every short2 the stride can hold and report the ones that
            // land on the islands. The element that IS the albedo unwrap has near-zero black.
            struct UvCandidate { UINT offset; float blackPct, meanLum, lo0, hi0, lo1, hi1; };
            UvCandidate best[3] = {{0, 101.0f, 0, 0, 0, 0, 0}, {0, 101.0f, 0, 0, 0, 0, 0},
                                   {0, 101.0f, 0, 0, 0, 0, 0}};
            {
                float lo0 = 1e30f, hi0 = -1e30f, lo1 = 1e30f, hi1 = -1e30f;
                bool measured = false;
                // WHERE THE MESH ACTUALLY LANDS, not merely how far its uv ranges.
                //
                // The range alone said "u -0.988..2.744, so it leaves 0..1" and every conclusion
                // drawn from that was wrong, because the sampler WRAPS: a uv of 2.744 is a uv of
                // 0.744 and lands on the map like any other. A bounding box cannot distinguish a
                // mesh whose islands sit neatly on the diffuse's islands from one that samples
                // the empty space between them, and that distinction is the whole question for
                // the bra and the underwear - whose diffuse maps are 88.7% and 73.7% black.
                //
                // So: wrap every vertex's uv the way the sampler does, look up the texel it
                // reaches, and count how many of them are black. That is a statement about the
                // RESULT. If it comes back near zero the mesh never touches the empty space and
                // the black squares have some other cause entirely; if it is large the padding
                // work was aimed at the right place after all.
                if (g_curLayout.texcoordOffset >= 0 &&
                    g_curLayout.texcoordType == D3DDECLTYPE_SHORT2 && g_stream0 &&
                    g_stream0Stride && g_curDrawVertexCount >= 4 &&
                    VertexRangeFits(g_stream0, g_stream0Offset, g_curDrawFirstVertex,
                                    g_curDrawVertexCount, g_stream0Stride)) {
                    void* m = nullptr;
                    if (SUCCEEDED(g_stream0->Lock(
                            g_stream0Offset + g_curDrawFirstVertex * g_stream0Stride,
                            g_curDrawVertexCount * g_stream0Stride, &m,
                            D3DLOCK_READONLY | D3DLOCK_NOSYSLOCK)) && m) {
                        const unsigned char* b = static_cast<const unsigned char*>(m);
                        const UINT o0 = static_cast<UINT>(g_curLayout.texcoordOffset);
                        // The RANGE is still stepped - 32 samples bound a box perfectly well.
                        // The COVERAGE below is not: it walks every vertex, because a mesh can
                        // put a handful of stray islands in the empty space and a 1-in-32 sample
                        // would miss exactly that.
                        const UINT stp = g_curDrawVertexCount / 32 ? g_curDrawVertexCount / 32 : 1;
                        for (UINT i = 0; i < g_curDrawVertexCount; i += stp) {
                            const short* p2 =
                                reinterpret_cast<const short*>(b + i * g_stream0Stride + o0);
                            const float u = p2[0] * kShortUVScale, v = p2[1] * kShortUVScale;
                            if (u < lo0) lo0 = u;
                            if (u > hi0) hi0 = u;
                            if (v < lo1) lo1 = v;
                            if (v > hi1) hi1 = v;
                        }
                        if (!dif.empty() && dw && dh &&
                            dif.size() >= static_cast<size_t>(dw) * dh * 4) {
                            try { cover = dif; } catch (...) { cover.clear(); }
                            for (UINT i = 0; i < g_curDrawVertexCount; ++i) {
                                const short* p2 =
                                    reinterpret_cast<const short*>(b + i * g_stream0Stride + o0);
                                float u = p2[0] * kShortUVScale, v = p2[1] * kShortUVScale;
                                u -= std::floor(u);          // exactly what ADDRESS_WRAP does
                                v -= std::floor(v);
                                UINT tx = static_cast<UINT>(u * dw);
                                UINT ty = static_cast<UINT>(v * dh);
                                if (tx >= dw) tx = dw - 1;
                                if (ty >= dh) ty = dh - 1;
                                const size_t t = (static_cast<size_t>(ty) * dw + tx) * 4;
                                const unsigned bb = dif[t], gg = dif[t + 1], rr = dif[t + 2];
                                ++coverTotal;
                                if (rr <= 8 && gg <= 8 && bb <= 8) ++coverBlack;
                                coverLum += (rr * 77 + gg * 151 + bb * 28) >> 8;
                                // Mark it RED in the overlay. Red on black is a vertex that
                                // reached the empty space; pink on white is one that landed on
                                // an island. One picture, no interpretation needed.
                                if (!cover.empty()) {
                                    cover[t] = 0;
                                    cover[t + 1] = 0;
                                    cover[t + 2] = 255;
                                }
                            }
                            // ---- the index buffer: which of these vertices does the draw use?
                            if (g_curDrawIndexed && g_curDrawPrimCount &&
                                (g_curDrawPrimType == D3DPT_TRIANGLELIST ||
                                 g_curDrawPrimType == D3DPT_TRIANGLESTRIP)) {
                                const UINT nIdx = (g_curDrawPrimType == D3DPT_TRIANGLESTRIP)
                                                      ? g_curDrawPrimCount + 2
                                                      : g_curDrawPrimCount * 3;
                                std::vector<unsigned> idx;
                                IDirect3DIndexBuffer9* ib = nullptr;
                                const bool wasInt = g_internal;
                                g_internal = true;
                                g_internalThread = GetCurrentThreadId();
                                if (SUCCEEDED(dev->GetIndices(&ib)) && ib) {
                                    D3DINDEXBUFFER_DESC ibd{};
                                    void* im = nullptr;
                                    if (SUCCEEDED(ib->GetDesc(&ibd)) && ibd.Size) {
                                        const UINT isz = (ibd.Format == D3DFMT_INDEX32) ? 4 : 2;
                                        const UINT byteStart = g_curDrawStartIndex * isz;
                                        const UINT byteLen = nIdx * isz;
                                        if (byteStart + byteLen <= ibd.Size &&
                                            SUCCEEDED(ib->Lock(byteStart, byteLen, &im,
                                                               D3DLOCK_READONLY)) && im) {
                                            try {
                                                idx.resize(nIdx);
                                                for (UINT i = 0; i < nIdx; ++i)
                                                    idx[i] = (isz == 4)
                                                        ? static_cast<const unsigned*>(im)[i]
                                                        : static_cast<const unsigned short*>(im)[i];
                                            } catch (...) { idx.clear(); }
                                            ib->Unlock();
                                        }
                                    }
                                    ib->Release();
                                }
                                g_internal = wasInt;
                                if (!idx.empty()) {
                                    std::vector<unsigned char> used;
                                    try {
                                        used.assign(g_curDrawVertexCount, 0);
                                        coverRef = dif;
                                    } catch (...) { used.clear(); coverRef.clear(); }
                                    if (!used.empty()) {
                                        refIndices = static_cast<unsigned>(idx.size());
                                        for (unsigned v : idx) {
                                            if (v < refIdxMin) refIdxMin = v;
                                            if (v > refIdxMax) refIdxMax = v;
                                            // absolute vertex = baseVertex + index; our window
                                            // starts at baseVertex + minIndex
                                            const long long abs =
                                                static_cast<long long>(g_curDrawBaseVertex) + v;
                                            const long long rel =
                                                abs - static_cast<long long>(g_curDrawFirstVertex);
                                            if (rel < 0 || rel >= g_curDrawVertexCount) { ++refOutside; continue; }
                                            used[static_cast<size_t>(rel)] = 1;
                                        }
                                        // Coverage, on the referenced vertices only.
                                        double sx[2] = {0, 0}, sy[2] = {0, 0}, sxx[2] = {0, 0}, sxy[2] = {0, 0};
                                        const UINT o1 = (g_curLayout.texcoord1Offset >= 0)
                                                            ? static_cast<UINT>(g_curLayout.texcoord1Offset)
                                                            : 0xFFFFFFFFu;
                                        const bool haveUv1 = (o1 != 0xFFFFFFFFu) &&
                                                             (o1 + 4 <= g_stream0Stride);
                                        try {
                                            uv1CellVis.assign(64 * 64, 0);
                                            uv1CellCut.assign(64 * 64, 0);
                                            if (!pat.empty() && pw && ph) {
                                                uv1Over.assign(static_cast<size_t>(pw) * 4 * ph * 4 * 4, 0);
                                                for (UINT y = 0; y < ph * 4; ++y)
                                                    for (UINT x = 0; x < pw * 4; ++x)
                                                        memcpy(&uv1Over[(static_cast<size_t>(y) * pw * 4 + x) * 4],
                                                               &pat[(static_cast<size_t>(y / 4) * pw + x / 4) * 4], 4);
                                            }
                                        } catch (...) { uv1CellVis.clear(); uv1CellCut.clear(); uv1Over.clear(); }
                                        for (UINT i = 0; i < g_curDrawVertexCount; ++i) {
                                            if (!used[i]) continue;
                                            ++refDistinct;
                                            const short* p2 = reinterpret_cast<const short*>(
                                                b + i * g_stream0Stride + o0);
                                            float u = p2[0] * kShortUVScale, v = p2[1] * kShortUVScale;
                                            u -= std::floor(u);
                                            v -= std::floor(v);
                                            UINT tx = static_cast<UINT>(u * dw);
                                            UINT ty = static_cast<UINT>(v * dh);
                                            if (tx >= dw) tx = dw - 1;
                                            if (ty >= dh) ty = dh - 1;
                                            const size_t t = (static_cast<size_t>(ty) * dw + tx) * 4;
                                            const unsigned bb = dif[t], gg = dif[t + 1], rr = dif[t + 2];
                                            if (rr <= 8 && gg <= 8 && bb <= 8) ++refBlack;
                                            refLum += (rr * 77 + gg * 151 + bb * 28) >> 8;
                                            if (!coverRef.empty()) {
                                                coverRef[t] = 0; coverRef[t + 1] = 0; coverRef[t + 2] = 255;
                                            }
                                            if (haveUv1) {
                                                const short* q2 = reinterpret_cast<const short*>(
                                                    b + i * g_stream0Stride + o1);
                                                // uv1 the way the shader computes it, wrapped
                                                if (!uv1CellVis.empty()) {
                                                    const bool visible = dif[t + 3] >= 128;
                                                    float pu = q2[0] * kShortUVScale * patTileU;
                                                    float pv = q2[1] * kShortUVScale * patTileV;
                                                    if (pu < uv1Lo0) uv1Lo0 = pu;
                                                    if (pu > uv1Hi0) uv1Hi0 = pu;
                                                    if (pv < uv1Lo1) uv1Lo1 = pv;
                                                    if (pv > uv1Hi1) uv1Hi1 = pv;
                                                    pu -= std::floor(pu);
                                                    pv -= std::floor(pv);
                                                    UINT cx = static_cast<UINT>(pu * 64), cy = static_cast<UINT>(pv * 64);
                                                    if (cx >= 64) cx = 63;
                                                    if (cy >= 64) cy = 63;
                                                    (visible ? uv1CellVis : uv1CellCut)[cy * 64 + cx] = 1;
                                                    if (visible) ++uv1Vis; else ++uv1Cut;
                                                    if (!uv1Over.empty()) {
                                                        UINT ox = static_cast<UINT>(pu * pw * 4), oy = static_cast<UINT>(pv * ph * 4);
                                                        if (ox >= pw * 4) ox = pw * 4 - 1;
                                                        if (oy >= ph * 4) oy = ph * 4 - 1;
                                                        unsigned char* d4 = &uv1Over[(static_cast<size_t>(oy) * pw * 4 + ox) * 4];
                                                        d4[0] = 0; d4[1] = visible ? 255 : 0; d4[2] = visible ? 0 : 255; d4[3] = 255;
                                                    }
                                                }
                                                for (int a = 0; a < 2; ++a) {
                                                    const int d0 = p2[a], d1 = q2[a];
                                                    const unsigned dd = static_cast<unsigned>(
                                                        d1 > d0 ? d1 - d0 : d0 - d1);
                                                    if (dd > uvMaxDiff) uvMaxDiff = dd;
                                                    sx[a] += d0; sy[a] += d1;
                                                    sxx[a] += static_cast<double>(d0) * d0;
                                                    sxy[a] += static_cast<double>(d0) * d1;
                                                }
                                                if (p2[0] == q2[0] && p2[1] == q2[1]) ++uvSame;
                                            }
                                        }
                                        // Least-squares uv1 = s*uv0 + o per axis, over the same
                                        // set, with the worst residual - the number that decided
                                        // INDEPENDENT before, now on vertices this draw uses.
                                        if (haveUv1 && refDistinct >= 8) {
                                            const double n = refDistinct;
                                            for (int a = 0; a < 2; ++a) {
                                                const double var = sxx[a] - sx[a] * sx[a] / n;
                                                if (var <= 1e-6) continue;
                                                fitS[a] = static_cast<float>((sxy[a] - sx[a] * sy[a] / n) / var);
                                                fitO[a] = static_cast<float>(sy[a] / n - fitS[a] * sx[a] / n);
                                                fitOk[a] = true;
                                            }
                                            for (UINT i = 0; i < g_curDrawVertexCount; ++i) {
                                                if (!used[i]) continue;
                                                const short* p2 = reinterpret_cast<const short*>(
                                                    b + i * g_stream0Stride + o0);
                                                const short* q2 = reinterpret_cast<const short*>(
                                                    b + i * g_stream0Stride + o1);
                                                for (int a = 0; a < 2; ++a) {
                                                    if (!fitOk[a]) continue;
                                                    const float r = std::fabs(q2[a] - (fitS[a] * p2[a] + fitO[a]));
                                                    if (r > fitWorst[a]) fitWorst[a] = r;
                                                }
                                            }
                                        }
                                        if (!uv1CellVis.empty()) {
                                            for (size_t c2 = 0; c2 < 64 * 64; ++c2) {
                                                if (uv1CellVis[c2] && uv1CellCut[c2]) ++uv1CellsBoth;
                                                else if (uv1CellVis[c2]) ++uv1CellsVis;
                                                else if (uv1CellCut[c2]) ++uv1CellsCut;
                                            }
                                            // second pass: visible vertices sitting in a shared cell
                                            for (UINT i = 0; i < g_curDrawVertexCount; ++i) {
                                                if (!used[i]) continue;
                                                const short* p2 = reinterpret_cast<const short*>(b + i * g_stream0Stride + o0);
                                                float u = p2[0] * kShortUVScale, v = p2[1] * kShortUVScale;
                                                u -= std::floor(u); v -= std::floor(v);
                                                UINT tx = static_cast<UINT>(u * dw), ty = static_cast<UINT>(v * dh);
                                                if (tx >= dw) tx = dw - 1;
                                                if (ty >= dh) ty = dh - 1;
                                                if (dif[(static_cast<size_t>(ty) * dw + tx) * 4 + 3] < 128) continue;
                                                const short* q2 = reinterpret_cast<const short*>(b + i * g_stream0Stride + o1);
                                                float pu = q2[0] * kShortUVScale * patTileU, pv = q2[1] * kShortUVScale * patTileV;
                                                pu -= std::floor(pu); pv -= std::floor(pv);
                                                UINT cx = static_cast<UINT>(pu * 64), cy = static_cast<UINT>(pv * 64);
                                                if (cx >= 64) cx = 63;
                                                if (cy >= 64) cy = 63;
                                                if (uv1CellCut[cy * 64 + cx]) ++uv1VisInSharedCell;
                                            }
                                        }
                                        refDone = true;
                                    }
                                }
                            }
                            // Every short2 the stride can hold, at the confirmed 1/1024. The
                            // position and normal are not short2 and will score badly; that is
                            // fine, the scan is ranked and only the top three are reported.
                            for (UINT o = 0; o + 4 <= g_stream0Stride; o += 2) {
                                unsigned long long nb = 0, lum = 0;
                                float clo0 = 1e30f, chi0 = -1e30f, clo1 = 1e30f, chi1 = -1e30f;
                                for (UINT i = 0; i < g_curDrawVertexCount; ++i) {
                                    const short* p2 = reinterpret_cast<const short*>(
                                        b + i * g_stream0Stride + o);
                                    float u = p2[0] * kShortUVScale, v = p2[1] * kShortUVScale;
                                    if (u < clo0) clo0 = u;
                                    if (u > chi0) chi0 = u;
                                    if (v < clo1) clo1 = v;
                                    if (v > chi1) chi1 = v;
                                    u -= std::floor(u);
                                    v -= std::floor(v);
                                    UINT tx = static_cast<UINT>(u * dw);
                                    UINT ty = static_cast<UINT>(v * dh);
                                    if (tx >= dw) tx = dw - 1;
                                    if (ty >= dh) ty = dh - 1;
                                    const size_t t = (static_cast<size_t>(ty) * dw + tx) * 4;
                                    const unsigned bb = dif[t], gg = dif[t + 1], rr = dif[t + 2];
                                    if (rr <= 8 && gg <= 8 && bb <= 8) ++nb;
                                    lum += (rr * 77 + gg * 151 + bb * 28) >> 8;
                                }
                                UvCandidate c{o, nb * 100.0f / g_curDrawVertexCount,
                                              lum / static_cast<float>(g_curDrawVertexCount),
                                              clo0, chi0, clo1, chi1};
                                for (int k = 0; k < 3; ++k) {
                                    if (c.blackPct < best[k].blackPct) {
                                        for (int j = 2; j > k; --j) best[j] = best[j - 1];
                                        best[k] = c;
                                        break;
                                    }
                                }
                            }
                        }
                        g_stream0->Unlock();
                        measured = true;
                    }
                }
                if (measured) {
                    // No verdict appended. The earlier version of this line ended in "so the
                    // clamp shapes this garment", which was a conclusion the measurement had not
                    // reached and which turned out to be false - the clamp constants are zero and
                    // the shader's cmp selects the UNCLAMPED coordinate. State the numbers.
                    Log("      albedo uv (TEXCOORD0 * 1/1024): u %.3f..%.3f  v %.3f..%.3f  "
                        "(the sampler WRAPS, so a uv of 2.744 reaches the same texel as 0.744)",
                        lo0, hi0, lo1, hi1);
                    if (coverTotal) {
                        Log("      COVERAGE: of %llu vertices, %llu (%.1f%%) land on a texel that "
                            "is BLACK in the diffuse; mean luminance of the texels reached is "
                            "%.1f of 255.",
                            coverTotal, coverBlack,
                            coverBlack * 100.0 / static_cast<double>(coverTotal),
                            coverLum / static_cast<double>(coverTotal));
                        for (int k = 0; k < 3; ++k) {
                            if (best[k].blackPct > 100.0f) break;
                            const char* label =
                                (static_cast<int>(best[k].offset) == g_curLayout.texcoordOffset)
                                    ? " = TEXCOORD0"
                                : (static_cast<int>(best[k].offset) == g_curLayout.texcoord1Offset)
                                    ? " = TEXCOORD1"
                                    : "";
                            Log("      SCAN #%d: short2 at vertex offset %u%s -> %.1f%% on black, "
                                "mean lum %.1f | range u %.3f..%.3f v %.3f..%.3f  (stride %u)",
                                k + 1, best[k].offset, label, best[k].blackPct, best[k].meanLum,
                                best[k].lo0, best[k].hi0, best[k].lo1, best[k].hi1,
                                g_stream0Stride);
                        }
                    }
                    if (refDone) {
                        Log("      INDEX RANGE: %s base=%d minIndex=%u numVertices=%u startIndex=%u "
                            "prims=%u -> %u indices name %u DISTINCT vertices (index span %u..%u); "
                            "%u indices fell outside the numVertices window",
                            (g_curDrawPrimType == D3DPT_TRIANGLESTRIP) ? "STRIP" : "LIST",
                            g_curDrawBaseVertex, g_curDrawMinIndex, g_curDrawVertexCount,
                            g_curDrawStartIndex, g_curDrawPrimCount, refIndices, refDistinct,
                            refIdxMin, refIdxMax, refOutside);
                        if (refDistinct)
                            Log("      COVERAGE (REFERENCED ONLY): of %u distinct vertices this draw "
                                "uses, %u (%.1f%%) land on BLACK; mean luminance %.1f of 255. "
                                "The range walk above said %.1f%%.",
                                refDistinct, refBlack, refBlack * 100.0 / refDistinct,
                                refLum / static_cast<double>(refDistinct),
                                coverTotal ? coverBlack * 100.0 / static_cast<double>(coverTotal) : 0.0);
                        if (fitOk[0] || fitOk[1])
                            Log("      UV1 vs UV0 (REFERENCED ONLY): identical on %u of %u vertices, "
                                "max |uv1-uv0| = %u raw | fit u1 = %.4f*u0 %+.1f (worst %.1f), "
                                "v1 = %.4f*v0 %+.1f (worst %.1f)  [raw shorts; 1024 = one tile]",
                                uvSame, refDistinct, uvMaxDiff,
                                fitS[0], fitO[0], fitWorst[0], fitS[1], fitO[1], fitWorst[1]);
                    } else if (g_curDrawIndexed) {
                        Log("      INDEX RANGE: could not read the index buffer for this draw "
                            "(type %d, prims %u) - the referenced-only numbers are absent, not zero",
                            static_cast<int>(g_curDrawPrimType), g_curDrawPrimCount);
                    }
                    if (refDone && (uv1Vis || uv1Cut)) {
                        Log("      DECAL SPACE: Pattern_Map_Tiling %s(%.3f, %.3f) | uv1*tiling/1024 "
                            "spans u %.3f..%.3f v %.3f..%.3f (%.1f x %.1f tiles) | %u visible + %u cut "
                            "vertices | of the 64x64 wrapped cells they touch: %u visible-only, %u "
                            "cut-only, %u BOTH | %u of the %u visible vertices share a cell with a "
                            "cut one -> %s",
                            patTileFound ? "" : "(not declared, assumed 1) ", patTileU, patTileV,
                            uv1Lo0, uv1Hi0, uv1Lo1, uv1Hi1, uv1Hi0 - uv1Lo0, uv1Hi1 - uv1Lo1,
                            uv1Vis, uv1Cut, uv1CellsVis, uv1CellsCut, uv1CellsBoth,
                            uv1VisInSharedCell, uv1Vis,
                            uv1CellsBoth == 0
                                ? "a uv1-space bake can carry the cutout with NO conflict"
                                : (uv1VisInSharedCell * 10 < uv1Vis
                                       ? "a uv1-space bake keeps the cutout except at these few"
                                       : "the cutout cannot be expressed in uv1 space; it must stay on uv0"));
                    }
                    // THE ALPHA SIDE, which is where the answer turned out to live. The
                    // diffuse's real transparency (after the decoder fix this is the texture's
                    // own 1-bit alpha, not a fabricated 255), the format it arrived in, the
                    // Tint_color alpha the shader multiplies in, and the blend/test state the
                    // game draws this garment with.
                    {
                        size_t a0 = 0;
                        for (size_t i = 0; i + 3 < dif.size(); i += 4) if (dif[i + 3] < 128) ++a0;
                        int fmt = -1;
                        if (g_curPS.albedoStage >= 0 && g_curPS.albedoStage < 8 &&
                            g_curTexture[g_curPS.albedoStage] &&
                            g_curTexture[g_curPS.albedoStage]->GetType() == D3DRTYPE_TEXTURE) {
                            D3DSURFACE_DESC sd{};
                            if (SUCCEEDED(static_cast<IDirect3DTexture9*>(
                                              g_curTexture[g_curPS.albedoStage])->GetLevelDesc(0, &sd)))
                                fmt = static_cast<int>(sd.Format);
                        }
                        const float tintA = (g_curPS.tintColorReg >= 0 &&
                                             g_curPS.tintColorReg < static_cast<int>(kMaxPsConst))
                                                ? g_psConst[g_curPS.tintColorReg][3] : -1.0f;
                        float dc[4] = {1, 1, 1, 1};
                        if (g_curPS.diffuseColorMulReg >= 0 &&
                            g_curPS.diffuseColorMulReg < static_cast<int>(kMaxPsConst))
                            memcpy(dc, g_psConst[g_curPS.diffuseColorMulReg], 16);
                        const float* tc = (g_curPS.tintColorReg >= 0 &&
                                           g_curPS.tintColorReg < static_cast<int>(kMaxPsConst))
                                              ? g_psConst[g_curPS.tintColorReg] : nullptr;
                        Log("      ALPHA: diffuse fmt=%d (DXT1=827611204 DXT5=894720068 ARGB=21) | "
                            "%.1f%% of its texels are transparent (alpha < 128) | Tint_color (%.3f "
                            "%.3f %.3f %.3f) | Diffuse_Color c14 (%.3f %.3f %.3f %.3f) "
                            "| game state: alphaBlend=%lu src=%lu dst=%lu alphaTest=%lu ref=%lu "
                            "func=%lu",
                            fmt, dif.empty() ? 0.0 : a0 * 100.0 / (dif.size() / 4),
                            tc ? tc[0] : -1.0f, tc ? tc[1] : -1.0f, tc ? tc[2] : -1.0f, tintA,
                            dc[0], dc[1], dc[2], dc[3],
                            static_cast<unsigned long>(ShadowGetRS(dev, D3DRS_ALPHABLENDENABLE)),
                            static_cast<unsigned long>(ShadowGetRS(dev, D3DRS_SRCBLEND)),
                            static_cast<unsigned long>(ShadowGetRS(dev, D3DRS_DESTBLEND)),
                            static_cast<unsigned long>(ShadowGetRS(dev, D3DRS_ALPHATESTENABLE)),
                            static_cast<unsigned long>(ShadowGetRS(dev, D3DRS_ALPHAREF)),
                            static_cast<unsigned long>(ShadowGetRS(dev, D3DRS_ALPHAFUNC)));
                    }
                    // WHICH SHADERS, by bytecode size. The cloth family's vertex shader
                    // permutations are all different sizes (ir_sr3pccloth_c: 2032/1784/2340,
                    // _s: 1364/1116/1688, _bs: 1296/1044/1636, _mc: 2132/1884/2440, and the
                    // ir_at_ variants differ again), so the size alone names the file AND the
                    // permutation, and the disassembly can then be read for the right one
                    // instead of the one that happened to be opened first.
                    {
                        UINT vsz = 0, psz = 0;
                        if (g_lastVS) g_lastVS->GetFunction(nullptr, &vsz);
                        if (g_lastPS) g_lastPS->GetFunction(nullptr, &psz);
                        Log("      BOUND SHADERS: vertex %u bytes, pixel %u bytes | diffuse %s",
                            vsz, psz,
                            (g_curPS.albedoStage >= 0 && g_curPS.albedoStage < 8 &&
                             g_curTexture[g_curPS.albedoStage] &&
                             g_rtTextures.count(g_curTexture[g_curPS.albedoStage]))
                                ? "IS a render target" : "is a plain texture");
                    }
                }
            }
            // AND THE INPUTS THEMSELVES. Reasoning about these from a mean lost every time it was
            // tried; looking at them is what worked.
            if (g_clampDumps < 8) {
                ++g_clampDumps;
                char ddir[MAX_PATH] = {0};
                GetModuleFileNameA(GetModuleHandleW(nullptr), ddir, MAX_PATH);
                char* dsl = strrchr(ddir, 0x5C);
                if (dsl) *(dsl + 1) = 0;
                char dpath[MAX_PATH];
                sprintf_s(dpath, "%ssr3-remix-hard-%u-pattern.dds", ddir, g_clampDumps);
                if (WriteBgraDds(dpath, pw, ph, pat.data()))
                    Log("      INPUT pattern -> %s", dpath);
                if (!dif.empty() && dw && dh) {
                    sprintf_s(dpath, "%ssr3-remix-hard-%u-diffuse.dds", ddir, g_clampDumps);
                    if (WriteBgraDds(dpath, dw, dh, dif.data()))
                        Log("      INPUT diffuse -> %s", dpath);
                }
                // The coverage overlay. Looking at this beside the diffuse is what a percentage
                // cannot give: WHERE the strays are, and whether they form the squares the user
                // sees or are scattered noise.
                if (!cover.empty() && dw && dh) {
                    sprintf_s(dpath, "%ssr3-remix-hard-%u-coverage.dds", ddir, g_clampDumps);
                    if (WriteBgraDds(dpath, dw, dh, cover.data()))
                        Log("      COVERAGE overlay (red = a vertex reached this texel) -> %s",
                            dpath);
                }
                if (!coverRef.empty() && dw && dh) {
                    sprintf_s(dpath, "%ssr3-remix-hard-%u-coverage-ref.dds", ddir, g_clampDumps);
                    if (WriteBgraDds(dpath, dw, dh, coverRef.data()))
                        Log("      COVERAGE overlay, REFERENCED vertices only -> %s", dpath);
                }
                if (!uv1Over.empty() && pw && ph) {
                    sprintf_s(dpath, "%ssr3-remix-hard-%u-uv1.dds", ddir, g_clampDumps);
                    if (WriteBgraDds(dpath, pw * 4, ph * 4, uv1Over.data()))
                        Log("      DECAL SPACE overlay (pattern x4; green = visible vertex, red = cut) -> %s",
                            dpath);
                }
            }
            if (have) {
                // WHAT THE CONSTANTS MEAN, decoded from ir_sr3pccloth_c ps[6] rather than
                // assumed from their names. The shader does NOT simply pin the coordinate:
                //
                //   add r0.w, r1.x, -c13.x      ; r1.x is 1.0 (mov r1.xz, c6 ; c6.x = 1)
                //   mul r2.w, r0.w, c9.y        ; c9.y = 512
                //   mad r0.w, r0.w, c9.y, c9.z  ; c9.z = 1
                //   max r3.x, v5.y, -r2.w       ; window is [-(1-ClampV1)*512, (1-ClampV1)*512+1]
                //   min r2.w, r0.w, r3.x
                //   abs r0.w, c13.x
                //   cmp r3.y, -r0.w, v5.y, r2.w ; ClampV1 == 0 -> take v5.y UNCLAMPED
                //
                // So ClampV1 = 0 disables the V clamp outright, and ClampU1 = 0 opens the U
                // window to [-512, 513], which no uv in this game leaves. Both read 0.00000 at
                // runtime for the player's clothing. The game is NOT pinning anything here; it
                // falls through to the sampler, which the INHERITED probe above measures as
                // WRAP - the same thing the conversion path does.
                //
                // The previous version of this line asserted the opposite as a fact and sent the
                // work after a clamp that was never active.
                // The windows themselves, not a verdict. U is always pinned to its window; V is
                // bypassed outright when ClampV1 is exactly zero. A window of [-512, 513] is no
                // clamp in practice; one of [0, 1] is ADDRESS_CLAMP.
                const float uPad = (1.0f - cl[0]) * 512.0f;
                const float vPad = (1.0f - cl[1]) * 512.0f;
                char vwin[48];
                if (got[1] && cl[1] == 0.0f) strcpy_s(vwin, "V bypassed (ClampV1 == 0)");
                else sprintf_s(vwin, "V [%.1f, %.1f]", -vPad, vPad + 1.0f);
                Log("      CLAMP on the albedo (%d of 4 declared): ClampU1 %s%.5f  ClampV1 %s%.5f "
                    "ClampU2 %s%.5f  ClampV2 %s%.5f  -> windows: U [%.1f, %.1f], %s",
                    haveCount,
                    got[0] ? "" : "(absent) ", cl[0], got[1] ? "" : "(absent) ", cl[1],
                    got[2] ? "" : "(absent) ", cl[2], got[3] ? "" : "(absent) ", cl[3],
                    -uPad, uPad + 1.0f, vwin);
            } else
                Log("      this shader declares no ClampU1/V1/U2/V2 - the albedo is not sampled "
                    "through a window here");
        }
        // Not uniform as a TEXTURE - but does this garment read more than one texel of it?
        float pu = 0.0f, pv = 0.0f;
        if (!ClothPatternIsSampledAtOnePoint(&pu, &pv)) {
            if (g_patMode != PatternUv::Affine) {
                // TWO INDEPENDENT UNWRAPS. This is a real structural limit, not a missing trick.
                //
                // The albedo is a tiling detail map on TEXCOORD0 - the underwear's spans about
                // 3.7 tiles, and the game samples it with ADDRESS_WRAP, measured from the
                // sampler state (addressU=1), not assumed. The pattern is a DECAL placed
                // independently on TEXCOORD1: a cat face on the underwear, a star on the bra.
                //
                // So one albedo texel is visited by many surface points carrying different
                // pattern colours, and NO single texture in the albedo's space can hold that.
                // Folding them is not hard, it is impossible.
                //
                // What IS true is that the decal is small and the pattern's background covers
                // most of the garment - which is why the unmodded bra reads as one magenta with
                // a star on it, not as a two-colour object. Taking the pattern's DOMINANT texel
                // gets the garment's colour right and loses the decal.
                //
                // That is an approximation and is named as one in the log. It replaces showing
                // the raw untinted diffuse, which is white, and is wrong in every respect rather
                // than one.
                if (!g_settings.clothDominantPattern) {
                    ++g_clothUniformNotUniform;
                    g_clothCache[key] = ClothTex{nullptr, nullptr};
                    return nullptr;
                }
                unsigned best = 0, bestCount = 0;
                {
                    // Most frequent texel, counted on a 5-bit-per-channel quantisation so that
                    // filtering noise inside one flat region does not split its own vote.
                    static unsigned hist[32768];
                    memset(hist, 0, sizeof(hist));
                    for (size_t q2 = 0; q2 + 3 < pat.size(); q2 += 4) {
                        const unsigned k2 = ((pat[q2 + 2] >> 3) << 10) |
                                            ((pat[q2 + 1] >> 3) << 5) | (pat[q2] >> 3);
                        if (++hist[k2] > bestCount) { bestCount = hist[k2]; best = k2; }
                    }
                    // and the representative full-precision texel from that bucket
                    for (size_t q2 = 0; q2 + 3 < pat.size(); q2 += 4) {
                        const unsigned k2 = ((pat[q2 + 2] >> 3) << 10) |
                                            ((pat[q2 + 1] >> 3) << 5) | (pat[q2] >> 3);
                        if (k2 == best) {
                            pick[0] = pat[q2]; pick[1] = pat[q2 + 1];
                            pick[2] = pat[q2 + 2]; pick[3] = pat[q2 + 3];
                            break;
                        }
                    }
                }
                hardClass = true;      // dump THIS one, not whatever came first
                ++g_clothDominantGen;
                if (g_clothDominantGen <= 8)
                    Log("CLOTH DOMINANT #%u: the pattern is a DECAL on an independent uv set, so "
                        "no one texture in the albedo's space can carry it. Taking its dominant "
                        "texel BGRA %u %u %u (%u%% of the map) as the FALLBACK colour; the mesh "
                        "bake below recovers the decal wherever a visible triangle reaches.",
                        g_clothDominantGen, pick[0], pick[1], pick[2],
                        static_cast<unsigned>(bestCount * 400ull / (pat.size() ? pat.size() : 4)));
                if (g_settings.clothMeshDecal) {
                    const bool ok3 = BakeDecal(dev, dw, dh, dif, pat, pw, ph, pick, decal);
                    if (ok3 && !decal.idx.empty()) decalIdx = decal.idx[0];
                    else decalIdx.clear();
                    if (g_clothDecalGen < 8) {
                        ++g_clothDecalGen;
                        if (ok3) {
                            Log("CLOTH DECAL #%u: the MESH relates the two uv sets. %u triangles "
                                "rasterised into the %ux%u albedo (%u skipped: fully cut or not a "
                                "panel); %u of %u visible texels got a per-texel pattern colour "
                                "(%.1f%%); %u texels where two visible triangles of equal rank "
                                "disagreed (%.2f%% of written). The rest keep the fallback colour.",
                                g_clothDecalGen, decal.tris, dw, dh, decal.skipped, decal.written,
                                decal.visibleTexels,
                                decal.visibleTexels ? decal.written * 100.0 / decal.visibleTexels : 0.0,
                                decal.conflicts,
                                decal.written ? decal.conflicts * 100.0 / decal.written : 0.0);
                            Log("CLOTH DECAL TILES #%u: %u conflict texels (%u opposite-winding, %u "
                                "same-winding) = %u disagreeing triangle pairs over %u triangles -> %u "
                                "tile%s laid out as %u+%u+%u+%u copies = %u texture widths. Tris per "
                                "tile: %u / %u / %u / %u; %u seam vertices duplicated; %u list "
                                "triangles; refused: %u.",
                                g_clothDecalGen, decal.conflicts, decal.oppositeWindingConflicts,
                                decal.sameWindingConflicts, decal.conflictPairs, decal.conflictTris,
                                decal.tiles, decal.tiles == 1 ? "" : "s",
                                decal.copies[0], decal.copies[1], decal.copies[2], decal.copies[3],
                                decal.totalCopies,
                                decal.tileTris[0], decal.tileTris[1], decal.tileTris[2], decal.tileTris[3],
                                decal.dups, static_cast<unsigned>(decal.listIdx.size() / 3),
                                decal.seamCrossers);
                            // WHERE the disagreements sit on the island: red = opposite winding,
                            // yellow = same winding, over the diffuse.
                            if (!decal.conflictMap.empty() && decal.conflictMap.size() == static_cast<size_t>(dw) * dh) {
                                std::vector<unsigned char> cm;
                                try { cm = dif; } catch (...) { cm.clear(); }
                                if (!cm.empty()) {
                                    for (size_t i2 = 0; i2 < decal.conflictMap.size(); ++i2) {
                                        if (!decal.conflictMap[i2]) continue;
                                        cm[i2 * 4 + 0] = 0;
                                        cm[i2 * 4 + 1] = decal.conflictMap[i2] == 2 ? 255 : 0;
                                        cm[i2 * 4 + 2] = 255;
                                        cm[i2 * 4 + 3] = 255;
                                    }
                                    char cdir[MAX_PATH] = {0};
                                    GetModuleFileNameA(GetModuleHandleW(nullptr), cdir, MAX_PATH);
                                    char* csl = strrchr(cdir, 0x5C);
                                    if (csl) *(csl + 1) = 0;
                                    char cpath[MAX_PATH];
                                    sprintf_s(cpath, "%ssr3-remix-decal-%u-conflicts.dds", cdir, g_clothDecalGen);
                                    if (WriteBgraDds(cpath, dw, dh, cm.data()))
                                        Log("      conflict map (red = opposite winding, yellow = same) -> %s", cpath);
                                }
                            }
                        } else {
                            Log("CLOTH DECAL #%u: the mesh bake could not run for this draw "
                                "(indexed=%d prims=%u uv1 stream=%d type=%d) - fallback colour only",
                                g_clothDecalGen, g_curDrawIndexed ? 1 : 0, g_curDrawPrimCount,
                                g_curLayout.texcoord1Stream, g_curLayout.texcoord1Type);
                        }
                    }
                }
            } else {
                // AFFINE: the pattern rides the albedo's own unwrap, so it can be resolved per
                // texel right here. Nothing about the mesh is needed - only the map from one uv
                // to the other, which the fit just measured.
                perTexelPattern = true;
            }
        }
        if (!perTexelPattern) {
        // (one-point: a single texel for the whole surface)
        int px = static_cast<int>(pu * static_cast<float>(pw)) % static_cast<int>(pw);
        int py = static_cast<int>(pv * static_cast<float>(ph)) % static_cast<int>(ph);
        if (px < 0) px += static_cast<int>(pw);
        if (py < 0) py += static_cast<int>(ph);
        const unsigned char* q = &pat[(static_cast<size_t>(py) * pw + px) * 4];
        pick[0] = q[0]; pick[1] = q[1]; pick[2] = q[2]; pick[3] = q[3];
        ++g_clothOnePointGen;
        if (g_clothOnePointGen <= 8)
            Log("CLOTH ONE-POINT #%u: the pattern varies, but this garment samples it at a "
                "single uv (%.4f %.4f) -> texel BGRA %u %u %u. One colour for the surface, and "
                "no mesh needed to work that out.",
                g_clothOnePointGen, pu, pv, pick[0], pick[1], pick[2]);
        }
        if (perTexelPattern) {
            ++g_clothAffineGen;
            if (g_clothAffineGen <= 8)
                Log("CLOTH AFFINE #%u: the pattern varies AND rides the albedo's own unwrap "
                    "(uv1 = %.4f*u0 %+.1f, %.4f*v0 %+.1f). Resolved per texel in the albedo's "
                    "texture space - no mesh.",
                    g_clothAffineGen, g_patScale[0], g_patOffset[0],
                    g_patScale[1], g_patOffset[1]);
        }
    }
    // THE SAME RECIPE AND THE SAME OUTPUT PIPELINE AS ClothAlbedo BELOW.
    //
    // The corset is the one garment that has always rendered correctly, and it is the one that
    // goes through ClothAlbedo's pattern-space path. When this function used its own arithmetic
    // instead - a lerp chain, a plain multiply, no encode - its garments came out the right hue
    // and the wrong brightness. "Everything else should work like the corset" is not a
    // preference; it is the observation that there should only be ONE colour pipeline here.
    //
    // So the three steps that path takes are taken here too:
    //
    //   1. the pattern channels become LINEAR weights through g_gammaLUT (x^2.2), and the colour
    //      is their weighted SUM - not a lerp from white;
    //   2. the whole thing is scaled by clothTintScale (clothAlbedoPercent/100, and the ini
    //      carries 200 - which is most of the brightness that was missing);
    //   3. LinearToSrgb encodes ONCE on the way out.
    //
    // The desaturation escape comes with it. A pattern texel that is near-grey means "no
    // customisation colour here", and dropping that branch would tint regions the game leaves
    // alone. For the flat primaries this function accepts, the test lands on the weighted-sum
    // branch anyway - but it is copied rather than assumed away, because the next uniform
    // pattern may well be a grey one.
    BuildClothLUTs();
    // One derivation, used for a single texel or for every texel. Same maths either way - the
    // per-texel path must not become a second pipeline, which is the mistake that gave the
    // bracelets the right hue and the wrong brightness.
    auto colourFromPatternTexel = [&](const unsigned char* bgra, float* outC) {
        const unsigned char pR = bgra[2], pG = bgra[1], pB = bgra[0];
        const float fr = pR / 255.0f, fg = pG / 255.0f, fb = pB / 255.0f;
        const float sum = fr + fg + fb;
        const float mean = sum * (1.0f / 3.0f);
        const float dev = fabsf(fr - mean) + fabsf(fg - mean) + fabsf(fb - mean);
        const float test = sum - (dev * 165.016495f + 256.0f) / 255.0f;
        if (test < 0.0f) {
            const float wr = g_gammaLUT[pR], wg = g_gammaLUT[pG], wb = g_gammaLUT[pB];
            for (int k = 0; k < 3; ++k)
                outC[k] = wr * col[0][k] + wg * col[1][k] + wb * col[2][k];
        } else {
            const unsigned char pk[3] = {pR, pG, pB};
            for (int k = 0; k < 3; ++k) outC[k] = g_desatLUT[pk[k]];
        }
    };
    // THE PLAYER SHADER'S OWN CHAIN - ir_sr3pccloth_c ps[6], read from the disassembly:
    //
    //     add_pp r5.xyz, -r1.x, c3        ; C - 1            (r1.x is 1.0)
    //     mad_pp r5.xyz, r6.z, r5, c6.x   ; lerp(1, C, blue)
    //     lrp_pp r7.xyz, r6.y, c2, r5     ; lerp(that, B, green)
    //     lrp_pp r5.xyz, r6.x, c1, r7     ; lerp(that, A, red)
    //
    // There is NO desaturation branch in it and it is not a weighted sum. On a pure-channel texel
    // the two agree - (0,0,1) is C either way - which is why every flat-pattern garment renders
    // right through colourFromPatternTexel. On a MIXED texel they do not: cyan (0,1,1) is B here
    // and B+C there, white (1,1,1) is A here and "near-grey, no colour" there. The underwear's cat
    // is cyan with white eyes and the bra's star is cyan, so the first bake came out cyan and
    // white where the game shows magenta, yellow and yellow. Used on the mesh-decal branch only.
    auto colourFromPatternTexelChain = [&](const unsigned char* bgra, float* outC) {
        const float r = g_gammaLUT[bgra[2]], g = g_gammaLUT[bgra[1]], b = g_gammaLUT[bgra[0]];
        for (int k = 0; k < 3; ++k) {
            float v = 1.0f + b * (col[2][k] - 1.0f);
            v += g * (col[1][k] - v);
            v += r * (col[0][k] - v);
            outC[k] = v;
        }
    };
    float c[3];
    colourFromPatternTexel(pick, c);
    // ALREADY NORMALISED. The setting is parsed as num("clothBrightness", 100) / 100.0f, so
    // g_settings.clothBrightness is 1.0 for the default of 100 - dividing by 100 again made it
    // 0.01 and multiplied every garment down by a hundred. That is the whole "result mean 0.3 of
    // 255": the colour was right, the diffuse decoded at 204, and this line threw the product
    // away. The CPU baker beside it has always used the value directly.
    // clothTintScale, the same scale the corset is drawn with - NOT clothBrightness, which is
    // the CPU baker's separate knob. Using the wrong one is what left these garments dark even
    // after the double-division was fixed.
    const float bright = g_settings.clothTintScale;
    // Tint_color, from the pixel shader's own constant (c37 on this family; 5.0 measured).
    float tintC = 5.0f;
    if (g_curPS.tintColorReg >= 0 && g_curPS.tintColorReg < static_cast<int>(kMaxPsConst)) {
        const float t = g_psConst[g_curPS.tintColorReg][0];
        if (std::isfinite(t) && t > 0.0f && t < 50.0f) tintC = t;
    }
    auto curve = [&](float c0) -> float {
        if (!g_settings.clothColourCurve) return c0 * bright;
        const float x = c0 * tintC;
        return x / (1.0f + x);
    };

    // WHAT DID THE DIFFUSE ACTUALLY DECODE TO?
    //
    // The first run of this generator produced the right colour and a result mean of 0.3 of 255
    // - black. The colour arithmetic is provably correct (a flat red pattern selected colour A
    // and returned it exactly), so either the diffuse arrived empty or the multiply is wrong, and
    // those need different fixes. This is the one number that separates them.
    //
    // It matters that this runs INSIDE the draw hook, while the CPU baker that decodes the same
    // maps successfully runs from Present. A D3DPOOL_DEFAULT texture is not promised to be
    // lockable, and a lock that fails mid-draw would hand back exactly this: a decode that
    // succeeds and is full of zeroes.
    double difAcc = 0.0;
    for (size_t i = 0; i + 3 < dif.size(); i += 4)
        difAcc += dif[i] + dif[i + 1] + dif[i + 2];
    const double difMean = dif.empty() ? 0.0 : difAcc / (dif.size() * 0.75);

    std::vector<unsigned> pixels;
    try { pixels.assign(static_cast<size_t>(dw) * dh, 0xFF000000u); }
    catch (...) { ++g_clothGenFailed; return nullptr; }
    double acc = 0.0;
    size_t transparent = 0;   // texels whose diffuse alpha says "not there"

    // One tile of the generated texture: the diffuse times the colour the pattern puts on each
    // texel, through the map a bake produced for THAT tile. Tile 0 is decalIdx; the bra's second
    // cup lives in tile 1 with its own map. Stats are per call, so they describe the last tile.
    auto genTile = [&](const std::vector<int>& tileIdx, std::vector<unsigned>& outPix) {
    acc = 0.0;
    transparent = 0;
    for (size_t i = 0; i < static_cast<size_t>(dw) * dh; ++i) {
        const unsigned char* q = &dif[i * 4];                 // BGRA
        float cc[3] = {c[0], c[1], c[2]};
        if (!tileIdx.empty() && tileIdx[i] >= 0) {
            // The mesh said which pattern texel this albedo texel shows.
            colourFromPatternTexelChain(&pat[static_cast<size_t>(tileIdx[i]) * 4], cc);
        } else if (perTexelPattern) {
            // This texel's albedo uv, mapped into the pattern's uv by the measured fit. The fit
            // is in RAW SHORT units, which is the space the vertex attributes are in, so the
            // texel's 0..1 coordinate goes up by 1024 and the result comes back down by it.
            const size_t tx = i % dw, ty = i / dw;
            const float u0 = ((tx + 0.5f) / static_cast<float>(dw)) * 1024.0f;
            const float v0 = ((ty + 0.5f) / static_cast<float>(dh)) * 1024.0f;
            const float u1 = (g_patScale[0] * u0 + g_patOffset[0]) * kShortUVScale;
            const float v1 = (g_patScale[1] * v0 + g_patOffset[1]) * kShortUVScale;
            int px = static_cast<int>(u1 * static_cast<float>(pw)) % static_cast<int>(pw);
            int py = static_cast<int>(v1 * static_cast<float>(ph)) % static_cast<int>(ph);
            if (px < 0) px += static_cast<int>(pw);
            if (py < 0) py += static_cast<int>(ph);
            // The player shader's chain, as on the mesh-decal branch: a mixed texel such as the
            // backpack's cyan stomach is B here, and was raw cyan through the desaturation test.
            colourFromPatternTexelChain(&pat[(static_cast<size_t>(py) * pw + px) * 4], cc);
        }
        // The diffuse texel is sRGB, so it goes to LINEAR before being multiplied by a colour
        // the pattern path also treats as linear. Multiplying two sRGB values and encoding the
        // product - what this did before - is a different curve, and it is why the same colour
        // that reads correctly on the corset read dull here.
        const unsigned char src[3] = {q[2], q[1], q[0]};      // R, G, B
        unsigned char o[3];
        for (int k = 0; k < 3; ++k) {
            const float v = LinearToSrgb(g_gammaLUT[src[k]] * curve(cc[k]));
            const float cl = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
            o[k] = static_cast<unsigned char>(cl * 255.0f + 0.5f);
            acc += o[k];
        }
        if (q[3] < 128) ++transparent;
        outPix[i] = (static_cast<unsigned>(q[3]) << 24) | (static_cast<unsigned>(o[0]) << 16) |
                    (static_cast<unsigned>(o[1]) << 8) | static_cast<unsigned>(o[2]);
    }
    };
    genTile(decalIdx, pixels);

    // ------------------------------------------------------------------ FURTHER TILES
    // Side by side: tile k occupies columns [k*dw, (k+1)*dw). The vertices of the components
    // assigned to tile k are shifted by whole tiles in SkinAndBind and the texture matrix is
    // scaled by 1/tiles at bind time, so each cup samples its own image. Alpha is the diffuse's
    // in every tile, which is what keeps the cut geometry invisible wherever it lands.
    // decalTiles is the generated texture's width IN TILES: every region's copies, side by side.
    UINT decalTiles = 1;
    if (g_settings.clothDecalTiles && decal.tiles > 1 && decal.idx.size() == decal.tiles &&
        decal.shiftRaw.size() == g_curDrawVertexCount && decal.totalCopies >= decal.tiles) {
        std::vector<unsigned> wide, tilePix;
        bool okw = true;
        try { wide.assign(static_cast<size_t>(dw) * decal.totalCopies * dh, 0xFF000000u); } catch (...) { okw = false; }
        for (unsigned k = 0; okw && k < decal.tiles; ++k) {
            const std::vector<unsigned>* src = &pixels;
            if (k > 0) {
                try { tilePix.assign(static_cast<size_t>(dw) * dh, 0xFF000000u); } catch (...) { okw = false; break; }
                genTile(decal.idx[k], tilePix);
                src = &tilePix;
            }
            for (unsigned c2 = 0; c2 < decal.copies[k]; ++c2) {
                const size_t col = static_cast<size_t>(decal.offsets[k] + c2) * dw;
                for (UINT y = 0; y < dh; ++y)
                    memcpy(&wide[static_cast<size_t>(y) * dw * decal.totalCopies + col],
                           &(*src)[static_cast<size_t>(y) * dw], static_cast<size_t>(dw) * 4);
            }
        }
        if (okw) { pixels.swap(wide); decalTiles = decal.totalCopies; }
    }
    const UINT gw = dw * decalTiles;   // the GENERATED texture's width; dw stays the diffuse's

    // ------------------------------------------------------------------ PAD THE UNUSED SPACE
    //
    // These diffuse maps are mostly EMPTY: measured on the 2026-09-09 outfit, the underwear's is
    // 88.7%% black and the bra's 74.0%%, with a few bright islands carrying the actual garment.
    // Multiplying by a colour leaves the black black, and the user sees black square patches on
    // both - so the mesh is sampling that padding.
    //
    // WHY it samples the padding - corrected 2026-09-09 after the COVERAGE probe. The first
    // version of this comment blamed a lost pixel-shader clamp; the clamp constants are zero at
    // runtime and the shader's own cmp bypasses it, so nothing was lost (see the clampReg comment
    // in ShaderInfo). The measured cause is simpler and worse: through TEXCOORD0 * 1/1024 the
    // 256x256 garment lands 0.3% of its vertices on black, the underwear lands 65.7% and the bra
    // 63.6%, and their overlays show the vertices sprinkled evenly across the whole map. On those
    // two draws TEXCOORD0 is NOT the albedo's unwrap. The coordinate is wrong, not unclamped.
    //
    // Padding cannot fix a wrong coordinate; it only makes the strays less visible by giving the
    // empty space a colour. It stays because it costs nothing on a garment whose mesh already
    // sits on its islands (the 256x256 one dilates 8.5% and renders right) and it softens island
    // edges under bilinear filtering. The real fix for the two garments is the right texcoord,
    // which the SCAN probe is there to name.
    if (g_settings.clothPadUnused) {
        // DILATE the islands outward, rather than flooding the empty space with one average.
        //
        // The flat fill removed the black, and the user reported the squares were then "a similar
        // color to the main parts" - still visible. That is exactly what one average does: the
        // islands carry shading and the padding does not, so every island keeps a visible border
        // against it.
        //
        // Nearest-neighbour dilation has no such seam. Each empty texel takes the colour of the
        // nearest filled one, so the padding CONTINUES whatever island it borders and a sample
        // that strays outside gets what the edge it strayed over would have given. This is the
        // ordinary way atlases are padded, and the reason is the same one here: a sampler that
        // reaches past an island must not be able to tell.
        //
        // Bounded at 64 passes. A 256-wide map could in principle need more, but an unfilled
        // texel that far from any island is one no sample will reach, and an unbounded loop on a
        // pathological map is worse than a few texels of black.
        std::vector<unsigned char> filled;
        bool ok2 = false;
        try { filled.assign(static_cast<size_t>(gw) * dh, 0); ok2 = true; } catch (...) {}
        if (ok2) {
            // SEEDED FROM THE GENERATED PIXEL, NOT THE DIFFUSE.
            //
            // It used to ask whether the DIFFUSE texel was above 8 and then propagate the
            // GENERATED colour, and those are not the same question. A diffuse of 10 passes
            // "above 8", but 10/255 through gamma 2.2 times a colour and a scale of 2 comes out
            // at about 1 of 255 - black. So the near-black rim of every island counted as a
            // seed, and the dilation spread black outward from it, faithfully. The dumped
            // texture shows exactly that: dark wedges radiating from the island edges, 19.4% of
            // the underwear and 39.7% of the bra.
            //
            // The counter was not wrong either - every empty texel really was reached. It was
            // reached with black. "0 texels left unreached" was true and meaningless, which is
            // the more dangerous kind of diagnostic.
            //
            // Only a texel that carries real colour may seed or spread.
            size_t empties = 0;
            for (size_t i = 0; i < static_cast<size_t>(gw) * dh; ++i) {
                const unsigned p2 = pixels[i];
                const unsigned r2 = (p2 >> 16) & 0xFF, g2 = (p2 >> 8) & 0xFF, b2 = p2 & 0xFF;
                filled[i] = (r2 > 16 || g2 > 16 || b2 > 16) ? 1 : 0;
                if (!filled[i]) ++empties;
            }
            const size_t total = static_cast<size_t>(gw) * dh;
            if (empties && empties < total) {
                const size_t before = empties;
                std::vector<unsigned> next = pixels;
                std::vector<unsigned char> nextFilled = filled;
                for (int pass = 0; pass < 64 && empties; ++pass) {
                    bool moved = false;
                    for (UINT y = 0; y < dh; ++y)
                        for (UINT x = 0; x < gw; ++x) {
                            const size_t i = static_cast<size_t>(y) * gw + x;
                            if (filled[i]) continue;
                            // the four neighbours, wrapped - the sampler wraps too, so the
                            // padding should be continuous across the seam for the same reason
                            const UINT xm = (x + gw - 1) % gw, xp = (x + 1) % gw;
                            const UINT ym = (y + dh - 1) % dh, yp = (y + 1) % dh;
                            const size_t nb[4] = {static_cast<size_t>(y) * gw + xm,
                                                  static_cast<size_t>(y) * gw + xp,
                                                  static_cast<size_t>(ym) * gw + x,
                                                  static_cast<size_t>(yp) * gw + x};
                            unsigned acc2[3] = {0, 0, 0};
                            unsigned n2 = 0;
                            for (int k2 = 0; k2 < 4; ++k2)
                                if (filled[nb[k2]]) {
                                    const unsigned p2 = pixels[nb[k2]];
                                    acc2[0] += (p2 >> 16) & 0xFF;
                                    acc2[1] += (p2 >> 8) & 0xFF;
                                    acc2[2] += p2 & 0xFF;
                                    ++n2;
                                }
                            if (!n2) continue;
                            // Colour only. A transparent texel keeps its alpha 0 - the fill is
                            // there so bilinear filtering at the cutout edge blends toward the
                            // garment's colour, not so the texel becomes visible.
                            next[i] = (pixels[i] & 0xFF000000u) | ((acc2[0] / n2) << 16) |
                                      ((acc2[1] / n2) << 8) | (acc2[2] / n2);
                            nextFilled[i] = 1;
                            --empties;
                            moved = true;
                        }
                    if (!moved) break;
                    pixels = next;
                    filled = nextFilled;
                }
                if (g_clothPadReports < 8) {
                    ++g_clothPadReports;
                    // Measured on the RESULT, not on the loop counter. The counter said 0
                    // while two fifths of the texture was black; a number taken from the thing
                    // itself cannot say that.
                    size_t stillDark = 0;
                    for (size_t i2 = 0; i2 < total; ++i2) {
                        const unsigned p3 = pixels[i2];
                        if (((p3 >> 16) & 0xFF) <= 16 && ((p3 >> 8) & 0xFF) <= 16 &&
                            (p3 & 0xFF) <= 16) ++stillDark;
                    }
                    Log("CLOTH PAD #%u: %ux%u was %.1f%% colourless; after dilation %.1f%% of "
                        "the FINAL texture is still black (%u texels the loop never reached)",
                        g_clothPadReports, gw, dh,
                        before * 100.0 / static_cast<double>(total),
                        stillDark * 100.0 / static_cast<double>(total),
                        static_cast<unsigned>(empties));
                }
            }
        }
    }

    // WHAT WE ACTUALLY HAND REMIX. The squares are reported as still black while the fill
    // reports 0 texels unreached, and those two cannot both describe this texture - so write it
    // out and look at it rather than reasoning about which is lying.
    if (hardClass && g_genDumps < 8) {
        ++g_genDumps;
        char gdir[MAX_PATH] = {0};
        GetModuleFileNameA(GetModuleHandleW(nullptr), gdir, MAX_PATH);
        char* gsl = strrchr(gdir, 0x5C);
        if (gsl) *(gsl + 1) = 0;
        char gpath[MAX_PATH];
        sprintf_s(gpath, "%ssr3-remix-gen-%u-%ux%u.dds", gdir, g_genDumps, gw, dh);
        std::vector<unsigned char> bgra;
        try {
            bgra.assign(static_cast<size_t>(gw) * dh * 4, 0);
            for (size_t i = 0; i < static_cast<size_t>(gw) * dh; ++i) {
                const unsigned p2 = pixels[i];
                bgra[i * 4 + 0] = static_cast<unsigned char>(p2 & 0xFF);
                bgra[i * 4 + 1] = static_cast<unsigned char>((p2 >> 8) & 0xFF);
                bgra[i * 4 + 2] = static_cast<unsigned char>((p2 >> 16) & 0xFF);
                bgra[i * 4 + 3] = static_cast<unsigned char>((p2 >> 24) & 0xFF);
            }
            if (WriteBgraDds(gpath, gw, dh, bgra.data()))
                Log("CLOTH GENERATED DUMP #%u -> %s", g_genDumps, gpath);
        } catch (...) {}
    }

    IDirect3DTexture9* staging = nullptr;
    IDirect3DTexture9* generated = nullptr;
    const bool wasInternal = g_internal;
    g_internal = true;
    g_internalThread = GetCurrentThreadId();
    HRESULT hr = dev->CreateTexture(gw, dh, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM,
                                    &staging, nullptr);
    if (SUCCEEDED(hr) && staging) {
        D3DLOCKED_RECT dst{};
        if (SUCCEEDED(staging->LockRect(0, &dst, nullptr, 0)) && dst.pBits) {
            for (UINT y = 0; y < dh; ++y)
                memcpy(static_cast<unsigned char*>(dst.pBits) + y * dst.Pitch,
                       &pixels[static_cast<size_t>(y) * gw], static_cast<size_t>(gw) * 4);
            staging->UnlockRect(0);
        }
        hr = dev->CreateTexture(gw, dh, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &generated,
                                nullptr);
        if (SUCCEEDED(hr) && generated && FAILED(dev->UpdateTexture(staging, generated))) {
            generated->Release();
            generated = nullptr;
        }
        staging->Release();
    }
    g_internal = wasInternal;

    if (!generated) {
        ++g_clothGenFailed;
        g_clothCache[key] = ClothTex{nullptr, nullptr};
        return nullptr;
    }
    ++g_clothUniformGen;
    if (g_clothUniformGen <= 16)
        Log("CLOTH UNIFORM #%u: diffuse %ux%u * one colour (%.3f %.3f %.3f) from a flat pattern "
            "(%.3f %.3f %.3f) | a(%.3f %.3f %.3f) b(%.3f %.3f %.3f) c(%.3f %.3f %.3f) | "
            "DIFFUSE DECODED mean %.1f of 255 | result mean %.1f of 255",
            g_clothUniformGen, dw, dh, c[0], c[1], c[2],
            pick[2] / 255.0f, pick[1] / 255.0f, pick[0] / 255.0f,
            col[0][0], col[0][1], col[0][2], col[1][0], col[1][1], col[1][2],
            col[2][0], col[2][1], col[2][2], difMean,
            acc / (3.0 * static_cast<double>(dw) * dh));
    if (decalTiles > 1 && generated) {
        ClothTileLayout lay;
        lay.tiles = decalTiles;
        lay.firstVertex = g_curDrawFirstVertex;
        lay.vertexCount = g_curDrawVertexCount;
        lay.minIndex = g_curDrawMinIndex;
        lay.shiftRaw = decal.shiftRaw;
        lay.dupSrc = decal.dupSrc;
        lay.dupShiftRaw = decal.dupShiftRaw;
        lay.listIdx = decal.listIdx;
        g_clothTileLayout[generated] = lay;
    }
    if (transparent && generated && g_settings.clothCutout) {
        g_clothCutoutTex.insert(generated);
        if (g_clothCutoutGen < 8)
            Log("CLOTH CUTOUT #%u: %.1f%% of this %ux%u diffuse is transparent in its alpha "
                "channel. The generated texture carries that alpha, the dilation leaves it alone, "
                "and the converted draw alpha-tests it, so the template mesh is cut to shape.",
                ++g_clothCutoutGen, transparent * 100.0 / (static_cast<double>(dw) * dh), dw, dh);
    }

    pattern->AddRef();
    g_clothCache[key] = ClothTex{pattern, generated};
    ++g_clothGenerated;
    ++g_clothBound;
    return generated;
}

IDirect3DBaseTexture9* ClothAlbedo(IDirect3DDevice9* dev) {
    if (!g_settings.generateCloth) return nullptr;
    const ShaderInfo& ps = g_curPS;
    // Reported once per distinct reason. A silent `return nullptr` at the top of a generator
    // is indistinguishable from "this draw never happened", and telling those apart is the whole
    // question for the underwear.
    if (ps.patternStage < 0 || ps.patternStage >= 8) {
        if (ps.diffuseColorReg[0] >= 0 && g_clothNoPattern < 4) {
            ++g_clothNoPattern;
            Log("CLOTH DECLINED #%u: this draw carries Diffuse_Color_a/b/c but NO "
                "Pattern_MapSampler, so the generator cannot run on it", g_clothNoPattern);
        }
        return nullptr;
    }
    if (ps.diffuseColorReg[0] < 0 || ps.diffuseColorReg[1] < 0 || ps.diffuseColorReg[2] < 0)
        return nullptr;
    IDirect3DBaseTexture9* pattern = g_curTexture[ps.patternStage];
    if (!pattern || pattern->GetType() != D3DRTYPE_TEXTURE) return nullptr;

    float col[3][4]{};
    for (int i = 0; i < 3; ++i) {
        const int r = ps.diffuseColorReg[i];
        if (r >= 0 && r < static_cast<int>(kMaxPsConst)) memcpy(col[i], g_psConst[r], 16);
    }

    // The PLAYER's family: a real Diffuse_Map, with the pattern on a second uv set. Handled in
    // the diffuse map's own texture space when the pattern is uniform - see ClothAlbedoUniform.
    // A varying pattern falls through to nullptr exactly as before, and the CPU baker covers it.
    if (ps.albedoStage != ps.patternStage) {
        if (!g_settings.clothUniformFromDiffuse) return nullptr;
        if (ps.albedoStage < 0 || ps.albedoStage >= 8) return nullptr;
        IDirect3DBaseTexture9* diffuse = g_curTexture[ps.albedoStage];
        if (!diffuse || diffuse == pattern) return nullptr;
        return ClothAlbedoUniform(dev, pattern, diffuse, col);
    }

    const unsigned long long key = ClothKey(pattern, col);
    const auto it = g_clothCache.find(key);
    if (it != g_clothCache.end()) {
        if (it->second.generated) ++g_clothBound;
        return it->second.generated;
    }

    // A cap WITH A FLUSH. "Stop accepting at the cap" has produced a permanent cliff twice in
    // this shim - once in the mesh albedo cache and once in the bind-pose cache - where newly
    // streamed content could never be served again.
    if (g_clothCache.size() >= kMaxClothTextures) {
        ReleaseClothCache();
        ++g_clothCacheFlushes;
    }

    BuildClothLUTs();
    IDirect3DTexture9* src = static_cast<IDirect3DTexture9*>(pattern);
    D3DSURFACE_DESC d{};
    const bool haveDesc = SUCCEEDED(src->GetLevelDesc(0, &d));
    const bool dxt1 = haveDesc && (d.Format == D3DFMT_DXT1);
    const bool dxt5 = haveDesc && (d.Format == D3DFMT_DXT5 || d.Format == D3DFMT_DXT3);
    const bool raw32 = haveDesc && (d.Format == D3DFMT_A8R8G8B8 || d.Format == D3DFMT_X8R8G8B8);
    if (!haveDesc || (!dxt1 && !dxt5 && !raw32) ||
        d.Width == 0 || d.Height == 0 || d.Width > 1024 || d.Height > 1024) {
        ++g_clothGenFailed;
        bool named = false;
        for (unsigned i = 0; i < g_clothFmtReports; ++i)
            if (g_clothFmtSeen[i] == static_cast<int>(d.Format)) { named = true; break; }
        if (!named && g_clothFmtReports < 8) {
            g_clothFmtSeen[g_clothFmtReports++] = static_cast<int>(d.Format);
            Log("CLOTH: cannot generate from pattern format %d at %ux%u - decoder does not read it",
                static_cast<int>(d.Format), d.Width, d.Height);
        }
        g_clothCache[key] = ClothTex{nullptr, nullptr};   // do not retry every draw
        return nullptr;
    }

    D3DLOCKED_RECT lr{};
    if (FAILED(src->LockRect(0, &lr, nullptr, D3DLOCK_READONLY)) || !lr.pBits) {
        ++g_clothGenFailed;
        g_clothCache[key] = ClothTex{nullptr, nullptr};
        return nullptr;
    }

    // 16, not 3. The character wears fourteen customised items and three dumps could only ever
    // show the first three generated - which on the 2026-09-06 run were the ones that already
    // looked right. The failing ones were never in the sample.
    const bool dumping = g_settings.clothDump && g_clothDumps < 16;
    std::vector<unsigned char> srcDump, outDump;
    if (dumping) {
        srcDump.assign(static_cast<size_t>(d.Width) * d.Height * 3, 0);
        outDump.assign(static_cast<size_t>(d.Width) * d.Height * 3, 0);
    }

    std::vector<unsigned> pixels(static_cast<size_t>(d.Width) * d.Height, 0xFF000000u);
    const unsigned char* bits = static_cast<const unsigned char*>(lr.pBits);
    const UINT blockBytes = dxt1 ? 8u : 16u;      // DXT3/DXT5 prepend an 8-byte alpha block
    const UINT colourAt = dxt1 ? 0u : 8u;
    for (UINT by = 0; by < d.Height; by += 4) {
        for (UINT bx = 0; bx < d.Width; bx += 4) {
            unsigned char texel[16][3];
            if (!raw32) {
                const unsigned char* blk =
                    bits + (by / 4) * lr.Pitch + (bx / 4) * blockBytes + colourAt;
                // DXT3 and DXT5 colour blocks are always 4-colour; only DXT1 uses the endpoint
                // ordering to signal the 3-colour form.
                DecodeDXT1Block(blk, texel, !dxt1);
            }
            for (int t = 0; t < 16; ++t) {
                const UINT x = bx + (t % 4), y = by + (t / 4);
                if (x >= d.Width || y >= d.Height) continue;
                unsigned char rawTexel[3];
                if (raw32) {
                    const unsigned char* q = bits + y * lr.Pitch + x * 4;   // BGRA
                    rawTexel[0] = q[2]; rawTexel[1] = q[1]; rawTexel[2] = q[0];
                }
                const unsigned char* p = raw32 ? rawTexel : texel[t];
                const float pr = p[0] / 255.0f, pg = p[1] / 255.0f, pb = p[2] / 255.0f;
                const float sum = pr + pg + pb;
                const float mean = sum * (1.0f / 3.0f);
                const float dev = fabsf(pr - mean) + fabsf(pg - mean) + fabsf(pb - mean);
                const float test = sum - (dev * 165.016495f + 256.0f) / 255.0f;
                float rgb[3];
                if (test < 0.0f) {
                    const float wr = g_gammaLUT[p[0]], wg = g_gammaLUT[p[1]], wb = g_gammaLUT[p[2]];
                    for (int k = 0; k < 3; ++k)
                        rgb[k] = wr * col[0][k] + wg * col[1][k] + wb * col[2][k];
                } else {
                    for (int k = 0; k < 3; ++k) rgb[k] = g_desatLUT[p[k]];
                }
                unsigned out = 0xFF000000u;
                unsigned char enc[3];
                for (int k = 0; k < 3; ++k) {
                    const float v = LinearToSrgb(rgb[k] * g_settings.clothTintScale);
                    enc[k] = static_cast<unsigned char>(v * 255.0f + 0.5f);
                    out |= static_cast<unsigned>(enc[k]) << (16 - k * 8);
                }
                pixels[static_cast<size_t>(y) * d.Width + x] = out;
                if (dumping) {
                    const size_t o = (static_cast<size_t>(y) * d.Width + x) * 3;
                    for (int k = 0; k < 3; ++k) {
                        srcDump[o + k] = p[k];      // what the DXT decode produced
                        outDump[o + k] = enc[k];    // what Remix will be handed
                    }
                }
            }
        }
    }
    src->UnlockRect(0);

    // Staged through SYSTEMMEM and copied up with UpdateTexture: DEFAULT pool is not lockable for
    // writing, and UpdateTexture is also the upload Remix hashes - which is what gives each
    // outfit a stable, replaceable texture hash of its own.
    IDirect3DTexture9* staging = nullptr;
    IDirect3DTexture9* generated = nullptr;
    // SAVED, not assumed. This runs inside BeginFFP, which has already set g_internal for its own
    // device calls, so clearing it unconditionally on the way out would hand the rest of the
    // conversion a flag it still owns and believes is set.
    const bool wasInternal = g_internal;
    g_internal = true;
    g_internalThread = GetCurrentThreadId();
    HRESULT hr = dev->CreateTexture(d.Width, d.Height, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM,
                                    &staging, nullptr);
    if (SUCCEEDED(hr) && staging) {
        D3DLOCKED_RECT dst{};
        if (SUCCEEDED(staging->LockRect(0, &dst, nullptr, 0)) && dst.pBits) {
            for (UINT y = 0; y < d.Height; ++y)
                memcpy(static_cast<unsigned char*>(dst.pBits) + y * dst.Pitch,
                       &pixels[static_cast<size_t>(y) * d.Width], d.Width * 4);
            staging->UnlockRect(0);
        }
        hr = dev->CreateTexture(d.Width, d.Height, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                                &generated, nullptr);
        if (SUCCEEDED(hr) && generated) {
            if (FAILED(dev->UpdateTexture(staging, generated))) {
                generated->Release();
                generated = nullptr;
            }
        }
        staging->Release();
    }
    g_internal = wasInternal;

    if (!generated) {
        ++g_clothGenFailed;
        g_clothCache[key] = ClothTex{nullptr, nullptr};
        return nullptr;
    }
    if (dumping) {
        ++g_clothDumps;
        Log("CLOTH DUMP #%u: %ux%u | a(%.3f %.3f %.3f) b(%.3f %.3f %.3f) c(%.3f %.3f %.3f)",
            g_clothDumps, d.Width, d.Height,
            col[0][0], col[0][1], col[0][2], col[1][0], col[1][1], col[1][2],
            col[2][0], col[2][1], col[2][2]);
        DumpRaw("pattern", g_clothDumps, d.Width, d.Height, srcDump.data());
        DumpRaw("result", g_clothDumps, d.Width, d.Height, outDump.data());
    }

    pattern->AddRef();       // the key holds its address
    g_clothCache[key] = ClothTex{pattern, generated};
    ++g_clothGenerated;
    ++g_clothBound;
    if (g_clothGenerated <= 4)
        Log("CLOTH GENERATED #%u: %ux%u from pattern %p | a(%.2f %.2f %.2f) b(%.2f %.2f %.2f) "
            "c(%.2f %.2f %.2f)",
            g_clothGenerated, d.Width, d.Height, static_cast<void*>(pattern),
            col[0][0], col[0][1], col[0][2], col[1][0], col[1][1], col[1][2],
            col[2][0], col[2][1], col[2][2]);
    return generated;
}


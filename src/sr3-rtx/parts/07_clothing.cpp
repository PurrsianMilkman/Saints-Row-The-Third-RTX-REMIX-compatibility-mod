// parts/07_clothing.cpp - customisable clothing, mesh-resolved decals, the player's clothing family.
// NOT a standalone translation unit: #included, in order, by ../sr3rtx.cpp inside its anonymous
// namespace. Originally sr3rtx.cpp lines 3522-4410 (old file:line citations map here).
// ------------------------------------------------- customisable clothing, generated per outfit
//
// SR3 builds clothing colour PER TEXEL from a mask and three player-chosen constants, and Remix
// never runs the game's pixel shaders. No texture-stage arrangement can reproduce that, which is
// why every previous attempt at character colour failed. The answer is to compute the texture
// ourselves and hand Remix the result.
//
// THE RECIPE, from ir_sr3npcclothfull_c.fxo_pc shader [8]:
//
//     sum  = p.r + p.g + p.b
//     dev  = |p.r-sum/3| + |p.g-sum/3| + |p.b-sum/3|
//     test = sum - (dev*165.016495 + 256)/255
//     test <  0 -> p.r^2.2*Diffuse_Color_a + p.g^2.2*Diffuse_Color_b + p.b^2.2*Diffuse_Color_c
//     test >= 0 -> saturate((p - 0.372549) * 1.59375) ^ 2.2
//
// `test` measures how chromatic a texel is; achromatic texels take the desaturated branch, which
// is how trim, buckles and skin escape being tinted by the customisation colours.
//
// Tint_color is deliberately NOT applied. It measured as (5.0, 5.0, 5.0) in every observed draw -
// an exposure multiplier for the inferred-lighting pipeline applied after lighting, not a material
// colour. Baking a factor of five into an 8-bit texture would clip every channel to white.
//
// Cheap because the patterns are tiny: measured 32x32 DXT1 in every case, 1024 texels, so a whole
// outfit generates in microseconds and costs 4 KB. The gamma is exact rather than approximated -
// every input to pow(x, 2.2) is an 8-bit channel, so a 256-entry table covers the entire domain.
float g_gammaLUT[256];        // x^2.2
float g_desatLUT[256];        // saturate((x - 0.372549) * 1.59375) ^ 2.2
bool g_clothLUTReady = false;

// The recipe's pow(x, 2.2) is an sRGB-to-LINEAR conversion, so everything it produces is linear
// light. Storing that straight into an 8-bit texture was wrong: Remix reads an 8-bit albedo as
// sRGB and linearises it again, so the value reaching the path tracer was albedo^2.2 - 0.5 became
// 0.22. That is the reported "clothes are really dark", and it is a round-trip error rather than a
// mistake in the recipe.
//
// The values are therefore re-encoded to sRGB on the way out, so Remix's own decode returns
// exactly the linear albedo the shader computes.
inline float LinearToSrgb(float v) {
    if (v <= 0.0f) return 0.0f;
    if (v >= 1.0f) return 1.0f;
    return powf(v, 1.0f / 2.2f);
}

void BuildClothLUTs() {
    if (g_clothLUTReady) return;
    for (int i = 0; i < 256; ++i) {
        const float x = static_cast<float>(i) / 255.0f;
        g_gammaLUT[i] = powf(x, 2.2f);
        const float d = max(0.0f, min(1.0f, (x - 0.372549f) * 1.59375f));
        g_desatLUT[i] = powf(d, 2.2f);
    }
    g_clothLUTReady = true;
}

// Single-colour garments come out right and multi-colour ones do not, which separates two
// candidates that argument cannot: the DXT decode producing wrong texels, or the channel-to-colour
// mapping being wrong. Both are visible immediately in the pixels, and neither is visible in a
// counter - so the first few outfits are written out as raw RGB alongside their decoded source
// pattern, and looked at rather than reasoned about.
//
// Raw rather than an image format on purpose: no encoder in the shim, and the dimensions are in
// the filename.
unsigned g_clothDumps = 0;

// Defined further down, with the DDS writer. The character texture probe above is one of this
// file recurring ordering traps: the probe sits early, its helpers late.
bool DumpTextureDds(IDirect3DBaseTexture9* base, const char** outPath);

void DumpRaw(const char* tag, unsigned index, UINT w, UINT h,
             const unsigned char* rgb) {
    char path[MAX_PATH];
    if (index) sprintf_s(path, "cloth-%s-%u-%ux%u.raw", tag, index, w, h);
    else       sprintf_s(path, "chartex-%s-%ux%u.raw", tag, w, h);
    FILE* f = nullptr;
    if (fopen_s(&f, path, "wb") != 0 || !f) return;
    fwrite(rgb, 1, static_cast<size_t>(w) * h * 3, f);
    fclose(f);
    Log("    dumped %s", path);
}



// ---------------------------------------------------------------------------------------------
// Write a captured texture as an uncompressed BGRA8 DDS with a full mip chain.
//
// Remix materials name their textures by FILE PATH (remixapi_Path is a wchar_t*), not by D3D9
// handle - dxvk_GetVkImage goes the other way, it reads Remix's OUTPUT. So the only route from the
// game's own textures into an API material is through a file, and this writes it.
//
// DDS rather than PNG because it needs no compression library: header plus raw pixels. Mip-maps
// are generated because the runtime complains otherwise - its own strings carry "Please make sure
// all replacement textures have mip-maps" and "A suboptimal replacement texture detected".
//
// ALPHA IS FORCED TO 255. The character atlas is X8R8G8B8, so its alpha byte is undefined; writing
// it through unchanged would hand Remix a texture that may be entirely transparent. That failure
// would look exactly like the alpha-test bug already paid for once today, so it is pre-empted here
// rather than diagnosed later.
bool WriteBgraDds(const char* path, UINT w, UINT h, const unsigned char* bgra) {
    if (!w || !h) return false;
    unsigned mips = 1;
    for (UINT mw = w, mh = h; mw > 1 || mh > 1; ++mips) {
        mw = mw > 1 ? mw / 2 : 1;
        mh = mh > 1 ? mh / 2 : 1;
    }
    FILE* f = nullptr;
    if (fopen_s(&f, path, "wb") != 0 || !f) return false;

    unsigned hdr[32] = {0};
    hdr[0] = 124;                    // dwSize
    hdr[1] = 0x1 | 0x2 | 0x4 | 0x8 | 0x1000 | 0x20000;  // CAPS HEIGHT WIDTH PITCH PIXELFORMAT MIPMAPCOUNT
    hdr[2] = h;
    hdr[3] = w;
    hdr[4] = w * 4;                  // pitch
    hdr[6] = mips;
    hdr[18] = 32;                    // ddspf.dwSize
    hdr[19] = 0x1 | 0x40;            // ALPHAPIXELS | RGB
    hdr[21] = 32;                    // bit count
    hdr[22] = 0x00FF0000;            // R
    hdr[23] = 0x0000FF00;            // G
    hdr[24] = 0x000000FF;            // B
    hdr[25] = 0xFF000000;            // A
    hdr[26] = 0x1000 | 0x400000 | 0x8;  // TEXTURE | MIPMAP | COMPLEX
    const unsigned magic = 0x20534444;  // "DDS "
    fwrite(&magic, 4, 1, f);
    fwrite(hdr, 4, 31, f);

    std::vector<unsigned char> cur, next;
    try {
        cur.resize(static_cast<size_t>(w) * h * 4);
    } catch (...) { fclose(f); return false; }
    for (size_t i = 0, n = static_cast<size_t>(w) * h; i < n; ++i) {
        cur[i * 4 + 0] = bgra[i * 4 + 0];
        cur[i * 4 + 1] = bgra[i * 4 + 1];
        cur[i * 4 + 2] = bgra[i * 4 + 2];
        // The buffer's own alpha. This wrote 255, so every dump of the bra's and underwear's
        // diffuse reported "alpha==255: 100%" while the same bytes measured 90% transparent in
        // the probe - and the cutout went unseen for a whole day of looking at pictures of it.
        cur[i * 4 + 3] = bgra[i * 4 + 3];
    }
    UINT mw = w, mh = h;
    for (unsigned m = 0; m < mips; ++m) {
        fwrite(cur.data(), 1, static_cast<size_t>(mw) * mh * 4, f);
        if (mw == 1 && mh == 1) break;
        const UINT nw = mw > 1 ? mw / 2 : 1, nh = mh > 1 ? mh / 2 : 1;
        try {
            next.assign(static_cast<size_t>(nw) * nh * 4, 0);
        } catch (...) { break; }
        for (UINT y = 0; y < nh; ++y)
            for (UINT x = 0; x < nw; ++x)
                for (int c = 0; c < 4; ++c) {
                    const UINT x0 = (mw > 1) ? x * 2 : 0, y0 = (mh > 1) ? y * 2 : 0;
                    const UINT x1 = (mw > 1) ? x0 + 1 : x0, y1 = (mh > 1) ? y0 + 1 : y0;
                    const unsigned sum =
                        cur[((size_t)y0 * mw + x0) * 4 + c] + cur[((size_t)y0 * mw + x1) * 4 + c] +
                        cur[((size_t)y1 * mw + x0) * 4 + c] + cur[((size_t)y1 * mw + x1) * 4 + c];
                    next[((size_t)y * nw + x) * 4 + c] = static_cast<unsigned char>(sum / 4);
                }
        cur.swap(next);
        mw = nw; mh = nh;
    }
    fclose(f);
    return true;
}

// Absolute path beside the exe. Remix resolves the path itself, and a relative one depends on a
// working directory this shim does not control.
char g_atlasDdsPath[MAX_PATH] = {0};
bool g_atlasDdsReady = false;
unsigned g_atlasDdsFails = 0;

void WriteAtlasDdsOnce(UINT w, UINT h, const unsigned char* bgra) {
    if (g_atlasDdsReady || g_atlasDdsFails) return;
    char dir[MAX_PATH] = {0};
    if (!GetModuleFileNameA(GetModuleHandleW(nullptr), dir, MAX_PATH)) { ++g_atlasDdsFails; return; }
    char* slash = strrchr(dir, '\\');
    if (slash) *(slash + 1) = 0;
    sprintf_s(g_atlasDdsPath, "%ssr3-remix-atlas.dds", dir);
    if (WriteBgraDds(g_atlasDdsPath, w, h, bgra)) {
        g_atlasDdsReady = true;
        Log("remix api: wrote the character atlas as %s (%ux%u BGRA8 + mips) - a Remix material "
            "names textures by FILE PATH, so this is the only route from the game's own texture "
            "into an API material", g_atlasDdsPath, w, h);
    } else {
        ++g_atlasDdsFails;
        Log("remix api: FAILED to write %s", g_atlasDdsPath);
    }
}

struct ClothTex { IDirect3DBaseTexture9* pattern; IDirect3DTexture9* generated; };
std::unordered_map<unsigned long long, ClothTex> g_clothCache;
// Generated textures whose source diffuse had transparent texels. SetupTextureStages decides the
// alpha path BEFORE ClothAlbedo picks the texture, so the flag cannot ride the normal return;
// BeginFFP looks the bound texture up here instead and applies the alpha test after the bind.
// Cleared wherever the cache is, since these are the cache's own pointers.
std::unordered_set<IDirect3DBaseTexture9*> g_clothCutoutTex;
unsigned g_clothCutoutGen = 0;
// Per generated texture that is wider than the diffuse: how many tiles, and the raw-u shift each
// vertex of the draw needs to land in its tile. Looked up at bind time; applied in SkinAndBind.
struct ClothTileLayout {
    unsigned tiles = 1;
    UINT firstVertex = 0, vertexCount = 0, minIndex = 0;   // the draw this was baked from
    std::vector<float> shiftRaw;        // per original vertex: add to raw u0 (home tile)
    std::vector<unsigned> dupSrc;       // per duplicate: the ring-relative vertex it copies
    std::vector<float> dupShiftRaw;     // per duplicate: its own tile shift
    std::vector<unsigned> listIdx;      // triangle list; values are minIndex + ring-relative
    IDirect3DIndexBuffer9* ib = nullptr;   // listIdx uploaded, created on first bind
    UINT tris = 0;
};
std::unordered_map<IDirect3DBaseTexture9*, ClothTileLayout> g_clothTileLayout;
ClothTileLayout* g_clothRemap = nullptr;   // the current draw's, or null
void ReleaseClothTileLayouts() {
    for (auto& kv : g_clothTileLayout)
        if (kv.second.ib) kv.second.ib->Release();
    g_clothTileLayout.clear();
}
constexpr size_t kMaxClothTextures = 512;   // 32x32 RGBA is 4 KB, so this is ~2 MB
unsigned g_clothGenerated = 0, g_clothBound = 0, g_clothGenFailed = 0, g_clothCacheFlushes = 0;

// Measured 2026-08-20: 234 of 287 outfits could not be generated, against 53 that could. The
// first version accepted DXT1 only, on the strength of a probe that happened to see nothing else -
// eight samples, all 32x32 DXT1. The generated outfits then measured 256x256 as well, so the
// sample was not representative of the population, only of what the probe caught first. That is
// the "counters are not evidence" lesson in a new place: eight observations described eight draws.
//
// DXT5 differs from DXT1 only in carrying a separate alpha block ahead of an identical colour
// block, and its colour block always uses the 4-colour interpolation. Uncompressed sources need
// no decode at all. Anything still unhandled is now NAMED rather than counted, because a format
// number can be looked up and a failure count cannot.
unsigned g_clothFmtReports = 0;
int g_clothFmtSeen[8]{};

// One DXT1 block: two RGB565 endpoints and sixteen 2-bit indices.
void DecodeDXT1Block(const unsigned char* b, unsigned char out[16][3], bool fourColour) {
    const unsigned c0 = b[0] | (b[1] << 8);
    const unsigned c1 = b[2] | (b[3] << 8);
    unsigned char pal[4][3];
    auto expand = [](unsigned c, unsigned char* d) {
        d[0] = static_cast<unsigned char>(((c >> 11) & 0x1F) * 255 / 31);
        d[1] = static_cast<unsigned char>(((c >> 5) & 0x3F) * 255 / 63);
        d[2] = static_cast<unsigned char>((c & 0x1F) * 255 / 31);
    };
    expand(c0, pal[0]);
    expand(c1, pal[1]);
    if (fourColour || c0 > c1) {
        for (int k = 0; k < 3; ++k) {
            pal[2][k] = static_cast<unsigned char>((2 * pal[0][k] + pal[1][k]) / 3);
            pal[3][k] = static_cast<unsigned char>((pal[0][k] + 2 * pal[1][k]) / 3);
        }
    } else {
        // The 3-colour variant: index 3 is transparent black, and these masks use it as a colour.
        for (int k = 0; k < 3; ++k) {
            pal[2][k] = static_cast<unsigned char>((pal[0][k] + pal[1][k]) / 2);
            pal[3][k] = 0;
        }
    }
    const unsigned bits = b[4] | (b[5] << 8) | (b[6] << 16) | (static_cast<unsigned>(b[7]) << 24);
    for (int i = 0; i < 16; ++i) {
        const unsigned idx = (bits >> (i * 2)) & 3u;
        for (int k = 0; k < 3; ++k) out[i][k] = pal[idx][k];
    }
}

// Decode any texture this shim can read into 8-bit RGB. Shared with the clothing generator so
// there is one implementation of the block formats rather than two that can disagree.
bool DecodeTextureRGB(IDirect3DTexture9* tex, std::vector<unsigned char>& rgb, UINT& w, UINT& h) {
    D3DSURFACE_DESC d{};
    if (!tex || FAILED(tex->GetLevelDesc(0, &d))) return false;
    const bool dxt1 = (d.Format == D3DFMT_DXT1);
    const bool dxt5 = (d.Format == D3DFMT_DXT5 || d.Format == D3DFMT_DXT3);
    const bool raw32 = (d.Format == D3DFMT_A8R8G8B8 || d.Format == D3DFMT_X8R8G8B8);
    if ((!dxt1 && !dxt5 && !raw32) || d.Width == 0 || d.Height == 0 ||
        d.Width > 4096 || d.Height > 4096)
        return false;

    D3DLOCKED_RECT lr{};
    if (FAILED(tex->LockRect(0, &lr, nullptr, D3DLOCK_READONLY)) || !lr.pBits) return false;
    w = d.Width;
    h = d.Height;
    rgb.assign(static_cast<size_t>(w) * h * 3, 0);
    const unsigned char* bits = static_cast<const unsigned char*>(lr.pBits);
    if (raw32) {
        for (UINT y = 0; y < h; ++y)
            for (UINT x = 0; x < w; ++x) {
                const unsigned char* q = bits + y * lr.Pitch + x * 4;   // BGRA
                unsigned char* o = &rgb[(static_cast<size_t>(y) * w + x) * 3];
                o[0] = q[2]; o[1] = q[1]; o[2] = q[0];
            }
    } else {
        const UINT blockBytes = dxt1 ? 8u : 16u;
        const UINT colourAt = dxt1 ? 0u : 8u;
        for (UINT by = 0; by < h; by += 4)
            for (UINT bx = 0; bx < w; bx += 4) {
                unsigned char texel[16][3];
                DecodeDXT1Block(bits + (by / 4) * lr.Pitch + (bx / 4) * blockBytes + colourAt,
                                texel, !dxt1);
                for (int t = 0; t < 16; ++t) {
                    const UINT x = bx + (t % 4), y = by + (t / 4);
                    if (x >= w || y >= h) continue;
                    unsigned char* o = &rgb[(static_cast<size_t>(y) * w + x) * 3];
                    for (int k = 0; k < 3; ++k) o[k] = texel[t][k];
                }
            }
    }
    tex->UnlockRect(0);
    return true;
}

// Defined with the texture decoders, far below. This generator sits early in the file.
bool TextureToBgra(IDirect3DBaseTexture9* base, std::vector<unsigned char>& out,
                   UINT& width, UINT& height);

// ------------------------------------------------------------------ the decal, resolved by the mesh
//
// For every texel of the albedo (TEXCOORD0 space), which pattern texel (TEXCOORD1 space) does
// the surface show there? No formula relates the two sets on these garments - measured, the fit
// is nowhere near affine - but every triangle carries both, so rasterising the triangles in
// TEXCOORD0 space with TEXCOORD1 interpolated across them answers it per texel.
//
// Only triangles with at least one VISIBLE vertex (diffuse alpha >= 128 at its uv0) are drawn.
// The template's cut geometry is most of the mesh (208 of the underwear's 271 vertices) and it
// is what scattered uv0 across 3.7 tiles; alpha already removes it from the screen, so it must
// not be allowed to overwrite the panels' colour either. A texel is written only where the
// diffuse is visible, and a triangle with more visible vertices outranks one with fewer. Two
// visible triangles of equal rank disagreeing on a texel is a CONFLICT and is counted.
unsigned g_clothDecalGen = 0;
struct DecalBake {
    unsigned tiles = 1;
    std::vector<std::vector<int>> idx;   // per tile: albedo texel -> pattern texel index, or -1
    std::vector<float> shiftRaw;         // per vertex of the draw: add to raw u0 (1024 = one tile)
    std::vector<unsigned> dupSrc;        // seam vertices duplicated for the other tile
    std::vector<float> dupShiftRaw;
    std::vector<unsigned> listIdx;       // the draw as a triangle list over originals + duplicates
    unsigned written = 0, visibleTexels = 0, conflicts = 0, tris = 0, skipped = 0;
    unsigned oppositeWindingConflicts = 0, sameWindingConflicts = 0;
    unsigned tileTris[4] = {0, 0, 0, 0};
    // Each tile is laid out as `copies` side-by-side repeats of its image, so a panel whose uv0
    // crosses the texture's wrap seam - 47 of the bra's visible triangles do - keeps wrapping
    // seamlessly inside its own region instead of running into the neighbour's image.
    unsigned copies[4] = {1, 1, 1, 1}, offsets[4] = {0, 0, 0, 0}, totalCopies = 1;
    unsigned seamCrossers = 0, dups = 0, conflictPairs = 0, conflictTris = 0;
    std::vector<unsigned char> conflictMap;   // per albedo texel: 0 none, 1 opposite winding, 2 same
};
bool BakeDecal(IDirect3DDevice9* dev, UINT dw, UINT dh, const std::vector<unsigned char>& dif,
               const std::vector<unsigned char>& pat, UINT pw, UINT ph,
               const unsigned char* dominant, DecalBake& out) {
    (void)dominant;
    out = DecalBake{};
    if (!g_curDrawIndexed || !g_curDrawPrimCount) return false;
    if (g_curDrawPrimType != D3DPT_TRIANGLELIST && g_curDrawPrimType != D3DPT_TRIANGLESTRIP)
        return false;
    if (!dw || !dh || !pw || !ph || dif.size() < static_cast<size_t>(dw) * dh * 4 ||
        pat.size() < static_cast<size_t>(pw) * ph * 4)
        return false;
    if (g_curLayout.texcoordOffset < 0 || g_curLayout.texcoordType != D3DDECLTYPE_SHORT2 ||
        g_curLayout.texcoord1Offset < 0 || g_curLayout.texcoord1Type != D3DDECLTYPE_SHORT2 ||
        g_curLayout.texcoord1Stream != 0)
        return false;
    if (!g_stream0 || !g_stream0Stride || g_curDrawVertexCount < 3 ||
        !VertexRangeFits(g_stream0, g_stream0Offset, g_curDrawFirstVertex, g_curDrawVertexCount,
                         g_stream0Stride))
        return false;

    // Pattern_Map_TilingU/V, the way vs[2] applies them: mul o2.xy, r2, c1.x with r2 = c6/c7 * v4.
    float tileU = 1.0f, tileV = 1.0f;
    for (int t = 0; t < g_curVS.tilingCount; ++t)
        if (!_stricmp(g_curVS.tiling[t].base, "Pattern_Map")) {
            const int ur = g_curVS.tiling[t].uReg, vr = g_curVS.tiling[t].vReg;
            if (ur >= 0 && ur < static_cast<int>(kMaxVsConst)) tileU = g_vsConst[ur][0];
            if (vr >= 0 && vr < static_cast<int>(kMaxVsConst)) tileV = g_vsConst[vr][0];
            break;
        }

    // The draw's indices.
    const UINT nIdx = (g_curDrawPrimType == D3DPT_TRIANGLESTRIP) ? g_curDrawPrimCount + 2
                                                                  : g_curDrawPrimCount * 3;
    std::vector<unsigned> idx;
    {
        IDirect3DIndexBuffer9* ib = nullptr;
        const bool wasInt = g_internal;
        g_internal = true;
        g_internalThread = GetCurrentThreadId();
        if (SUCCEEDED(dev->GetIndices(&ib)) && ib) {
            D3DINDEXBUFFER_DESC ibd{};
            void* im = nullptr;
            if (SUCCEEDED(ib->GetDesc(&ibd)) && ibd.Size) {
                const UINT isz = (ibd.Format == D3DFMT_INDEX32) ? 4 : 2;
                const UINT byteStart = g_curDrawStartIndex * isz, byteLen = nIdx * isz;
                if (byteStart + byteLen <= ibd.Size &&
                    SUCCEEDED(ib->Lock(byteStart, byteLen, &im, D3DLOCK_READONLY)) && im) {
                    try {
                        idx.resize(nIdx);
                        for (UINT i = 0; i < nIdx; ++i)
                            idx[i] = (isz == 4) ? static_cast<const unsigned*>(im)[i]
                                                : static_cast<const unsigned short*>(im)[i];
                    } catch (...) { idx.clear(); }
                    ib->Unlock();
                }
            }
            ib->Release();
        }
        g_internal = wasInt;
    }
    if (idx.empty()) return false;

    // Per vertex: uv0 in UNWRAPPED texel units, uv1 in tiles, and whether it is visible.
    struct V { float x, y, u1, v1; bool vis; };
    std::vector<V> vv;
    {
        void* m = nullptr;
        if (FAILED(g_stream0->Lock(g_stream0Offset + g_curDrawFirstVertex * g_stream0Stride,
                                   g_curDrawVertexCount * g_stream0Stride, &m,
                                   D3DLOCK_READONLY | D3DLOCK_NOSYSLOCK)) || !m)
            return false;
        try { vv.resize(g_curDrawVertexCount); } catch (...) { g_stream0->Unlock(); return false; }
        const unsigned char* b = static_cast<const unsigned char*>(m);
        const UINT o0 = static_cast<UINT>(g_curLayout.texcoordOffset);
        const UINT o1 = static_cast<UINT>(g_curLayout.texcoord1Offset);
        for (UINT i = 0; i < g_curDrawVertexCount; ++i) {
            const short* p0 = reinterpret_cast<const short*>(b + i * g_stream0Stride + o0);
            const short* p1 = reinterpret_cast<const short*>(b + i * g_stream0Stride + o1);
            V& v = vv[i];
            const float u = p0[0] * kShortUVScale, w = p0[1] * kShortUVScale;
            v.x = u * dw;
            v.y = w * dh;
            v.u1 = p1[0] * kShortUVScale * tileU;
            v.v1 = p1[1] * kShortUVScale * tileV;
            float uw = u - std::floor(u), ww = w - std::floor(w);
            UINT tx = static_cast<UINT>(uw * dw), ty = static_cast<UINT>(ww * dh);
            if (tx >= dw) tx = dw - 1;
            if (ty >= dh) ty = dh - 1;
            v.vis = dif[(static_cast<size_t>(ty) * dw + tx) * 4 + 3] >= 128;
        }
        g_stream0->Unlock();
    }

    // Triangles as ring-relative vertex indices, with their uv0 winding. A strip's stitching
    // triangles have zero area and are dropped; the list built below does not need them.
    const bool strip = (g_curDrawPrimType == D3DPT_TRIANGLESTRIP);
    const UINT triCount = strip ? (nIdx >= 2 ? nIdx - 2 : 0) : nIdx / 3;
    struct T { unsigned a, b, c; float area; unsigned char vis; unsigned char tile; };
    std::vector<T> tt;
    try { tt.reserve(triCount); } catch (...) { return false; }
    for (UINT tri = 0; tri < triCount; ++tri) {
        unsigned ia, ib2, ic;
        if (strip) { ia = idx[tri]; ib2 = idx[tri + 1]; ic = idx[tri + 2]; }
        else       { ia = idx[tri * 3]; ib2 = idx[tri * 3 + 1]; ic = idx[tri * 3 + 2]; }
        const long long ra = static_cast<long long>(g_curDrawBaseVertex) + ia - g_curDrawFirstVertex;
        const long long rb = static_cast<long long>(g_curDrawBaseVertex) + ib2 - g_curDrawFirstVertex;
        const long long rc = static_cast<long long>(g_curDrawBaseVertex) + ic - g_curDrawFirstVertex;
        if (ra < 0 || rb < 0 || rc < 0 || ra >= g_curDrawVertexCount || rb >= g_curDrawVertexCount ||
            rc >= g_curDrawVertexCount)
            continue;
        T t{static_cast<unsigned>(ra), static_cast<unsigned>(rb), static_cast<unsigned>(rc), 0.0f, 0, 0};
        const V& A = vv[t.a];
        const V& B = vv[t.b];
        const V& C = vv[t.c];
        t.area = (B.x - A.x) * (C.y - A.y) - (B.y - A.y) * (C.x - A.x);
        if (std::fabs(t.area) < 1e-6f) continue;
        t.vis = static_cast<unsigned char>((A.vis ? 1 : 0) + (B.vis ? 1 : 0) + (C.vis ? 1 : 0));
        tt.push_back(t);
    }
    if (tt.empty()) return false;
    for (size_t i = 0; i < static_cast<size_t>(dw) * dh; ++i)
        if (dif[i * 4 + 3] >= 128) ++out.visibleTexels;

    // THE RASTERISER. Pass 0 records which triangle wrote each texel and every disagreement,
    // split by whether the two triangles wind the same way in uv0; later passes build one map
    // per tile from the triangles assigned to it.
    std::vector<int> writer;
    std::vector<unsigned char> prio;
    std::vector<std::pair<unsigned, unsigned>> pairs;   // conflicting triangle pairs, with repeats
    try { out.conflictMap.assign(static_cast<size_t>(dw) * dh, 0); } catch (...) { out.conflictMap.clear(); }
    auto raster = [&](int filterTile, std::vector<int>& map, bool record) -> bool {
        try {
            map.assign(static_cast<size_t>(dw) * dh, -1);
            prio.assign(static_cast<size_t>(dw) * dh, 0);
            if (record) writer.assign(static_cast<size_t>(dw) * dh, -1);
        } catch (...) { return false; }
        for (size_t ti = 0; ti < tt.size(); ++ti) {
            const T& t = tt[ti];
            if (filterTile >= 0 && t.tile != filterTile) continue;
            if (t.vis == 0) { if (record) ++out.skipped; continue; }
            const V& A = vv[t.a];
            const V& B = vv[t.b];
            const V& C = vv[t.c];
            const float minx = std::floor(min(A.x, min(B.x, C.x))), maxx = std::ceil(max(A.x, max(B.x, C.x)));
            const float miny = std::floor(min(A.y, min(B.y, C.y))), maxy = std::ceil(max(A.y, max(B.y, C.y)));
            if (maxx - minx > 4.0f * dw || maxy - miny > 4.0f * dh) { if (record) ++out.skipped; continue; }
            if (record) ++out.tris;
            const float inv = 1.0f / t.area;
            for (int y = static_cast<int>(miny); y <= static_cast<int>(maxy); ++y)
                for (int x = static_cast<int>(minx); x <= static_cast<int>(maxx); ++x) {
                    const float px = x + 0.5f, py = y + 0.5f;
                    float w0 = ((B.x - px) * (C.y - py) - (B.y - py) * (C.x - px)) * inv;
                    float w1 = ((C.x - px) * (A.y - py) - (C.y - py) * (A.x - px)) * inv;
                    float w2 = 1.0f - w0 - w1;
                    const float eps = -1e-4f;
                    if (w0 < eps || w1 < eps || w2 < eps) continue;
                    const int tx = ((x % static_cast<int>(dw)) + static_cast<int>(dw)) % static_cast<int>(dw);
                    const int ty = ((y % static_cast<int>(dh)) + static_cast<int>(dh)) % static_cast<int>(dh);
                    const size_t tex = static_cast<size_t>(ty) * dw + tx;
                    if (dif[tex * 4 + 3] < 128) continue;
                    float u1 = w0 * A.u1 + w1 * B.u1 + w2 * C.u1;
                    float v1 = w0 * A.v1 + w1 * B.v1 + w2 * C.v1;
                    u1 -= std::floor(u1);
                    v1 -= std::floor(v1);
                    UINT ppx = static_cast<UINT>(u1 * pw), ppy = static_cast<UINT>(v1 * ph);
                    if (ppx >= pw) ppx = pw - 1;
                    if (ppy >= ph) ppy = ph - 1;
                    const int pidx = static_cast<int>(ppy * pw + ppx);
                    // Any disagreement about this texel's pattern colour is a conflict between the
                    // two triangles, whichever of them the rank rule lets win. Recorded as a pair,
                    // because the tile assignment is a colouring of exactly that graph.
                    if (record && map[tex] >= 0 && map[tex] != pidx && writer[tex] >= 0 &&
                        writer[tex] != static_cast<int>(ti)) {
                        const unsigned char* q0 = &pat[static_cast<size_t>(map[tex]) * 4];
                        const unsigned char* q1 = &pat[static_cast<size_t>(pidx) * 4];
                        if (q0[0] != q1[0] || q0[1] != q1[1] || q0[2] != q1[2]) {
                            ++out.conflicts;
                            const bool sameSign = (tt[static_cast<size_t>(writer[tex])].area < 0) == (t.area < 0);
                            if (sameSign) ++out.sameWindingConflicts; else ++out.oppositeWindingConflicts;
                            if (!out.conflictMap.empty()) out.conflictMap[tex] = sameSign ? 2 : 1;
                            pairs.push_back(std::make_pair(static_cast<unsigned>(writer[tex]), static_cast<unsigned>(ti)));
                        }
                    }
                    if (map[tex] < 0 || prio[tex] < t.vis) {
                        map[tex] = pidx;
                        prio[tex] = t.vis;
                        if (record) writer[tex] = static_cast<int>(ti);
                    }
                }
        }
        return true;
    };

    std::vector<int> pass0;
    if (!raster(-1, pass0, true)) return false;

    // TILE ASSIGNMENT = A COLOURING OF THE CONFLICT GRAPH. Triangles are the nodes, a recorded
    // disagreement is an edge, and each triangle in mesh order takes the lowest tile holding no
    // triangle it disagrees with. Nothing about winding or connectivity is assumed: the bra's
    // 433 conflicts were 176 between opposite windings and 257 within the same one, so a rule
    // built on either would have missed most of them. Triangles that disagree with nothing stay
    // in tile 0 - they agree with everyone about every texel they touch, so any tile is correct
    // for them - and the seam that leaves between tiles is paid for in duplicated vertices.
    unsigned tiles = 1;
    if (g_settings.clothDecalTiles && !pairs.empty()) {
        std::sort(pairs.begin(), pairs.end());
        pairs.erase(std::unique(pairs.begin(), pairs.end()), pairs.end());
        out.conflictPairs = static_cast<unsigned>(pairs.size());
        // adjacency, both directions
        std::vector<std::vector<unsigned>> adj(tt.size());
        try {
            for (const auto& p : pairs) { adj[p.first].push_back(p.second); adj[p.second].push_back(p.first); }
        } catch (...) { return false; }
        for (size_t ti = 0; ti < tt.size(); ++ti) if (!adj[ti].empty()) ++out.conflictTris;
        constexpr unsigned kMaxTiles = 4;
        std::vector<int> assigned(tt.size(), -1);
        for (size_t ti = 0; ti < tt.size(); ++ti) {
            bool taken[kMaxTiles] = {false, false, false, false};
            for (unsigned o : adj[ti]) if (assigned[o] >= 0) taken[assigned[o]] = true;
            unsigned k = 0;
            while (k < kMaxTiles - 1 && taken[k]) ++k;
            assigned[ti] = static_cast<int>(k);
            tt[ti].tile = static_cast<unsigned char>(k);
            if (k + 1 > tiles) tiles = k + 1;
        }
        // Each tile's visible extent decides how many side-by-side copies of its image it gets:
        // enough that the panel, shifted to start inside the region, never leaves it. A span of
        // 0.8 tiles that crosses the wrap seam needs 2 copies; the band and straps in tile 0 may
        // need more. Cut geometry may wander into any region: every region carries the same
        // alpha in texture space, so it stays invisible wherever it lands.
        float minU[kMaxTiles], maxU[kMaxTiles];
        for (unsigned k = 0; k < kMaxTiles; ++k) { minU[k] = 1e30f; maxU[k] = -1e30f; }
        for (const T& t : tt) {
            if (!t.vis) continue;
            const float u0 = min(vv[t.a].x, min(vv[t.b].x, vv[t.c].x)) / dw;
            const float u1 = max(vv[t.a].x, max(vv[t.b].x, vv[t.c].x)) / dw;
            if (u0 < minU[t.tile]) minU[t.tile] = u0;
            if (u1 > maxU[t.tile]) maxU[t.tile] = u1;
        }
        constexpr unsigned kMaxTotalCopies = 12;
        unsigned total = 0;
        for (unsigned k = 0; k < tiles; ++k) {
            const float span = (minU[k] < 1e29f) ? (maxU[k] - minU[k]) : 0.0f;
            out.copies[k] = static_cast<unsigned>(std::ceil(span)) + 1u;
            if (out.copies[k] < 1) out.copies[k] = 1;
            out.offsets[k] = total;
            total += out.copies[k];
        }
        out.totalCopies = total;
        if (total > kMaxTotalCopies) ++out.seamCrossers;   // a panel too wide to house; refuse
        if (out.seamCrossers || tiles < 2) tiles = 1;
        if (tiles > 1) {
            for (const T& t : tt) ++out.tileTris[t.tile];
            // Per tile: the shift that puts its visible panel at the start of its region.
            float shift[kMaxTiles];
            for (unsigned k = 0; k < kMaxTiles; ++k) {
                const float m = (minU[k] < 1e29f) ? std::floor(minU[k]) : 0.0f;
                shift[k] = (static_cast<float>(out.offsets[k]) - m) * 1024.0f;
            }
            // Per vertex: the home tile is the lowest tile among its triangles. Any triangle in
            // another tile references a duplicate carrying that tile's shift instead.
            std::vector<unsigned char> home;
            try { home.assign(g_curDrawVertexCount, static_cast<unsigned char>(kMaxTiles)); out.shiftRaw.assign(g_curDrawVertexCount, 0.0f); }
            catch (...) { return false; }
            for (const T& t : tt)
                for (unsigned vi : {t.a, t.b, t.c}) if (t.tile < home[vi]) home[vi] = t.tile;
            for (unsigned i = 0; i < g_curDrawVertexCount; ++i) {
                if (home[i] >= kMaxTiles) home[i] = 0;   // referenced by no triangle
                out.shiftRaw[i] = shift[home[i]];
            }
            std::vector<int> dupOf;
            try { dupOf.assign(static_cast<size_t>(g_curDrawVertexCount) * kMaxTiles, -1); out.listIdx.reserve(tt.size() * 3); }
            catch (...) { return false; }
            try {
                for (const T& t : tt) {
                    for (unsigned vi : {t.a, t.b, t.c}) {
                        unsigned ref;
                        if (t.tile == home[vi]) {
                            ref = g_curDrawMinIndex + vi;
                        } else {
                            int& d = dupOf[static_cast<size_t>(vi) * kMaxTiles + t.tile];
                            if (d < 0) {
                                d = static_cast<int>(out.dupSrc.size());
                                out.dupSrc.push_back(vi);
                                out.dupShiftRaw.push_back(shift[t.tile]);
                            }
                            ref = g_curDrawMinIndex + g_curDrawVertexCount + static_cast<unsigned>(d);
                        }
                        out.listIdx.push_back(ref);
                    }
                }
            } catch (...) { return false; }
            out.dups = static_cast<unsigned>(out.dupSrc.size());
        }
    }
    if (tiles == 1) for (T& t : tt) t.tile = 0;
    out.tiles = tiles;

    try { out.idx.resize(tiles); } catch (...) { return false; }
    if (tiles == 1) {
        out.idx[0].swap(pass0);
    } else {
        for (unsigned k = 0; k < tiles; ++k)
            if (!raster(static_cast<int>(k), out.idx[k], false)) return false;
    }
    for (int v : out.idx[0]) if (v >= 0) ++out.written;
    return out.written > 0;
}

unsigned g_clothUniformGen = 0, g_clothUniformNotUniform = 0, g_clothUniformNoDiffuse = 0;

// ------------------------------------------------------------------ the player's clothing family
//
// ClothAlbedo() below refuses any material whose ranked albedo is not the Pattern_Map itself, and
// that refusal is what left the player in her underwear textures. Her garments sample a REAL
// Diffuse_Map and combine it with the customisation colours through a SECOND uv set - a different
// recipe - so the generator declined all fourteen of them and the shim bound the raw texture
// instead. The dumped inputs say exactly what that looks like: the bracelets, the choker and the
// gem all have WHITE or grey diffuse maps meant to be tinted, and the beanie's pattern is solid
// green. White bracelets, a white choker and a green hat is not a coincidence - it is those two
// maps rendered untinted.
//
// But the second uv set is only needed when the pattern VARIES across the garment. Measured on
// the 2026-09-06 run, five of the player's nine baked garments have a pattern that is one flat
// colour - #1 solid blue, #3 solid green, #6 solid red, #7 solid blue, #8 solid green - and a
// uniform pattern means the lerp chain produces ONE colour for the whole surface. One colour
// times a diffuse map needs no rasterisation, no second uv set, and no assumption about where
// UVs live: it is a multiply in the diffuse map's own texture space, which is exactly the space
// TEXCOORD0 already samples.
//
// That is what makes this different from the three fixes that failed on the underwear. Each of
// those tried to make a UV-space bake work; this one removes the UV space from the problem.
//
// A varying pattern still returns nullptr and still needs the CPU baker, so nothing that works
// today is disturbed.
IDirect3DBaseTexture9* ClothAlbedoUniform(IDirect3DDevice9* dev, IDirect3DBaseTexture9* pattern,
                                          IDirect3DBaseTexture9* diffuse, const float col[3][4]);

unsigned long long ClothKey(IDirect3DBaseTexture9* pattern, const float col[3][4]) {
    unsigned long long h = 1469598103934665603ull;
    h ^= reinterpret_cast<uintptr_t>(pattern);
    h *= 1099511628211ull;
    for (int c = 0; c < 3; ++c)
        for (int k = 0; k < 3; ++k) {
            float v = col[c][k];
            if (!std::isfinite(v)) v = 0.0f;
            const unsigned q = static_cast<unsigned>(max(0.0f, min(1.0f, v)) * 255.0f + 0.5f);
            h ^= q; h *= 1099511628211ull;
        }
    return h;
}

void ReleaseClothCache() {
    for (auto& kv : g_clothCache) {
        if (kv.second.pattern) kv.second.pattern->Release();
        if (kv.second.generated) kv.second.generated->Release();
    }
    g_clothCache.clear(); g_clothCutoutTex.clear(); ReleaseClothTileLayouts(); g_clothRemap = nullptr;
}

// Does this draw sample the PATTERN at a single point?
//
// The pattern rides a second uv set, and the assumption all along was that a varying pattern
// therefore needs the mesh to relate the two sets. The captured vertices say otherwise. Sampled
// across a cloth draw, uv1 comes in three shapes:
//
//     uv1 == uv0                 the pattern shares the garment's own unwrap
//     uv1 == one constant        every vertex reads the SAME pattern texel
//     uv1 genuinely varying      the hard case
//
// The middle one is common - `uv1(864 269)` on every sampled vertex of one garment, `uv1(0 0)`
// on another - and it means the whole surface takes ONE colour however detailed the pattern is.
// pat_llheart01, the tiling heart the player has on her underwear, is a 64x64 texture with two
// regions; read at a single uv it yields a single colour, which is exactly why the unmodded game
// shows that garment as flat colour with the logo barely distinguishable.
//
// So the uniformity test was asking the wrong question. It asked whether the pattern TEXTURE has
// more than one colour. What matters is whether this GARMENT samples more than one texel of it.
unsigned g_onePointWhy = 0;

// Every way out of this says which way it was. It returned false on the underwear and reported
// nothing, which is the same silence that made "the draw is not converted" look like a finding
// when the draw was converted all along. A predicate used as a gate has to be able to explain a
// refusal, or the next step is guesswork again.
// How this garment's pattern coordinates relate to its albedo coordinates. Measured per draw
// from the vertex stream; the three answers need three different treatments and conflating them
// is what stalled the underwear for four builds.
enum class PatternUv { OnePoint, Affine, Independent };

// Filled when the answer is Affine: uv1_raw = scale*uv0_raw + offset, in RAW SHORT units.
float g_patScale[2] = {1.0f, 1.0f};
float g_patOffset[2] = {0.0f, 0.0f};
PatternUv g_patMode = PatternUv::Independent;

bool ClothPatternIsSampledAtOnePoint(float* outU, float* outV) {
    const bool tell = (g_onePointWhy < 6);
    auto no = [&](const char* why) -> bool {
        if (tell) { ++g_onePointWhy; Log("ONE-POINT declined: %s", why); }
        return false;
    };
    if (g_curLayout.texcoord1Offset < 0)
        return no("this vertex declaration has NO TEXCOORD1 - the pattern's own coordinates are "
                  "not in the stream at all");
    if (g_curLayout.texcoord1Type != D3DDECLTYPE_SHORT2) {
        if (tell) {
            ++g_onePointWhy;
            Log("ONE-POINT declined: TEXCOORD1 is type %d, not SHORT2 - the reader only decodes "
                "short2", g_curLayout.texcoord1Type);
        }
        return false;
    }
    if (!g_stream0 || !g_stream0Stride) return no("no stream 0 bound");
    if (g_curDrawVertexCount < 8) {
        if (tell) {
            ++g_onePointWhy;
            Log("ONE-POINT declined: only %u vertices in this draw, the sampler wants 8",
                g_curDrawVertexCount);
        }
        return false;
    }
    if (!VertexRangeFits(g_stream0, g_stream0Offset, g_curDrawFirstVertex,
                         g_curDrawVertexCount, g_stream0Stride))
        return no("the vertex range does not fit the buffer - reading it would be out of bounds");
    void* mapped = nullptr;
    if (FAILED(g_stream0->Lock(g_stream0Offset + g_curDrawFirstVertex * g_stream0Stride,
                               g_curDrawVertexCount * g_stream0Stride, &mapped,
                               D3DLOCK_READONLY | D3DLOCK_NOSYSLOCK)) || !mapped)
        return no("the vertex buffer refused a read lock");
    const unsigned char* base = static_cast<const unsigned char*>(mapped);
    const UINT off = static_cast<UINT>(g_curLayout.texcoord1Offset);
    const UINT step = g_curDrawVertexCount / 16 ? g_curDrawVertexCount / 16 : 1;
    const short* first = reinterpret_cast<const short*>(base + off);
    const short u0 = first[0], v0 = first[1];
    bool constant = true;
    for (UINT i = step; i < g_curDrawVertexCount && constant; i += step) {
        const short* p = reinterpret_cast<const short*>(base + i * g_stream0Stride + off);
        if (p[0] != u0 || p[1] != v0) constant = false;
    }
    // The spread, so "not constant" is a measurement rather than a verdict. A pattern read
    // across a handful of texels is still nearly one colour; one read across the whole map is
    // not, and the numbers separate those.
    short lo[2] = {u0, v0}, hi[2] = {u0, v0};
    for (UINT i = step; i < g_curDrawVertexCount; i += step) {
        const short* p = reinterpret_cast<const short*>(base + i * g_stream0Stride + off);
        for (int c = 0; c < 2; ++c) {
            if (p[c] < lo[c]) lo[c] = p[c];
            if (p[c] > hi[c]) hi[c] = p[c];
        }
    }
    // IS UV1 AN AFFINE FUNCTION OF UV0?
    //
    // "It varies" closes the one-colour shortcut but not the problem. The pattern is a decal
    // laid over the garment, and a decal is normally placed by SCALING the garment's own unwrap
    // rather than by a second independent layout. If uv1 = uv0 * s + o holds, the pattern can be
    // resolved per texel in the DIFFUSE's space - sample it at (uv * s + o) - and the mesh is not
    // needed after all. If it does not hold, the mesh genuinely is required and that is worth
    // knowing for certain rather than assuming in either direction.
    //
    // Least squares over the sampled vertices, then the WORST residual - a mean residual can hide
    // a fit that is right in the middle and wrong at the edges, which for a decal is exactly
    // where it would show.
    double fitS[2] = {0, 0}, fitO[2] = {0, 0}, worst[2] = {0, 0};
    bool fitted = false;
    if (g_curLayout.texcoordOffset >= 0 && g_curLayout.texcoordType == D3DDECLTYPE_SHORT2) {
        const UINT off0 = static_cast<UINT>(g_curLayout.texcoordOffset);
        double n = 0, sx[2] = {0, 0}, sy[2] = {0, 0}, sxx[2] = {0, 0}, sxy[2] = {0, 0};
        for (UINT i = 0; i < g_curDrawVertexCount; i += step) {
            const short* a = reinterpret_cast<const short*>(base + i * g_stream0Stride + off0);
            const short* b = reinterpret_cast<const short*>(base + i * g_stream0Stride + off);
            for (int c = 0; c < 2; ++c) {
                const double x = a[c], y = b[c];
                sx[c] += x; sy[c] += y; sxx[c] += x * x; sxy[c] += x * y;
            }
            n += 1.0;
        }
        if (n >= 3.0) {
            fitted = true;
            for (int c = 0; c < 2 && fitted; ++c) {
                const double den = n * sxx[c] - sx[c] * sx[c];
                if (den == 0.0) { fitted = false; break; }
                fitS[c] = (n * sxy[c] - sx[c] * sy[c]) / den;
                fitO[c] = (sy[c] - fitS[c] * sx[c]) / n;
            }
            if (fitted)
                for (UINT i = 0; i < g_curDrawVertexCount; i += step) {
                    const short* a =
                        reinterpret_cast<const short*>(base + i * g_stream0Stride + off0);
                    const short* b =
                        reinterpret_cast<const short*>(base + i * g_stream0Stride + off);
                    for (int c = 0; c < 2; ++c) {
                        const double r = fabs(fitS[c] * a[c] + fitO[c] - b[c]);
                        if (r > worst[c]) worst[c] = r;
                    }
                }
        }
    }
    g_stream0->Unlock();
    // Record the fit so the caller can use it. AFFINE with a tiny residual means the pattern
    // rides the garment's own unwrap and can be resolved in the albedo's texture space with no
    // mesh at all - garment #1 of the 2026-09-09 run fits u = 1.0000*u0 + 0.0 with a worst
    // residual of EXACTLY 0.0, i.e. the two uv sets are the same numbers.
    //
    // 8 raw short units is the tolerance, which at 1/1024 is under a hundredth of a uv - far
    // tighter than a pattern texel on any of these maps, so a fit this good cannot be a
    // coincidence of sampling.
    g_patMode = PatternUv::Independent;
    if (constant) {
        g_patMode = PatternUv::OnePoint;
    } else if (fitted && worst[0] < 8.0 && worst[1] < 8.0) {
        g_patMode = PatternUv::Affine;
        g_patScale[0] = static_cast<float>(fitS[0]);
        g_patScale[1] = static_cast<float>(fitS[1]);
        g_patOffset[0] = static_cast<float>(fitO[0]);
        g_patOffset[1] = static_cast<float>(fitO[1]);
    }
    if (!constant) {
        if (tell) {
            ++g_onePointWhy;
            Log("ONE-POINT declined: TEXCOORD1 VARIES across this draw - u spans %d..%d, "
                "v spans %d..%d (raw shorts, /1024).",
                lo[0], hi[0], lo[1], hi[1]);
            if (fitted)
                Log("      UV1 vs UV0 least-squares fit: u = %.4f*u0 %+.1f (worst residual %.1f), "
                    "v = %.4f*v0 %+.1f (worst residual %.1f) -> %s",
                    fitS[0], fitO[0], worst[0], fitS[1], fitO[1], worst[1],
                    (worst[0] < 8.0 && worst[1] < 8.0)
                        ? "AFFINE. The pattern can be resolved in the diffuse's own texture space "
                          "by sampling it at uv*s+o - NO MESH NEEDED."
                        : "NOT affine - the two unwraps are independent and the mesh really is "
                          "required to relate them.");
            else
                Log("      UV1 vs UV0 fit could not be computed (no short2 TEXCOORD0, or too few "
                    "distinct samples) - this says nothing either way.");
        }
        return false;
    }
    *outU = static_cast<float>(u0) * kShortUVScale;
    *outV = static_cast<float>(v0) * kShortUVScale;
    return true;
}

unsigned g_clothOnePointGen = 0;
unsigned g_clampReports = 0, g_clampDumps = 0, g_clothAffineGen = 0, g_clothDominantGen = 0;
unsigned g_clothPadReports = 0;
unsigned g_genDumps = 0;
unsigned g_clothNoPattern = 0;


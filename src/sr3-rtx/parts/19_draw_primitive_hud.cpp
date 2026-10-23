// parts/19_draw_primitive_hud.cpp - DrawPrimitive, HUD and composite rebuilding, user-pointer draws.
// NOT a standalone translation unit: #included, in order, by ../sr3rtx.cpp inside its anonymous
// namespace. Originally sr3rtx.cpp lines 13579-14286 (old file:line citations map here).
HRESULT WINAPI Hook_DrawPrimitive(IDirect3DDevice9* dev, D3DPRIMITIVETYPE type, UINT start,
                                  UINT count) {
    ++g_drawsTotal;
    ++g_drawIndexThisFrame;
    if (OcclusionProxyDraw(dev, count, count * 3, start)) return D3D_OK;
    // Was missing here, so this path used whatever the previous indexed draw left behind and
    // keyed the mesh-albedo cache on the wrong mesh. `start` identifies the run of vertices for
    // a non-indexed draw the way baseVertex+minIndex does for an indexed one.
    g_meshKeyBaseVertex = start;
    g_curDrawFirstVertex = start;
    // A primitive count is not a vertex count, and `count * 3` is only right for a triangle LIST.
    // For a strip or fan it over-estimates by roughly three times, which would send a probe
    // reading past the end of the mesh. Left at zero instead: the probes that use this window all
    // require a minimum size, so they simply decline rather than read something arbitrary.
    g_curDrawVertexCount = 0;
    g_curDrawIndexed = false;
    // 2026-09-18 AUDIT FIX: the rest of the indexed-draw shape, reset so a non-indexed draw is
    // described by ITS OWN values instead of whatever the previous INDEXED draw left behind.
    // BeginFFP folds minIndex/startIndex/primCount/baseVertex/primType into both the layer key
    // and the albedo-churn identity, so two unrelated non-indexed draws used to hash to one
    // identity (whatever the last indexed draw's leftover fields happened to be) and the churn
    // report could fire on a draw that never churned. primCount and primType have real values
    // for a non-indexed draw too (count and type, both already in hand here); baseVertex,
    // minIndex and startIndex describe indices this draw does not have, so they are zeroed
    // rather than carried over.
    g_curDrawBaseVertex = 0;
    g_curDrawMinIndex = 0;
    g_curDrawStartIndex = 0;
    g_curDrawPrimCount = count;
    g_curDrawPrimType = type;
    NoteIfFinalComposite(dev);   // injectProbe/injectControl PART 1/2: before Classify runs
    EmitLight(dev);   // harvest lights before anything else touches device state
    FFPScope scope;
    const Disp d = BeginFFP(dev, scope);
    ClearBackBufferOnce(dev);
    ProbeAtlasComposite(dev, count * 3, count, d);
    RecordFrameDraw(dev, count * 3, count, d);   // the frame dump covered only indexed draws
    if (d == Disp::Skip) { ++g_skippedDraws; g_worldWrittenSinceDraw = false; return D3D_OK; }
    if (d == Disp::PassThrough) NotePassedThrough(count);
    if (IsAtlasComposite(count, d) && DrawCompositeFixedFunction(dev)) {
        g_worldWrittenSinceDraw = false;
        return D3D_OK;
    }
    if (d == Disp::PassThrough) {
        if (g_settings.skipDeclinedPassThrough && g_lastVS) {
            ++g_declinedPassSkipped;
            EndFFP(dev, scope);
            g_worldWrittenSinceDraw = false;
            return D3D_OK;
        }
        ++g_passForwarded;
    }
    const bool ui = (d == Disp::Convert) ? false : BeginUIDemote(dev, d == Disp::Hide);
    const unsigned marked = (d == Disp::Mark) ? BeginMark(dev, g_markClearAllStages) : 0u;
    SyncBridgeVS(dev, (d == Disp::Convert) ? nullptr : g_lastVS);
    const InjectSuppression injectSup = BeforeShimDraw(dev, d, count);
    const HRESULT hr = g_origDrawPrimitive(dev, type, start, count);
    AfterShimDrawSite(dev, injectSup);
    EndMark(dev, marked);
    EndUIDemote(dev, ui);
    EndFFP(dev, scope);
    g_worldWrittenSinceDraw = false;
    return hr;
}

// ---------------------------------------------------------------- rebuilding the HUD
//
// MEASURED, not assumed. The HUD FORMAT probe asked the DEVICE directly and every HUD draw in the
// 2026-09-04 run answered identically:
//
//     stride 28 | vertexShader 14EB3380  pixelShader 15F32A90  fvf 0  (so: a declaration)
//     offset  0  float4 POSITION    offset 16  float2 TEXCOORD0    offset 24  d3dcolor COLOR0
//     alphaBlend=1  src=5 (SRCALPHA)  dst=6 (INVSRCALPHA)  alphaTest=0
//     vtx0  pos 2560.000    0.000 0 0   uv 1,0   colour FFFFFFFF
//     vtx1  pos 2560.000 1440.000 0 0   uv 1,1   colour FFFFFFFF
//
// 2560x1440 is the screen. The positions are ALREADY IN SCREEN PIXELS, which is exactly what
// D3D9 expects from a D3DFVF_XYZRHW vertex: pre-transformed, the whole transform pipeline
// skipped. So rebuilding this as fixed function is a REORDER of 28 bytes and nothing else - no
// projection to invert, no matrix to guess, no space to convert.
//
// FVF field order is fixed by D3D9 and is NOT the game's order: position, then diffuse, then
// texcoord. The game stores texcoord BEFORE colour, so the two must swap on the way through;
// copying the struct wholesale would land the colour bytes in the uv and paint the HUD with
// garbage.
//
// w is 0 in the source, which would be a divide-by-zero as an RHW. The game's vertex shader
// plainly ignores it. We write 1.
struct HudSrcVertex { float pos[4]; float uv[2]; DWORD color; };
struct HudFfpVertex { float x, y, z, rhw; DWORD color; float u, v; };
static_assert(sizeof(HudSrcVertex) == 28, "HUD source vertex must match the measured stride");
static_assert(sizeof(HudFfpVertex) == 28, "XYZRHW|DIFFUSE|TEX1 is 28 bytes");

std::vector<HudFfpVertex> g_hudVerts;
unsigned g_hudConverted = 0, g_hudConvertRefused = 0, g_hudDrawFailed = 0, g_hudRebuiltOnly = 0, g_hudRawAfterRebuild = 0;
HRESULT g_hudLastFail = 0;

// How many vertices a primitive count means. Getting this wrong reads past the end of the game's
// buffer, so it is spelled out per type rather than assuming triangle lists.
UINT VertsForPrimitives(D3DPRIMITIVETYPE type, UINT count) {
    switch (type) {
        case D3DPT_POINTLIST:     return count;
        case D3DPT_LINELIST:      return count * 2;
        case D3DPT_LINESTRIP:     return count + 1;
        case D3DPT_TRIANGLELIST:  return count * 3;
        case D3DPT_TRIANGLESTRIP: return count + 2;
        case D3DPT_TRIANGLEFAN:   return count + 2;
        default:                  return 0;
    }
}

// True if the draw was re-issued as fixed function. False means "not the format we measured",
// and the caller then passes the game's own draw through - an unrecognised HUD element is left
// exactly as it is today rather than dropped. This can only add HUD, never remove it.
bool DrawHudFixedFunction(IDirect3DDevice9* dev, D3DPRIMITIVETYPE type, UINT count,
                          const void* vertices, UINT stride) {
    if (!vertices || stride != sizeof(HudSrcVertex)) { ++g_hudConvertRefused; return false; }
    const UINT verts = VertsForPrimitives(type, count);
    if (!verts || verts > 4096) { ++g_hudConvertRefused; return false; }

    const HudSrcVertex* src = static_cast<const HudSrcVertex*>(vertices);
    g_hudVerts.resize(verts);
    for (UINT i = 0; i < verts; ++i) {
        g_hudVerts[i].x     = src[i].pos[0];
        g_hudVerts[i].y     = src[i].pos[1];
        g_hudVerts[i].z     = src[i].pos[2];
        g_hudVerts[i].rhw   = 1.0f;
        g_hudVerts[i].color = src[i].color;
        g_hudVerts[i].u     = src[i].uv[0];
        g_hudVerts[i].v     = src[i].uv[1];
    }

    // Save what we are about to change. A state block would be tidier, but SR3 comes up through
    // D3D9Ex and this runs inside the game's own draw, so the fewer device objects created on
    // this path the better.
    IDirect3DVertexShader9* oldVS = nullptr;
    IDirect3DPixelShader9* oldPS = nullptr;
    IDirect3DVertexDeclaration9* oldDecl = nullptr;
    DWORD oldFVF = 0;
    dev->GetVertexShader(&oldVS);
    dev->GetPixelShader(&oldPS);
    dev->GetVertexDeclaration(&oldDecl);
    dev->GetFVF(&oldFVF);

    g_internal = true;
    g_internalThread = GetCurrentThreadId();
    SyncBridgeVS(dev, nullptr);
    // ------------------------------------------------------------------------------------
    // THE PIXEL SHADER STAYS.
    //
    // The first version of this nulled it, the way BeginFFP does for world geometry, and the
    // result named its own mistake: the menu video went BLACK AND WHITE, menu text and
    // backgrounds vanished, and the in-game HUD disappeared while the main menu's survived.
    //
    // Those are one symptom, not four. SR3's UI pixel shader is not a plain texture fetch - a
    // video frame is Y/Cr/Cb planes across several stages combined into colour, and text is a
    // channel selected out of a font atlas. Replacing all of that with a single-stage
    // texture*diffuse MODULATE keeps the geometry and discards the shading, so the elements
    // that happen to BE a plain textured quad (the main menu's) survived and everything else
    // did not. Luma alone is exactly a greyscale video.
    //
    // Nulling it is right for the WORLD, where Remix wants the albedo texture and not the
    // shader's output. It is wrong here.
    //
    // And it should still satisfy Remix: vertex capture is about capturing VERTEX shader
    // output, and its refusal message names that - so a draw with fixed-function vertex
    // processing and the game's own pixel shader is the combination this needs. If Remix
    // declines it anyway the HUD disappears entirely, which is an unmistakable result rather
    // than an ambiguous one, and uiKeepPixelShader=0 restores today's behaviour with no
    // rebuild.
    if (!g_settings.uiKeepPixelShader) g_origSetPixelShader(dev, nullptr);
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
    // injectProbe/injectControl PART 1/2: this draw is always RHW, VS already nulled above.
    const InjectSuppression injectSup =
        BeforeShimDrawSite(dev, InjectSite::HudRebuild, false, count, true);

    // STATE THIS DRAW CHANGES IS SAVED AND PUT BACK.
    //
    // Found by audit, not by a symptom, which is the only reason it was found at all. Everything
    // below is written with g_origSetRenderState / g_origSetTextureStageState under g_internal,
    // so it bypasses the hooks - which means the shim's own shadow copies keep the GAME's values
    // while the device holds ours. ShadowGetRS and ShadowGetTSS are what the classifier reads to
    // decide what a draw is, so letting the two drift is a way to make every later decision wrong
    // on evidence that looks right.
    //
    // D3DRS_LIGHTING was the live one: it sat OUTSIDE the pixel-shader guard, so it ran on every
    // HUD draw - about 28 a frame - and was never restored. Pre-transformed vertices skip D3D9
    // lighting anyway, so the call was buying nothing and leaving the device changed.
    DWORD prevLighting = 0;
    dev->GetRenderState(D3DRS_LIGHTING, &prevLighting);
    struct SavedTss { DWORD stage, type, value; };
    SavedTss saved[10];
    unsigned savedCount = 0;
    if (!g_settings.uiKeepPixelShader) {
        static const DWORD kStage[10] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1};
        static const D3DTEXTURESTAGESTATETYPE kType[10] = {
            D3DTSS_COLOROP, D3DTSS_COLORARG1, D3DTSS_COLORARG2,
            D3DTSS_ALPHAOP, D3DTSS_ALPHAARG1, D3DTSS_ALPHAARG2,
            D3DTSS_TEXCOORDINDEX, D3DTSS_TEXTURETRANSFORMFLAGS,
            D3DTSS_COLOROP, D3DTSS_ALPHAOP};
        for (unsigned i = 0; i < 10; ++i) {
            DWORD v = 0;
            if (SUCCEEDED(dev->GetTextureStageState(kStage[i], kType[i], &v)))
                saved[savedCount++] = SavedTss{kStage[i], static_cast<DWORD>(kType[i]), v};
        }
    }

    // Only when the FIXED-FUNCTION pixel pipeline is really in use. With the game's pixel
    // shader bound these stage states are ignored for colour, and setting stage 1 to DISABLE
    // would take away a texture the shader still samples - which is precisely how the video lost
    // its colour planes.
    if (!g_settings.uiKeepPixelShader) {
    // texture * vertex colour, on both channels. The HUD tints its quads through COLOR0 - the
    // probe caught FFFFFFFF, but a faded or coloured element will not be white, and dropping
    // DIFFUSE here would make every fade-out stay solid.
    g_origSetTextureStageState(dev, 0, D3DTSS_COLOROP,   D3DTOP_MODULATE);
    g_origSetTextureStageState(dev, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    g_origSetTextureStageState(dev, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    g_origSetTextureStageState(dev, 0, D3DTSS_ALPHAOP,   D3DTOP_MODULATE);
    g_origSetTextureStageState(dev, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    g_origSetTextureStageState(dev, 0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    g_origSetTextureStageState(dev, 0, D3DTSS_TEXCOORDINDEX, 0);
    g_origSetTextureStageState(dev, 0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    g_origSetTextureStageState(dev, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    g_origSetTextureStageState(dev, 1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    // Only meaningful alongside the fixed-function pixel pipeline, and only inside this guard so
    // it is always paired with the restore below.
    g_origSetRenderState(dev, D3DRS_LIGHTING, FALSE);
    }
    g_internal = false;

    const HRESULT hr = g_origDrawPrimitiveUP(dev, type, count, g_hudVerts.data(),
                                             sizeof(HudFfpVertex));
    AfterShimDrawSite(dev, injectSup);

    g_internal = true;
    g_internalThread = GetCurrentThreadId();
    dev->SetFVF(oldFVF);
    g_origSetVertexDeclaration(dev, oldDecl);
    SyncBridgeVS(dev, oldVS);
    if (!g_settings.uiKeepPixelShader) g_origSetPixelShader(dev, oldPS);
    for (unsigned i = 0; i < savedCount; ++i)
        g_origSetTextureStageState(dev, saved[i].stage,
                                   static_cast<D3DTEXTURESTAGESTATETYPE>(saved[i].type),
                                   saved[i].value);
    g_origSetRenderState(dev, D3DRS_LIGHTING, prevLighting);
    g_internal = false;

    if (oldVS) oldVS->Release();
    if (oldPS) oldPS->Release();
    if (oldDecl) oldDecl->Release();

    if (SUCCEEDED(hr)) { ++g_hudConverted; return true; }
    // D3D9 itself refused the draw - a different failure from "not the format we measured", and
    // worth its own counter: with the pixel shader kept, this is where a mismatch between what
    // the shader declares and what fixed-function vertex processing emits would show up.
    ++g_hudDrawFailed;
    g_hudLastFail = hr;
    return false;
}

// ---------------------------------------------------------------- rebuilding the composites
//
// THE CHARACTER'S SKIN. Measured on both sides of the capture switch by the atlas snoop:
//
//     capture ON    atlas 2048x1024  mean 182.7      atlas 1024x512  mean 169.6
//     capture OFF   atlas 2048x1024  mean   0.0      atlas 1024x512  mean   0.0
//
// and the probe named the draws that build them:
//
//     2048x1024  PASS  v=6 p=2  ps='mask_mapSampler'      2 DXT5 textures, stages 0 and 3
//     1024x512   PASS  v=6 p=2  ps='baseSampler'          2 DXT5 textures, stages 0 and 3
//     1024x512   PASS  v=6 p=2  ps='base_samplerSampler'  SRCALPHA/INVSRCALPHA
//
// Six vertices, two triangles, no projTM: a QUAD covering its render target, running a real
// pixel shader over two source textures the same size as the target. A 1:1 blit.
//
// So we do not need the game's vertices at all - a full-target quad reproduces them. What we do
// need is its PIXEL SHADER, and that is the whole point: the menu video came back in colour the
// moment the shader was kept, which proved that fixed-function VERTEX processing plus the game's
// own pixel shader is a combination Remix executes with capture off. Vertex capture is about
// capturing vertex shader output; nulling the vertex shader is the entire requirement.
//
// The half-texel offset is the standard D3D9 one. Source and target are the same size, so a
// 1:1 blit lands exactly on texel centres only with it, and without it the composite would be
// half a pixel soft - which on a mask map is the difference between a clean edge and a seam.
unsigned g_compositeFfp = 0, g_compositeFfpFailed = 0;
HRESULT g_compositeLastFail = 0;

bool DrawCompositeFixedFunction(IDirect3DDevice9* dev) {
    const float w = static_cast<float>(g_rt0Width);
    const float h = static_cast<float>(g_rt0Height);
    if (w < 1.0f || h < 1.0f) return false;
    const DWORD white = 0xFFFFFFFFu;
    const HudFfpVertex quad[6] = {
        {-0.5f,     -0.5f,     0.0f, 1.0f, white, 0.0f, 0.0f},
        { w - 0.5f, -0.5f,     0.0f, 1.0f, white, 1.0f, 0.0f},
        {-0.5f,      h - 0.5f, 0.0f, 1.0f, white, 0.0f, 1.0f},
        { w - 0.5f, -0.5f,     0.0f, 1.0f, white, 1.0f, 0.0f},
        { w - 0.5f,  h - 0.5f, 0.0f, 1.0f, white, 1.0f, 1.0f},
        {-0.5f,      h - 0.5f, 0.0f, 1.0f, white, 0.0f, 1.0f},
    };

    IDirect3DVertexShader9* oldVS = nullptr;
    IDirect3DVertexDeclaration9* oldDecl = nullptr;
    DWORD oldFVF = 0;
    dev->GetVertexShader(&oldVS);
    dev->GetVertexDeclaration(&oldDecl);
    dev->GetFVF(&oldFVF);

    g_internal = true;
    g_internalThread = GetCurrentThreadId();
    SyncBridgeVS(dev, nullptr);      // the pixel shader is deliberately left alone
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
    g_internal = false;

    // injectProbe/injectControl PART 1/2: this draw is always RHW, VS already nulled above.
    const InjectSuppression injectSup =
        BeforeShimDrawSite(dev, InjectSite::AtlasComposite, false, 2, true);
    const HRESULT hr = g_origDrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 2, quad,
                                             sizeof(HudFfpVertex));
    AfterShimDrawSite(dev, injectSup);

    g_internal = true;
    g_internalThread = GetCurrentThreadId();
    dev->SetFVF(oldFVF);
    g_origSetVertexDeclaration(dev, oldDecl);
    SyncBridgeVS(dev, oldVS);
    g_internal = false;

    if (oldVS) oldVS->Release();
    if (oldDecl) oldDecl->Release();

    if (SUCCEEDED(hr)) { ++g_compositeFfp; return true; }
    ++g_compositeFfpFailed;
    g_compositeLastFail = hr;
    return false;
}

// Is this the atlas composite? Tightly gated, because a false positive here replaces a real draw
// with a quad and would corrupt whatever it hit.
bool IsAtlasComposite(UINT primitiveCount, Disp d) {
    if (!g_settings.compositeFfp || d != Disp::PassThrough) return false;
    if (g_curVS.usesProjTM) return false;              // screen-space only
    if (primitiveCount != 2) return false;             // a quad, two triangles
    if (!g_rt0Width || g_rt0Width < 1024 || g_rt0Height < 512) return false;
    if (g_backBufferW && g_rt0Width == g_backBufferW && g_rt0Height == g_backBufferH)
        return false;                                  // never the screen
    // NOT a post-process target. The size gate alone also admitted the 1280x720
    // IR_GBuffer_Depth resolve the probe recorded, and replacing a post-process draw would
    // change the frame in a way that had nothing to do with the character - then get blamed on
    // the character. The post chain is made of SCREEN-SHAPED fractions (1280x720, 640x360,
    // 320x180 ... all 16:9); the character atlases are 2048x1024 and 1024x512, which are 2:1.
    // So the aspect ratio separates them exactly, and it needs nothing to have been observed
    // first - unlike matching against the atlas sizes the snoop records, which are not known
    // until the game has already written one.
    if (g_backBufferW && g_backBufferH) {
        const float screenAspect = static_cast<float>(g_backBufferW) / g_backBufferH;
        const float targetAspect = static_cast<float>(g_rt0Width) / g_rt0Height;
        if (std::fabs(targetAspect - screenAspect) < 0.01f) return false;
    }
    return true;
}

// ---------------------------------------------------------------- what are we losing?
//
// With capture off, every draw we PASS THROUGH is a shader draw Remix declines - so it is simply
// not drawn. That is the whole "the in-game HUD is invisible and the sub-menus have no text or
// background" report. This names them by shader instead of leaving it to be guessed, which is
// the rule that found the hair, the triangle strips and the HUD itself.
// sampler used to be a `const char*` set to g_curPS.firstSampler - the SAME global array on
// every call, since g_curPS is one struct instance reused for every shader. Every row therefore
// stored the identical pointer, `sampler == name` was true by construction regardless of which
// shader was actually current when the row was FIRST recorded, and the row printed whatever
// shader happened to be bound at REPORT time. A real copy, compared by content, is what the
// other name tables in this file already do.
struct PassCensusRow { char sampler[28]; UINT w, h; unsigned count; };
constexpr unsigned kPassCensusRows = 40;
PassCensusRow g_passCensus[kPassCensusRows];
unsigned g_passCensusUsed = 0;

void NotePassedThrough(UINT primitiveCount) {
    if (!g_settings.passCensus) return;
    const char* name = g_curPS.firstSampler;
    for (unsigned i = 0; i < g_passCensusUsed; ++i) {
        if (!strcmp(g_passCensus[i].sampler, name) && g_passCensus[i].w == g_rt0Width &&
            g_passCensus[i].h == g_rt0Height) { ++g_passCensus[i].count; return; }
    }
    if (g_passCensusUsed >= kPassCensusRows) return;
    strncpy_s(g_passCensus[g_passCensusUsed].sampler, name, _TRUNCATE);
    g_passCensus[g_passCensusUsed].w = g_rt0Width;
    g_passCensus[g_passCensusUsed].h = g_rt0Height;
    g_passCensus[g_passCensusUsed].count = 1;
    ++g_passCensusUsed;
    (void)primitiveCount;
}

// ---------------------------------------------------------------- the user-pointer draw calls
//
// PURE INSTRUMENT. Both of these pass the draw through untouched and change nothing about what
// the game or Remix does. They exist to answer one question with data instead of a hypothesis:
// WHERE IS THE HUD?
//
// The evidence that the question is open: the complete frame dump of 2026-09-03 records every
// DrawPrimitive and DrawIndexedPrimitive of one gameplay frame - 5,200 of them - and contains no
// HUD at all. Its 77 screen-space draws are the deferred resolve, the blur, the auto-exposure
// reduction, the LUT and the two composites, every one of them named in the dump. The final draw
// of the frame is the composite into the back buffer, and nothing follows it.
//
// Static analysis of the exe could not settle it either. Scanning .text for
// `call dword ptr [reg+0x148]` finds ZERO DrawIndexedPrimitive call sites even though the game
// makes thousands per frame, so SR3 does not call the device vtable with an inline displacement
// and the same scan proves nothing about the UP slots. The exe does import d3d9.dll directly and
// its .text is 12 MB; the call is in there, reached some other way. That is a dead end for THIS
// question, not a finding - so measure it at the boundary we already own.
//
// If these counters read zero in gameplay, the HUD is somewhere else again and the next place to
// look is a second device or swap chain. If they read non-zero, every one of those draws has
// been reaching Remix unclassified since the fork, which would explain flat planes standing in
// the world in front of the camera: Remix's vertex capture rebuilds a screen-space quad as world
// geometry unless something tells it the draw is UI, and nothing has ever told it that for a
// draw this shim never saw.
HRESULT WINAPI Hook_DrawPrimitiveUP(IDirect3DDevice9* dev, D3DPRIMITIVETYPE type, UINT count,
                                    const void* vertices, UINT stride) {
    ++g_drawsTotal;
    ++g_drawIndexThisFrame;
    ++g_upDrawsTotal;
    NoteIfFinalComposite(dev);   // injectProbe/injectControl PART 1/2

    // THE HUD, named from the frame dump of 2026-09-04 rather than guessed from a screenshot.
    // That dump holds 76 user-pointer draws and they split cleanly into two populations:
    //
    //   56 draws | zw=1 zt=0 | no projTM | Diffuse_MapSampler | 6 textures | 6-48 vertices
    //            | ALL of them into the BACK BUFFER, after the frame's final composite
    //   20 draws | zw=0 zt=1 | Orbital_mapSampler | scattered through the world passes
    //
    // The first block is the HUD: screen space, drawn last, straight onto the finished image,
    // with the depth test OFF - which is precisely what "draw on top of everything" means. The
    // second block is depth-TESTED and sits in the middle of the world passes, so it is an
    // in-world effect and must not be touched. `zt` separates them exactly, with no overlap, so
    // the test is a property of the draw rather than a threshold anybody tuned.
    //
    // Left alone, Remix's vertex capture rebuilds a screen-space quad as world geometry, which
    // puts a flat plane in front of the camera. The demote is the fix this file already carries
    // for the OTHER screen-space quads: an identity projection reads as orthographic, and with
    // rtx.orthographicIsUI = True Remix classifies the draw UI and rasterises it as a 2D
    // overlay. Session 14 recorded that behaviour as a FAILURE when the goal was to hide world
    // geometry; for a HUD a 2D overlay is exactly the goal, so the same measured behaviour is
    // the feature here.
    const bool hud = g_settings.uiDemoteUP && !g_curVS.usesProjTM &&
                     !ShadowGetRS(dev, D3DRS_ZENABLE);
    if (hud) ++g_upHudDemoted; else ++g_upLeftAlone;
    // injectProbe/injectControl PART 2(b): this population is the established "ALL of them
    // into the BACK BUFFER, after the frame's final composite" (see the comment above) - the
    // right place to notice the first HUD draw of the frame and, if the composite has already
    // been seen and the quad has not gone out yet, issue it right here, before anything else
    // about this particular HUD draw runs.
    NoteFirstHudAndMaybeInject(dev, hud);

    // ---------------------------------------------------------------- what IS a HUD vertex?
    //
    // Asked of the DEVICE, not of our own tracking, because our tracking can be stale in exactly
    // the way that matters here: SetFVF is vtable slot 89 and this shim has never hooked it, so
    // if the HUD draws with an FVF rather than a declaration then g_curDecl still describes some
    // earlier world mesh and every conclusion drawn from it would be about the wrong draw.
    //
    // The first question this answers is bigger than the format. If GetVertexShader returns
    // NULL, these draws are ALREADY fixed function - Remix accepts them whatever
    // rtx.useVertexCapture says, and the whole "shader draws are declined" story does not apply
    // to the HUD at all. If it returns a shader, the HUD needs rebuilding as fixed function
    // before it can survive with capture off, and this dump is the input to writing that.
    if (hud && g_hudFormatDumps < 6) {
        ++g_hudFormatDumps;
        IDirect3DVertexShader9* vs = nullptr;
        IDirect3DPixelShader9* ps = nullptr;
        IDirect3DVertexDeclaration9* vd = nullptr;
        DWORD fvf = 0;
        dev->GetVertexShader(&vs);
        dev->GetPixelShader(&ps);
        dev->GetVertexDeclaration(&vd);
        dev->GetFVF(&fvf);
        Log("HUD FORMAT #%u: prims=%u stride=%u | vertexShader=%p pixelShader=%p decl=%p "
            "fvf=0x%08lX | alphaBlend=%lu src=%lu dst=%lu alphaTest=%lu ref=%lu | tex0=%p",
            g_hudFormatDumps, count, stride, static_cast<void*>(vs), static_cast<void*>(ps),
            static_cast<void*>(vd), fvf,
            ShadowGetRS(dev, D3DRS_ALPHABLENDENABLE), ShadowGetRS(dev, D3DRS_SRCBLEND),
            ShadowGetRS(dev, D3DRS_DESTBLEND), ShadowGetRS(dev, D3DRS_ALPHATESTENABLE),
            ShadowGetRS(dev, D3DRS_ALPHAREF), static_cast<void*>(g_curTexture[0]));
        if (vd) {
            D3DVERTEXELEMENT9 all[64]{};
            UINT n = 0;
            if (SUCCEEDED(vd->GetDeclaration(all, &n))) {
                for (UINT i = 0; i < n; ++i) {
                    if (all[i].Stream == 0xFF) break;
                    Log("    elem stream=%u offset=%2u type=%-9s(%u) usage=%-12s index=%u",
                        all[i].Stream, all[i].Offset, DeclTypeName(all[i].Type), all[i].Type,
                        UsageName(all[i].Usage), all[i].UsageIndex);
                }
            }
        }
        // The first two vertices, raw and as floats. A HUD quad's corners are the fastest way to
        // tell which space these positions are already in - screen pixels, clip space or NDC -
        // and that decides whether a rebuild needs a transform at all.
        if (vertices && stride >= 8 && stride <= 128) {
            const unsigned char* v = static_cast<const unsigned char*>(vertices);
            for (int q = 0; q < 2; ++q) {
                char hex[400]; int p = 0;
                for (UINT b = 0; b < stride && p < 380; ++b)
                    p += _snprintf_s(hex + p, sizeof(hex) - p, _TRUNCATE, "%02X ",
                                     v[q * stride + b]);
                float f[8]{};
                const UINT nf = (stride / 4) < 8 ? (stride / 4) : 8;
                memcpy(f, v + q * stride, nf * 4);
                Log("    vtx%d raw: %s", q, hex);
                Log("    vtx%d  as floats: %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f",
                    q, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7]);
            }
        }
        // EVERY bound stage, not just stage 0. A video frame arrives as separate planes on
        // separate stages, and stage 0 alone is its luma - which is the whole greyscale story.
        for (int st = 0; st < 8; ++st) {
            if (!g_curTexture[st]) continue;
            UINT tw = 0, th = 0;
            int tfmt = 0;
            if (g_curTexture[st]->GetType() == D3DRTYPE_TEXTURE) {
                D3DSURFACE_DESC sd{};
                if (SUCCEEDED(static_cast<IDirect3DTexture9*>(g_curTexture[st])
                                  ->GetLevelDesc(0, &sd))) {
                    tw = sd.Width; th = sd.Height; tfmt = static_cast<int>(sd.Format);
                }
            }
            Log("    stage %d: tex=%p %ux%u fmt=%d", st,
                static_cast<void*>(g_curTexture[st]), tw, th, tfmt);
        }
        if (vs) vs->Release();
        if (ps) ps->Release();
        if (vd) vd->Release();
    }

    // Recorded so the dump NAMES it - render target, pixel shader, bound texture, blend state.
    // Identifying a draw by what it is beats identifying it from a screenshot, which is the rule
    // that found the hair and the triangle strips.
    g_dispReason = hud ? "HUD (user-pointer, depth test off) - demoted to a UI overlay"
                       : "user-pointer draw, depth-tested - in-world, left alone";
    RecordFrameDraw(dev, count * 3, count, hud ? Disp::Hide : Disp::PassThrough);

    // THE RASTERIZED-FRAME INVARIANT, gated on `hud` because that population (no projTM, depth
    // test off) is exactly where a screen-space post effect and a real HUD icon are otherwise
    // indistinguishable - see CompositesEngineOutputToScreen's own comment (above Classify) for
    // why this exists at all: this function's HUD test is the confirmed gap that let a
    // rasterized-frame composite ride the HUD rebuild straight to Remix.
    if (hud && g_settings.blockEngineOutputToScreen) {
        int rtStage = -1;
        UINT rtW = 0, rtH = 0;
        if (CompositesEngineOutputToScreen(dev, &rtStage, &rtW, &rtH)) {
            const bool hasVS = g_lastVS != nullptr;
            LogRtInvariantBlock("DrawPrimitiveUP", rtStage, rtW, rtH, count, hasVS);
            if (hasVS) {
                ++g_rtInvariantUPPass;
                g_dispReason = "BLOCKED: UP draw samples a render target and writes the screen - "
                               "vertex shader bound, passed through untouched so Remix (capture "
                               "off) declines it";
                SyncBridgeVS(dev, g_lastVS);
                return g_origDrawPrimitiveUP(dev, type, count, vertices, stride);
            }
            ++g_rtInvariantUPSkip;
            g_dispReason = "BLOCKED: UP draw samples a render target and writes the screen - "
                           "already fixed function, skipped so it never reaches the device";
            return D3D_OK;
        }
    }
    const bool ui = BeginUIDemote(dev, hud);
    HRESULT hr = D3D_OK;
    // Rebuild it as fixed function if we can; pass the game's own draw through if we cannot.
    // A HUD draw rebuilt as fixed function must NOT also go out raw. It did, since 2026-09-04:
    // the brace was missing, so the game's own draw followed the rebuilt one - float4 PIXEL
    // positions (2560, 1440, 0) through whatever the bridge had bound (fixed function after a
    // converted draw) - and the pause menu, the map and the HUD landed in the world a thousand
    // units above the player. The frame dump of 2026-09-11 showed the menu's 58 HUD draws and
    // nothing else that could float. uiRawAfterHud=1 restores the double draw for an A/B.
    const bool rebuilt = hud && g_settings.uiConvertUP &&
                         DrawHudFixedFunction(dev, type, count, vertices, stride);
    if (rebuilt) ++g_hudRebuiltOnly;
    if (!rebuilt || g_settings.uiRawAfterHud) {
        if (rebuilt) ++g_hudRawAfterRebuild;
        SyncBridgeVS(dev, g_lastVS);
        // injectProbe/injectControl PART 1/2: only under an ACTIVE UI demote (hud) - with
        // hud false, BeginUIDemote above did nothing and this is a plain pass-through draw,
        // not one of the five sites PART 1 watches. CurrentDrawIsRHW asks the device directly
        // for the same reason the HUD-format probe does (see "what IS a HUD vertex?", above):
        // this draw may carry an FVF this shim's own declaration shadow knows nothing about.
        InjectSuppression injectSup;
        if (hud) {
            injectSup = BeforeShimDrawSite(dev, InjectSite::UIDemote, g_lastVS != nullptr,
                                           count, CurrentDrawIsRHW(dev));
        }
        hr = g_origDrawPrimitiveUP(dev, type, count, vertices, stride);
        if (hud) AfterShimDrawSite(dev, injectSup);
    }
    EndUIDemote(dev, ui);
    return hr;
}

HRESULT WINAPI Hook_DrawIndexedPrimitiveUP(IDirect3DDevice9* dev, D3DPRIMITIVETYPE type,
                                           UINT minIndex, UINT numVertices, UINT count,
                                           const void* indices, D3DFORMAT indexFormat,
                                           const void* vertices, UINT stride) {
    ++g_drawsTotal;
    ++g_drawIndexThisFrame;
    ++g_upIndexedTotal;
    NoteIfFinalComposite(dev);   // injectProbe/injectControl PART 1/2 - for completeness only:
                                 // this function issues no fixed-function draw of its own
    g_dispReason = "DrawIndexedPrimitiveUP - user-pointer draw, NOT classified by this shim";
    // THE RASTERIZED-FRAME INVARIANT. Found by searching for every exit this shim makes a draw
    // visible to Remix through: this function classifies NOTHING at all, an unconditional
    // pass-through, so a composite draw arriving through THIS call has never had any protection,
    // HUD-test gap or otherwise. See CompositesEngineOutputToScreen, above Classify.
    if (g_settings.blockEngineOutputToScreen) {
        int rtStage = -1;
        UINT rtW = 0, rtH = 0;
        if (CompositesEngineOutputToScreen(dev, &rtStage, &rtW, &rtH)) {
            const bool hasVS = g_lastVS != nullptr;
            LogRtInvariantBlock("DrawIndexedPrimitiveUP", rtStage, rtW, rtH, count, hasVS);
            g_dispReason = hasVS
                ? "BLOCKED: indexed UP draw samples a render target and writes the screen - "
                  "vertex shader bound, passed through untouched so Remix (capture off) declines "
                  "it"
                : "BLOCKED: indexed UP draw samples a render target and writes the screen - "
                  "already fixed function, skipped so it never reaches the device";
            RecordFrameDraw(dev, numVertices, count, hasVS ? Disp::PassThrough : Disp::Skip);
            if (hasVS) {
                ++g_rtInvariantIndexedUPPass;
                SyncBridgeVS(dev, g_lastVS);
                return g_origDrawIndexedPrimitiveUP(dev, type, minIndex, numVertices, count,
                                                    indices, indexFormat, vertices, stride);
            }
            ++g_rtInvariantIndexedUPSkip;
            return D3D_OK;
        }
    }
    RecordFrameDraw(dev, numVertices, count, Disp::PassThrough);
    SyncBridgeVS(dev, g_lastVS);
    return g_origDrawIndexedPrimitiveUP(dev, type, minIndex, numVertices, count, indices,
                                        indexFormat, vertices, stride);
}

// Remix 1.5.2 offers exactly two sky mechanisms: rtx.skyBoxTextures (hash list) and
// rtx.skyDrawcallIdThreshold - "the first N draw calls of a frame are sky". The threshold is
// the one that handles a sky DOME rather than a tagged texture, and setting it needs to know
// where in the frame the sky is drawn. Sky is drawn early with depth writes off, so record the
// first draws of one frame with their depth state and let the number be read off rather than
// guessed. This is what "handle them safely" needs: the sky stays in the render, but Remix
// treats it as environment instead of a solid shell around the player.
// First attempt only looked at the first 24 draws, on the assumption that sky is drawn first.
// Measured: draws 2-23 all have depth writes AND depth test enabled, so SR3's sky is NOT an
// early draw - which also means rtx.skyDrawcallIdThreshold may be the wrong mechanism for it.
// So scan a WHOLE frame instead and report every depth-disabled draw wherever it falls, with
// enough identifying detail to decide between the threshold and rtx.skyBoxTextures.
unsigned g_skyProbeFrame = 0, g_skyReports = 0;
void ProbeSkyOrder(IDirect3DDevice9* dev, UINT numVertices, UINT primitiveCount) {
    if (g_skyProbeFrame || !g_haveCamera) return;
    const DWORD zwrite = ShadowGetRS(dev, D3DRS_ZWRITEENABLE);
    const DWORD zenable = ShadowGetRS(dev, D3DRS_ZENABLE);
    if ((zwrite && zenable) || g_skyReports >= 40) return;
    ++g_skyReports;

    // Measure the shape in WORLD space and say which shader made it. For these draws objTM is
    // unused, so the vertex positions already are world positions. Reporting extent plus the
    // offset from the camera distinguishes a sky dome (huge, centred on the camera) from a
    // light volume (small, sitting out in the world) from a shell welded to the viewpoint.
    char shape[128];
    shape[0] = '\0';
    if (g_stream0 && g_stream0Stride >= 12 && g_curLayout.posOffset >= 0 &&
        numVertices >= 3 && numVertices <= 4096) {
        void* mapped = nullptr;
        if (SUCCEEDED(g_stream0->Lock(g_stream0Offset, numVertices * g_stream0Stride, &mapped,
                                      D3DLOCK_READONLY | D3DLOCK_NOSYSLOCK)) && mapped) {
            float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
            const unsigned char* p = static_cast<const unsigned char*>(mapped);
            for (UINT i = 0; i < numVertices; ++i) {
                const float* v = reinterpret_cast<const float*>(
                    p + i * g_stream0Stride + g_curLayout.posOffset);
                for (int k = 0; k < 3; ++k) {
                    if (v[k] < lo[k]) lo[k] = v[k];
                    if (v[k] > hi[k]) hi[k] = v[k];
                }
            }
            g_stream0->Unlock();
            const float cx = (lo[0] + hi[0]) * 0.5f, cy = (lo[1] + hi[1]) * 0.5f,
                        cz = (lo[2] + hi[2]) * 0.5f;
            const float dx = cx - g_camX, dy = cy - g_camY, dz = cz - g_camZ;
            _snprintf_s(shape, sizeof(shape), _TRUNCATE,
                        " size=%.1fx%.1fx%.1f centre=(%.1f %.1f %.1f) distFromCam=%.1f",
                        hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2], cx, cy, cz,
                        std::sqrt(dx * dx + dy * dy + dz * dz));
        }
    }

    static const char* kLightKind[] = {"no", "POINT-LIGHT", "SPOT-LIGHT", "DIR-LIGHT"};
    Log("  depth-disabled draw #%u: verts=%-5u prims=%-5u zwrite=%lu tex0=%p objTM=%d "
        "lightVolume=%s ps='%s'%s",
        g_drawIndexThisFrame, numVertices, primitiveCount, zwrite,
        static_cast<void*>(g_curTexture[0]), g_curVS.usesObjTM,
        kLightKind[g_curPS.light.kind], g_curPS.firstSampler, shape);
}

void InstallCrashFilter();   // defined with the crash handler, below the draw hooks


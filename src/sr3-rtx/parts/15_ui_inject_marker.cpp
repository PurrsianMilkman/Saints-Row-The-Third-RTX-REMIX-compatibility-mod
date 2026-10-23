// parts/15_ui_inject_marker.cpp - UI demotion, injectProbe/injectControl, the marker texture.
// NOT a standalone translation unit: #included, in order, by ../sr3rtx.cpp inside its anonymous
// namespace. Originally sr3rtx.cpp lines 10103-10793 (old file:line citations map here).
// ---------------------------------------------------------------- UI demotion
//
// Some draws are not scene geometry and must not be ray-traced, but must still EXECUTE - the
// game's post-process chain and its own light buffer depend on them, and dropping draw classes
// outright is what froze the image and crashed the runtime in earlier sessions.
//
// Remix 1.5.2 has `orthographicIsUI`: a draw whose PROJECTION is orthographic is classified as
// UI and kept out of the ray-traced world. So swapping in an orthographic projection for the
// duration of such a draw hides it from the path tracer while the draw itself still happens.
// The game never reads fixed-function transform state, so this is invisible to the engine.
//
// *** PROJECTION ONLY. NEVER THE VIEW. *** An earlier attempt at this set both, and Remix
// resolves the light injected by EmitLight against the view matrix in force during that draw -
// so overriding the view moved or dropped every light while the counters still showed healthy
// injection. That regression is documented in docs/worklog.md; the projection alone is what
// Remix's orthographic test reads.
//
// Two populations, both measured from capture_2026-08-17:
//   * 15 four-vertex quads at distance 1.0 from the camera, aspect 1.71 (16:9) - screen-aligned
//     fullscreen quads: the composite/post chain, plus the sun and moon sprites.
//   * flattened ellipsoids 26-54 units across and ~4.7 tall hugging the player - inferred
//     lighting's light volumes, whose parameters EmitLight has already harvested into a real
//     Remix light, making the mesh itself redundant.
// Acts ONLY on the decision Classify already made.
//
// This used to fall back to a second property test (`ShouldDemoteToUI`) whenever the caller did
// not pass hide=true, and that was a genuine bug: it re-classified draws Classify had already
// decided to leave alone. A real material whose only sampler is Normal_MapSampler and which does
// not write depth was being turned into a UI overlay - i.e. real world geometry given the exact
// treatment that produced "the shapes changed transform". Two classifiers disagreeing about the
// same draw is how that happened, so now there is one.
//
// The removed test was also built on a theory the frame dump disproved: the unit cubes were read
// as untextured "proxy volumes", when the prepass signature is a shader with NO samplers at all,
// which Classify handles directly.
bool BeginUIDemote(IDirect3DDevice9* dev, bool hide) {
    if (!hide) return false;
    g_internal = true;
    g_internalThread = GetCurrentThreadId();
    g_origSetTransform(dev, D3DTS_PROJECTION, &kIdentity);
    g_curDeviceProj = kIdentity;   // see g_curDeviceProj's own comment - kept exact everywhere
    g_internal = false;
    ++g_demotedToUI;
    return true;
}

void EndUIDemote(IDirect3DDevice9* dev, bool active) {
    if (!active) return;
    g_internal = true;
    g_internalThread = GetCurrentThreadId();
    if (g_haveAppliedProj) {
        g_origSetTransform(dev, D3DTS_PROJECTION, &g_appliedProj);
        g_curDeviceProj = g_appliedProj;   // see g_curDeviceProj's own comment
    } else {
        // Nothing known to restore yet; make sure the next converted draw writes its own.
        // The device is left at the identity BeginUIDemote set, which is exactly what
        // g_curDeviceProj already holds, so it needs no update in this branch.
        g_haveAppliedProj = false;
    }
    g_internal = false;
}

// ---------------------------------------------------------------- injectProbe / injectControl
//
// THE PERMANENT FIX, grounded in Remix's own source (tag remix-1.5.2) rather than in the guess
// blockEngineOutputToScreen above was built on. Remix composites its path-traced image in
// RtxContext::injectRTX, which has two callers: D3D9Rtx::triggerInjectRTX, fired from
// internalPrepareDraw when a draw classifies as UI (d3d9_rtx.cpp:507-512), and a Present-time
// fallback that is a no-op once injection has already happened this frame. The decisive line, at
// the top of internalPrepareDraw, is what makes the ORDER of one frame's draws matter: before the
// trigger a draw carrying a vertex shader is genuinely invisible (captured, not rasterised);
// after it, EVERY draw in the frame - shader or not - is rasterised raw into whatever render
// target is bound, until EndFrame. So if a SHIM-ISSUED fixed-function draw satisfies Remix's own
// UI test before the game's real final composite has run, that composite (and everything after
// it) paints straight onto the screen over the path-traced image. That is "the rasterized frame
// replaces the scene", camera-angle dependent because it depends on which of the shim's own
// draws happens to meet the test first, which depends on what is visible.
//
// PART 1, injectProbe: watches every fixed-function draw the shim itself issues - converted
// world draws (BeginFFP, Disp::Convert), marker draws (BeginMark, Disp::Mark), UI demotions
// (BeginUIDemote, Disp::Hide), the rebuilt HUD (DrawHudFixedFunction) and the character atlas
// composite (DrawCompositeFixedFunction) - against EvaluateInjectTrigger, below, and reports
// which one (if any) is first to satisfy it each frame, against where the game's own final
// composite and first HUD draw land. Read-only: it changes no render state and no pixel by
// itself.
//
// PART 2, injectControl: (a) for a PRETRANSFORMED (RHW) shim draw that would satisfy the trigger
// before the composite has run, forces its projection off-identity for just that one draw - RHW
// vertices ignore the transform pipeline for rendering (D3D9 skips it entirely), so this changes
// nothing visible; Remix's own makeDrawCallType then falls through past the UI test to the
// POSITIONT branch, which is Rasterized with NO trigger either way - the SAME visual outcome as
// the UI-classified path, so breaking the UI test costs nothing and stops the accidental
// trigger. A non-RHW draw that satisfies the predicate is left alone and only reported: its
// transform places real geometry, and retargeting it blindly would move or distort it. (b) issues
// the GTA4-RTX-mod's own deliberate, invisible trigger quad
// (gta4-rtx/src/gta4/modules/renderer.cpp:3141-3196, manually_trigger_remix_injection) once per
// frame, immediately after the game's final composite and before the first HUD draw, so the
// trigger fires at the RIGHT moment on every frame by construction rather than by camera-angle
// accident.
enum class InjectSite { ConvertedWorld, MarkerDraw, UIDemote, HudRebuild, AtlasComposite };
inline const char* InjectSiteName(InjectSite s) {
    switch (s) {
        case InjectSite::ConvertedWorld: return "ConvertedWorld";
        case InjectSite::MarkerDraw:     return "MarkerDraw";
        case InjectSite::UIDemote:       return "UIDemote";
        case InjectSite::HudRebuild:     return "HudRebuild";
        case InjectSite::AtlasComposite: return "AtlasComposite";
        default:                         return "?";
    }
}

struct InjectResult { bool trigger; const char* reason; };

// Per-frame state: the FIRST shim draw (any site) that satisfied the predicate, the game's final
// composite, and the first HUD draw - reset in TallyAndResetInjectFrame, called from Hook_Present
// at the same point g_drawIndexThisFrame itself resets, so "this frame" means exactly what it
// means everywhere else in this file.
bool g_frameShimTriggerSeen = false;
unsigned g_frameShimTriggerIndex = 0;
InjectSite g_frameShimTriggerSite = InjectSite::ConvertedWorld;
const char* g_frameShimTriggerReason = "";
bool g_frameShimTriggerRHW = false;
UINT g_frameShimTriggerRTW = 0, g_frameShimTriggerRTH = 0;
char g_frameShimTriggerSamplers[256] = {};

bool g_frameCompositeSeen = false;
unsigned g_frameCompositeIndex = 0;

bool g_frameFirstHudSeen = false;
unsigned g_frameFirstHudIndex = 0;

bool g_frameDeliberateQuadIssued = false;

// The per-frame counter line PART 1 asks for: frames where the first shim trigger preceded the
// final composite (the bug), frames where it followed (correct - either a natural later trigger
// or PART 2(b)'s own deliberate one), frames with no shim trigger at all this frame (Remix's own
// Present-time fallback is what injects those).
unsigned long long g_injectFramesTotal = 0;
unsigned long long g_injectFramesTriggerBeforeComposite = 0;
unsigned long long g_injectFramesTriggerAfterComposite = 0;
unsigned long long g_injectFramesNoShimTrigger = 0;

// Capped at 12, re-armed by the capture key exactly like ProbeDecalDraw/LogRtInvariantBlock -
// see RearmCaptures. Only frames where the trigger preceded the composite are named: that is the
// population that proves the diagnosis.
unsigned g_injectProbeReports = 0;
// PART 2(a)'s own second population - satisfied the predicate, was NOT pretransformed, left
// unaltered. Same cap, same rearm.
unsigned g_injectNonRhwReports = 0;

unsigned long long g_injectSuppressedRHW = 0;    // PART 2(a): forced off-identity, safe (RHW)
unsigned long long g_injectReportedNonRHW = 0;   // PART 2(a): satisfied the predicate, NOT RHW

unsigned long long g_deliberateQuadsIssued = 0, g_deliberateQuadsFailed = 0;
HRESULT g_deliberateQuadLastFail = 0;

// Remix's OWN trigger predicate for RtxContext::injectRTX (remix-1.5.2, d3d9_rtx.cpp), read off
// the source rather than guessed - makeDrawCallType's ordered checks (403-533) and
// isRenderingUI (555-568), walked in the SAME order here against the state THIS shim's own
// fixed-function draws actually carry at the moment they reach the device:
//
//   1. a programmable vertex shader bound -> Ignored, can never trigger (425-427). Exact: hasVS,
//      the caller's own read of g_lastVS (or, for the two RHW-only draws below, the fact that
//      they null it themselves) at the moment of its draw.
//   2. primitive count 0 -> never a trigger. Exact.
//   3. unsupported topology -> APPROXIMATED AWAY. Every draw this shim itself issues is a
//      TRIANGLELIST or TRIANGLESTRIP, both supported, so this check can never fire on the
//      population injectProbe watches.
//   4. no colour render target -> approximated as g_rt0Width/Height both nonzero, this file's own
//      standing assumption everywhere else a render target is read (BeginFFP,
//      CompositesEngineOutputToScreen).
//   5. RGB colour-write mask off -> exact, off D3DRS_COLORWRITEENABLE's shadow.
//   6. the shadow-mask heuristic -> APPROXIMATED AWAY. This project has no shadow render path of
//      its own to test against, and stating that plainly is safer than guessing Remix's internal
//      test.
//   7. render target not the size of the back buffer -> exact (g_rt0Width/Height vs
//      g_backBufferW/H) - the SAME gate CompositesEngineOutputToScreen uses above, and why the
//      atlas composite (writes 2048x1024 or 1024x512, never the screen) can never reach the UI
//      test below no matter what its projection holds.
//   8. stencil-shadow state -> APPROXIMATED AWAY, same reasoning as 6.
//   9. isRenderingUI(): !UseProgrammableVS() && orthographicIsUI() && transforms[PROJECTION]
//      [3][3] == 1.0f && !ZWRITEENABLE. Condition 1 is already known true by this point (hasVS
//      false, or step 1 above already returned). orthographicIsUI() is folded into the explicit
//      _44 check: this project's OWN UI-demotion mechanism (BeginUIDemote, above) already treats
//      swapping in an identity projection as satisfying Remix's orthographic test - "Remix 1.5.2
//      has orthographicIsUI: a draw whose PROJECTION is orthographic is classified as UI", read
//      off g_curDeviceProj (see its own comment) rather than a fresh bridge call. ZWRITEENABLE is
//      exact, off its shadow.
//  10. POSITIONT (pretransformed) -> exact: `rhw`, the caller's own knowledge of the vertex
//      format it just built or forwarded.
InjectResult EvaluateInjectTrigger(IDirect3DDevice9* dev, bool hasVS, UINT primCount, bool rhw) {
    if (hasVS)
        return {false, "has a vertex shader bound - Remix Ignores it with capture off, can never trigger"};
    if (primCount == 0)
        return {false, "primCount 0"};
    if (!g_rt0Width || !g_rt0Height)
        return {false, "no render target 0 bound"};
    const DWORD cw = ShadowGetRS(dev, D3DRS_COLORWRITEENABLE);
    if ((cw & 0x7u) == 0)
        return {false, "RGB colour-write mask off"};
    if (!g_backBufferW || !g_backBufferH || g_rt0Width != g_backBufferW || g_rt0Height != g_backBufferH)
        return {false, "render target not the back buffer's size - Rasterized, no trigger (497-503)"};
    const bool zwrite = ShadowGetRS(dev, D3DRS_ZWRITEENABLE) != 0;
    const bool orthoIdentity = (g_curDeviceProj._44 == 1.0f);
    if (orthoIdentity && !zwrite)
        return {true, "UI TEST SATISFIED: identity projection (orthographicIsUI), ZWRITEENABLE off, no vertex shader - this is the trigger"};
    if (rhw)
        return {false, "POSITIONT (pretransformed) - Rasterized, no trigger (512-517)"};
    return {false, "neither the UI test nor POSITIONT - ordinary rasterized geometry, no trigger"};
}

// Only needed where the caller does not already know its own vertex format for certain (the
// HUD's RAW forward path in Hook_DrawPrimitiveUP - a user-pointer draw where the game may bind
// either a declaration or a plain FVF, see that function's own "what IS a HUD vertex?" comment).
// Every other call site below knows its format by construction and passes rhw directly with no
// bridge call: ConvertedWorld is never RHW (BeginFFP builds a real world*view*proj), HudRebuild
// and AtlasComposite are always RHW (they set D3DFVF_XYZRHW themselves), MarkerDraw and UIDemote
// read g_curLayout, already parsed by Classify for a declaration-based draw.
bool CurrentDrawIsRHW(IDirect3DDevice9* dev) {
    if (g_curLayout.parsed) return g_curLayout.hasPositionT;
    DWORD fvf = 0;
    return SUCCEEDED(dev->GetFVF(&fvf)) && (fvf & D3DFVF_XYZRHW) != 0;
}

// "The game's final composite" - the SAME recognition the rasterized-frame invariant uses
// (CompositesEngineOutputToScreen, above): a screen-space draw (no vertex-shader projTM) that
// writes render target 0 at EXACTLY the back buffer's own size while sampling a render target on
// some bound stage. Kept as its own small function rather than a call to
// CompositesEngineOutputToScreen so this identification works whether or not
// blockEngineOutputToScreen is enabled - injectProbe/injectControl must find the composite on
// their own. The D3DUSAGE_RENDERTARGET fallback CompositesEngineOutputToScreen falls back to for
// a render target it never saw created is left out here to add no bridge call to the ~1,700
// draws/frame that reach this check and are not it; g_rtTextures alone has caught the composite
// in every capture this project has measured.
bool IsGameFinalComposite(IDirect3DDevice9* dev) {
    (void)dev;
    if (g_curVS.usesProjTM) return false;
    if (!g_backBufferW || !g_backBufferH || !g_rt0Width || !g_rt0Height) return false;
    if (g_rt0Width != g_backBufferW || g_rt0Height != g_backBufferH) return false;
    const bool haveReflection = g_curPS.isPixelShader && g_curPS.anySampler;
    for (int st = 0; st < 8; ++st) {
        IDirect3DBaseTexture9* const t = g_curTexture[st];
        if (!t) continue;
        if (haveReflection && !g_curPS.samplerName[st][0]) continue;
        if (g_rtTextures.count(t)) return true;
    }
    return false;
}

// Called at the ENTRY of all four draw hooks, before Classify has even run - g_curVS/g_curPS/
// g_curTexture/g_rt0Width are shadows already valid by then, the same ones
// CompositesEngineOutputToScreen reads from inside Classify. Only the FIRST composite of the
// frame is recorded; SR3 draws exactly one per frame in every capture this project has measured,
// and the first is what "before/after" needs.
void NoteIfFinalComposite(IDirect3DDevice9* dev) {
    if (!g_settings.injectProbe && !g_settings.injectControl) return;
    if (g_frameCompositeSeen) return;
    if (!IsGameFinalComposite(dev)) return;
    g_frameCompositeSeen = true;
    g_frameCompositeIndex = g_drawIndexThisFrame;
}

// PART 2(a) and PART 1's own recording, together: called once per candidate draw, ahead of the
// real draw call, so a suppression can still change what reaches the device. Returns whether it
// suppressed the trigger (and what to restore afterward, via AfterShimDrawSite) so the caller can
// put the projection back once its own draw call has gone out.
struct InjectSuppression { bool active = false; D3DMATRIX savedProj{}; };

InjectSuppression BeforeShimDrawSite(IDirect3DDevice9* dev, InjectSite site, bool hasVS,
                                     UINT primCount, bool rhw) {
    InjectSuppression sup;
    if (!g_settings.injectProbe && !g_settings.injectControl) return sup;
    const InjectResult r = EvaluateInjectTrigger(dev, hasVS, primCount, rhw);
    if (!r.trigger) return sup;

    bool recordAsTrigger = true;
    if (g_settings.injectControl && !g_frameCompositeSeen) {
        if (rhw) {
            // Safe exactly because Remix rasterises an RHW draw the same way whichever branch
            // classifies it - see this section's own top comment. _44 = 0.0f matches the value a
            // real perspective projection already carries in this codebase (FromRegisters), so
            // it is not a value Remix's test has any special-case reason to treat differently.
            sup.savedProj = g_curDeviceProj;
            D3DMATRIX m = g_curDeviceProj;
            m._44 = 0.0f;
            const bool wasInternal = g_internal;
            g_internal = true;
            g_internalThread = GetCurrentThreadId();
            g_origSetTransform(dev, D3DTS_PROJECTION, &m);
            g_internal = wasInternal;
            g_curDeviceProj = m;
            sup.active = true;
            ++g_injectSuppressedRHW;
            recordAsTrigger = false;   // it no longer will, by construction
        } else {
            ++g_injectReportedNonRHW;
            if (g_injectNonRhwReports < 12) {
                ++g_injectNonRhwReports;
                Log("INJECT CONTROL #%u: %s draw satisfies the trigger predicate and is NOT "
                    "pretransformed - left UNALTERED rather than retargeted blindly (its own "
                    "transform places real geometry). reason=%s prims=%u target=%ux%u",
                    g_injectNonRhwReports, InjectSiteName(site), r.reason, primCount, g_rt0Width,
                    g_rt0Height);
            }
        }
    }

    if (recordAsTrigger && !g_frameShimTriggerSeen) {
        g_frameShimTriggerSeen = true;
        g_frameShimTriggerIndex = g_drawIndexThisFrame;
        g_frameShimTriggerSite = site;
        g_frameShimTriggerReason = r.reason;
        g_frameShimTriggerRHW = rhw;
        g_frameShimTriggerRTW = g_rt0Width;
        g_frameShimTriggerRTH = g_rt0Height;
        int p = 0;
        g_frameShimTriggerSamplers[0] = 0;
        for (int st = 0; st < 8; ++st) {
            if (!g_curTexture[st]) continue;
            const char* name = g_curPS.samplerName[st][0] ? g_curPS.samplerName[st] : "(unreflected)";
            p += _snprintf_s(g_frameShimTriggerSamplers + p, sizeof(g_frameShimTriggerSamplers) - p,
                             _TRUNCATE, "%s%d:%s", p ? " " : "", st, name);
        }
    }
    return sup;
}

void AfterShimDrawSite(IDirect3DDevice9* dev, const InjectSuppression& sup) {
    if (!sup.active) return;
    const bool wasInternal = g_internal;
    g_internal = true;
    g_internalThread = GetCurrentThreadId();
    g_origSetTransform(dev, D3DTS_PROJECTION, &sup.savedProj);
    g_internal = wasInternal;
    g_curDeviceProj = sup.savedProj;
}

// Thin wrapper for Hook_DrawPrimitive/Hook_DrawIndexedPrimitive: the three dispositions that are
// a shim-issued fixed-function draw through THIS call site (see the switch). PassThrough and
// Skip are the game's own draw, forwarded untouched or dropped - not something the shim itself
// built or retargeted, so PART 1 does not watch them here: PassThrough still carries the game's
// real vertex shader (or EvaluateInjectTrigger's own step 1 would already exclude it), and Skip
// never reaches the device at all.
InjectSuppression BeforeShimDraw(IDirect3DDevice9* dev, Disp d, UINT primCount) {
    InjectSite site;
    bool hasVS, rhw;
    switch (d) {
        case Disp::Convert:
            site = InjectSite::ConvertedWorld; hasVS = false; rhw = false; break;
        case Disp::Mark:
            site = InjectSite::MarkerDraw; hasVS = (g_lastVS != nullptr);
            rhw = g_curLayout.parsed && g_curLayout.hasPositionT; break;
        case Disp::Hide:
            site = InjectSite::UIDemote; hasVS = (g_lastVS != nullptr);
            rhw = g_curLayout.parsed && g_curLayout.hasPositionT; break;
        default:
            return InjectSuppression{};
    }
    return BeforeShimDrawSite(dev, site, hasVS, primCount, rhw);
}

// PART 2(b). The GTA4-RTX-mod recipe (gta4-rtx/src/gta4/modules/renderer.cpp:3141-3196,
// manually_trigger_remix_injection), adapted to this shim's own save/restore conventions (see
// DrawCompositeFixedFunction and the shader-bake function, both above and below this one, for the
// same pattern). Four D3DFVF_XYZRHW|D3DFVF_DIFFUSE vertices, alpha 0, spanning about 0.01 pixel
// at (-0.5,-0.5) - invisible on its own terms (sub-pixel, so it is not expected to cover a single
// sample) with no pixel shader bound to do anything with it besides. FOGENABLE off, both shaders
// null, ZWRITEENABLE off, matching the recipe exactly. D3DTS_PROJECTION is force-set to identity
// here, which the recipe's own source does not need (RHW vertices ignore it for POSITIONING) but
// this shim does: Remix's classifier reads whatever the CURRENT projection transform is,
// regardless of the draw's own vertex format, and this shim's leftover value could be anything a
// previous converted draw left behind. Every piece of state touched is saved first and restored
// after, including the render target, which is forced to the real back buffer for the duration
// of the draw and then put back.
void IssueDeliberateInjectQuad(IDirect3DDevice9* dev) {
    IDirect3DVertexShader9* oldVS = nullptr;
    IDirect3DPixelShader9* oldPS = nullptr;
    IDirect3DVertexDeclaration9* oldDecl = nullptr;
    IDirect3DSurface9* oldRT0 = nullptr;
    IDirect3DSurface9* backBuffer = nullptr;
    DWORD oldFVF = 0;
    dev->GetVertexShader(&oldVS);
    dev->GetPixelShader(&oldPS);
    dev->GetVertexDeclaration(&oldDecl);
    dev->GetFVF(&oldFVF);
    dev->GetRenderTarget(0, &oldRT0);
    const DWORD oldZW = ShadowGetRS(dev, D3DRS_ZWRITEENABLE);
    const DWORD oldFog = ShadowGetRS(dev, D3DRS_FOGENABLE);
    const D3DMATRIX oldProj = g_curDeviceProj;   // exact, no bridge call - see its own comment

    HRESULT hr = dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer);

    g_internal = true;
    g_internalThread = GetCurrentThreadId();
    if (SUCCEEDED(hr) && backBuffer) dev->SetRenderTarget(0, backBuffer);
    g_origSetTransform(dev, D3DTS_PROJECTION, &kIdentity);
    g_curDeviceProj = kIdentity;
    SyncBridgeVS(dev, nullptr);
    g_origSetPixelShader(dev, nullptr);
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    g_origSetRenderState(dev, D3DRS_ZWRITEENABLE, FALSE);
    g_origSetRenderState(dev, D3DRS_FOGENABLE, FALSE);

    struct QuadVertex { float x, y, z, rhw; DWORD color; };
    const DWORD invisible = 0x00000000u;   // alpha 0, on top of being sub-pixel
    const QuadVertex quad[4] = {
        {-0.50f, -0.50f, 0.0f, 1.0f, invisible},
        {-0.49f, -0.50f, 0.0f, 1.0f, invisible},
        {-0.50f, -0.49f, 0.0f, 1.0f, invisible},
        {-0.49f, -0.49f, 0.0f, 1.0f, invisible},
    };
    hr = g_origDrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, quad, sizeof(QuadVertex));

    g_origSetRenderState(dev, D3DRS_FOGENABLE, oldFog);
    g_origSetRenderState(dev, D3DRS_ZWRITEENABLE, oldZW);
    dev->SetFVF(oldFVF);
    g_origSetVertexDeclaration(dev, oldDecl);
    g_origSetTransform(dev, D3DTS_PROJECTION, &oldProj);
    g_curDeviceProj = oldProj;
    SyncBridgeVS(dev, oldVS);
    g_origSetPixelShader(dev, oldPS);
    if (oldRT0) { dev->SetRenderTarget(0, oldRT0); oldRT0->Release(); }
    g_internal = false;

    if (backBuffer) backBuffer->Release();
    if (oldVS) oldVS->Release();
    if (oldPS) oldPS->Release();
    if (oldDecl) oldDecl->Release();

    if (SUCCEEDED(hr)) ++g_deliberateQuadsIssued;
    else { ++g_deliberateQuadsFailed; g_deliberateQuadLastFail = hr; }
}

// Called from Hook_DrawPrimitiveUP once it knows `hud` (see that function's own "THE HUD"
// comment) - the established chronological population, "ALL of them into the BACK BUFFER, after
// the frame's final composite". The first HUD draw of the frame is recorded here, and if PART
// 2(b) is on, the composite has already been seen this frame, and the quad has not gone out yet,
// it goes out NOW: between the last draw that actually ran (the composite, or whatever the game
// drew right after it) and this HUD draw, which is what makes "immediately after the composite
// and before the first HUD draw" literal rather than approximate. If the frame never shows a
// final composite, nothing here ever fires and Remix's own Present-time fallback is what injects
// that frame - deliberately: PART 2(b) only acts once it knows where "after the composite" is.
void NoteFirstHudAndMaybeInject(IDirect3DDevice9* dev, bool isHudDraw) {
    if (!isHudDraw) return;
    if (!g_frameFirstHudSeen) {
        g_frameFirstHudSeen = true;
        g_frameFirstHudIndex = g_drawIndexThisFrame;
    }
    if (g_settings.injectControl && g_frameCompositeSeen && !g_frameDeliberateQuadIssued) {
        g_frameDeliberateQuadIssued = true;
        IssueDeliberateInjectQuad(dev);
    }
}

// Called once per frame, from Hook_Present at the SAME point g_drawIndexThisFrame itself resets -
// after every draw of the frame has been seen, so the comparison between the first shim trigger
// and the final composite is settled for good before anything is printed or the state is
// dropped for the next frame.
void TallyAndResetInjectFrame() {
    if (g_settings.injectProbe || g_settings.injectControl) {
        ++g_injectFramesTotal;
        if (!g_frameShimTriggerSeen) {
            ++g_injectFramesNoShimTrigger;
        } else if (g_frameCompositeSeen && g_frameShimTriggerIndex < g_frameCompositeIndex) {
            ++g_injectFramesTriggerBeforeComposite;
            if (g_injectProbeReports < 12) {
                ++g_injectProbeReports;
                char hudMsg[32];
                if (g_frameFirstHudSeen) _snprintf_s(hudMsg, _TRUNCATE, "%u", g_frameFirstHudIndex);
                else strcpy_s(hudMsg, "none this frame");
                Log("INJECT PROBE #%u: first shim trigger at draw %u (site=%s, %s) PRECEDED the "
                    "final composite at draw %u - the bug. RHW=%d target=%ux%u samplers: %s | "
                    "first HUD draw at %s",
                    g_injectProbeReports, g_frameShimTriggerIndex,
                    InjectSiteName(g_frameShimTriggerSite), g_frameShimTriggerReason,
                    g_frameCompositeIndex, g_frameShimTriggerRHW ? 1 : 0, g_frameShimTriggerRTW,
                    g_frameShimTriggerRTH,
                    g_frameShimTriggerSamplers[0] ? g_frameShimTriggerSamplers : "(none bound)",
                    hudMsg);
            }
        } else {
            ++g_injectFramesTriggerAfterComposite;
        }
    }
    g_frameCompositeSeen = false;
    g_frameCompositeIndex = 0;
    g_frameFirstHudSeen = false;
    g_frameFirstHudIndex = 0;
    g_frameDeliberateQuadIssued = false;
    g_frameShimTriggerSeen = false;
    g_frameShimTriggerIndex = 0;
    g_frameShimTriggerReason = "";
    g_frameShimTriggerRHW = false;
    g_frameShimTriggerRTW = 0;
    g_frameShimTriggerRTH = 0;
    g_frameShimTriggerSamplers[0] = 0;
}

// ---------------------------------------------------------------- the marker texture
//
// Created once, at device init. D3DPOOL_DEFAULT because SR3 comes up through CreateDeviceEx and
// D3D9Ex rejects D3DPOOL_MANAGED outright - a managed texture here would simply fail to create
// and the whole mechanism would silently do nothing. DEFAULT is not lockable, so the content is
// written into a SYSTEMMEM staging texture and copied up with UpdateTexture, which is also the
// upload Remix hashes.
void CreateMarkerTexture(IDirect3DDevice9* dev) {
    if (g_marker) return;
    IDirect3DTexture9* staging = nullptr;
    HRESULT hr = dev->CreateTexture(4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &staging,
                                    nullptr);
    if (FAILED(hr) || !staging) { Log("marker: staging CreateTexture failed 0x%08lX", hr); return; }

    D3DLOCKED_RECT rect{};
    if (SUCCEEDED(staging->LockRect(0, &rect, nullptr, 0)) && rect.pBits) {
        for (int y = 0; y < 4; ++y) {
            DWORD* row = reinterpret_cast<DWORD*>(static_cast<unsigned char*>(rect.pBits) +
                                                  y * rect.Pitch);
            for (int x = 0; x < 4; ++x) row[x] = 0xFFFF00FFu | (static_cast<DWORD>(y * 4 + x) << 8);
        }
        staging->UnlockRect(0);
    }

    hr = dev->CreateTexture(4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g_marker, nullptr);
    if (FAILED(hr) || !g_marker) {
        Log("marker: default-pool CreateTexture failed 0x%08lX - mode 3 will pass through", hr);
        g_marker = nullptr;
        staging->Release();
        return;
    }
    hr = dev->UpdateTexture(staging, g_marker);
    staging->Release();
    if (FAILED(hr)) Log("marker: UpdateTexture failed 0x%08lX", hr);
    Log("marker texture created: 4x4 A8R8G8B8 magenta at %p. Tag it in the Remix menu "
        "(Alt+X -> Game Setup) and put its hash in rtx.ignoreTextures.",
        static_cast<void*>(g_marker));
}

// Bind the marker for the duration of one draw and return the stages that must be put back.
//
// STAGE 0 ONLY. The first version also cleared stages 1-7, on the theory that a prepass draw
// arrives carrying whatever the previous material draw left bound (true - the frame dump shows
// all 2,043 ps='' draws with a real game texture still on stage 0) and that Remix might hash one
// of those instead of ours. That was defensive guesswork, and the first run disproved it: the
// ignore fired with the marker on stage 0, so stage 0 is what Remix hashes.
//
// It also cost real frame time. Marking runs on ~1,700 draws a frame, and every SetTexture here
// bypasses the state shadow by design and crosses the 32->64-bit bridge as IPC. At 2(1+N) calls
// per draw that was on the order of 10,000 extra round trips a frame - the exact failure
// docs/sr2-fork.md section 6 records from SR2, walked into anyway. Stage 0 alone is 2 per draw,
// the same as the converted path, which measured at 0.79 ms for 1,400 draws.
//
// If Remix did need the other stages cleared, the failure announces itself: the magenta comes
// back. That is why this is worth testing rather than keeping "just in case".
// Stage 0 alone is enough for a geometry draw, but NOT for a screen-space one - measured the
// hard way on 2026-08-18.
//
// The composite quads were marked correctly and Remix drew them anyway, as a magenta sheet over
// the whole screen. The marker's hash was removed from rtx.uiTextures and they still appeared;
// only clearing the ENTIRE uiTextures list in the Remix menu stopped them. The reason is the
// stages we leave alone: a post quad arrives with the game's own textures still bound on stages
// 1-7, several of which are legitimately tagged as UI, and Remix categorises the draw from any
// of them. One UI-listed texture on any stage makes the whole draw UI, and UI is rasterised as a
// 2D overlay - so our magenta stage 0 got painted over the path-traced world.
//
// So a marked draw that must not be categorised from its leftovers clears all eight stages. That
// is the wide clear removed for cost on 2026-08-18, brought back for the ~23 draws a frame that
// actually need it instead of the ~1,700 that did not. At 2 calls per stage it is under 400
// bridge calls a frame against the ~10,000 that made the first version expensive.
unsigned BeginMark(IDirect3DDevice9* dev, bool clearAllStages) {
    if (!g_marker) return 0;
    unsigned touched = 0;
    g_internal = true;
    g_internalThread = GetCurrentThreadId();
    if (g_curTexture[0] != g_marker) { g_origSetTexture(dev, 0, g_marker); touched |= 1u; }
    if (clearAllStages) {
        for (DWORD stage = 1; stage < 8; ++stage)
            if (g_curTexture[stage]) {
                g_origSetTexture(dev, stage, nullptr);
                touched |= (1u << stage);
            }
    }
    g_internal = false;
    if (touched) {
        ++g_markedDraws;
        for (unsigned bit = touched; bit; bit &= bit - 1) ++g_markSetTextures;
    }
    return touched;
}

void EndMark(IDirect3DDevice9* dev, unsigned touched) {
    if (!touched) return;
    g_internal = true;
    g_internalThread = GetCurrentThreadId();
    for (DWORD stage = 0; stage < 8; ++stage)
        if (touched & (1u << stage)) {
            g_origSetTexture(dev, stage, g_curTexture[stage]);
            ++g_markSetTextures;
        }
    g_internal = false;
}

struct FFPScope {
    bool active = false;
    unsigned touched = 0;   // bitmask of texture stages we changed and must put back
    IDirect3DVertexShader9* vs = nullptr;
    IDirect3DPixelShader9* ps = nullptr;
    IDirect3DVertexDeclaration9* decl = nullptr;   // set only when the uv stream was bound
    bool uvStream = false;                         // our float2 texcoord stream is bound
    bool uvFreq = false;                           // and its frequency was set for instancing
    bool deinstanced = false;                      // stream frequencies reset for this draw
};

// Returns the disposition actually applied, so the caller knows whether to hide the draw.
// Defined with the uv conversion, further down - it belongs beside SetVertexDeclaration.
IDirect3DVertexDeclaration9* FloatUVDeclaration(IDirect3DDevice9* dev,
                                                IDirect3DVertexDeclaration9* src);
IDirect3DVertexBuffer9* UvBufferFor(IDirect3DDevice9* dev);
extern DWORD g_uvStream;
extern unsigned g_uvStreamDraws;
extern unsigned g_uvSet1Draws, g_uvSet1Unavailable, g_uvSet1WrongStream;

// FIX A: which vertex texcoord usage index THIS draw's albedo should be sampled from, per the
// bound vertex shader's own input declarations (ShaderInfo::declaresTexcoord0 /
// lowestDeclaredTexcoord, filled in ReflectShader from its dcl stream). The decalonly/
// diffcol_normal_decal/decalonly_cuberef/fauxinterior families declare a TEXCOORD1 input and NO
// TEXCOORD0 at all, and build their decal coordinate from it - the shim always sampled index 0
// regardless, which left those draws textured from whatever garbage sits there, or nothing.
// Gated: with the switch off this always answers 0, exactly today's behaviour.
inline int DecalUvSourceSet() {
    if (!g_settings.decalUvFromDeclaredSet) return 0;
    if (!g_curVS.declaresTexcoord0 && g_curVS.lowestDeclaredTexcoord == 1) return 1;
    return 0;
}


// Swap TEXCOORD0 from SHORT2 to an equivalent FLOAT2 stream.
//
// Remix DISCARDS SHORT2 texcoords - "[rtx-interleaver] Unsupported texcoord buffer format (80)",
// VkFormat 80 = R16G16_SSCALED - and that is the documented cause of "the world renders as flat
// material colour".
//
// This is safe to do to the GAME'S OWN draw and not only to a converted one, which is what lets
// cameraOnly use it. The replacement buffer holds the RAW short values widened to float, and a
// SHORT2 declaration delivers exactly those raw values to a shader. The game's vertex shader
// therefore reads identical numbers either way: a change of representation, not of the render.
bool InstallFloatUV(IDirect3DDevice9* dev, FFPScope& scope, bool deinstance) {
    if (!g_settings.remixShortUV || g_curLayout.skinned || !g_curDecl) return false;
    // FIX A: which vertex texcoord usage index this draw's albedo actually needs - see
    // DecalUvSourceSet. Testing the CHOSEN set's own type here, rather than always
    // texcoordType, is what lets a decalonly/fauxinterior draw (set 1, NO usage-index-0 input
    // at all) be recognised and converted instead of being judged against set 0's fields.
    const int uvSet = DecalUvSourceSet();
    const int srcType = (uvSet == 1) ? g_curLayout.texcoord1Type : g_curLayout.texcoordType;
    if (srcType != D3DDECLTYPE_SHORT2) {
        // set 1: this draw's own vertex shader wants TEXCOORD1 and the bound declaration does
        // not carry a usable one. Previously (before FIX A existed) a draw like this fell
        // through the OLD set-0-only test and returned false right here with NOTHING counted -
        // the exact silent failure the background for FIX A describes. Counted now, so
        // "failures: 0 convert" can never again hide this class.
        if (uvSet == 1) ++g_uvSet1Unavailable;
        return false;
    }
    // set 1 ONLY, crash fix 2026-09-14: these decal draws are INSTANCED, so their vertex
    // shader's own declaration legitimately places texcoord1 in stream 1, not stream 0 - but
    // UvBufferFor only ever reads STREAM 0's vertex data (at texcoord1Offset, using stream 0's
    // stride). Taking the set-1 path for a stream-1 (or otherwise unresolved) element reads
    // stream-1-shaped offsets out of stream 0's bytes - exactly the access violation that
    // crashed world load: 0xC0000005 reading near 0x59ADF000. Refuse it here, before
    // UvBufferFor ever runs, the same way the type check just above refuses an unusable type.
    if (uvSet == 1 && (g_curLayout.texcoord1Stream != 0 || g_curLayout.texcoord1Offset < 0)) {
        ++g_uvSet1WrongStream;
        return false;
    }
    IDirect3DVertexBuffer9* uvBuf = UvBufferFor(dev);
    IDirect3DVertexDeclaration9* alt = uvBuf ? FloatUVDeclaration(dev, g_curDecl) : nullptr;
    if (!uvBuf || !alt) return false;
    g_origSetVertexDeclaration(dev, alt);
    g_origSetStreamSource(dev, g_uvStream, uvBuf, g_uvBindOffset, kUvBytesPerVertex);
    // Our entries are indexed per vertex exactly as stream 0 is, so under instancing the two need
    // the same frequency. The default would read one uv per INSTANCE instead of one per vertex.
    if (g_instancedDraw && !deinstance) {
        g_origSetStreamSourceFreq(dev, g_uvStream, D3DSTREAMSOURCE_INDEXEDDATA | 1);
        scope.uvFreq = true;
    }
    scope.decl = g_curDecl;
    scope.uvStream = true;
    ++g_uvStreamDraws;
    if (uvSet == 1) ++g_uvSet1Draws;
    return true;
}


// parts/20_recorders_probes.cpp - timing, frame recorder, rolling draw record, frame stepping, small probes.
// NOT a standalone translation unit: #included, in order, by ../sr3rtx.cpp inside its anonymous
// namespace. Originally sr3rtx.cpp lines 14287-15368 (old file:line citations map here).
// ---------------------------------------------------------------- timing
//
// Stop inferring the stall from symptoms. Two fixes aimed at lock stalls did not clear it, and
// "it freezes when I look this way" is not something to keep reasoning about second-hand.
//
// This measures the only thing that settles it: wall-clock time per frame, and how much of that
// is spent inside OUR hooks. If the shim's share is small, the cost is in Remix or the bridge and
// nothing I do at the D3D9 boundary will fix it; if it is large, it is mine.
LARGE_INTEGER g_lastPresent{};
double g_frameMsAccum = 0.0, g_frameMsWorst = 0.0;
double g_shimMsAccum = 0.0, g_shimMsWorst = 0.0;
double g_shimMsThisFrame = 0.0;
unsigned g_timedFrames = 0;

// 2026-08-18: the accumulator above wraps ONLY DrawIndexedPrimitive/DrawPrimitive. So when the
// log read "frame worst 115 ms | shim worst 4.6 ms" it did NOT show the hitch was outside the
// shim - it showed the hitch was outside the DRAW PATH, which is a much weaker claim. Shader and
// texture creation, the render-target hook and Present itself were all outside the measurement,
// and creation work is exactly what varies with position: flying into a district streams in new
// materials. These close that gap by partitioning the whole Present-to-Present interval.
//
// The two "Last" values are from the PREVIOUS Present. That is deliberate and it is the correct
// attribution: g_lastPresent is stamped at the top of the hook, so everything the previous
// Present did after its own stamp - our report block, then the real Present - falls inside the
// interval being measured now.
double g_presentMsLast = 0.0;      // inside the real Present: Remix's own end-of-frame work
double g_ourPresentMsLast = 0.0;   // our end-of-frame block: the periodic report and bookkeeping
double g_presentMsAccum = 0.0, g_presentMsWorst = 0.0;
double g_otherMsAccum = 0.0;
unsigned g_hitches33 = 0, g_hitches100 = 0;   // frames over threshold, per report window
unsigned g_hitchLines = 0;
constexpr unsigned kMaxHitchLines = 60;       // a recorder that logs every hitch becomes one

// Which render target does converted geometry go to? If the same surfaces are converted into two
// different passes, Remix receives each one twice and they fight - which is what z-fighting on
// static geometry looks like. Counting conversions per target says whether that is happening,
// instead of assuming the prepass is the culprit.
struct ConvTarget { IDirect3DSurface9* surface; unsigned converted; UINT w, h; int fmt; };
constexpr int kMaxConvTargets = 12;
ConvTarget g_convTargets[kMaxConvTargets] = {};
int g_convTargetCount = 0;

void NoteConvertedTarget() {
    for (int i = 0; i < g_convTargetCount; ++i) {
        if (g_convTargets[i].surface == g_curRenderTarget) { ++g_convTargets[i].converted; return; }
    }
    if (g_convTargetCount >= kMaxConvTargets) return;
    ConvTarget& t = g_convTargets[g_convTargetCount++];
    t.surface = g_curRenderTarget;
    t.converted = 1;
    t.w = t.h = 0;
    t.fmt = 0;
    if (g_curRenderTarget) {
        D3DSURFACE_DESC d{};
        if (SUCCEEDED(g_curRenderTarget->GetDesc(&d))) { t.w = d.Width; t.h = d.Height; t.fmt = d.Format; }
    }
}

// ---------------------------------------------------------------- frame recorder
//
// One complete frame of everything the game submits, in submission order, to its own file.
// The per-frame counters say how much of each kind there is; this says what the frame IS -
// which passes run, in what order, against which render targets, with which materials, and
// what we did with each draw. That is the difference between tuning numbers and understanding
// the engine, and it is how the two-pass inferred-lighting structure was found.
//
// Written once, to sr3-rtx-frame.log, after the camera exists so it captures real gameplay.
// CAPTURE ON DEMAND.
//
// Both dumps fired once, automatically, on the first frame with enough draws to count as "in the
// world" - which is a second or two after loading, wherever the camera happens to point. Every
// character problem in this project has been diagnosed from whatever that arbitrary frame caught,
// and the user has now twice reported that it missed the thing they were looking at.
//
// A key press re-arms both, and each capture writes its own numbered file, so several can be taken
// in one session and compared. Polled once per frame in Present: GetAsyncKeyState's low bit is the
// "pressed since last call" latch, but it is unreliable when several things poll it, so the edge is
// detected from the high bit instead.
// Declared with the shape probe, further down; the capture re-arm needs them here.
extern FILE* g_shapeLog;
extern unsigned g_shapeProbeTarget;
extern unsigned g_shapeReports;
extern bool g_shapeProbeDone;

FILE* g_frameLog = nullptr;
bool g_frameDumpDone = false;
// Per-draw skinning facts, recorded ONLY on the frame-dump frame and printed on that draw's dump
// line. Three separate head symptoms were reported at once - misplaced, mis-textured, and
// sometimes doubled - and one wrong bind pose would produce all three, because a bind pose
// carries the geometry AND the UVs AND the identity of the mesh. Distinguishing "the bones are
// wrong" from "the mesh is wrong" needs the bind pose measured beside the posed result, per draw.
//
// The bind-pose EXTENT is what identifies the mesh: a head is a head-sized box whatever pose it
// is in, so a head draw whose bind pose measures a whole body is reading somebody else's mesh.
bool g_dumpSkin = false;              // this draw filled the fields below
bool g_dumpSkinCacheHit = false;      // bind pose came from the cache, not a fresh decode
UINT g_dumpSkinFirst = 0;
void* g_dumpSkinVB = nullptr;
unsigned g_dumpSkinBones = 0, g_dumpSkinLowBone = 0, g_dumpSkinHighBone = 0;
float g_dumpSkinBindMin[3] = {}, g_dumpSkinBindMax[3] = {};
float g_dumpSkinBindC[3] = {}, g_dumpSkinPosedC[3] = {};
// Where the bones this mesh actually uses came from. A head is its own mesh of 1426 vertices
// bound to 3-5 bones; the body beside it is bound to 47-58. Both index the SAME palette at c52,
// and every upload in this game starts at bone 0 - so if the head is drawn without an upload of
// its own, its bones 0..4 are the BODY's bones 0..4, which are the pelvis and spine. A head
// posed by the pelvis follows the body's core animation and parts company with the neck, which
// is the reported symptom stated exactly. These three fields decide it: a 5-bone mesh reading a
// 47-bone upload is reading somebody else's palette.
unsigned g_dumpSkinPalNewBones = 0, g_dumpSkinPalOldBones = 0;
unsigned g_dumpSkinPalNewDraw = 0, g_dumpSkinPalOldDraw = 0, g_dumpSkinPalGens = 0;
// Same per-draw-dump pattern, for the hardware (fixed-function) skin path - filled by
// SkinViaFixedFunction itself, right before it returns success, and consumed (then cleared) by
// RecordFrameDraw's own ffskin= annotation.
bool g_dumpSkinFF = false;
bool g_dumpSkinFFReuse = false;    // this draw reused the previous draw's staged palette
bool g_dumpSkinFFBone0 = false;    // this mesh uses bone 0 (WORLDMATRIX(0) IS D3DTS_WORLD)
unsigned g_dumpSkinFFBones = 0, g_dumpSkinFFLowBone = 0, g_dumpSkinFFHighBone = 0;
UINT g_dumpSkinFFSideOffset = 0, g_dumpSkinFFUvOffset = 0;
bool g_dumpSkinFFSingle = false;   // FIX 5: this draw was staged in single-bone mode (shader reads no BLENDWEIGHT)
int g_dumpSkinFFBake = 0;          // THE SHAPE BAKE: 0 none, 1 hit, 2 built, 3 rebuilt

unsigned g_frameDumpTarget = 0;
unsigned g_captureIndex = 0;        // 0 = the automatic first capture, then 1, 2, 3...
unsigned g_hudProbeReports = 0;     // the HUD draw probe's report count; re-armed with the captures
unsigned g_decalProbeReports = 0;   // ProbeDecalDraw's report count (defined far below); re-armed too
unsigned g_outsideWinReports = 0;   // OUTSIDE WINDOW probe's report count (TightenConvertedWindow, defined far below); re-armed too
// ---------------------------------------------------------------- the rolling draw record
// The last ringFrames frames' indexed draws, compact, written to sr3-rtx-ring-N.log on F9.
// The one-frame stretched polygons cannot be caught by a dump that fires two seconds after the
// key; this holds the frames BEFORE the key, each draw with its buffer's freshness.
struct RingDraw {
    unsigned frame, idx; unsigned char disp; UINT verts, prims;
    unsigned char proj, obj, skin, inst; signed char freshAge;
    char ps[20]; void* tex; void* alb; signed char albStage; signed char uvSet; float at[3]; const char* reason;
};
std::vector<std::vector<RingDraw>> g_ring;
unsigned g_ringSlotFrame = 0xFFFFFFFFu;
void RingRecordDraw(UINT numVertices, UINT primitiveCount, Disp d, int freshAge) {
    const int n = g_settings.ringFrames;
    if (n <= 0) return;
    if (g_ring.size() != static_cast<size_t>(n)) { try { g_ring.assign(static_cast<size_t>(n), {}); } catch (...) { return; } }
    auto& slot = g_ring[g_frames % static_cast<unsigned>(n)];
    if (g_ringSlotFrame != g_frames) { slot.clear(); g_ringSlotFrame = g_frames; }
    if (slot.size() >= 8192) return;
    RingDraw r{};
    r.frame = g_frames; r.idx = g_drawIndexThisFrame; r.disp = static_cast<unsigned char>(d);
    r.verts = numVertices; r.prims = primitiveCount;
    r.proj = g_curVS.usesProjTM ? 1 : 0; r.obj = g_curVS.usesObjTM ? 1 : 0; r.skin = g_curVS.skinned ? 1 : 0;
    r.inst = g_instancedDraw ? 1 : 0;
    r.freshAge = static_cast<signed char>(freshAge < 0 ? -1 : (freshAge > 100 ? 100 : freshAge));
    strncpy_s(r.ps, g_curPS.firstSampler ? g_curPS.firstSampler : "", _TRUNCATE);
    r.tex = g_curTexture[0];
    r.alb = g_lastEffectiveAlbedo;
    r.albStage = static_cast<signed char>(g_lastEffectiveAlbedoStage);
    r.uvSet = static_cast<signed char>(
        (g_lastEffectiveAlbedoStage >= 0 && g_lastEffectiveAlbedoStage < 8)
            ? g_curPS.samplerUv[g_lastEffectiveAlbedoStage] : -1);
    r.at[0] = g_vsConst[kRegObjTM][3]; r.at[1] = g_vsConst[kRegObjTM + 1][3]; r.at[2] = g_vsConst[kRegObjTM + 2][3];
    r.reason = g_dispReason;
    try { slot.push_back(r); } catch (...) {}
}
void CaptureFileName(char* out, size_t n, const char* stem);
void WriteRing() {
    if (g_ring.empty()) return;
    char fname[64];
    CaptureFileName(fname, sizeof(fname), "ring");
    FILE* f = _fsopen(fname, "w", _SH_DENYNO);
    if (!f) return;
    static const char* kD[] = {"CONVERT", "HIDE   ", "SKIP   ", "PASS   ", "MARK   "};
    const unsigned n = static_cast<unsigned>(g_ring.size());
    fprintf(f, "rolling draw record - the last %u frames before the capture key, oldest first\n"
               "FRESH=N: this draw's static stream-0 range was written N frames earlier (shown when within deferFreshFrames)\n\n", n);
    for (unsigned k = 1; k <= n; ++k) {
        const auto& slot = g_ring[(g_frames + k) % n];
        if (slot.empty()) continue;
        fprintf(f, "=== frame %u: %u draws ===\n", slot[0].frame, static_cast<unsigned>(slot.size()));
        for (const RingDraw& r : slot) {
            char fr[24] = "";
            if (r.freshAge >= 0) _snprintf_s(fr, _TRUNCATE, " FRESH=%d", r.freshAge);
            fprintf(f, "%5u %s v=%-6u p=%-6u vs%s%s%s inst=%u ps='%s' tex0=%p alb=%p@%d uv=%d at(%.1f %.1f %.1f)%s | %s\n",
                    r.idx, kD[r.disp < 5 ? r.disp : 2], r.verts, r.prims,
                    r.proj ? "[proj]" : "[    ]", r.obj ? "[obj]" : "[   ]", r.skin ? "[skin]" : "[    ]",
                    r.inst, r.ps, r.tex, r.alb, r.albStage, r.uvSet, r.at[0], r.at[1], r.at[2], fr, r.reason ? r.reason : "");
        }
    }
    fclose(f);
    Log("rolling draw record written to %s", fname);
}

bool g_frozen = false, g_pauseKeyWasDown = false, g_heldSinceLastPresent = false;
unsigned g_frameSteps = 0;
double g_frameStepHeldMs = 0.0;

bool g_captureKeyWasDown = false;
bool g_captureRequested = false;

// ASSET HASH PROBE: forward declarations. Defined far below (after GetTightIbShape/
// GetBaseMesh, which it reuses) but called from here - RearmCaptures arms it on the same F9
// trigger that re-arms the ring/frame dumps above - and from Hook_Present, which ticks the
// burst to its own end and logs the summary.
void ArmAssetHashProbe();
void TickAssetHashProbeBurst();

// Re-arm both dumps for the next frame. Called from Present, so no draw is half-recorded.
void RearmCaptures(bool ring) {
    ++g_captureIndex;
    g_frameDumpDone = false;
    g_frameDumpTarget = 0;
    if (g_frameLog) { fclose(g_frameLog); g_frameLog = nullptr; }
    g_shapeProbeDone = false;
    g_shapeProbeTarget = 0;
    g_shapeReports = 0;
    if (g_shapeLog) { fclose(g_shapeLog); g_shapeLog = nullptr; }
    g_hudProbeReports = 0;   // eight more "HUD draw:" blocks, with their matrices
    g_decalProbeReports = 0;   // eight more "DECAL PROBE" blocks, texture identity and UV range
    g_outsideWinReports = 0;   // twelve more "OUTSIDE WINDOW" blocks, TightenConvertedWindow's safety net
    g_rtInvariantProbeReports = 0;   // twelve more "BLOCKED (rasterized-frame invariant)" blocks
    g_injectProbeReports = 0;   // twelve more "INJECT PROBE" blocks (trigger preceded composite)
    g_injectNonRhwReports = 0;   // twelve more "INJECT CONTROL" blocks (satisfied, not RHW)
    if (ring) WriteRing();   // the frames BEFORE the key: where a one-frame glitch lives
    if (ring) ArmAssetHashProbe();   // ASSET HASH PROBE: same F9 trigger, its own burst window
    Log("CAPTURE %u armed - the next frame goes to sr3-rtx-frame-%u.log and "
        "sr3-rtx-shapes-%u.log (a manual capture dumps the frame whatever its draw count)",
        g_captureIndex, g_captureIndex, g_captureIndex);
    if (g_frozen) g_frameDumpTarget = g_frames + 1;   // frozen: the capture is the next stepped frame
}

void PollCaptureKey() {
    if (!g_settings.captureKey) return;
    const bool down = (GetAsyncKeyState(g_settings.captureKey) & 0x8000) != 0;
    if (down && !g_captureKeyWasDown) g_captureRequested = true;
    g_captureKeyWasDown = down;
    if (g_captureRequested) {
        g_captureRequested = false;
        RearmCaptures(true);
    }
}

// Capture 0 keeps the original filenames so nothing that reads them has to change.
// ---------------------------------------------------------------- frame stepping
// Held on the render thread AFTER the real Present, so the frame stays on screen while the
// game waits. The game's main thread stalls on the render thread within a frame, so a step is
// one game tick and one frame: hold the camera key while stepping and the camera turns one
// tick per press. Each stepped frame can carry a full frame dump (sr3-rtx-frame-N.log), so the
// frame that shows a one-frame fault already has its record when you see it.
void FrameStepHold() {
    if (!g_settings.frameStepPauseKey) return;
    const bool pauseDown = (GetAsyncKeyState(g_settings.frameStepPauseKey) & 0x8000) != 0;
    if (pauseDown && !g_pauseKeyWasDown) {
        g_frozen = !g_frozen;
        Log("FRAME STEP: %s at frame %u", g_frozen
            ? "FROZEN - the step key releases one frame, the capture key writes the ring, the pause key resumes"
            : "resumed", g_frames);
    }
    g_pauseKeyWasDown = pauseDown;
    if (!g_frozen) return;
    const LONGLONG t0 = Now();
    bool stepWasDown = (GetAsyncKeyState(g_settings.frameStepKey) & 0x8000) != 0;
    for (;;) {
        Sleep(4);
        const bool p = (GetAsyncKeyState(g_settings.frameStepPauseKey) & 0x8000) != 0;
        if (p && !g_pauseKeyWasDown) {
            g_pauseKeyWasDown = p;
            g_frozen = false;
            Log("FRAME STEP: resumed at frame %u after %u steps", g_frames, g_frameSteps);
            break;
        }
        g_pauseKeyWasDown = p;
        const bool st = (GetAsyncKeyState(g_settings.frameStepKey) & 0x8000) != 0;
        if (st && !stepWasDown) {
            ++g_frameSteps;
            if (g_settings.frameStepDumpEachStep) RearmCaptures(false);   // frozen: targets the next frame
            Log("FRAME STEP: step %u -> frame %u%s", g_frameSteps, g_frames + 1,
                g_settings.frameStepDumpEachStep ? " (full dump armed for it)" : "");
            break;
        }
        stepWasDown = st;
        PollCaptureKey();   // the capture key while frozen: the ring, and a dump armed for the next step
    }
    g_frameStepHeldMs += MsSince(t0);
    g_heldSinceLastPresent = true;
}

void CaptureFileName(char* out, size_t n, const char* stem) {
    if (g_captureIndex == 0) sprintf_s(out, n, "sr3-rtx-%s.log", stem);
    else                     sprintf_s(out, n, "sr3-rtx-%s-%u.log", stem, g_captureIndex);
}
// Draws submitted in the previous complete frame, and the floor that means "in the world".
constexpr unsigned kDumpMinDraws = 1500;
unsigned g_lastFrameDraws = 0;
IDirect3DSurface9* g_lastLoggedTarget = reinterpret_cast<IDirect3DSurface9*>(1);

// ---------------------------------------------------------------- what builds the atlas?
//
// Turning rtx.useVertexCapture off removed the fullscreen quads from in front of the camera, and
// blacked the character. That is not a guess - the atlas snoop measured it on both sides:
//
//     capture ON    atlas 2  2048x1024  mean 182.7      atlas 3  1024x512  mean 169.6
//     capture OFF   atlas 2  2048x1024  mean   0.0      atlas 3  1024x512  mean   0.0
//     (atlas 1, 1280x768, reads 14.3 in BOTH runs - it is CPU-written through LockRect and is
//      untouched by any of this, which is what proves the other two are GPU composites.)
//
// So SR3 builds the character's albedo with shader draws into an off-screen render target, and
// capture-off is a global skip of shader draws - rule 1 of this project, again: a draw whose
// RESULT THE ENGINE READS can never be skipped. The engine reads this one as a texture.
//
// The atlas is composited at character LOAD, not per frame, so no frame dump has ever contained
// it and nothing in this codebase knows what those draws are. This says what they are: the
// target, the shader, every bound texture, the blend, and the geometry. That is the input to
// deciding whether they can be re-issued as fixed function (which Remix accepts with capture
// off) or whether they need a different mechanism entirely.
unsigned g_atlasProbeReports = 0;
void ProbeAtlasComposite(IDirect3DDevice9* dev, UINT numVertices, UINT primitiveCount, Disp d) {
    if (!g_settings.atlasCompositeProbe || g_atlasProbeReports >= 64) return;
    // A render target at least 1024x512 that is NOT the screen. That admits the 2048x1024 and
    // 1024x512 atlases and excludes both the back buffer and the whole post-process chain, whose
    // targets are all smaller. Sized rather than named because the atlas surface is created at
    // character load and its address is not known in advance.
    if (!g_rt0Width || g_rt0Width < 1024 || g_rt0Height < 512) return;
    if (g_backBufferW && g_rt0Width == g_backBufferW && g_rt0Height == g_backBufferH) return;
    // A composite is a SCREEN-SPACE QUAD. Without this the 4096x4096 shadow map ate 60 of the
    // first 64 reports with ordinary world geometry, and the four draws that actually matter
    // nearly did not fit. A vertex shader that references projTM is drawing a 3D scene, not
    // compositing a texture.
    if (g_curVS.usesProjTM) return;

    ++g_atlasProbeReports;
    static const char* kDisp[] = {"CONVERT", "HIDE", "SKIP", "PASS", "MARK"};
    Log("ATLAS COMPOSITE #%u: target %ux%u | %s | v=%u p=%u | vs[%s%s%s] ps='%s' rank=%d | "
        "zw=%lu zt=%lu blend=%lu src=%lu dst=%lu alphaTest=%lu | %s",
        g_atlasProbeReports, g_rt0Width, g_rt0Height,
        kDisp[static_cast<int>(d)], numVertices, primitiveCount,
        g_curVS.usesProjTM ? "proj" : "", g_curVS.usesObjTM ? " obj" : "",
        g_curVS.skinned ? " skin" : "",
        g_curPS.firstSampler, g_curPS.albedoRank,
        ShadowGetRS(dev, D3DRS_ZWRITEENABLE), ShadowGetRS(dev, D3DRS_ZENABLE),
        ShadowGetRS(dev, D3DRS_ALPHABLENDENABLE), ShadowGetRS(dev, D3DRS_SRCBLEND),
        ShadowGetRS(dev, D3DRS_DESTBLEND), ShadowGetRS(dev, D3DRS_ALPHATESTENABLE),
        g_dispReason);
    // Every bound stage, with its size. A composite that reads one texture and writes it
    // straight through is fixed-function work; one that reads four and combines them is not, and
    // the count alone separates those two futures.
    for (int st = 0; st < 4; ++st) {
        if (!g_curTexture[st]) continue;
        UINT w = 0, h = 0;
        int fmt = 0;
        if (g_curTexture[st]->GetType() == D3DRTYPE_TEXTURE) {
            D3DSURFACE_DESC sd{};
            if (SUCCEEDED(static_cast<IDirect3DTexture9*>(g_curTexture[st])->GetLevelDesc(0, &sd))) {
                w = sd.Width; h = sd.Height; fmt = static_cast<int>(sd.Format);
            }
        }
        Log("    stage %d: tex=%p %ux%u fmt=%d", st, static_cast<void*>(g_curTexture[st]),
            w, h, fmt);
    }
}

void RecordFrameDraw(IDirect3DDevice9* dev, UINT numVertices, UINT primitiveCount, Disp d) {
    if (!g_settings.dumpFrame || g_frameDumpDone || !g_haveCamera) return;
    // Wait, rather than dumping the first frame that has a camera. The 2026-08-18 dump was taken
    // during loading: it recorded 94 converted draws where steady-state gameplay has 1,079, so
    // its populations described a half-built scene and its 1,302 pass-through draws in the
    // material pass were mostly "main camera not latched yet" rather than anything structural.
    // An instrument that samples the wrong moment is worse than none, because its numbers still
    // look authoritative.
    // A camera existing is not the same as being in the world. The delay alone was not enough:
    // the dump fired once during loading (3,877 draws, 94 converted) and once somewhere with 576
    // draws and 26 converted, while steady-state gameplay runs 2,500-5,000 draws with 600-1,500
    // converted. Both were quoted before anyone noticed they described nothing real.
    //
    // So the countdown does not START until a frame has actually submitted a world's worth of
    // geometry. That is a property of the frame rather than of elapsed time, so it cannot be
    // fooled by a slow load.
    // An F9 capture dumps whatever the frame holds: the pause menu is a few hundred draws and
    // the 1,500 floor (there to skip loading screens on the automatic capture) never let it in.
    const bool manual = g_captureIndex > 0;
    if (!g_frameDumpTarget) {
        if (g_lastFrameDraws < kDumpMinDraws && !manual) return;
        g_frameDumpTarget = g_frames + 1 + g_settings.dumpFrame;
    }
    if (g_frames + 1 != g_frameDumpTarget) return;
    // Check the gate AGAIN at the moment of firing, not only when the countdown started. The
    // third bad dump came from exactly this: the countdown began in gameplay and 1,800 frames
    // later landed on a 997-draw frame while the counters read 2,661. A condition tested once,
    // long before the thing it guards, guards nothing - so push the target forward instead.
    if (g_lastFrameDraws < kDumpMinDraws && !manual) {
        g_frameDumpTarget = g_frames + 1 + 60;
        return;
    }

    if (!g_frameLog) {
        char fname[64];
        CaptureFileName(fname, sizeof(fname), "frame");
        g_frameLog = _fsopen(fname, "w", _SH_DENYNO);
        if (!g_frameLog) { g_frameDumpDone = true; return; }
        fprintf(g_frameLog,
                "sr3-rtx complete frame dump - every DrawIndexedPrimitive in submission order\n"
                "frame %u, camera (%.1f %.1f %.1f)\n"
                "disposition: CONVERT = re-issued as fixed function and path-traced\n"
                "             HIDE    = still drawn, given an ortho projection so Remix ignores it\n"
                "             MARK    = our marker bound to stage 0, so Remix's ignore drops it\n"
                "             SKIP    = never reaches the device\n"
                "             PASS    = untouched; Remix reconstructs it from shader output\n"
                "the text after the last | on each line is WHY that disposition was chosen\n\n",
                g_frameDumpTarget, g_camX, g_camY, g_camZ);
    }

    // Announce render-target changes: this is what separates one pass from the next.
    if (g_curRenderTarget != g_lastLoggedTarget) {
        g_lastLoggedTarget = g_curRenderTarget;
        UINT w = 0, h = 0;
        int fmt = 0;
        if (g_curRenderTarget) {
            D3DSURFACE_DESC sd{};
            if (SUCCEEDED(g_curRenderTarget->GetDesc(&sd))) {
                w = sd.Width; h = sd.Height; fmt = static_cast<int>(sd.Format);
            }
        }
        fprintf(g_frameLog, "\n=== RENDER TARGET %p  %ux%u fmt=%d ===\n",
                static_cast<void*>(g_curRenderTarget), w, h, fmt);
    }

    // Must match the Disp enum exactly. It did not: Skip was inserted before PassThrough and
    // this array kept three entries, so every skipped draw was logged as "PASS" and every
    // pass-through read off the end of the array. That mislabelling hid the fact that 2,400
    // draws a frame were being skipped.
    static const char* kDisp[] = {"CONVERT", "HIDE   ", "SKIP   ", "PASS   ", "MARK   "};
    static_assert(sizeof(kDisp) / sizeof(kDisp[0]) == static_cast<int>(Disp::Count),
                  "kDisp must have one label per Disp value");

    // Skinning detail, dump-frame only. "bind" is the SIZE of the mesh in its own space, which
    // is what identifies it: a head is a head-sized box in every pose, so a head draw whose bind
    // box measures a whole body is reading another mesh's bind pose - and that ONE fault would
    // explain a misplaced head, a mis-textured head and a swapped head at the same time, because
    // a bind pose carries the geometry, the UVs and the identity together.
    // "moved" is bind centroid -> posed centroid: how far this mesh's own bones carried it.
    char skinTxt[248] = "";
    if (g_dumpSkin) {
        const float dx = g_dumpSkinPosedC[0] - g_dumpSkinBindC[0];
        const float dy = g_dumpSkinPosedC[1] - g_dumpSkinBindC[1];
        const float dz = g_dumpSkinPosedC[2] - g_dumpSkinBindC[2];
        _snprintf_s(skinTxt, _TRUNCATE,
                    " | skin first=%u vb=%p %s bind=%.2fx%.2fx%.2f at(%.2f %.2f %.2f) "
                    "moved %.2f bones=%u[%u..%u] pal=%u@d%u..%u@d%u gens=%u",
                    g_dumpSkinFirst, g_dumpSkinVB,
                    g_dumpSkinCacheHit ? "cached" : "DECODED",
                    g_dumpSkinBindMax[0] - g_dumpSkinBindMin[0],
                    g_dumpSkinBindMax[1] - g_dumpSkinBindMin[1],
                    g_dumpSkinBindMax[2] - g_dumpSkinBindMin[2],
                    g_dumpSkinBindC[0], g_dumpSkinBindC[1], g_dumpSkinBindC[2],
                    std::sqrt(dx * dx + dy * dy + dz * dz),
                    g_dumpSkinBones, g_dumpSkinLowBone, g_dumpSkinHighBone,
                    g_dumpSkinPalNewBones, g_dumpSkinPalNewDraw,
                    g_dumpSkinPalOldBones, g_dumpSkinPalOldDraw, g_dumpSkinPalGens);
        g_dumpSkin = false;   // so the next, non-skinned draw does not inherit this line
    }
    // Same idea, for the hardware (fixed-function) skin path - see g_dumpSkinFF's own comment.
    char ffTxt[200] = "";
    if (g_dumpSkinFF) {
        _snprintf_s(ffTxt, _TRUNCATE,
                    " | ffskin reuse=%d bone0=%d bones=%u[%u..%u] side@%u uv@%u single=%d bake=%d",
                    g_dumpSkinFFReuse ? 1 : 0, g_dumpSkinFFBone0 ? 1 : 0,
                    g_dumpSkinFFBones, g_dumpSkinFFLowBone, g_dumpSkinFFHighBone,
                    g_dumpSkinFFSideOffset, g_dumpSkinFFUvOffset, g_dumpSkinFFSingle ? 1 : 0,
                    g_dumpSkinFFBake);
        g_dumpSkinFF = false;   // so the next, non-FF-skinned draw does not inherit this line
    }
    // This draw's OWN camera and projection, from its constants: where the view matrix puts the
    // eye, whether the derived projection is perspective, whether the view differs from the
    // frame's main camera, and whether a vertex shader is bound at all. For tiny draws (a UI
    // quad) the raw positions too. Written so a pause-menu frame can be read without guessing.
    char xf[220] = "";
    {
        const D3DMATRIX view = FromRegisters(&g_vsConst[kRegWorld2View][0], 3);
        const D3DMATRIX viewProj = FromRegisters(&g_vsConst[kRegProjTM][0], 4);
        D3DMATRIX proj, invView;
        if (IsFinite(view) && IsFinite(viewProj) && DeriveProjection(view, viewProj, proj) && Invert(view, invView))
            _snprintf_s(xf, _TRUNCATE, " | cam(%.1f %.1f %.1f) proj(%.3f %.3f %.3f %.3f %s)%s%s",
                        invView._41, invView._42, invView._43, proj._11, proj._22, proj._34, proj._44,
                        IsPerspective(proj) ? "persp" : "ORTHO",
                        (g_haveMainCamera && !Same(view, g_mainView)) ? " OTHER-CAM" : "",
                        g_lastVS ? "" : " NOVS");
        else
            _snprintf_s(xf, _TRUNCATE, " | (no transform)%s", g_lastVS ? "" : " NOVS");
    }
    char rawTxt[240] = "";
    if (numVertices <= 8 && g_stream0 && g_stream0Stride >= 12 && g_curLayout.posOffset >= 0 &&
        g_curLayout.posType == D3DDECLTYPE_FLOAT3) {
        D3DVERTEXBUFFER_DESC vbd{};
        if (SUCCEEDED(g_stream0->GetDesc(&vbd)) && !(vbd.Usage & D3DUSAGE_DYNAMIC) &&
            VertexRangeFits(g_stream0, g_stream0Offset, g_curDrawFirstVertex, numVertices, g_stream0Stride)) {
            void* mapped = nullptr;
            if (SUCCEEDED(g_stream0->Lock(g_stream0Offset + g_curDrawFirstVertex * g_stream0Stride,
                                          numVertices * g_stream0Stride, &mapped,
                                          D3DLOCK_READONLY | D3DLOCK_NOSYSLOCK)) && mapped) {
                const unsigned char* b = static_cast<const unsigned char*>(mapped);
                int off = _snprintf_s(rawTxt, _TRUNCATE, " | raw");
                for (UINT i = 0; i < numVertices && i < 4 && off > 0 && off < 200; ++i) {
                    const float* v = reinterpret_cast<const float*>(b + i * g_stream0Stride + g_curLayout.posOffset);
                    const int n = _snprintf_s(rawTxt + off, sizeof(rawTxt) - off, _TRUNCATE, " (%.2f %.2f %.2f)", v[0], v[1], v[2]);
                    if (n < 0) break;
                    off += n;
                }
                g_stream0->Unlock();
            }
        }
    }
    // The chosen albedo's format and colour-channel count, cached (see AlbedoFormatFor) so
    // this costs nothing beyond the frame dump's own overhead - one GetLevelDesc per distinct
    // texture, not per draw. Printed below so a frame dump shows how many bound albedos are
    // colourless (D3DFMT_A8 and the like) across the WHOLE frame, not only the eight decal
    // probe slots.
    const AlbedoFormatInfo& albFmtLine = AlbedoFormatFor(g_lastEffectiveAlbedo);
    fprintf(g_frameLog,
            "%5u %s v=%-6u p=%-6u zw=%lu zt=%lu zf=%lu bias=%#lx blend=%lu(%lu/%lu) cw=%#lx layer=%u pull=%#x | vs%s%s%s ps='%s' rank=%d low='%s'@%d | "
            "tex0=%p%s alb=%p@%d albfmt=%u(%uch) uv=%d%s tc0type=%d tc0stream=%d ttff=%d aarg1=%u su=%.6f inst=%d at(%.1f %.1f %.1f) s0off=%u%s%s%s%s | %s\n",
            g_drawIndexThisFrame, kDisp[static_cast<int>(d)], numVertices, primitiveCount,
            ShadowGetRS(dev, D3DRS_ZWRITEENABLE), ShadowGetRS(dev, D3DRS_ZENABLE),
            ShadowGetRS(dev, D3DRS_ZFUNC), ShadowGetRS(dev, D3DRS_DEPTHBIAS),
            ShadowGetRS(dev, D3DRS_ALPHABLENDENABLE), ShadowGetRS(dev, D3DRS_SRCBLEND),
            ShadowGetRS(dev, D3DRS_DESTBLEND), ShadowGetRS(dev, D3DRS_COLORWRITEENABLE), g_curLayer,
            (d == Disp::Convert) ? g_curPullWhy : 0u,
            g_curVS.usesProjTM ? "[proj]" : "[     ]",
            g_curVS.usesObjTM ? "[obj]" : "[   ]",
            g_curVS.skinned ? "[skin]" : "[    ]",
            g_curPS.firstSampler, g_curPS.albedoRank,
            g_curPS.lowestSampler, g_curPS.lowestSamplerReg,
            static_cast<void*>(g_curTexture[0]),
            g_rtTextures.count(g_curTexture[0]) ? "(RT)" : "    ",
            static_cast<void*>(g_lastEffectiveAlbedo), g_lastEffectiveAlbedoStage,
            static_cast<unsigned>(albFmtLine.fmt), albFmtLine.channels,
            (g_lastEffectiveAlbedoStage >= 0 && g_lastEffectiveAlbedoStage < 8)
                ? g_curPS.samplerUv[g_lastEffectiveAlbedoStage] : -1,
            (g_lastEffectiveAlbedoStage >= 0 && g_lastEffectiveAlbedoStage < 8)
                ? (g_curPS.samplerUvDirect[g_lastEffectiveAlbedoStage] ? "" : "*") : "",
            // CHANGE 3, 2026-09-18: the five fields that would settle several open questions
            // at once, published from where each is actually set rather than recomputed
            // here. tc0type/tc0stream are g_curLayout's own fields (a tc0type=-1 alongside a
            // non-zero tc0stream names the declaration-side blind spot ParseDeclaration has
            // for a TEXCOORD0 that lives outside stream 0 - see VertexLayout::texcoordStream
            // and ParseDeclaration). ttff and aarg1 are read straight out of the TSS shadow
            // SetupTextureStages already maintains for stage 0, so this is the REAL state a
            // Convert draw was actually given, not a guess at it - on the Alpha_MaskSampler
            // rows this is what confirms or refutes CHANGE 1's premise directly. su is
            // g_decalProbeSu, already published by SetupTextureStages for ProbeDecalDraw and
            // reused here as-is rather than recomputed.
            g_curLayout.texcoordType, g_curLayout.texcoordStream,
            static_cast<int>(g_tssShadow[0][D3DTSS_TEXTURETRANSFORMFLAGS]),
            static_cast<unsigned>(g_tssShadow[0][D3DTSS_ALPHAARG1]),
            g_decalProbeSu,
            g_instancedDraw ? 1 : 0,
            // Two draws of one mesh at ONE position are a duplicate; at two positions they are two
            // objects that happen to share a texture. Nothing else in this line separates those,
            // and the head double-draw cannot be diagnosed without knowing which it is.
            g_vsConst[kRegObjTM][3], g_vsConst[kRegObjTM + 1][3], g_vsConst[kRegObjTM + 2][3],
            g_stream0Offset, skinTxt, ffTxt, xf, rawTxt, g_dispReason);
}

void FinishFrameDump() {
    if (!g_frameLog || g_frameDumpDone) return;
    if (g_frames + 1 != g_frameDumpTarget) {
        fprintf(g_frameLog, "\n%u draws total in the frame.\n", g_drawIndexThisFrame);
        fclose(g_frameLog);
        g_frameLog = nullptr;
        g_frameDumpDone = true;
        Log("frame dump written to sr3-rtx-frame.log (%u draws)", g_drawIndexThisFrame);
    }
}

// Dump every state that could distinguish a specific shape from ordinary world geometry. The
// discs surrounding the player write depth AND carry a colour texture, so none of the existing
// property tests separates them - this exists to find one that does, without resorting to
// removing their textures globally.
unsigned g_probeReports = 0;
void ProbeDraw(IDirect3DDevice9* dev, UINT numVertices, UINT primitiveCount, bool converted) {
    if (!g_settings.probeVertCount || g_probeReports >= 30 || !g_haveCamera) return;
    bool wanted = false;
    for (int i = 0; i < g_settings.probeVertCount; ++i)
        if (static_cast<int>(numVertices) == g_settings.probeVerts[i]) { wanted = true; break; }
    if (!wanted) return;
    ++g_probeReports;

    static const char* kBlend[] = {"?", "ZERO", "ONE", "SRCCOLOR", "INVSRCCOLOR", "SRCALPHA",
                                   "INVSRCALPHA", "DESTALPHA", "INVDESTALPHA", "DESTCOLOR",
                                   "INVDESTCOLOR", "SRCALPHASAT"};
    const DWORD srcb = ShadowGetRS(dev, D3DRS_SRCBLEND);
    const DWORD dstb = ShadowGetRS(dev, D3DRS_DESTBLEND);
    Log("PROBE verts=%u prims=%u converted=%d | zwrite=%lu ztest=%lu zfunc=%lu | "
        "alphablend=%lu src=%s dst=%s | alphatest=%lu ref=%lu | colorwrite=%#lx cull=%lu | "
        "fog=%lu | ps='%s' albedoRank=%d | projTM=%d objTM=%d instanced=%d | tex0=%p target=%p",
        numVertices, primitiveCount, converted ? 1 : 0,
        ShadowGetRS(dev, D3DRS_ZWRITEENABLE), ShadowGetRS(dev, D3DRS_ZENABLE),
        ShadowGetRS(dev, D3DRS_ZFUNC),
        ShadowGetRS(dev, D3DRS_ALPHABLENDENABLE),
        kBlend[srcb < 12 ? srcb : 0], kBlend[dstb < 12 ? dstb : 0],
        ShadowGetRS(dev, D3DRS_ALPHATESTENABLE), ShadowGetRS(dev, D3DRS_ALPHAREF),
        ShadowGetRS(dev, D3DRS_COLORWRITEENABLE), ShadowGetRS(dev, D3DRS_CULLMODE),
        ShadowGetRS(dev, D3DRS_FOGENABLE),
        g_curPS.firstSampler, g_curPS.albedoRank,
        g_curVS.usesProjTM, g_curVS.usesObjTM, g_instancedDraw ? 1 : 0,
        static_cast<void*>(g_curTexture[0]), static_cast<void*>(g_curRenderTarget));
}

// ---------------------------------------------------------------- HUD probe
//
// The HUD renders as flat planes floating in the world because Remix's vertex capture rebuilds it
// as world geometry. `Disp::Hide` cannot fix that - it sets D3DTS_PROJECTION, which is
// fixed-function state that Remix never reads for a shader-driven draw, proven when 5 demoted
// quads a frame changed nothing on screen.
//
// The mechanism that CAN work is converting these quads to fixed function, so Remix reads a
// projection it actually sees and `orthographicIsUI` fires. That requires knowing what space the
// HUD's vertex positions are already in, because nulling the vertex shader makes fixed function
// transform them itself:
//
//   * already clip space   -> identity world/view/projection passes them straight through;
//   * screen pixels        -> needs an orthographic projection sized to the back buffer;
//   * D3DDECLUSAGE_POSITIONT -> fixed function bypasses transformation entirely.
//
// Guessing between those would be three builds. This reports the declaration and the actual
// numbers so it is one.

// Re-aimed 2026-08-18 after the first version reported nothing: `g_isHudDraw` never fires,
// because the HUD is NOT screen-space by our test. The frame dump shows the last non-post draws
// referencing BOTH projTM and objTM and being converted like world geometry, into a 1280x720
// A8R8G8B8 surface that is distinct from the HDR material target. Converting UI with the world's
// view and projection is precisely how it ends up as planes standing in the world.
//
// So the trigger is the RENDER TARGET, not the shader: a back-buffer-sized 8-bit surface is where
// this engine composites its UI, while the scene lives in A16B16G16R16F. That is a property of
// the pass rather than an index into a list of targets - the distinction that matters, because
// selecting passes by target INDEX is a recorded dead end (indices are not stable).
void ProbeHudDraw(UINT minIndex, UINT numVertices, Disp d) {
    if (!g_settings.hudProbe) return;
    if (g_hudProbeReports >= 8 || !g_haveCamera) return;
    if (d != Disp::Convert && d != Disp::PassThrough) return;
    if (!g_curRenderTarget) return;
    D3DSURFACE_DESC rt{};
    if (FAILED(g_curRenderTarget->GetDesc(&rt))) return;
    const bool eightBitFullSize = (rt.Format == D3DFMT_A8R8G8B8 || rt.Format == D3DFMT_X8R8G8B8) &&
                                  rt.Width >= 1000;
    if (!eightBitFullSize) return;
    if (!g_stream0 || g_stream0Stride < 8) return;
    if (numVertices < 3 || numVertices > 256) return;
    D3DVERTEXBUFFER_DESC vbd{};
    if (FAILED(g_stream0->GetDesc(&vbd))) return;

    char raw[256];
    raw[0] = '\0';
    if (!(vbd.Usage & D3DUSAGE_DYNAMIC) && g_curLayout.posOffset >= 0) {
        void* mapped = nullptr;
        if (SUCCEEDED(g_stream0->Lock(g_stream0Offset + minIndex * g_stream0Stride,
                                      numVertices * g_stream0Stride, &mapped,
                                      D3DLOCK_READONLY | D3DLOCK_NOSYSLOCK)) && mapped) {
            const unsigned char* b = static_cast<const unsigned char*>(mapped);
            int off = 0;
            for (UINT i = 0; i < numVertices && i < 4; ++i) {
                const float* v = reinterpret_cast<const float*>(
                    b + i * g_stream0Stride + g_curLayout.posOffset);
                off += _snprintf_s(raw + off, sizeof(raw) - off, _TRUNCATE,
                                   "(%.3f %.3f %.3f) ", v[0], v[1], v[2]);
            }
            g_stream0->Unlock();
        }
    } else {
        strcpy_s(raw, "<dynamic buffer, not read>");
    }

    ++g_hudProbeReports;
    static const char* kD[] = {"CONVERT","HIDE","SKIP","PASS","MARK"};
    Log("HUD draw: %s verts=%u target %ux%u fmt=%d | stride=%u posType=%d posOffset=%d "
        "posStream=%d positionT=%d elements=%d usage=%#lx | vs proj=%d obj=%d ps='%s' rank=%d",
        kD[static_cast<int>(d)], numVertices, rt.Width, rt.Height, static_cast<int>(rt.Format),
        g_stream0Stride, g_curLayout.posType, g_curLayout.posOffset, g_curLayout.posStream,
        g_curLayout.hasPositionT ? 1 : 0, g_curLayout.elements, vbd.Usage,
        g_curVS.usesProjTM, g_curVS.usesObjTM, g_curPS.firstSampler, g_curPS.albedoRank);
    Log("    raw positions: %s", raw);
    // The transforms we would hand fixed function. If the projection is orthographic, Remix's own
    // orthographicIsUI would classify these correctly the moment they are converted - which is the
    // whole question this probe exists to answer.
    D3DMATRIX w, v, pj;
    if (ComputeTransforms(w, v, pj)) {
        Log("    proj row0 (%.4f %.4f %.4f %.4f) row3 (%.4f %.4f %.4f %.4f) perspective=%d",
            pj._11, pj._12, pj._13, pj._14, pj._41, pj._42, pj._43, pj._44,
            IsPerspective(pj) ? 1 : 0);
        Log("    world translation (%.2f %.2f %.2f), camera (%.2f %.2f %.2f)",
            w._41, w._42, w._43, g_camX, g_camY, g_camZ);
        const D3DMATRIX rawProjTM = FromRegisters(&g_vsConst[kRegProjTM][0], 4);
        Log("    raw projTM row0 (%.4f %.4f %.4f %.4f) row3 (%.4f %.4f %.4f %.4f) perspective=%d",
            rawProjTM._11, rawProjTM._12, rawProjTM._13, rawProjTM._14,
            rawProjTM._41, rawProjTM._42, rawProjTM._43, rawProjTM._44,
            IsPerspective(rawProjTM) ? 1 : 0);
    } else {
        Log("    ComputeTransforms failed - no usable transform for this draw");
    }
}

// ---------------------------------------------------------------- sky probe
//
// The sky returns with screenSpaceMode=0 but ONLY in the rasterised overlay, never in the path
// tracer. Yet the rfg-skybox shaders carry ordinary Diffuse_Map / Decal_Map samplers and should
// convert like anything else. So report what actually happens to them: disposition, size, where
// they sit relative to the camera, and how far the geometry extends.
//
// The far plane is the first thing to check. SR3's projection is near 0.15, far 5000, and a
// skybox drawn at a radius beyond that is clipped away entirely by fixed function - which would
// produce exactly "no sky in the path tracer" while the game's own shader path, which need not
// respect the same clip, still draws it.
unsigned g_skyDrawReports = 0;

void ProbeSkyDraw(UINT minIndex, UINT numVertices, Disp d) {
    if (!g_curPS.skyShader || g_skyDrawReports >= 12 || !g_haveCamera) return;
    if (!g_stream0 || g_stream0Stride < 12 || g_curLayout.posOffset < 0) return;
    if (numVertices < 3 || numVertices > 65536) return;
    D3DVERTEXBUFFER_DESC vbd{};
    if (FAILED(g_stream0->GetDesc(&vbd)) || (vbd.Usage & D3DUSAGE_DYNAMIC)) return;

    const UINT first = g_stream0Offset + minIndex * g_stream0Stride;
    void* mapped = nullptr;
    if (FAILED(g_stream0->Lock(first, numVertices * g_stream0Stride, &mapped,
                               D3DLOCK_READONLY | D3DLOCK_NOSYSLOCK)) || !mapped)
        return;
    D3DMATRIX world = kIdentity;
    if (g_instancedDraw) { if (!InstanceWorld(world)) world = kIdentity; }
    else if (g_curVS.usesObjTM) world = FromRegisters(&g_vsConst[kRegObjTM][0], 3);
    if (!IsFinite(world)) world = kIdentity;

    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    const unsigned char* base = static_cast<const unsigned char*>(mapped);
    const UINT step = (numVertices > 128) ? (numVertices / 128) : 1;
    for (UINT i = 0; i < numVertices; i += step) {
        const float* v = reinterpret_cast<const float*>(
            base + i * g_stream0Stride + g_curLayout.posOffset);
        if (!std::isfinite(v[0]) || !std::isfinite(v[1]) || !std::isfinite(v[2])) continue;
        const D3DVECTOR w = TransformPoint(v, world);
        const float c[3] = {w.x, w.y, w.z};
        for (int k = 0; k < 3; ++k) {
            if (c[k] < lo[k]) lo[k] = c[k];
            if (c[k] > hi[k]) hi[k] = c[k];
        }
    }
    g_stream0->Unlock();
    ++g_skyDrawReports;

    const float cx = (lo[0]+hi[0])*0.5f, cy = (lo[1]+hi[1])*0.5f, cz = (lo[2]+hi[2])*0.5f;
    const float dx = cx-g_camX, dy = cy-g_camY, dz = cz-g_camZ;
    static const char* kD[] = {"CONVERT","HIDE","SKIP","PASS","MARK"};
    Log("SKY draw: %s verts=%u ps='%s' rank=%d | size %.0fx%.0fx%.0f centre (%.0f %.0f %.0f) "
        "%.0f from camera | why: %s",
        kD[static_cast<int>(d)], numVertices, g_curPS.firstSampler, g_curPS.albedoRank,
        hi[0]-lo[0], hi[1]-lo[1], hi[2]-lo[2], cx, cy, cz,
        std::sqrt(dx*dx+dy*dy+dz*dz), g_dispReason);
}

// ---------------------------------------------------------------- back-buffer clear
//
// "Hall of mirrors" - the uncleared-framebuffer effect, where a region that nothing drew this
// frame still shows the last pixels that were there. Reported at some angles and locations while
// flying, i.e. wherever path-traced geometry does not cover the screen.
//
// The cause follows directly from what we did. Session 14 established that **the composite quad
// is the only draw that writes the back buffer**, and that dropping it froze the image entirely.
// We now MARK that quad, and Remix's ignore list drops it - so nothing writes the back buffer any
// more. Where geometry covers the screen this is invisible; where it does not, the stale contents
// show through.
//
// It also corrects a claim from session 15. "Ignore removes a draw from the ray-traced scene
// while the game's own rasterisation survives" was inferred from the engine's draw count not
// collapsing - but that count depends on the engine reading back the PREPASS, not on the back
// buffer being written. This artefact is direct evidence that the rasterisation is dropped too.
//
// The fix is to write the back buffer ourselves: one Clear per frame, issued the first time a
// draw targets it, which is before Remix composites anything. Black rather than a colour, so a
// region with genuinely nothing in it reads as empty instead of as an artefact.
IDirect3DSurface9* g_backBufferSurface = nullptr;
bool g_clearedThisFrame = false;
unsigned g_backBufferClears = 0;

void ClearBackBufferOnce(IDirect3DDevice9* dev) {
    if (!g_settings.clearBackBuffer || g_clearedThisFrame) return;
    if (!g_backBufferSurface || g_curRenderTarget != g_backBufferSurface) return;
    g_clearedThisFrame = true;
    g_internal = true;
    g_internalThread = GetCurrentThreadId();
    dev->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_ARGB(255, 0, 0, 0), 1.0f, 0);
    g_internal = false;
    ++g_backBufferClears;
}

// ---------------------------------------------------------------- bound albedo probe
//
// "the path tracer does not show texture. i only see basic material color."
//
// Every counter says otherwise: 999 converted draws a frame, 84 blanked, 0 named-but-not-bound,
// 0 left empty by the render-target exclusion. So the shim believes it is binding a real texture
// to nearly every surface, and the screen disagrees. One of those two is wrong and the counters
// cannot settle it, because they only record that a POINTER was non-null - never what it points
// at.
//
// This records what is actually bound: dimensions, format and mip count of the stage-0 texture on
// converted draws. A 4x4 or 1x1 surface renders as one flat colour, which is exactly the reported
// symptom; a 1024x1024 DXT means the texture is fine and the problem is on Remix's side of the
// bridge. Those two need completely different work, which is why this is worth one run.
//
// It also matters for the Remastered asset-replacement plan: Remix keys a replacement off the
// ORIGINAL texture's hash, so whatever is bound here is what the user will be mapping new assets
// onto. If the wrong texture reaches Remix, every replacement is authored against the wrong hash.
unsigned g_albedoProbeReports = 0;
char g_albedoProbeNames[20][28] = {};

void ProbeBoundAlbedo(Disp d) {
    if (d != Disp::Convert || g_albedoProbeReports >= 20) return;
    if (!g_curPS.isPixelShader || !g_curPS.firstSampler[0]) return;
    for (unsigned i = 0; i < g_albedoProbeReports; ++i)
        if (!strcmp(g_albedoProbeNames[i], g_curPS.firstSampler)) return;

    IDirect3DBaseTexture9* t = g_lastBoundAlbedo;
    if (!t) {
        strncpy_s(g_albedoProbeNames[g_albedoProbeReports], g_curPS.firstSampler, 27);
        ++g_albedoProbeReports;
        Log("ALBEDO BOUND #%u: ps='%s' rank=%d -> NOTHING BOUND", g_albedoProbeReports,
            g_curPS.firstSampler, g_curPS.albedoRank);
        return;
    }
    if (t->GetType() != D3DRTYPE_TEXTURE) return;
    D3DSURFACE_DESC sd{};
    if (FAILED(static_cast<IDirect3DTexture9*>(t)->GetLevelDesc(0, &sd))) return;

    strncpy_s(g_albedoProbeNames[g_albedoProbeReports], g_curPS.firstSampler, 27);
    ++g_albedoProbeReports;
    Log("ALBEDO BOUND #%u: ps='%s' rank=%d -> %ux%u fmt=%d mips=%lu pool=%d usage=0x%lX %s%s",
        g_albedoProbeReports, g_curPS.firstSampler, g_curPS.albedoRank, sd.Width, sd.Height,
        static_cast<int>(sd.Format), static_cast<unsigned long>(t->GetLevelCount()),
        static_cast<int>(sd.Pool), static_cast<unsigned long>(sd.Usage),
        g_rtTextures.count(t) ? "[RENDER TARGET] " : "",
        g_lastAlbedoFromCache ? "[from mesh cache]" : "");
}

// ---------------------------------------------------------------- decal / particle probe
//
// "particle effects like blood, bulletholes, tiretracks and others, their texture gets fullscreen
// as a plane blocking our camera like the composite passes."
//
// Blood, bullet holes and tyre tracks are small quads. A small quad that ends up covering the
// screen has been given a transform that is not its own, so the thing to record is the transform,
// not the geometry - and that needs no buffer lock at all, which keeps this off the list of
// probes that quietly cost a lock per draw forever.
//
// Named once per distinct sampler so a handful of lines identifies the material family rather
// than filling the log with one line per bullet hole.
unsigned g_decalReports = 0;
char g_decalNames[16][28] = {};

void ProbeDecal(IDirect3DDevice9* dev, UINT numVertices, Disp d) {
    // 64, not 12. A bullet hole is four vertices, but the engine may batch many impacts into one
    // draw, and a cap tight enough to mean "one quad" would miss exactly that case. Deduping by
    // sampler keeps the output to a handful of lines regardless of how many draws qualify.
    if (d != Disp::Convert || numVertices > 64 || g_decalReports >= 16) return;
    if (!g_curPS.isPixelShader || !g_curPS.firstSampler[0]) return;
    for (unsigned i = 0; i < g_decalReports; ++i)
        if (!strcmp(g_decalNames[i], g_curPS.firstSampler)) return;
    strncpy_s(g_decalNames[g_decalReports], g_curPS.firstSampler, 27);
    ++g_decalReports;
    // Read from the shadowed registers, exactly as the shape probe does, so the probe needs
    // nothing passed in and cannot disagree with what the conversion actually used.
    const D3DMATRIX world = g_curVS.usesObjTM ? FromRegisters(&g_vsConst[kRegObjTM][0], 3)
                                              : kIdentity;
    // The world matrix's three basis-vector lengths are the object's scale. A decal drawn at
    // its authored size has small ones; a quad stretched across the view has at least one that
    // is enormous, and that single number separates "it is a decal" from "it is a screen quad".
    const float sx = std::sqrt(world._11 * world._11 + world._12 * world._12 + world._13 * world._13);
    const float sy = std::sqrt(world._21 * world._21 + world._22 * world._22 + world._23 * world._23);
    const float sz = std::sqrt(world._31 * world._31 + world._32 * world._32 + world._33 * world._33);
    Log("DECAL/PARTICLE #%u: ps='%s' rank=%d verts=%u usesObjTM=%d | world scale "
        "(%.2f %.2f %.2f) translation (%.1f %.1f %.1f) | camera (%.1f %.1f %.1f) | blend=%lu",
        g_decalReports, g_curPS.firstSampler, g_curPS.albedoRank, numVertices,
        g_curVS.usesObjTM ? 1 : 0, sx, sy, sz, world._41, world._42, world._43,
        g_camX, g_camY, g_camZ, ShadowGetRS(dev, D3DRS_ALPHABLENDENABLE));
}

// ---------------------------------------------------------------- shape probe
//
// "Shapes around me... like a dish, a cap and a ring." That is a description of GEOMETRY, and the
// project has repeatedly guessed at such descriptions and been wrong - the same artefacts have
// been attributed to light volumes, to decals, to texture problems and to the prepass across
// sessions 9-16. So measure them instead: for ONE frame, compute every draw's world-space
// bounding box and report the ones that are small and near the player.
//
// A dish, a cap and a ring are all FLAT - one axis far smaller than the other two - which an
// axis-aligned box measures directly. Size, centre, distance from the camera, the shader that
// drew it and what we did with it should be enough to name each one.
//
// Cost is bounded by construction: one frame, and the LOCK is gated by the same conditions as the
// report, which is the mistake MeasureWorldExtent made - it capped its output at 20 lines while
// continuing to lock a vertex buffer on every converted draw, ~1,400 times a frame, to feed a
// diagnostic that had stopped printing.
FILE* g_shapeLog = nullptr;
unsigned g_shapeProbeTarget = 0;
// ---------------------------------------------------------------- skinning probe
//
// Everything the CPU skinning port has to know, measured instead of assumed. The algorithm
// itself is already settled from the disassembly of debug_diffuse_only_c.fxo_pc:
//
//   pos' = ( SUM_i  w_i * Bone[idx_i] ) * float4(pos, 1)  /  SUM_i w_i
//
// with Bone[] a row-major float3x4 palette at c52, 3 registers per bone, 64 bones, and objTM
// (c32) applied AFTER skinning - so the port can skin into object space and reuse the existing
// WORLD/VIEW/PROJECTION path unchanged.
//
// What the disassembly canNOT say, and this probe must:
//   - how blendweight/blendindices are STORED (UBYTE4 vs UBYTE4N changes the decode, and the
//     shader's divide-by-sum hides the difference until poses come out wrong);
//   - whether the base mesh sits in a static buffer we may read-lock, or a dynamic one we
//     must not;
//   - whether characters carry distinct objTM translations. That last one is not for the port
//     at all - it is the direct test of the instance-matching theory behind the AABB change.
unsigned g_skinProbeReports = 0;
unsigned g_skinProbeFrame = 0;

void ProbeSkinned(UINT minIndex, UINT numVertices, Disp d) {
    if (!g_settings.skinProbe || !g_haveCamera || !g_curLayout.skinned) return;
    if (g_skinProbeReports >= 12) return;
    // One frame, so the report is a coherent snapshot of a single scene rather than a mixture.
    if (!g_skinProbeFrame) g_skinProbeFrame = g_frames;
    if (g_frames != g_skinProbeFrame) return;

    ++g_skinProbeReports;

    const D3DMATRIX obj = FromRegisters(&g_vsConst[kRegObjTM][0], 3);
    // How much of the palette is live? An unused slot is left as whatever the last character
    // wrote, so "non-identity" is the wrong test; count slots that differ from their neighbour
    // instead, which at least distinguishes a populated palette from a stale one.
    int liveBones = 0;
    for (int b = 0; b < 64; ++b) {
        const float* r0 = &g_vsConst[kRegBonePalette + b * 3][0];
        if (!std::isfinite(r0[0]) || !std::isfinite(r0[3])) break;
        if (r0[0] != 0.0f || r0[1] != 0.0f || r0[2] != 0.0f || r0[3] != 0.0f) ++liveBones;
    }

    D3DVERTEXBUFFER_DESC vbd{};
    const bool haveDesc = g_stream0 && SUCCEEDED(g_stream0->GetDesc(&vbd));
    const bool dynamic = haveDesc && (vbd.Usage & D3DUSAGE_DYNAMIC) != 0;

    Log("SKINNED draw #%u: verts=%u disp=%d stride=%u | VB usage=0x%X%s | live bone regs %d "
        "| objTM t=(%.2f %.2f %.2f)",
        g_skinProbeReports, numVertices, static_cast<int>(d), g_stream0Stride,
        haveDesc ? vbd.Usage : 0u, dynamic ? " DYNAMIC - must not read-lock" : " static",
        liveBones, obj._41, obj._42, obj._43);
    Log("    decl: pos type=%s@%d  normal type=%s@%d  weights type=%s@%d  indices type=%s@%d",
        DeclTypeName(g_curLayout.posType), g_curLayout.posOffset,
        DeclTypeName(g_curLayout.normalType), g_curLayout.normalOffset,
        DeclTypeName(g_curLayout.blendWeightType), g_curLayout.blendWeightOffset,
        DeclTypeName(g_curLayout.blendIndexType), g_curLayout.blendIndexOffset);
    Log("    bone[0] rows (%.3f %.3f %.3f %.3f) (%.3f %.3f %.3f %.3f) (%.3f %.3f %.3f %.3f)",
        g_vsConst[kRegBonePalette][0], g_vsConst[kRegBonePalette][1],
        g_vsConst[kRegBonePalette][2], g_vsConst[kRegBonePalette][3],
        g_vsConst[kRegBonePalette + 1][0], g_vsConst[kRegBonePalette + 1][1],
        g_vsConst[kRegBonePalette + 1][2], g_vsConst[kRegBonePalette + 1][3],
        g_vsConst[kRegBonePalette + 2][0], g_vsConst[kRegBonePalette + 2][1],
        g_vsConst[kRegBonePalette + 2][2], g_vsConst[kRegBonePalette + 2][3]);

    // The raw bytes of one vertex's weight and index fields. This is what settles UBYTE4 vs
    // UBYTE4N: normalised weights arrive as bytes summing to ~255, and indices as small
    // integers well under 64. Guarded by the same static-buffer rule as the shape probe.
    if (dynamic || !haveDesc || !g_stream0 || g_stream0Stride < 16) return;
    if (g_curLayout.blendWeightOffset < 0 || g_curLayout.blendIndexOffset < 0) return;
    void* mapped = nullptr;
    if (FAILED(g_stream0->Lock(g_stream0Offset + minIndex * g_stream0Stride,
                               min(numVertices, 4u) * g_stream0Stride, &mapped,
                               D3DLOCK_READONLY | D3DLOCK_NOSYSLOCK)) || !mapped)
        return;
    const unsigned char* base = static_cast<const unsigned char*>(mapped);
    for (UINT i = 0; i < min(numVertices, 3u); ++i) {
        const unsigned char* w = base + i * g_stream0Stride + g_curLayout.blendWeightOffset;
        const unsigned char* x = base + i * g_stream0Stride + g_curLayout.blendIndexOffset;
        const float* p = reinterpret_cast<const float*>(
            base + i * g_stream0Stride + g_curLayout.posOffset);
        Log("    vert %u: pos(%.3f %.3f %.3f) weightBytes[%u %u %u %u] indexBytes[%u %u %u %u]",
            i, p[0], p[1], p[2], w[0], w[1], w[2], w[3], x[0], x[1], x[2], x[3]);
    }
    g_stream0->Unlock();
}

// The one refusal this shim actually sees, measured across every run: a skinned declaration
// carrying BLENDINDICES and NO BLENDWEIGHT, 38 draws a frame. Each of those falls back to
// pass-through and Remix reconstructs it as a second copy, so they are a known contributor to
// the reported double draw on characters.
//
// Treating them as rigid single-bone attachment - weight 1.0 on index component 0 - was tried
// on 2026-08-19 and made character clothing VANISH, so the setting is off. Vanishing means the
// vertices were sent somewhere the camera never looks, which is what happens when a position is
// multiplied by a palette slot that does not hold that vertex's bone.
//
// Component 0 was an assumption, not a measurement: ProbeSkinned cannot say anything here
// because it returns early whenever blendWeightOffset is negative, which is precisely this
// layout. So nothing has ever looked at these bytes.
//
// This transforms one vertex by the palette slot named by EACH of the four index components and
// prints all four results. A bone that owns this vertex puts it near the rest of the character;
// a slot that does not puts it at the origin, at a wild coordinate, or on top of another limb.
// One reading of this log answers whether the layout is bone-driven at all, and if so by which
// component - the two questions the vanishing clothes left open.
unsigned g_rigidProbeReports = 0;
unsigned g_rigidProbeFrame = 0;

void ProbeRigidSkinned(UINT firstVertex, UINT numVertices) {
    if (!g_settings.rigidSkinProbe || !g_haveCamera) return;
    if (g_rigidProbeReports >= 6) return;
    // Only the weightless layout. Anything else already decodes and is not in question.
    if (g_curLayout.blendWeightType >= 0 || g_curLayout.blendIndexOffset < 0) return;
    if (g_curLayout.posType != D3DDECLTYPE_FLOAT3 || g_curLayout.posOffset < 0) return;
    if (!g_rigidProbeFrame) g_rigidProbeFrame = g_frames;
    if (g_frames != g_rigidProbeFrame) return;

    D3DVERTEXBUFFER_DESC vbd{};
    if (!g_stream0 || FAILED(g_stream0->GetDesc(&vbd))) return;
    if (vbd.Usage & D3DUSAGE_DYNAMIC) return;
    if (!VertexRangeFits(g_stream0, g_stream0Offset, firstVertex, min(numVertices, 3u),
                         g_stream0Stride))
        return;

    void* mapped = nullptr;
    if (FAILED(g_stream0->Lock(g_stream0Offset + firstVertex * g_stream0Stride,
                               min(numVertices, 3u) * g_stream0Stride, &mapped,
                               D3DLOCK_READONLY | D3DLOCK_NOSYSLOCK)) || !mapped)
        return;

    ++g_rigidProbeReports;
    const D3DMATRIX obj = FromRegisters(&g_vsConst[kRegObjTM][0], 3);
    // Whether the palette below belongs to THIS object decides whether skinning by it would
    // place these vertices or scatter them. Without this the positions printed below cannot be
    // told apart from the previous character's pose.
    const bool sameFrame = (g_lastBoneUploadFrame == g_frames);
    const unsigned drawsAgo = sameFrame ? g_drawIndexThisFrame - g_lastBoneUploadDraw : 0u;
    Log("RIGID-SKIN probe #%u: verts=%u stride=%u indices=%s@%d | objTM t=(%.2f %.2f %.2f) "
        "| bone palette last written %s",
        g_rigidProbeReports, numVertices, g_stream0Stride,
        DeclTypeName(g_curLayout.blendIndexType), g_curLayout.blendIndexOffset,
        obj._41, obj._42, obj._43,
        !sameFrame ? "IN AN EARLIER FRAME - stale"
                   : (drawsAgo == 0 ? "by this draw's own setup"
                                    : "earlier this frame"));
    if (sameFrame && drawsAgo > 0)
        Log("    (%u draws ago - same character if its parts are drawn consecutively, another "
            "object if not)", drawsAgo);

    const unsigned char* base = static_cast<const unsigned char*>(mapped);
    const float* palette = &g_vsConst[kRegBonePalette][0];
    for (UINT i = 0; i < min(numVertices, 3u); ++i) {
        const unsigned char* v = base + i * g_stream0Stride;
        const float* pos = reinterpret_cast<const float*>(v + g_curLayout.posOffset);
        const unsigned char* x = v + g_curLayout.blendIndexOffset;
        // D3DCOLOR stores BGRA, so its bytes reach the shader in a different order than UBYTE4.
        // Reading them the same way here would make the two layouts disagree with the decoder.
        unsigned idx[4];
        if (g_curLayout.blendIndexType == D3DDECLTYPE_D3DCOLOR) {
            idx[0] = x[2]; idx[1] = x[1]; idx[2] = x[0]; idx[3] = x[3];
        } else {
            for (int k = 0; k < 4; ++k) idx[k] = x[k];
        }
        Log("    vert %u: bind pos(%.3f %.3f %.3f) indexBytes[%u %u %u %u]",
            i, pos[0], pos[1], pos[2], idx[0], idx[1], idx[2], idx[3]);
        for (int k = 0; k < 4; ++k) {
            if (idx[k] >= static_cast<unsigned>(kBonesMax)) {
                Log("        via component %d (bone %u): out of palette range", k, idx[k]);
                continue;
            }
            const float* r = palette + idx[k] * kRegsPerBone * 4;
            float out[3];
            for (int row = 0; row < 3; ++row)
                out[row] = r[row * 4 + 0] * pos[0] + r[row * 4 + 1] * pos[1] +
                           r[row * 4 + 2] * pos[2] + r[row * 4 + 3];
            // Row length ~1 means a rotation; far from 1 means this slot is not a bone matrix
            // for this vertex, which is the difference between a limb and a vanished garment.
            const float rowLen = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
            Log("        via component %d (bone %3u): pos(%.3f %.3f %.3f) row0 length %.3f",
                k, idx[k], out[0], out[1], out[2], rowLen);
        }
    }
    g_stream0->Unlock();
}


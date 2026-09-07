// sr3-rtx - fixed-function conversion shim for Saints Row: The Third (2011, DX9).
//
// Ported from the design of BRAGme/sr2-rtx-remix-proxy (MIT), which solves the same problem
// on Saints Row 2 - same studio, same engine lineage, and as it turns out the same bone
// palette layout (c52, 3 registers per bone). That project is a d3d9.dll proxy; this is an
// ASI that patches the device vtable, but the rendering approach below is theirs.
//
// THE APPROACH
// RTX Remix path-traces FIXED-FUNCTION geometry natively. Anything drawn through vertex and
// pixel shaders it must RECONSTRUCT from shader output, and that reconstruction is the root of
// nearly every defect this project has chased: meshes duplicated in mirrored positions, quads
// welded to the camera, stale albedo, wrong UVs. So rather than publishing a camera and hoping
// vertex capture guesses right, every eligible draw is re-issued as fixed function: null both
// shaders, hand D3D9 real WORLD/VIEW/PROJECTION matrices through SetTransform, bind the real
// albedo to stage 0, draw, restore. Remix then receives unambiguous geometry.
//
// The matrices live at fixed vertex-shader constant registers, recovered by disassembling
// shaders.vpp_pc (see docs/shader-map.md):
//
//     c28  projTM         4 regs   world -> clip   (fused VIEW * PROJECTION)
//     c32  objTM          3 regs   object -> world
//     c48  IR_World2View  3 regs   world -> view
//     c52  Bone_weights   3 regs per bone, up to 64 bones
//
// SR3 is easier here than SR2, which fuses everything into one c4-c7 block and has to
// decompose it analytically. We get VIEW handed to us, so PROJECTION is just inverse(V) * VP.
//
// WHAT WAS DELETED, AND WHY
// Everything before the port was a heuristic sitting on the D3D9 boundary trying to classify
// and suppress draws: demoteCameraMismatch, demoteLightVolumes, demoteViewSpheres,
// skipCompositePasses, skipLightVolumes, cullBlankScreenQuads, cullStaleAlbedoDraws,
// cullFullscreenQuads, collapseTarget/skipTargets, the degenerate-collapse vertex shader, the
// render-target index inventory, screenSpaceAsUI/skinnedAsUI/proceduralAsUI, and the mode=0/1
// split. Each treated a symptom of bad reconstruction. Converting to fixed function removes
// the reconstruction, so the symptoms have no source to come from - and every one of those
// switches was measured to cause its own regression. They are gone rather than defaulted off;
// the old file is kept at docs/evidence/pre-sr2-fork/sr3rtx.cpp.bak.

#include <d3d9.h>
#include <windows.h>

#include <dbghelp.h>

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <share.h>
#include <shlwapi.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

// ---------------------------------------------------------------- constants

constexpr UINT kRegProjTM = 28;      // 4 regs, VIEW * PROJECTION
constexpr UINT kRegObjTM = 32;       // 3 regs, object -> world
constexpr UINT kRegWorld2View = 48;  // 3 regs, world -> view
// Bone_weights: row_major float3x4[64] at c52, 192 registers. Confirmed two ways - the CTAB of
// 882 shaders in re/shader_constants.csv all agree on (float4, reg 52, count 192), and the
// disassembly addresses it as c52[a0] after multiplying the blend index by 3. Identical to
// SR2's layout, which is what made the port worth attempting.
constexpr UINT kRegBonePalette = 52;
constexpr int kBonesMax = 64;
constexpr int kRegsPerBone = 3;
// c0.x in ir_sr3npcskinfull_mc: the scale that turns a SHORT4 morph delta into world units.
constexpr float kMorphScale = 0.000122070313f;   // 1/8192
constexpr UINT kRegBoneStart = 52;   // bone palette, 3 regs per bone

constexpr UINT kMaxVsConst = 256;
constexpr UINT kMaxPsConst = 96;

// IDirect3DDevice9 vtable slots. Verified live against this build.
constexpr int kSlotCreateDevice = 16;
constexpr int kSlotCreateDeviceEx = 20;
constexpr int kSlotPresent = 17;
constexpr int kSlotCreateTexture = 23;
constexpr int kSlotSetRenderTarget = 37;
constexpr int kSlotSetTransform = 44;
constexpr int kSlotSetMaterial = 49;
constexpr int kSlotSetLight = 51;
constexpr int kSlotLightEnable = 53;
constexpr int kSlotSetRenderState = 57;
constexpr int kSlotGetRenderState = 58;
constexpr int kSlotSetTexture = 65;
constexpr int kSlotSetTextureStageState = 67;
constexpr int kSlotSetSamplerState = 69;
constexpr int kSlotDrawPrimitive = 81;
constexpr int kSlotDrawIndexedPrimitive = 82;
// The USER-POINTER draw calls. Never hooked until now, which means every draw the game makes
// through them has been INVISIBLE to this shim - it does not appear in the frame dump, in any
// counter, or in any classification. The complete frame dump of 2026-09-03 contains 5,200 draws
// and NOT ONE of them is the HUD; its last draw is the composite into the back buffer. So the
// HUD is submitted somewhere we were not looking, and these two slots are where a D3D9 game
// usually puts it: UI vertices are built fresh every frame, which is exactly what the UP calls
// are for.
constexpr int kSlotDrawPrimitiveUP = 83;
constexpr int kSlotDrawIndexedPrimitiveUP = 84;
constexpr int kSlotSetVertexDeclaration = 87;
constexpr int kSlotCreateVertexShader = 91;
constexpr int kSlotSetVertexShader = 92;
constexpr int kSlotSetVertexShaderConstantF = 94;
constexpr int kSlotSetStreamSource = 100;
constexpr int kSlotSetStreamSourceFreq = 102;
constexpr int kSlotSetIndices = 104;
constexpr int kSlotCreatePixelShader = 106;
constexpr int kSlotSetPixelShader = 107;
constexpr int kSlotSetPixelShaderConstantF = 109;
constexpr int kSlotCreateQuery = 118;   // last method on IDirect3DDevice9
// IDirect3DQuery9: QueryInterface/AddRef/Release, GetDevice, GetType, GetDataSize, Issue, GetData
constexpr int kSlotQueryGetData = 7;

const D3DMATRIX kIdentity = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};

// ---------------------------------------------------------------- logging

FILE* g_log = nullptr;

void Log(const char* format, ...) {
    if (!g_log) return;
    va_list args;
    va_start(args, format);
    vfprintf(g_log, format, args);
    va_end(args);
    fputc('\n', g_log);
    fflush(g_log);
}

// ---------------------------------------------------------------- timing primitives
//
// These live here, far from the frame accumulators they feed, because the shader and texture
// creation hooks sit several hundred lines above those accumulators and have to be timed too.
// See the note beside g_presentMsLast for why that gap mattered.
LARGE_INTEGER g_qpcFreq{};

inline LONGLONG Now() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}
inline double MsSince(LONGLONG start) {
    if (!g_qpcFreq.QuadPart) return 0.0;
    return static_cast<double>(Now() - start) * 1000.0 / static_cast<double>(g_qpcFreq.QuadPart);
}

// Our hook work that is NOT the draw path: creation hooks, the render-target hook. Reset every
// Present alongside g_shimMsThisFrame.
double g_otherMsThisFrame = 0.0;
unsigned g_shadersCreatedThisFrame = 0, g_texturesCreatedThisFrame = 0;

// ---------------------------------------------------------------- settings

// SETTLED BEHAVIOUR - measured, no longer configurable.
//
// A toggle is a hypothesis that has not been settled. Once it is settled it belongs in the code
// with the measurement beside it, because a switch nobody varies is a switch nobody re-checks -
// and stale switches interacting is exactly what this fork was created to escape (the pre-fork
// build carried ~20 of them). Four were removed on 2026-08-17 with the evidence recorded here.
//
//   skipDepthOnly   REMOVED - inert. D3DRS_COLORWRITEENABLE == 0 occurs ZERO times in a complete
//                   frame dump, so the test never fired. It also read that state through
//                   ShadowGetRS, which returns 0 on a failed query - meaning the only way it
//                   could ever fire was a false positive that would skip the entire world.
//   finiteFar       REMOVED - inert on SR3. Its projection is near=0.15 far=5000 (Q=1.00003),
//                   a perfectly ordinary finite projection, and the substitution counter read 0
//                   across every run. The mechanism is real on SR2; this engine does not need it.
//   startupDelayMs  REMOVED - always 0.
//   skipComposite   REMOVED - proven harmful. The composite quad is the only draw that writes the
//                   back buffer, so dropping it left stale content there and the image froze at
//                   a healthy 56 fps.
constexpr bool kPerspectiveOnly = true;   // auxiliary cameras are not the visible scene
constexpr bool kMainCameraOnly = true;    // SR3 renders 3-4 cameras/frame; only one is the player's
// uv = raw * tiling / 1024, read out of the vertex-shader disassembly:
//     mul r0.x, c<TilingU>.x, v1.x ; mul o1.xy, r0, 0.0009765625
constexpr float kShortUVScale = 1.0f / 1024.0f;
// The UV divide for the CURRENT draw: the vertex shader's own def constant when it has an
// unambiguous one, and the historic 1/1024 when it does not. Both the fixed-function conversion
// and the API character capture ask through here, so the two can never drift apart again - which
// is the bug that put the underwear's UVs at four times their proper size in BOTH copies.
float CurrentShortUVScale();

// The float2 texture-coordinate stream; see the uv conversion further down.
constexpr DWORD kUvStreamDefault = 7;              // clamped to the device's stream count at init
constexpr UINT kUvBytesPerVertex = 8;              // float2
constexpr size_t kMaxUvBufferBytes = 48u << 20;    // total held across all converted buffers


struct Settings {
    bool ffp = true;             // master switch; 0 = passive observer, for A/B against no conversion
    bool convertSkinned = false; // characters need CPU skinning first (phase 2)
    bool skipMirrored = true;    // skip passes whose handedness differs from the main camera
    bool cacheMeshAlbedo = true; // pin each mesh to its first-seen texture (streaming eviction)
    bool skipUntextured = true;  // don't convert passes whose shader samples nothing (the prepass)
    // Don't convert a draw into a render target that cannot hold colour. 0 restores the old
    // behaviour, which is the A/B for the doubled characters.
    bool skipNonColourTargets = true;
    // Don't convert a draw whose PIXEL SHADER writes more than one render target. 0 restores
    // the old behaviour, which is the A/B for the head texture and the head z-fighting.
    bool skipDeferredGBuffer = true;
    int screenSpaceMode = 2;     // 0 pass, 1 ortho demote, 2 mark the ones sampling a render target
    // Demote the HUD to a UI overlay. The HUD arrives through DrawPrimitiveUP, which nothing
    // hooked until 2026-09-04, so it has been reaching Remix unclassified since the fork.
    // 0 restores that - the A/B for anything this changes.
    bool uiDemoteUP = true;
    // REBUILD the HUD as fixed function instead of merely demoting it. Remix declines a shader
    // draw when rtx.useVertexCapture is off; a fixed-function draw it accepts. This is what lets
    // the HUD survive capture-off, which is the configuration that finally removed the fullscreen
    // quads from in front of the camera.
    bool uiConvertUP = true;
    // KEEP the game's pixel shader on the rebuilt HUD draw. Nulling it is what this shim does for
    // WORLD geometry, where Remix wants the albedo texture rather than the shader - but for the
    // UI the pixel shader IS the content, and throwing it away is what produced a greyscale
    // video, missing text and blank menu backgrounds. See the note above DrawHudFixedFunction.
    bool uiKeepPixelShader = true;
    // Re-issue the CHARACTER ATLAS composites with fixed-function vertex processing and the
    // game's own pixel shader, so Remix executes them with vertex capture off. This is the same
    // technique that brought the menu video back in colour, applied to the draw that builds the
    // character's skin.
    bool compositeFfp = true;
    // Generate the albedo for the PLAYER's clothing family - a separate Diffuse_Map plus a
    // Pattern_Map on a second UV set - in the DIFFUSE map's own texture space, whenever the
    // pattern is a single uniform colour. See ClothAlbedoUniform.
    bool clothUniformFromDiffuse = true;
    // Give hair its STRAND detail by modulating the hair colour with the Dob_Map's strand
    // channel, instead of shipping one flat colour. 0 restores the flat constant.
    bool hairStrandsFromDob = true;
    // Take the UV divide from the vertex shader's own def constant instead of assuming 1/1024.
    // 0 restores the hardcoded value, which is the A/B for anything this changes.
    bool uvScaleFromShader = true;
    // Census of every draw we PASS THROUGH. With capture off a passed-through shader draw is
    // invisible, so this names exactly what is being lost.
    bool passCensus = true;
    // One-shot dump of every draw into a character-atlas-sized render target. The atlas is
    // composited at character load, not per frame, so no frame dump has ever caught it.
    bool atlasCompositeProbe = true;
    bool compositeToTexturePass = true;  // never SKIP a composite into an off-screen texture
    bool markAuxCamera = false;  // mark the 512x288 reflection pass; needs the post chain hidden
    bool clearBackBuffer = true; // nothing writes it once the composite is marked - see the clear
    bool hideLightVolumes = true;// light volumes (their light is already injected separately)
    bool injectLights = true;
    bool rankAlbedo = true;      // use CTAB sampler names to choose base colour
    bool excludeRTAlbedo = true; // never use a render-target texture as base colour
    float lightScale = 1.0f;
    float lightRangeScale = 1.0f;
    int hiddenPassMode = 3;      // 0 pass, 1 ortho demote, 2 skip, 3 bind the marker texture
    // --- diagnostics ---
    int dumpFrame = 1800;        // frames to wait after the camera appears before dumping one
    bool logLayouts = true;      // one line per distinct vertex layout, capped
    bool shapeProbe = true;      // one frame of world-space bounds for draws near the camera
    bool skinProbe = true;       // one frame of skinned-draw layout, bones and objTM
    bool rigidSkinProbe = true;  // one frame of the weightless BLENDINDICES layout
    bool remixShortUV = true;    // re-declare short2 texcoords as short2n so Remix reads them
    bool deinstanceConverted = true;  // reset stream frequency on single-instance converted draws
    bool clothProbe = true;      // one pass over the customisable-clothing material inputs
    bool diffuseColorProbe = true;  // distribution of the Diffuse_Color constant we discard
    bool rtAlbedoCopy = true;    // re-upload a render-target albedo so Remix can hash it
    bool atlasSnoop = true;      // snoop the game's own writes to the character atlas
    bool cameraOnly = false;     // hand Remix a camera and touch no draw at all
    bool cameraOnlyFloatUV = false;  // also convert SHORT2 texcoords on pass-through draws
    bool cameraMainViewOnly = true;  // give Remix ONLY the main scene camera, not all ~11
    bool convertDynamicUV = true;    // convert SHORT2 UVs on DYNAMIC buffers via the snoop
    bool remixApi = true;            // initialize Remix's programmatic API and register the device
    bool remixApiCamera = false;     // AND hand it the camera through SetupCamera (step 1b)
    bool remixApiTestCube = true;    // step 2: one API-submitted mesh, to prove it appears
    bool remixApiCharacter = true;   // step 3a: submit one character mesh through the API
    bool remixApiSkinning = false;   // step 3c: let Remix skin. CRASHED 2026-09-02, see the ini
    bool remixApiClothAlbedo = false;  // replace a cloth slot texture with a generated one
    bool bakeShaderAlbedo = false;    // run the game's pixel shader into a UV-space target
    bool clothUseDiffuse = true;      // multiply the customisation colour by the Diffuse_Map
    float clothBrightness = 1.0f;     // scale the baked cloth albedo (tuning, not a fix)
    float remixApiCharacterOffset = 3.0f;  // stand it beside the real one, so they cannot z-fight
    bool scanCommandBlocks = true;   // walk each command block to see what the engine will do
    // OFF by default and on its own key. Reading the GAME's render targets with
    // GetRenderTargetData froze SR3 twice - once unbounded, and again at one read per 120
    // frames with a 60-read budget, which means the cost was never the problem: Remix's
    // D3D9 cannot service a readback of a surface it owns and is using. Sharing a gate with
    // atlasSnoop meant the harmless snoop could not be enabled without the dangerous probe.
    bool rtContentProbe = false;
    int rtAlbedoRetries = 240;   // frames to keep retrying while the source reads back blank
    bool generateCloth = true;   // build clothing albedo per outfit from the pattern + colours
    bool clothDump = false;      // write the first 3 outfits and their patterns out as raw RGB
    bool charTexDump = false;    // write every named stage of the probed character materials
    int captureKey = 0x78;       // VK_F9: re-arm the frame and shape dumps, 0 disables
    // Refuse to read a bone palette for a draw whose vertex shader declares no BLENDINDICES
    // input. Such a shader places the mesh by objTM alone; posing it costs a bone matrix that
    // belongs to whatever was drawn before. 0 restores the old fallback-to-c52 behaviour, which
    // is what makes this a controlled experiment rather than an assertion.
    int skinRequireBoneDecl = 1;
    int skinRingMB = 8;          // size of the CPU-skinning ring; larger means fewer wraps
    bool forceOcclusionVisible = false;  // answer the engine's occlusion queries "fully visible"
    bool rejectStaleBones = true;  // drop bone influences belonging to a different object
    bool clampBonesToUpload = true;  // ignore bones outside this object's own palette upload
    bool vehicleBonesOff = false;    // single-bone draws: ignore the palette, use objTM alone
    bool paletteSetupScope = true;   // only pose by a palette published in this draw's own setup
    float clothTintScale = 1.0f; // clothAlbedoPercent/100; see the ini
    // OFF. Three attempts, three regressions, and no run in which it demonstrably reduced the
    // double-draw it was written for. See the ini for the full history. Kept as a switch because
    // the analysis is sound and the key is now complete; it simply has never paid for itself.
    // Drop a skinned draw whose identical triangles, in the identical POSE, were already
    // converted this frame.
    //
    // Twice wrong before this. First the key omitted the index range, so every material
    // sub-range of one mesh looked like a duplicate. Then it omitted the pose, so two NPCs in the
    // same garment looked like one object and the second lost its clothing. The key now folds in
    // eight bone matrices, which is what makes "the same object" mean the same object.
    bool dedupSkinned = false;
    // Extend the same duplicate test to NON-skinned draws. Measured 2026-08-27: 94 redundant
    // converted copies a frame, and only 4 of them were skinned.
    bool dedupAll = false;
    // Apply the per-character morph delta from stream 2. 0 renders the base mesh, which is what
    // every build before 2026-08-27 did.
    bool applyMorph = true;
    bool tintFallbackAlbedo = false;  // OFF: measured to darken clothing, see SetupTextureStages
    bool skinRigidSingleBone = false; // OFF: correlated with clothing vanishing, see GetBaseMesh
    // Any frame at or above this many ms writes one line partitioning where the time went.
    // 40 ms is ~2.3 frames at the measured 58 fps: long enough that the player sees a hitch,
    // short enough to catch the 100 ms ones with room to spare.
    int hitchMs = 40;
    int probeVerts[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
    int probeVertCount = 0;
} g_settings;

void LoadSettings() {
    char path[MAX_PATH]{};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (char* slash = strrchr(path, '\\')) strcpy_s(slash + 1, 32, "sr3-rtx.ini");

    auto flag = [&](const char* key, bool def) {
        return GetPrivateProfileIntA("sr3-rtx", key, def ? 1 : 0, path) != 0;
    };
    auto num = [&](const char* key, int def) {
        return GetPrivateProfileIntA("sr3-rtx", key, def, path);
    };

    g_settings.ffp = flag("ffp", true);
    g_settings.convertSkinned = flag("convertSkinned", false);
    g_settings.skipMirrored = flag("skipMirrored", true);
    g_settings.cacheMeshAlbedo = flag("cacheMeshAlbedo", true);
    g_settings.skipUntextured = flag("skipUntextured", true);
    g_settings.skipNonColourTargets = flag("skipNonColourTargets", true);
    g_settings.skipDeferredGBuffer = flag("skipDeferredGBuffer", true);
    g_settings.screenSpaceMode = num("screenSpaceMode", 2);
    g_settings.uiDemoteUP = flag("uiDemoteUP", true);
    g_settings.uiConvertUP = flag("uiConvertUP", true);
    g_settings.uiKeepPixelShader = flag("uiKeepPixelShader", true);
    g_settings.compositeFfp = flag("compositeFfp", true);
    g_settings.clothUniformFromDiffuse = flag("clothUniformFromDiffuse", true);
    g_settings.hairStrandsFromDob = flag("hairStrandsFromDob", true);
    g_settings.uvScaleFromShader = flag("uvScaleFromShader", true);
    g_settings.passCensus = flag("passCensus", true);
    g_settings.atlasCompositeProbe = flag("atlasCompositeProbe", true);
    g_settings.compositeToTexturePass = flag("compositeToTexturePass", true);
    g_settings.markAuxCamera = flag("markAuxCamera", false);
    g_settings.clearBackBuffer = flag("clearBackBuffer", true);
    g_settings.hideLightVolumes = flag("hideLightVolumes", true);
    g_settings.injectLights = flag("injectLights", true);
    g_settings.rankAlbedo = flag("rankAlbedo", true);
    g_settings.excludeRTAlbedo = flag("excludeRTAlbedo", true);
    g_settings.logLayouts = flag("logLayouts", true);
    g_settings.lightScale = num("lightScalePercent", 100) / 100.0f;
    g_settings.lightRangeScale = num("lightRangePercent", 100) / 100.0f;
    g_settings.dumpFrame = num("dumpFrame", 1800);
    g_settings.shapeProbe = flag("shapeProbe", true);
    g_settings.hitchMs = num("hitchMs", 40);
    g_settings.skinProbe = flag("skinProbe", true);
    g_settings.rigidSkinProbe = flag("rigidSkinProbe", true);
    g_settings.remixShortUV = flag("remixShortUV", true);
    g_settings.deinstanceConverted = flag("deinstanceConverted", true);
    g_settings.clothProbe = flag("clothProbe", true);
    g_settings.diffuseColorProbe = flag("diffuseColorProbe", true);
    g_settings.rtAlbedoCopy = flag("rtAlbedoCopy", true);
    g_settings.atlasSnoop = flag("atlasSnoop", true);
    g_settings.cameraOnly = flag("cameraOnly", false);
    g_settings.cameraOnlyFloatUV = flag("cameraOnlyFloatUV", false);
    g_settings.cameraMainViewOnly = flag("cameraMainViewOnly", true);
    g_settings.convertDynamicUV = flag("convertDynamicUV", true);
    g_settings.remixApi = flag("remixApi", true);
    g_settings.remixApiCamera = flag("remixApiCamera", false);
    g_settings.remixApiTestCube = flag("remixApiTestCube", true);
    g_settings.remixApiCharacter = flag("remixApiCharacter", true);
    g_settings.remixApiSkinning = flag("remixApiSkinning", false);
    g_settings.remixApiClothAlbedo = flag("remixApiClothAlbedo", false);
    g_settings.bakeShaderAlbedo = flag("bakeShaderAlbedo", true);
    g_settings.clothUseDiffuse = flag("clothUseDiffuse", true);
    g_settings.clothBrightness =
        static_cast<float>(num("clothBrightness", 100)) / 100.0f;
    g_settings.remixApiCharacterOffset =
        static_cast<float>(num("remixApiCharacterOffset", 3));
    g_settings.scanCommandBlocks = flag("scanCommandBlocks", true);
    g_settings.rtContentProbe = flag("rtContentProbe", false);
    g_settings.rtAlbedoRetries = num("rtAlbedoRetries", 240);
    g_settings.generateCloth = flag("generateCloth", true);
    g_settings.clothDump = flag("clothDump", false);
    g_settings.charTexDump = flag("charTexDump", false);
    g_settings.captureKey = num("captureKey", 0x78);
    g_settings.skinRequireBoneDecl = num("skinRequireBoneDecl", 1);
    g_settings.skinRingMB = num("skinRingMB", 8);
    g_settings.forceOcclusionVisible = flag("forceOcclusionVisible", false);
    g_settings.rejectStaleBones = flag("rejectStaleBones", true);
    g_settings.clampBonesToUpload = flag("clampBonesToUpload", true);
    g_settings.vehicleBonesOff = flag("vehicleBonesOff", false);
    g_settings.paletteSetupScope = flag("paletteSetupScope", true);
    g_settings.clothTintScale = num("clothAlbedoPercent", 100) / 100.0f;
    g_settings.dedupSkinned = flag("dedupSkinned", false);
    g_settings.dedupAll = flag("dedupAll", false);
    g_settings.applyMorph = flag("applyMorph", true);
    g_settings.tintFallbackAlbedo = flag("tintFallbackAlbedo", false);
    g_settings.skinRigidSingleBone = flag("skinRigidSingleBone", false);
    g_settings.hiddenPassMode = num("hiddenPassMode", 3);
    {
        char list[128]{};
        GetPrivateProfileStringA("sr3-rtx", "probeVerts", "", list, sizeof(list), path);
        char* ctx = nullptr;
        for (char* tok = strtok_s(list, ", \t", &ctx);
             tok && g_settings.probeVertCount < 8; tok = strtok_s(nullptr, ", \t", &ctx)) {
            const int v = atoi(tok);
            if (v > 0) g_settings.probeVerts[g_settings.probeVertCount++] = v;
        }
    }
}

// ---------------------------------------------------------------- math

// Shader constants hold rows of a column-vector matrix (clip = M * pos, per the dp4 ordering
// in the disassembly). SetTransform expects the row-vector form, so register rows become
// columns. Missing rows default to the affine identity.
D3DMATRIX FromRegisters(const float* regs, int rows) {
    D3DMATRIX m{};
    for (int col = 0; col < 4; ++col)
        for (int row = 0; row < 4; ++row)
            (&m._11)[col * 4 + row] = (row < rows) ? regs[row * 4 + col] : 0.0f;
    if (rows < 4) m._44 = 1.0f;
    return m;
}

bool Invert(const D3DMATRIX& in, D3DMATRIX& out) {
    const float* m = &in._11;
    float inv[16];

    inv[0]  =  m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4]  = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8]  =  m[4]*m[9]*m[15]  - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14]  + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    inv[1]  = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5]  =  m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9]  = -m[0]*m[9]*m[15]  + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] =  m[0]*m[9]*m[14]  - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    inv[2]  =  m[1]*m[6]*m[15]  - m[1]*m[7]*m[14]  - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7]  - m[13]*m[3]*m[6];
    inv[6]  = -m[0]*m[6]*m[15]  + m[0]*m[7]*m[14]  + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7]  + m[12]*m[3]*m[6];
    inv[10] =  m[0]*m[5]*m[15]  - m[0]*m[7]*m[13]  - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7]  - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14]  + m[0]*m[6]*m[13]  + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6]  + m[12]*m[2]*m[5];
    inv[3]  = -m[1]*m[6]*m[11]  + m[1]*m[7]*m[10]  + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7]   + m[9]*m[3]*m[6];
    inv[7]  =  m[0]*m[6]*m[11]  - m[0]*m[7]*m[10]  - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7]   - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11]  + m[0]*m[7]*m[9]   + m[4]*m[1]*m[11] - m[4]*m[3]*m[9]  - m[8]*m[1]*m[7]   + m[8]*m[3]*m[5];
    inv[15] =  m[0]*m[5]*m[10]  - m[0]*m[6]*m[9]   - m[4]*m[1]*m[10] + m[4]*m[2]*m[9]  + m[8]*m[1]*m[6]   - m[8]*m[2]*m[5];

    float det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
    if (std::fabs(det) < 1e-12f) return false;
    det = 1.0f / det;
    for (int i = 0; i < 16; ++i) (&out._11)[i] = inv[i] * det;
    return true;
}

D3DMATRIX Multiply(const D3DMATRIX& a, const D3DMATRIX& b) {
    D3DMATRIX r{};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            for (int k = 0; k < 4; ++k)
                (&r._11)[i * 4 + j] += (&a._11)[i * 4 + k] * (&b._11)[k * 4 + j];
    return r;
}

bool IsFinite(const D3DMATRIX& m) {
    for (int i = 0; i < 16; ++i)
        if (!std::isfinite((&m._11)[i])) return false;
    return true;
}

bool Same(const D3DMATRIX& a, const D3DMATRIX& b) {
    return memcmp(&a, &b, sizeof(D3DMATRIX)) == 0;
}

D3DVECTOR TransformPoint(const float* v, const D3DMATRIX& m) {
    return {v[0]*m._11 + v[1]*m._21 + v[2]*m._31 + m._41,
            v[0]*m._12 + v[1]*m._22 + v[2]*m._32 + m._42,
            v[0]*m._13 + v[1]*m._23 + v[2]*m._33 + m._43};
}

D3DVECTOR TransformDir(const float* v, const D3DMATRIX& m) {
    D3DVECTOR d{v[0]*m._11 + v[1]*m._21 + v[2]*m._31,
                v[0]*m._12 + v[1]*m._22 + v[2]*m._32,
                v[0]*m._13 + v[1]*m._23 + v[2]*m._33};
    const float len = std::sqrt(d.x*d.x + d.y*d.y + d.z*d.z);
    if (len > 1e-6f) { d.x /= len; d.y /= len; d.z /= len; }
    return d;
}

// ---------------------------------------------------------------- originals

typedef HRESULT(WINAPI* CreateDevice_t)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD,
                                        D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
typedef HRESULT(WINAPI* CreateDeviceEx_t)(IDirect3D9Ex*, UINT, D3DDEVTYPE, HWND, DWORD,
                                          D3DPRESENT_PARAMETERS*, D3DDISPLAYMODEEX*,
                                          IDirect3DDevice9Ex**);
typedef HRESULT(WINAPI* Present_t)(IDirect3DDevice9*, const RECT*, const RECT*, HWND,
                                   const RGNDATA*);
typedef HRESULT(WINAPI* CreateTexture_t)(IDirect3DDevice9*, UINT, UINT, UINT, DWORD, D3DFORMAT,
                                         D3DPOOL, IDirect3DTexture9**, HANDLE*);
typedef HRESULT(WINAPI* SetRenderTarget_t)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
typedef HRESULT(WINAPI* SetTransform_t)(IDirect3DDevice9*, D3DTRANSFORMSTATETYPE,
                                        const D3DMATRIX*);
typedef HRESULT(WINAPI* SetMaterial_t)(IDirect3DDevice9*, const D3DMATERIAL9*);
typedef HRESULT(WINAPI* SetLight_t)(IDirect3DDevice9*, DWORD, const D3DLIGHT9*);
typedef HRESULT(WINAPI* LightEnable_t)(IDirect3DDevice9*, DWORD, BOOL);
typedef HRESULT(WINAPI* SetRenderState_t)(IDirect3DDevice9*, D3DRENDERSTATETYPE, DWORD);
typedef HRESULT(WINAPI* GetRenderState_t)(IDirect3DDevice9*, D3DRENDERSTATETYPE, DWORD*);
typedef HRESULT(WINAPI* SetTexture_t)(IDirect3DDevice9*, DWORD, IDirect3DBaseTexture9*);
typedef HRESULT(WINAPI* SetTextureStageState_t)(IDirect3DDevice9*, DWORD,
                                                D3DTEXTURESTAGESTATETYPE, DWORD);
typedef HRESULT(WINAPI* SetSamplerState_t)(IDirect3DDevice9*, DWORD, D3DSAMPLERSTATETYPE, DWORD);
typedef HRESULT(WINAPI* DrawPrimitive_t)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
typedef HRESULT(WINAPI* DrawIndexedPrimitive_t)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT,
                                                UINT, UINT, UINT, UINT);
typedef HRESULT(WINAPI* DrawPrimitiveUP_t)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT,
                                           const void*, UINT);
typedef HRESULT(WINAPI* DrawIndexedPrimitiveUP_t)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT,
                                                  UINT, UINT, const void*, D3DFORMAT,
                                                  const void*, UINT);
typedef HRESULT(WINAPI* SetVertexDeclaration_t)(IDirect3DDevice9*,
                                                IDirect3DVertexDeclaration9*);
typedef HRESULT(WINAPI* CreateVertexShader_t)(IDirect3DDevice9*, const DWORD*,
                                              IDirect3DVertexShader9**);
typedef HRESULT(WINAPI* SetVertexShader_t)(IDirect3DDevice9*, IDirect3DVertexShader9*);
typedef HRESULT(WINAPI* SetVSConstF_t)(IDirect3DDevice9*, UINT, const float*, UINT);
typedef HRESULT(WINAPI* SetStreamSource_t)(IDirect3DDevice9*, UINT, IDirect3DVertexBuffer9*,
                                           UINT, UINT);
typedef HRESULT(WINAPI* SetStreamSourceFreq_t)(IDirect3DDevice9*, UINT, UINT);
typedef HRESULT(WINAPI* SetIndices_t)(IDirect3DDevice9*, IDirect3DIndexBuffer9*);
typedef HRESULT(WINAPI* CreatePixelShader_t)(IDirect3DDevice9*, const DWORD*,
                                             IDirect3DPixelShader9**);
typedef HRESULT(WINAPI* SetPixelShader_t)(IDirect3DDevice9*, IDirect3DPixelShader9*);
typedef HRESULT(WINAPI* SetPSConstF_t)(IDirect3DDevice9*, UINT, const float*, UINT);

CreateDevice_t g_origCreateDevice = nullptr;
CreateDeviceEx_t g_origCreateDeviceEx = nullptr;
Present_t g_origPresent = nullptr;
CreateTexture_t g_origCreateTexture = nullptr;
SetRenderTarget_t g_origSetRenderTarget = nullptr;
SetTransform_t g_origSetTransform = nullptr;
SetMaterial_t g_origSetMaterial = nullptr;
SetLight_t g_origSetLight = nullptr;
LightEnable_t g_origLightEnable = nullptr;
SetRenderState_t g_origSetRenderState = nullptr;
GetRenderState_t g_origGetRenderState = nullptr;
SetTexture_t g_origSetTexture = nullptr;
SetTextureStageState_t g_origSetTextureStageState = nullptr;
SetSamplerState_t g_origSetSamplerState = nullptr;
DrawPrimitive_t g_origDrawPrimitive = nullptr;
DrawIndexedPrimitive_t g_origDrawIndexedPrimitive = nullptr;
DrawPrimitiveUP_t g_origDrawPrimitiveUP = nullptr;
DrawIndexedPrimitiveUP_t g_origDrawIndexedPrimitiveUP = nullptr;
SetVertexDeclaration_t g_origSetVertexDeclaration = nullptr;
CreateVertexShader_t g_origCreateVertexShader = nullptr;
SetVertexShader_t g_origSetVertexShader = nullptr;
SetVSConstF_t g_origSetVSConstF = nullptr;
SetStreamSource_t g_origSetStreamSource = nullptr;
SetStreamSourceFreq_t g_origSetStreamSourceFreq = nullptr;
SetIndices_t g_origSetIndices = nullptr;
CreatePixelShader_t g_origCreatePixelShader = nullptr;
SetPixelShader_t g_origSetPixelShader = nullptr;
SetPSConstF_t g_origSetPSConstF = nullptr;

bool g_internal = false;   // guards against re-entering our own hooks

// ---------------------------------------------------------------- shader reflection

// Saints Row uses "inferred lighting": each light is a volume draw whose pixel shader carries
// the light's parameters in its constants. Recognised by the constant names in the embedded
// CTAB, then re-emitted as a D3D9 light for Remix to convert into a real ray-traced light.
struct LightShader {
    enum Kind { NotLight, Point, Spot, Directional } kind = NotLight;
    int regPos = -1, regDir = -1, regColor = -1, regInfo = -1, regSpot = -1;
};

struct ShaderInfo {
    LightShader light;
    int albedoStage = -1;    // best colour-carrying sampler
    int albedoRank = 0;      // 0 => this shader binds no colour texture at all
    bool usesProjTM = false; // VS: false implies the shader emits clip space itself
    bool usesObjTM = false;  // VS: false implies the geometry is already in world space
    bool skinned = false;    // VS: has a Bone_weights palette
    // WHERE that palette lives, taken from the shader's own constant table rather than assumed.
    // The whole shim has hardcoded c52 since session 1, on the strength of ONE character
    // shader's CTAB. A vehicle shader is free to declare Bone_weights anywhere, and if it does,
    // every car part has been skinned by whatever happened to sit at c52 - which is a character.
    // -1 means this shader declares no palette.
    int boneReg = -1;
    // THE UV DIVIDE THIS SHADER APPLIES, read from its own def constants.
    //
    // kShortUVScale has been a hardcoded 1/1024 since session 1, on the strength of one shader
    // ending `mul o1.xy, r0, 0.0009765625`. The 2026-09-06 vertex audit shows that is not
    // universal: nine captured parts land inside the unit square, and the underwear spans
    // u[-1.072 2.787] - a range of 3.859 where its siblings span 0.98. Almost exactly four times.
    // A mesh whose shader divides by 4096, read at 1/1024, lands exactly there.
    //
    // 0 means this shader defs no such constant and the caller should keep its old default, so a
    // shader we cannot read behaves exactly as it does today.
    float uvDefScale = 0.0f;
    bool uvDefAmbiguous = false;   // more than one distinct candidate: do not guess between them
    // Does this vertex shader actually CONSUME blend weights? Read from its dcl instructions,
    // not from the vertex declaration - the two disagree, and that disagreement is the bug.
    //
    // Disassembled 2026-08-25. ir_sr3cardiffusespec_g_v shader[0], a vehicle:
    //
    //     dcl_position v0 / dcl_normal v1 / dcl_blendindices v2      <- no dcl_blendweight
    //     mul r2.x, c0.z, v2.x        (c0.z = 3, three registers per bone)
    //     mova a0.x, r2.x
    //     dp4 r0.x, c52[a0.x], r1     <- ONE bone, unweighted
    //     dp4 r1.x, c32, r0           <- then objTM
    //
    // ir_sr3npcclothfull_c shader[0], a character:
    //
    //     dcl_blendweight v4 / dcl_blendindices v5
    //     mul r2, v4.y, c52[a0.x]
    //     mad r2, v4.x, c52[a0.y], r2 ... four weighted bones
    //
    // The vertex DECLARATION on a vehicle draw still carries BLENDWEIGHT data, so deciding from
    // it makes us blend four bones where the game blends one. The other three indices are
    // whatever those bytes happen to hold, and they address a palette that belongs to somebody
    // else - so a rigid part is dragged bodily toward a character and follows it. Every vertex
    // shares the indices, which is why the mesh translates without deforming.
    bool usesBlendWeights = false;
    // Does this vertex shader consume BLENDINDICES at all?
    //
    // This is the question that decides whether a draw is skinned, and it is NOT the same as
    // "the vertex declaration carries BLENDINDICES". Disassembled 2026-08-26, the car body and
    // car glass ship in two variants over the SAME mesh:
    //
    //   ir_sr3cardiffusespec_g_v   dcl_position v0 / dcl_normal v1 / dcl_blendindices v2
    //                              mul r2.x, c0.z, v2.x / mova a0.x, r2.x
    //                              dp4 r0.x, c52[a0.x], r1      <- bone, THEN objTM
    //
    //   ir_sr3cardiffusespec_g_s   dcl_position v0 / dcl_normal v1     <- no blendindices
    //                              dp4 r1.x, c32, r0            <- objTM alone
    //
    // The static variant does not read a palette, does not declare one, and places the mesh by
    // objTM only. The declaration is shared with the vehicle variant, so it still carries
    // BLENDINDICES - which is all the shim looked at. boneReg is then -1, the code below fell
    // back to c52, and the part was posed by whatever palette the last CHARACTER draw left
    // there. That is a detached bumper following an NPC's skeleton while its physics body sits
    // exactly where it belongs, which is the reported bug verbatim.
    //
    // Read from the dcl stream, not from the constant table: a shader that declares no input
    // cannot index a palette no matter what CTAB parsing does or fails to do.
    bool usesBlendIndices = false;
    // Does this vertex shader read the MORPH stream - dcl_position1 / dcl_normal1?
    //
    // SR3 ships `_c` and `_mc` variants of every character shader. Disassembled 2026-08-27,
    // ir_sr3npcskinfull_mc shader[0]:
    //
    //     def c0, 0.000122070313, 2, -1, 3        (c0.x = 1/8192)
    //     dcl_position v0 / dcl_position1 v4 / dcl_normal v2 / dcl_normal1 v5
    //     mov r1.xyz, v4
    //     mad r1.xyz, r1, c0.x, v0        <- position = v0 + v4/8192, BEFORE the bone blend
    //     dp4 r2.x, r2, r1                <- then the bones, then objTM
    //
    // That is character customisation: the face and body are authored as a base mesh plus a
    // per-character delta in a second stream. Reading only v0 renders every NPC with the SAME
    // base head - a head that is not theirs, differs from NPC to NPC, sits slightly off the
    // neck, and no longer matches the surface its texture was authored for.
    bool usesMorph = false;
    // How many RENDER TARGETS this pixel shader writes.
    //
    // SR3 is a deferred renderer and draws each object twice. Disassembled 2026-08-28,
    // ir_sr3npcskinfull_mc - the head:
    //
    //   shader[6]  oC0.xy = normal, oC0.zw = const, oC1 = data, oC2 = data
    //              samples Blend_Map (ONE scalar, into a fresnel term) + Normal_Map
    //              -> writes NO colour at all
    //   shader[8]  oC0 = r2 * c37 (Tint_color)
    //              samples Diffuse_Map, Sphere_Map, IR_LBuffer
    //              -> the actual material pass
    //
    // The albedo ranker scores Blend_Map at 70 and hands the G-buffer pass a specular mask as its
    // base colour, which is a correctly placed head wearing the wrong texture. The world's
    // G-buffer shaders do the same thing and even sample Diffuse_Map first - then overwrite the
    // result with the normal map before using it, so that load is dead.
    //
    // 1,343 of this game's pixel shaders write three targets; 1,880 write one.
    unsigned rtCount = 0;
    bool isPixelShader = false;
    bool anySampler = false;
    // The sampler that WON the albedo ranking, not merely the first one declared. The tiling
    // pair that applies is the one named after this map.
    char albedoSampler[28] = {};
    // Every sampler by stage. firstSampler names one of them and albedoSampler names the winner;
    // neither answers "what else is bound and where", which is the question when the colour is in
    // a texture this shim is not choosing.
    char samplerName[8][20] = {};
    // Per-material texture tiling. Disassembly of the world shaders (2026-08-16) shows the
    // primary UV built as:
    //     mul r0.x, c<TilingU>.x, v1.x
    //     mul r0.y, c<TilingV>.x, v1.y
    //     mul o1.xy, r0, 0.0009765625      ; 1/1024
    // so uv = raw * tiling / 1024. The 1/1024 is a literal, but the tiling factors are
    // uniforms whose REGISTERS DIFFER BETWEEN SHADERS (c1/c2 in ir_bbsimple2_decal_s, c2/c3 in
    // ir_bbsimple_1uv_decal_s), so they have to be read from each shader's constant table
    // rather than assumed. Shaders with no tiling constants scale by 1/1024 alone.
    // EVERY pair is kept, with the name of the map it belongs to, because the choice cannot be
    // made here: the tiling constants live in the VERTEX shader and the albedo sampler that
    // decides which pair applies is in the PIXEL shader. Resolved per draw by TilingForAlbedo.
    struct TilingPair {
        char base[24];      // the map name, e.g. "Normal_Map" out of "Normal_Map_TilingU"
        int uReg;
        int vReg;
    };
    static constexpr int kMaxTilingPairs = 4;
    TilingPair tiling[kMaxTilingPairs]{};
    int tilingCount = 0;
    // The inferred-lighting stipple pattern. Its presence in a shader with NO colour sampler is
    // the signature of SR3's DSF/normal prepass - confirmed by disassembly, not inferred:
    // ir_bb_tod_window_bs.fxo_pc shader [6] samples Normal_Map (s0) and IR_Stipple_Pattern_2D
    // (s11), decodes a tangent-space normal and writes specular power, with no colour anywhere;
    // shader [8] of the SAME FILE is the material pass and carries Diffuse_Map, Decal_Map,
    // Specular_Map, IR_LBuffer and Tint_color. 415 pixel shaders across the game match [6]'s
    // shape (236 stipple alone, 179 stipple + Normal_Map).
    // SR3's sky IS geometry, drawn by the rfg-skybox family inherited from Red Faction
    // Guerrilla: rfg-skybox_s, -clouds_s, -clouds-2_s, -matte_s, -overhead_s, -simple_s, -stars,
    // -meteors. Most carry Diffuse_Map or Decal_Map and would convert normally, so "the sky is
    // black" is not explained by our classification and needs to be observed rather than argued.
    // Recognised by constants unique to that family.
    bool skyShader = false;
    bool hasStipple = false;
    // Every sampler this shader declares is a PREPASS utility - a stipple pattern, a normal map,
    // a depth or shadow map. Never surface colour, and never the inferred-lighting inputs a real
    // material samples. Measured 2026-08-18: all 83 such draws in a steady-state frame went to
    // the G16R16 prepass target and every one shared its exact vertex/primitive counts with a
    // properly coloured converted draw, i.e. each is a duplicate of a mesh we already convert.
    bool prepassSamplersOnly = false;
    bool samplesLBuffer = false;  // reads IR_LBuffer: the mark of a pass that produces colour
    bool samplesGBufferNormals = false;  // reads the G-buffer normals: a screen-space pass
    bool samplesParticleDepth = false;   // reads Depth_buffer: an rl_particle_* billboard
    int alphaThresholdReg = -1;  // Alpha_Threshold: the shader does its own cutout with texkill
    // Materials with NO diffuse map by design, whose albedo is a CONSTANT. The shader file names
    // say it outright - ir_bbsimple2_nodiffmap - and the disassembly ends with
    //     mul_pp oC0, r1, c37        ; r1 = lighting, c37 = Tint_color
    // so the constant IS the base colour, not a modulation on top of one. 117 of the 121 shaders
    // that sample IR_GBuffer_DSF_Data without a colour map carry Tint_color.
    //
    // This answers the question YOUR-INSTRUCTIONS carried for weeks as "where do those materials
    // get their colour?", and it is why they must NOT be hidden: unlike the normal prepass they
    // are unique geometry in the material pass - zero of the 150 shared a shape with any other
    // converted draw - so hiding them would punch holes in the world.
    int colourConstReg = -1;
    int colourConstRank = 0;
    // SR3's customisable clothing builds its base colour PER TEXEL from a mask texture and three
    // constants, which is why no texture-stage arrangement has ever produced correct clothing
    // colour. Read from ir_sr3npcclothfull_c.fxo_pc shader [8] rather than inferred:
    //
    //     texld r3, v0, s0                  ; Pattern_Map
    //     sum  = p.r + p.g + p.b
    //     dev  = |p.r-sum/3| + |p.g-sum/3| + |p.b-sum/3|
    //     test = sum - (dev*165.016495 + 256)/255
    //     test <  0 -> albedo = p.r^2.2*Diffuse_Color_a + p.g^2.2*Diffuse_Color_b
    //                                                   + p.b^2.2*Diffuse_Color_c
    //     test >= 0 -> albedo = saturate((p - 0.372549) * 1.59375) ^ 2.2
    //     mul oC0, r1, c37                  ; and the whole result * Tint_color
    //
    // So the three channels are not simple masks: they are gamma-2.2 weights, and a SELECTOR
    // switches whole texels to a desaturated branch - which is how trim and skin escape being
    // tinted by the three customisation colours. 34 shader entries across 19 files carry the
    // full set, the whole ir_*sr3pccloth* / ir_*sr3npccloth* family, player and NPC alike.
    char colourConstName[24] = {};        // which constant won the colour ranking
    // WHICH TEXCOORD FEEDS EACH SAMPLER, read from the shader's own texld instructions.
    //
    // This shim has assumed since the fork that the albedo is sampled with TEXCOORD0 and the
    // Pattern_Map with TEXCOORD1. Disassembling the player's cloth shaders shows that is not a
    // rule, it is a coincidence that holds for some variants:
    //
    //   ir_sr3pccloth_bs   [6]   texld r6, v1, s0     s0 <- TEXCOORD1
    //   ir_at_sr3pccloth_bs[8]   texld r5, v1, s2     s2 <- TEXCOORD1   (the pattern)
    //                            texld r4, r4, s0     s0 <- a COMPUTED coordinate built from
    //                                                       TEXCOORD6 and clamped against c2/c3
    //
    // So a garment's albedo can arrive through TEXCOORD6, and the CPU baker has been rasterising
    // into TEXCOORD0 space regardless - which is exactly why its islands need not land where the
    // mesh samples them. -1 means the coordinate is computed rather than taken straight from an
    // input register, and that is worth knowing too: a computed coordinate cannot be reproduced
    // by resampling and says so instead of being guessed at.
    int samplerUv[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
    bool sawTexld = false;
    int patternStage = -1;                // the Pattern_Map sampler
    int diffuseColorReg[3] = {-1, -1, -1};   // Diffuse_Color_a, _b, _c
    // Diffuse_Color, a SEPARATE uniform (c11 in the player cloth shaders) that scales the diffuse
    // map before the customisation colours are applied. Not one of the three above.
    int diffuseColorMulReg = -1;
    int tintColorReg = -1;
    // Hair renders white and neither of its textures holds the colour: the Dob_Map is white
    // strands on a green field and the Diffuse_Map is smooth directional data. ir_sr3pchair_c
    // multiplies its result by Hair_Spec_Color2 (`mul_pp r0.xyz, r0, c4`), so despite the name
    // these are the likeliest carriers of the chosen hair colour.
    int hairColorReg[2] = {-1, -1};
    // First sampler this shader declares, kept only so a blanked draw can say WHAT it sampled.
    // Over half of converted draws currently render untextured and the reason has been inferred
    // rather than observed.
    char firstSampler[28] = {};
};

// Keyed on the shader's own address, and every entry holds a reference.
//
// Without that reference this is the 2026-08-18 use-after-free again, in its quietest form: a
// released shader's address can be handed straight back to a newly created one, and every lookup
// then answers with the PREVIOUS shader's reflection - wrong sampler names, wrong albedo rank,
// wrong skinned flag. It does not crash; it silently mis-classifies draws, which is far harder to
// find than a crash and matches the shape of "surfaces are the wrong colour".
//
// Nothing is ever erased, so the reference also pins the shader for the process lifetime. SR3 has
// 7,276 shaders in total, so the ceiling is bounded and small.
std::unordered_map<void*, ShaderInfo> g_shaders;

size_t ShaderLength(const DWORD* tokens) {
    for (size_t i = 1; i < 65536; ++i) {
        if (tokens[i] == 0x0000FFFF) return (i + 1) * sizeof(DWORD);
        if ((tokens[i] & 0xFFFF) == 0xFFFE) i += (tokens[i] >> 16) & 0x7FFF;
    }
    return 0;
}

// Which sampler carries base colour? Measured across every pixel shader in the game: stage 0
// holds Diffuse_Map 804 times but a NORMAL map 903 times (Normal_Map 415 + Damage_Normal_Map
// 488). Remix reads stage 0 as albedo, so those ~900 materials were being shaded with a
// tangent-space normal map - the green/orange "yellow geometry" the user identified. Many have
// no Diffuse sampler at all, so matching the name "Diffuse" alone finds nothing. Rank every
// sampler and take the best; rank 0 means no colour texture exists, and binding nothing
// (white) still beats binding a normal map.
int AlbedoRank(const char* name) {
    if (_strnicmp(name, "Diffuse", 7) == 0) return 100;
    // "Diffuse" is not always the prefix - Decal_diffuse_mapSampler and friends were scoring 0
    // and having their stage 0 blanked despite naming a diffuse map outright.
    if (StrStrIA(name, "diffuse")) return 95;
    if (StrStrIA(name, "Decal_Map")) return 90;
    if (StrStrIA(name, "Pattern_Map")) return 80;
    if (StrStrIA(name, "Blend_Map")) return 70;
    if (StrStrIA(name, "Grime") || StrStrIA(name, "Dirt_Rust")) return 60;
    if (StrStrIA(name, "base_sampler")) return 50;
    // Sky cloud layers. rfg-skybox-clouds-2_s samples Layer01_map + Layer23_map and nothing
    // else, so without this it scores 0 and the sky renders blank.
    if (StrStrIA(name, "Layer01_map") || StrStrIA(name, "Layer23_map")) return 45;
    if (StrStrIA(name, "Illumination") || StrStrIA(name, "Glow")) return 20;
    return 0;
}

ShaderInfo ReflectShader(const DWORD* tokens) {
    ShaderInfo result;
    LightShader& info = result.light;
    const size_t length = ShaderLength(tokens);
    if (!length) return result;

    const char* bytes = reinterpret_cast<const char*>(tokens);
    // Scan the instruction stream for dcl_blendweight on an INPUT register. Opcode 0x1F is DCL;
    // its first source token carries the D3DDECLUSAGE in the low five bits (BLENDWEIGHT == 1) and
    // its second is the destination register. Length lives in bits 24-27 for SM2+, so the walk
    // steps instruction by instruction rather than guessing.
    {
        const DWORD* t = tokens + 1;                       // skip the version token
        const DWORD* end = tokens + length / sizeof(DWORD);
        unsigned rtMask = 0;
        while (t < end && *t != 0x0000FFFF) {               // 0xFFFF is END
            const DWORD op = *t & 0xFFFF;
            const DWORD len = (*t & 0x0F000000) >> 24;
            if (op == 0x1F && t + 2 < end) {                // D3DSIO_DCL
                const DWORD usage = t[1] & 0x1F;
                const DWORD regType = ((t[2] & 0x70000000) >> 28) | ((t[2] & 0x00001800) >> 8);
                if (usage == 1 /* D3DDECLUSAGE_BLENDWEIGHT */ && regType == 1 /* INPUT */)
                    result.usesBlendWeights = true;
                if (usage == 2 /* D3DDECLUSAGE_BLENDINDICES */ && regType == 1 /* INPUT */)
                    result.usesBlendIndices = true;
                // POSITION with usage index 1. The index lives in bits 16-19 of the same
                // token as the usage; index 2/3/4 are the instance transform, not a morph.
                if (usage == 0 /* D3DDECLUSAGE_POSITION */ && ((t[1] >> 16) & 0xF) == 1 &&
                    regType == 1 /* INPUT */)
                    result.usesMorph = true;
            }
            // D3DSIO_TEX / texld (0x42) in ps_3_0: dst, source coordinate, sampler.
            //
            // The coordinate's register type lives in the same split field every register uses -
            // bits 28-30 with bits 11-12 - and type 1 is D3DSPR_INPUT, the vN registers that
            // dcl_texcoordN declares. The sampler's index is the low bits of the third token.
            // A coordinate that is anything other than an input register was computed by the
            // shader and is recorded as -1.
            if (op == 0x42 && len >= 3 && t + 3 < end) {
                const DWORD srcTok = t[2], smpTok = t[3];
                const DWORD srcType = ((srcTok & 0x70000000) >> 28) | ((srcTok & 0x00001800) >> 8);
                const DWORD smpType = ((smpTok & 0x70000000) >> 28) | ((smpTok & 0x00001800) >> 8);
                const unsigned smp = smpTok & 0x7FF;
                if (smpType == 10 /* D3DSPR_SAMPLER */ && smp < 8) {
                    result.sawTexld = true;
                    result.samplerUv[smp] =
                        (srcType == 1 /* D3DSPR_INPUT */) ? static_cast<int>(srcTok & 0x7FF) : -1;
                }
            }
            // D3DSIO_DEF (0x51): `def cN, x, y, z, w`. One destination token, then four floats.
            //
            // A UV divide is a reciprocal power of two applied to a .xy write, so that is exactly
            // what is accepted here - between 1/256 and 1/16384, and a power of two to within a
            // rounding error. That is narrow enough that ordinary shader constants do not qualify,
            // and it is checked rather than assumed: if a shader defs two DIFFERENT such values
            // there is no way to tell which one scales the texcoords without following the
            // dataflow, so the result is marked ambiguous and the caller keeps its old default.
            // Guessing between them is how this project has lost days before.
            if (op == 0x51 && t + 5 < end) {
                float v[4];
                memcpy(v, &t[2], sizeof(v));
                for (int c = 0; c < 2; ++c) {          // x and y; a uv divide writes .xy
                    if (!(v[c] > 0.0f) || v[c] > 1.0f / 256.0f) continue;
                    const double inv = 1.0 / static_cast<double>(v[c]);
                    const unsigned n = static_cast<unsigned>(inv + 0.5);
                    if (n < 256u || n > 16384u) continue;
                    if (n & (n - 1u)) continue;                       // not a power of two
                    if (inv - n > 0.01 || n - inv > 0.01) continue;   // and exactly one
                    if (result.uvDefScale == 0.0f) result.uvDefScale = v[c];
                    else if (result.uvDefScale != v[c]) result.uvDefAmbiguous = true;
                }
            }
            // Count distinct COLOROUT destinations. The destination is the first token after the
            // opcode; register type 8 is D3DSPR_COLOROUT, split across bits 28-30 and 11-12 the
            // same way every other register type is.
            if (op != 0x1F && op != 0x51 && len >= 1 && t + 1 < end) {
                const DWORD d = t[1];
                const DWORD dt = ((d & 0x70000000) >> 28) | ((d & 0x00001800) >> 8);
                if (dt == 8) rtMask |= 1u << (d & 3);
            }
            // A comment block - CTAB is one, and it sits immediately after the version token -
            // carries its length in bits 16-30, NOT in the 24-27 field the instructions use.
            // Reading it from the wrong field walks into the middle of the constant table and
            // the dcl instructions are never reached at all.
            if (op == 0xFFFE) {
                // Capped against `end`. A corrupt length would otherwise walk the pointer far
                // past the buffer before the loop condition caught it.
                const DWORD skip = 1 + ((*t >> 16) & 0x7FFF);
                if (static_cast<size_t>(end - t) <= skip) break;
                t += skip;
                continue;
            }
            t += 1 + len;
            if (!len) break;                                // malformed; stop rather than spin
        }
        for (unsigned b = 0; b < 4; ++b) if (rtMask & (1u << b)) ++result.rtCount;
    }

    const char* ctab = nullptr;
    for (size_t i = 0; i + 4 <= length; ++i)
        if (memcmp(bytes + i, "CTAB", 4) == 0) { ctab = bytes + i; break; }
    if (!ctab) return result;

    const char* base = ctab + 4;
    const DWORD size = *reinterpret_cast<const DWORD*>(base);
    const DWORD count = *reinterpret_cast<const DWORD*>(base + 12);
    const DWORD infoOffset = *reinterpret_cast<const DWORD*>(base + 16);
    if (size != 28 || count > 256) return result;

    for (DWORD i = 0; i < count; ++i) {
        const char* entry = base + infoOffset + i * 20;
        if (entry + 20 > bytes + length) break;
        const DWORD nameOffset = *reinterpret_cast<const DWORD*>(entry);
        const WORD registerSet = *reinterpret_cast<const WORD*>(entry + 4);
        const WORD reg = *reinterpret_cast<const WORD*>(entry + 6);
        const char* name = base + nameOffset;
        if (name < bytes || name >= bytes + length) continue;

        if (StrStrIA(name, "Cloud_Fade_Height") || StrStrIA(name, "Layer_strengths") ||
            StrStrIA(name, "TOD_Light_Dir") || StrStrIA(name, "Layer01_map") ||
            StrStrIA(name, "Layer23_map") || StrStrIA(name, "Star_strength"))
            result.skyShader = true;

        if (!strcmp(name, "projTM")) result.usesProjTM = true;
        else if (!strcmp(name, "objTM")) result.usesObjTM = true;
        else if (!strcmp(name, "Bone_weights")) {
            result.skinned = true;
            result.boneReg = reg;
        }

        if (registerSet == 3) {   // D3DXRS_SAMPLER
            if (StrStrIA(name, "Stipple")) result.hasStipple = true;
            // Inferred lighting: a pass that produces visible colour reads the light buffer to
            // shade itself. That makes this the property that separates a material from a
            // G-buffer fill, without needing to recognise every sampler name in the game.
            if (StrStrIA(name, "IR_LBuffer")) result.samplesLBuffer = true;
            // Reading the G-buffer's NORMALS means reading the orientation of geometry that is
            // already on screen. Only a screen-space pass needs that - a real surface carries
            // its own normals and has no reason to ask what is behind it.
            if (StrStrIA(name, "IR_GBuffer_Normals")) result.samplesGBufferNormals = true;
            // `Depth_buffer` is the PARTICLE system's soft-fade sampler and nothing else uses
            // that spelling. Water reads `IR_GBuffer_Depth`, projectors read `Depth_map` - the
            // names are distinct, which is what makes this test exact rather than approximate.
            if (StrStrIA(name, "Depth_buffer")) result.samplesParticleDepth = true;
            // Positive identification, not "anything unrecognised". A shader is only treated as
            // a prepass if EVERY sampler it declares is one of these; an unknown sampler name
            // keeps it a material. That is the safeguard the old `albedoRank == 0` prepass test
            // lacked - it hid ~2,400 real draws a frame including 17,601-vertex terrain, and it
            // is recorded as a dead end for exactly that reason.
            const bool utility = StrStrIA(name, "Stipple") || StrStrIA(name, "Normal_Map") ||
                                 StrStrIA(name, "Depth_map") || StrStrIA(name, "shadow_map");
            if (!result.anySampler) result.prepassSamplersOnly = true;   // first sampler seen
            if (!utility) result.prepassSamplersOnly = false;
            result.anySampler = true;
            if (!result.firstSampler[0])
                strncpy_s(result.firstSampler, name, sizeof(result.firstSampler) - 1);
            if (reg >= 0 && reg < 8) strncpy_s(result.samplerName[reg], name, 19);
            if (!_stricmp(name, "Pattern_MapSampler")) result.patternStage = reg;
            const int rank = AlbedoRank(name);
            if (rank > result.albedoRank) {
                result.albedoRank = rank;
                result.albedoStage = reg;
                strncpy_s(result.albedoSampler, name, sizeof(result.albedoSampler) - 1);
            }
            continue;
        }

        // The constant that carries base colour for a no-diffuse-map material. Ranked, because a
        // shader can name several: the specific ones win over the generic Tint_color, which is
        // simply the most common. A shader combining two (albedo * tint) would need real
        // dataflow analysis to resolve; taking the highest-ranked single constant is an
        // approximation, and a flat colour that is close beats white that is certainly wrong.
        // The shader performs its OWN alpha cutout. `ir_at_*` - at for alpha test - foliage,
        // decals, windows and cloth all do this with `texkill` against Alpha_Threshold, and
        // there is no D3D9 alpha-test render state involved at all. Without noticing that, the
        // cutout rule below ("alpha test on AND ref > 0") never fires for them and every leaf
        // card renders as a solid opaque rectangle.
        if (!_stricmp(name, "Alpha_Threshold")) result.alphaThresholdReg = reg;

        if (registerSet == 2) {   // D3DXRS_FLOAT4
            int crank = 0;
            if (!_stricmp(name, "Diffuse_Color")) crank = 100;
            else if (!_stricmp(name, "Base_Paint_Color")) crank = 90;
            else if (!_stricmp(name, "Glass_Color")) crank = 80;
            else if (!_stricmp(name, "Base_Color")) crank = 70;
            else if (!_stricmp(name, "Draw_Color")) crank = 60;
            else if (!_stricmp(name, "Tint_color")) crank = 50;
            if (!_stricmp(name, "Diffuse_Color")) result.diffuseColorMulReg = reg;
            else if (!_stricmp(name, "Diffuse_Color_a")) result.diffuseColorReg[0] = reg;
            else if (!_stricmp(name, "Diffuse_Color_b")) result.diffuseColorReg[1] = reg;
            else if (!_stricmp(name, "Diffuse_Color_c")) result.diffuseColorReg[2] = reg;
            else if (!_stricmp(name, "Tint_color")) result.tintColorReg = reg;
            else if (!_stricmp(name, "Hair_Spec_Color1")) result.hairColorReg[0] = reg;
            else if (!_stricmp(name, "Hair_Spec_Color2")) result.hairColorReg[1] = reg;
            if (crank > result.colourConstRank) {
                result.colourConstRank = crank;
                result.colourConstReg = reg;
                // Inside the same test as the register, or the two describe different constants -
                // which is what made the first CHAR CONST report print "Tint_color(c14)".
                strncpy_s(result.colourConstName, name, sizeof(result.colourConstName) - 1);
            }
        }

        // Texture tiling. Several pairs coexist - Normal_Map, Decal_Map, Diffuse, Grime - and
        // each belongs to ONE map. Preferring Normal_Map_Tiling, as this did until 2026-08-20,
        // was measured wrong: it is the pair driving the vertex shader's primary UV output,
        // which feeds the NORMAL map, while the texture this shim binds as albedo is the
        // DIFFUSE map. A detail normal is tiled far more densely than the diffuse it detail-maps,
        // so the diffuse came out correspondingly too dense - the reported "some surfaces like
        // roads tiled too densely". 443 shaders carry a Normal_Map pair and only 44 carry a
        // diffuse one, so most of the world was being scaled by a factor that never applied to it.
        {
            const char* tiling = StrStrIA(name, "_Tiling");
            const bool isU = tiling && (StrStrIA(name, "TilingU") || StrStrIA(name, "tiling_u"));
            const bool isV = tiling && (StrStrIA(name, "TilingV") || StrStrIA(name, "tiling_v"));
            if (isU || isV) {
                char base[24] = {};
                const size_t len = min(static_cast<size_t>(tiling - name), sizeof(base) - 1);
                memcpy(base, name, len);
                int slot = -1;
                for (int t = 0; t < result.tilingCount; ++t)
                    if (!_stricmp(result.tiling[t].base, base)) { slot = t; break; }
                if (slot < 0 && result.tilingCount < ShaderInfo::kMaxTilingPairs) {
                    slot = result.tilingCount++;
                    strncpy_s(result.tiling[slot].base, base, sizeof(base) - 1);
                    result.tiling[slot].uReg = -1;
                    result.tiling[slot].vReg = -1;
                }
                if (slot >= 0) {
                    if (isU) result.tiling[slot].uReg = reg;
                    if (isV) result.tiling[slot].vReg = reg;
                }
            }
        }

        if (!strcmp(name, "IR_Light_Pos")) info.regPos = reg;
        else if (!strcmp(name, "IR_Light_Dir")) info.regDir = reg;
        else if (!strcmp(name, "IR_Light_Color")) info.regColor = reg;
        else if (!strcmp(name, "IR_Light_Info")) info.regInfo = reg;
        else if (!strcmp(name, "IR_Spot_Info")) info.regSpot = reg;
    }

    // Directional lights carry no IR_Light_Info (no distance falloff); spots add IR_Spot_Info.
    if (info.regPos >= 0 && info.regColor >= 0) {
        if (info.regSpot >= 0) info.kind = LightShader::Spot;
        else if (info.regInfo >= 0) info.kind = LightShader::Point;
        else info.kind = LightShader::Directional;
    }
    return result;
}

// ---------------------------------------------------------------- tracked device state

float g_vsConst[kMaxVsConst][4] = {};
float g_psConst[kMaxPsConst][4] = {};

ShaderInfo g_curVS, g_curPS;

// How many draws took a scale OTHER than the historic 1/1024, and what that scale was. Reported
// rather than applied silently: this changes UVs for the whole game, so it has to be visible.
unsigned g_uvScaleFromShaderUsed = 0, g_uvScaleAmbiguous = 0;
float g_uvScaleLast = 0.0f;

float CurrentShortUVScale() {
    if (!g_settings.uvScaleFromShader) return kShortUVScale;
    if (g_curVS.uvDefAmbiguous) { ++g_uvScaleAmbiguous; return kShortUVScale; }
    if (g_curVS.uvDefScale <= 0.0f) return kShortUVScale;
    if (g_curVS.uvDefScale != kShortUVScale) {
        ++g_uvScaleFromShaderUsed;
        g_uvScaleLast = g_curVS.uvDefScale;
    }
    return g_curVS.uvDefScale;
}
IDirect3DVertexShader9* g_lastVS = nullptr;
IDirect3DPixelShader9* g_lastPS = nullptr;

IDirect3DBaseTexture9* g_curTexture[8] = {};
std::unordered_set<IDirect3DBaseTexture9*> g_rtTextures;   // created with D3DUSAGE_RENDERTARGET

// Vertex declaration facts needed at draw time. Only stream 0 is recorded: if the fields we
// need are not all there we must know it rather than silently read the wrong bytes.
struct VertexLayout {
    bool parsed = false;
    bool hasNormal = false, hasTexcoord = false, hasColor = false, hasPositionT = false;
    bool skinned = false;
    int texcoordType = -1;
    int texcoordOffset = -1;
    // THE SECOND UV SET. SR3's clothing shaders declare three texcoord inputs -
    // dcl_texcoord v1, dcl_texcoord1 v4, dcl_texcoord2 v5 - and the PATTERN map is sampled with
    // TEXCOORD1, not TEXCOORD0. Only set 0 was ever decoded, which is why the customisation
    // pattern could never be evaluated: half its input was not being read.
    int texcoord1Type = -1;
    int texcoord1Offset = -1;
    int texcoord1Stream = -1;
    // Fixed function can only transform an uncompressed position. SR3 compresses most vertex
    // fields (normals as ubyte4n, UVs as short2), so a compressed POSITION is entirely
    // plausible and would be read as raw integers - scattering the mesh across the world.
    int posType = -1;
    int posOffset = -1;
    int posStream = -1;
    int elements = 0;
    // Where an instanced draw keeps its world matrix. Measured 2026-08-16: three float4 rows
    // declared as POSITION with usage indices 2, 3 and 4, in stream 4 at offsets 0/16/32 -
    // the same row-major 3x4 layout objTM uses, just delivered through a vertex stream.
    int instStream = -1;
    int instRowOffset[3] = {-1, -1, -1};
    bool hasInstanceTransform = false;
    // Skinning inputs. The vertex shader consumes these as dcl_blendweight/dcl_blendindices,
    // but the DECLARATION is what says how they are stored, and CPU skinning has to decode
    // them exactly. Measured rather than assumed: the shader normalises by the weight sum
    // (rcp of w.x+w.y+w.z+w.w), which means raw un-normalised integer weights are legal here
    // and a UBYTE4-vs-UBYTE4N mix-up would not show up as an obvious error, just wrong poses.
    int blendWeightType = -1, blendWeightOffset = -1;
    int blendIndexType = -1, blendIndexOffset = -1;
    int normalType = -1, normalOffset = -1;
    // The MORPH stream: POSITION1 and NORMAL1, which SR3 puts in stream 2. Present in the
    // declaration for every skinned mesh; only the `_mc` shaders actually read it, so the
    // decision to apply it belongs to ShaderInfo::usesMorph, not here.
    int morphStream = -1;
    int morphPosType = -1, morphPosOffset = -1;
    int morphNrmType = -1, morphNrmOffset = -1;
};
std::unordered_map<void*, VertexLayout> g_layouts;
VertexLayout g_curLayout;

// Stream 0, tracked so a draw can read its own vertices for the UV measurement below.
IDirect3DVertexBuffer9* g_stream0 = nullptr;
UINT g_stream0Offset = 0, g_stream0Stride = 0;
// All streams, because SR3 puts per-object transforms in an instance stream - measured at
// stream 4, which the first version of this tracking (streams 0-3 only) could not even see.
constexpr int kMaxStreams = 8;
IDirect3DVertexBuffer9* g_streamVB[kMaxStreams] = {};
UINT g_streamStride[kMaxStreams] = {}, g_streamOffset[kMaxStreams] = {};
IDirect3DVertexDeclaration9* g_curDecl = nullptr;
IDirect3DSurface9* g_curRenderTarget = nullptr;
// How many COLOUR channels the live render target's format carries. Measured once per target
// change, never per draw - GetDesc is a bridge round trip.
//
// A target with fewer than three cannot hold an albedo, whatever the pixel shader samples. SR3's
// character prepass goes to a 2560x1440 D3DFMT_G16R16 (two channels, no blue, no alpha) with
// Diffuse_MapSampler bound, so it passes the sampler-less test and was being CONVERTED - and
// Remix then path-traced the identical mesh twice, once from that pass and once from the real
// A16B16G16R16F material pass. That is the doubled head, and it is why heads z-fight.
// Per SLOT, because this game binds more than one. SR3 is a deferred renderer: its G-buffer pass
// binds the 2-channel normal/depth buffer at index 0 and a colour target alongside it. Asking
// only about index 0 therefore answers a different question than "does this draw write colour",
// and answering the wrong one hid 2,180 draws a frame of world geometry on 2026-08-27 - the
// whole city, whenever the camera pointed at it.
constexpr unsigned kMaxRTSlots = 4;
unsigned g_rtChannels[kMaxRTSlots] = {4, 4, 4, 4};
bool g_rtBound[kMaxRTSlots] = {true, false, false, false};
// Slot 0's size, from the SAME GetDesc the channel count already costs - no extra bridge round
// trip on a hook this file names as a hitch candidate. Used to tell a composite into an
// OFF-SCREEN texture from a composite of the screen itself.
UINT g_rt0Width = 0, g_rt0Height = 0;
unsigned g_skipNonColourTarget = 0, g_mrtColourElsewhere = 0;

// True if ANY bound target could hold an albedo. That is the real precondition: a draw writes no
// colour only when nothing it is writing to can carry any.
inline bool AnyColourTarget() {
    for (unsigned i = 0; i < kMaxRTSlots; ++i)
        if (g_rtBound[i] && g_rtChannels[i] >= 3) return true;
    return false;
}

// Colour channels per format. Unknown formats answer 4: this rule may only ever REMOVE a draw,
// so anything it does not recognise must fall on the side of leaving it alone.
inline unsigned FormatColourChannels(D3DFORMAT f) {
    switch (f) {
        case D3DFMT_A8: case D3DFMT_L8: case D3DFMT_L16:
        case D3DFMT_R16F: case D3DFMT_R32F:
            return 1;
        case D3DFMT_A8L8: case D3DFMT_G16R16:
        case D3DFMT_G16R16F: case D3DFMT_G32R32F:
            return 2;
        default:
            return 4;
    }
}

// Instance-stream contents, cached one lock per buffer per frame. Locking per draw would be
// ~860 buffer locks a frame, each a round trip across the 32->64-bit bridge; the whole buffer
// is filled once per frame and indexed by offset, so one copy serves every draw that uses it.
// The snapshot is only valid until the game next WRITES the buffer. SR3's instance buffers are
// dynamic and filled progressively through the frame - lock, write a batch, draw it, lock
// again, write the next - so a once-per-frame snapshot taken at the first use serves stale
// transforms to every later batch. Which objects get stale data shifts with draw order, which
// shifts with the camera: that is the flicker on repeated objects. Invalidating on the game's
// own Lock keeps the one-lock-per-fill economy while staying correct.
struct InstanceCache {
    std::vector<unsigned char> data;
    // Which bytes the game has WRITTEN since the last whole-buffer discard, parallel to `data`.
    //
    // The snooped copy is refreshed only when the game locks the buffer, and that measures at 0.7
    // writes a frame against ~377 instanced draws - so almost every converted instanced draw is
    // reading bytes written on some earlier frame. That is fine if the buffer holds static world
    // transforms and fatal if it does not, and nothing here has ever distinguished the two.
    //
    // D3DLOCK_DISCARD means "the previous contents are gone": the game gets a fresh allocation and
    // typically fills only the part it needs, so everything outside that span is stale even though
    // our copy still holds plausible-looking numbers. An object whose transform is read from a
    // stale region lands wherever the previous occupant was - which, if that is behind the camera,
    // looks exactly like "it entered the frustum and was deleted".
    std::vector<unsigned char> fresh;
    unsigned frame = 0xFFFFFFFFu;
    bool valid = false;
};
std::unordered_map<IDirect3DVertexBuffer9*, InstanceCache> g_instCache;
unsigned g_instanceConverted = 0, g_instanceLocks = 0, g_instCacheInvalidations = 0;
unsigned g_instFreshTransform = 0;   // instanced draws whose transform bytes were freshly written
unsigned g_instStaleTransform = 0;   // ...and those reading bytes left over from a previous fill
unsigned g_instDiscards = 0;         // whole-buffer discards seen on an instance stream
unsigned g_instStaleReports = 0;

// Occlusion-query interception; the hooks themselves live next to HookDevice. Declared here so
// the per-frame report, which is written earlier in the file, can read them.
// "seen" counts every occlusion readback whether or not we override it, so the setting can be
// left off for one run to measure the baseline. "forced" is the falsifier: on with 0 forced
// means the hook never fired.
unsigned g_occlusionQueriesSeen = 0, g_occlusionForced = 0, g_queriesCreated = 0;

// Each mesh's first-seen albedo. SR3 streams textures, and under memory pressure it evicts a
// wall's unique texture and rebinds a shared fallback - so a building's face changes material
// as you move. The game's own renderer hides this, but Remix hashes what it is handed, so the
// surface visibly swaps. Remembering the first real texture a mesh was drawn with and rebinding
// it is the same workaround the SR2 proxy settled on.
// Every entry holds an AddRef on its texture, so this is a hard cap on how many textures the
// shim can pin against the game's streamer.
// Breadcrumbs for the crash handler. The crash to explain happens on fast movement and on
// freefall - i.e. under heavy streaming - and the leading hypothesis is a texture the mesh cache
// outlived. If the faulting address matches the albedo we last bound, that is settled on sight
// instead of argued about.
IDirect3DBaseTexture9* g_lastBoundAlbedo = nullptr;
bool g_lastAlbedoFromCache = false;

constexpr size_t kMaxMeshAlbedo = 4096;
unsigned g_meshAlbedoFlushes = 0;
std::unordered_map<unsigned long long, IDirect3DBaseTexture9*> g_meshAlbedo;
unsigned g_albedoRestored = 0;
unsigned g_meshKeyBaseVertex = 0;   // set per draw, disambiguates meshes in a shared buffer
// The current draw's vertex window. Probes that run inside SetupTextureStages have no other
// way to reach it, and sampling the whole shared buffer instead would mix meshes together.
UINT g_curDrawFirstVertex = 0;
UINT g_curDrawVertexCount = 0;
float g_appliedU = 0.0f, g_appliedV = 0.0f;   // cached texture-matrix scale
unsigned g_uvMatrixWrites = 0;
unsigned g_samplerProbeReports = 0;
bool g_alphaStateOverridden = false;
DWORD g_savedAlphaTest = 0, g_savedAlphaRef = 0, g_savedAlphaFunc = 0;
unsigned g_drawIndexThisFrame = 0;
// UP draws, counted separately so "how many draws did we never see?" has an answer rather than
// an assumption. Split by entry point because the two carry different vertex data.
unsigned g_upDrawsTotal = 0, g_upIndexedTotal = 0;
unsigned g_upHudDemoted = 0, g_upLeftAlone = 0;
unsigned g_hudFormatDumps = 0;
char g_blankNames[16][28] = {};
unsigned g_blankReports = 0;
bool g_instancedDraw = false;   // stream 0 carries an instance count
UINT g_instanceCount = 1;
// The raw frequency setting per stream, so a converted draw can restore exactly what the game had
// rather than an assumption about what it probably was.
UINT g_streamFreq[4] = {1, 1, 1, 1};

// Dirty tracking. Transforms are re-applied only when a register that feeds them was written,
// which is the difference between three SetTransform calls per draw and three per change.
bool g_viewProjDirty = false, g_worldDirty = false;
bool g_viewProjValid = false;
bool g_worldWritten = false;          // objTM written since the last shader change
bool g_worldWrittenSinceDraw = false;
// How recently was the bone palette uploaded? Skinning by a palette the game did not write for
// this object means posing it with the PREVIOUS character's bones, which puts its pieces
// wherever that character's limbs were - the shape of "clothing vanished".
//
// Measured as a distance in draws rather than a per-draw boolean: one palette upload serves
// every material range of a character, so a boolean cleared at each draw would call all but the
// first of them stale. Zero means this draw's own setup wrote it; a small number means the same
// character; a large one means the palette belongs to something else entirely.
// How often baseVertex is non-zero on a skinned draw. If this is zero the fix above is inert
// and the wrong heads have another cause; if it is not, it was decoding the wrong character.
unsigned g_skinNonZeroBaseVertex = 0;
unsigned g_lastBoneUploadDraw = 0;
unsigned g_lastBoneUploadFrame = 0;
// How many bones the last palette upload actually covered, and what the skinned draws did with
// it. These exist to decide ONE question: is a skinned draw posed by its own object's bones?
//   staleFrame  - the palette was last written in an EARLIER frame, so it belongs to whatever
//                 was drawn then. Unambiguously wrong.
//   beyondReach - the mesh references a bone the last upload never wrote. Those registers still
//                 hold the previous object's palette, so the part is posed onto that object.
//   farUpload   - palette written this frame but many draws ago; suspicious, not conclusive.
unsigned g_lastBoneUploadBones = 0;
unsigned g_skinPaletteStaleFrame = 0, g_skinPaletteBeyondReach = 0, g_skinPaletteFarUpload = 0;
unsigned g_skinPaletteOwn = 0, g_skinMaxBoneSeen = 0, g_skinMinReachSeen = 0xFFFFFFFFu;

// Per-bone provenance, because "the reach of the last upload" was the WRONG measurement: SR3
// writes a palette across several SetVertexShaderConstantF calls, so a small trailing call made a
// perfectly valid palette look one bone deep. What actually matters is not how far one call
// reached but whether all the bones a mesh uses were written for the SAME object.
//
// Recorded as the draw index at which each bone's registers were last written. A mesh whose used
// bones were written at widely separated draw indices is being posed by a MIXTURE of palettes -
// some of its own object's bones, some left over from whatever was drawn before. That is the
// shape of "a detached car door is standing where that NPC's arm is", and it needs no assumption
// about how many calls an upload takes or what order they arrive in.
unsigned g_boneWrittenDraw[kBonesMax] = {};
unsigned g_boneWrittenUploadBones[kBonesMax] = {};
// Which OBJECT owned the palette when each bone was written.
//
// Upload size is a weak fingerprint - a 20-bone car and a 20-bone character are indistinguishable
// by it. objTM is not: it is the object's own placement, and the game rewrites it for every
// object it draws. A bone written while a DIFFERENT objTM was in force belongs to a different
// object, whatever size its skeleton was. Since every palette upload in this game starts at c52,
// bone 0 is rewritten by every single object that uploads one - so a part pinned to a low bone
// index picks up whoever wrote last, continuously. That is "windows move all the time".
unsigned g_objGeneration = 0;
// Which draw setup published the palette currently in the shadow, and which setup we are in now.
// A draw may only pose by that palette if the two agree.
unsigned g_setupId = 0, g_paletteSetupId = 0xFFFFFFFFu;
unsigned g_boneWrittenObjGen[kBonesMax] = {};
float g_lastObjTM[3] = {};
unsigned g_boneWrittenFrame[kBonesMax] = {};
unsigned g_skinMixedPalette = 0, g_skinBoneNeverWritten = 0, g_skinCleanPalette = 0;
unsigned g_skinWorstSpread = 0;
// How far back a bone may have been written and still count as part of this object's palette.
// One object's upload is a burst of consecutive calls sharing a draw index or within a draw or
// two of it; the measured bad cases sat 276 draws away, so the two populations are nowhere near
// each other and this threshold is not a fine judgement.
constexpr unsigned kStaleBoneDraws = 4;
// Falsifiers for the rejection. If rejectStaleBones is on and "influences rejected" is 0, the
// filter never fired. If "vertices left in bind pose" is large, rejection is too aggressive and
// is flattening meshes rather than repairing them.
unsigned g_staleInfluencesRejected = 0, g_skinVertsBindPose = 0;
unsigned g_skinMixedReports = 0;
unsigned g_skinForeignBoneReg = 0, g_skinC52BoneReg = 0, g_skinBoneRegSeen = 0;
unsigned g_skinDisplaced = 0, g_skinAtRest = 0, g_skinDisplaceReports = 0;
float g_skinWorstDisplace = 0.0f;
unsigned g_skinFewBones = 0, g_skinFewBonesForeign = 0, g_skinFewBonesOwn = 0;
unsigned g_skinFewBoneReports = 0, g_rigidOffsetReports = 0;
// Per-mesh history for the drift measurement above.
struct DriftEntry { bool seen=false; unsigned frame=0; float world[3]={}; float obj[3]={}; float bone[3]={}; };
std::unordered_map<unsigned long long, DriftEntry> g_driftTrack;
unsigned g_driftMoved = 0, g_driftStill = 0, g_driftReports = 0, g_paletteOutOfScope = 0;
float g_driftWorst = 0.0f;
// Draws split by which transform the SHADER actually performs, so the split is visible rather
// than assumed. "single bone" is the vehicle/rigid-attachment path.
unsigned g_skinFourBone = 0, g_skinSingleBone = 0;
unsigned g_rigidOwnObject = 0, g_rigidForeignObject = 0, g_rigidForeignReports = 0;
// Falsifiers for the clamp. ON with 0 stale verts means it never fired. A stale count close to
// the total vertex count means it is rejecting nearly everything and flattening meshes instead.
unsigned g_staleBoneVerts = 0, g_skinVertsTotal = 0, g_vehicleBonesSkipped = 0;
unsigned g_shadowCheckReports = 0, g_shadowMismatch = 0, g_shadowMatch = 0;
// __popcnt64 is not available to this 32-bit build; the mask is only 64 bits and this runs a
// handful of times per frame, so a plain loop costs nothing worth optimising.
inline unsigned BonesUsedCount(unsigned long long m) {
    unsigned n = 0;
    while (m) { n += static_cast<unsigned>(m & 1ull); m >>= 1; }
    return n;
}
// A short history of palette uploads: which REGISTERS each call wrote and when. The draw index
// alone cannot tell "the car refreshed five of its own bones" from "another object clobbered five
// of the car's bones" - the register ranges can, because one object's upload is a contiguous
// block starting at c52 and a foreign write lands wherever that object's own bones live.
struct PaletteWrite { unsigned draw, frame, startReg, endReg; };
PaletteWrite g_paletteHistory[24] = {};
unsigned g_paletteHistoryPos = 0;

// The last matrix actually written to the device, with a validity flag each. The flag is NOT
// optional: identity is a legitimate and very common world matrix (every shader that does not
// take objTM gets one), so using identity as an "unknown" sentinel makes the first such draw
// after an invalidation skip its own SetTransform and silently inherit whatever the device was
// last left holding.
D3DMATRIX g_appliedWorld{}, g_appliedView{}, g_appliedProj{};
bool g_haveAppliedWorld = false, g_haveAppliedView = false, g_haveAppliedProj = false;
bool g_ffpActive = false, g_lightingSetUp = false;

// Cached camera, for logging and for placing injected lights. g_cameraCaptured resets each
// frame (it gates the once-per-frame capture); g_haveCamera does not, so a light volume drawn
// before the frame's first converted draw still has a view matrix to be placed against.
D3DMATRIX g_cameraView = kIdentity;
bool g_cameraCaptured = false, g_haveCamera = false;
float g_camX = 0, g_camY = 0, g_camZ = 0;
float g_loggedProj11 = 0, g_loggedProj33 = 0;

// Distinct perspective view matrices seen within one frame.
constexpr unsigned kMaxFrameCameras = 16;
D3DMATRIX g_frameCameras[kMaxFrameCameras];
unsigned g_frameCameraCount = 0, g_maxFrameCameras = 0;

DWORD g_createTick = 0;
unsigned g_frames = 0;

// Statistics.
unsigned g_drawsTotal = 0, g_ffpConverted = 0, g_albedoBlanked = 0, g_albedoMoved = 0;
unsigned g_skipNotEligible = 0, g_skipSkinned = 0, g_skipScreenSpace = 0, g_skipOrtho = 0;
unsigned g_skipNoVP = 0, g_skipVertexFormat = 0;
unsigned g_outlierDraws = 0;   // converted draws whose vertices land absurdly far away
// How often a shader declared objTM but we had not seen the upload for this draw setup. Large
// numbers here mean the engine uploads the matrix before binding the shader, which is exactly
// the case the old gate discarded.
unsigned g_objTMWithoutFreshUpload = 0;
unsigned g_worldMatrixChanges = 0;
unsigned g_skipNoObjTM = 0;
unsigned g_skipInstanced = 0;
unsigned g_skipMirrored = 0;
unsigned g_skipUntextured = 0;
unsigned g_skipStipplePrepass = 0;   // the second prepass shape: stipple sampler, no colour map
unsigned g_screenSpaceMarked = 0;    // post/composite quads sampling a render target
unsigned g_compositeToTexture = 0;   // ...of those, ones rendering into an OFF-SCREEN texture
unsigned g_constantAlbedo = 0;
unsigned g_constantAlbedoRejected = 0;
unsigned g_deinstancedDraws = 0;   // converted draws whose stream frequency was reset   // constant was an exposure factor, not a colour       // materials coloured from a shader constant, not a texture
unsigned g_skinRefuseReports = 0;
char g_skinRefuseNames[8][64] = {};
unsigned g_albedoStage0Raw = 0;      // stage 0 taken raw with no shader reflection to guide it
// Duplicate-draw detection.
//
// The first version of this keyed on the VERTEX range only (buffer, offset, stride, minIndex,
// numVertices) and reported 69 "repeats" of 88 skinned draws. That number was wrong. SR3 draws
// one character mesh as many material sub-ranges over the SAME vertex range - the frame dump has
// v=7977 with p=3408, p=427, p=677 and so on - so every legitimate sub-range counted as a repeat
// of the first. A measurement that cannot tell "the other half of the same mesh" from "the same
// triangles again" cannot support a fix.
//
// The key now includes the INDEX range, so a hit means the identical triangles were already
// submitted this frame. That is a genuine coincident duplicate, and for a path tracer it is a
// second surface in the same place rather than the blended layer a rasteriser resolves.
// Sized for EVERY converted draw, not just the skinned ones: ~1,000 a frame once dedupAll is on.
// At 512 the table filled part way through the frame and every later duplicate went unnoticed,
// silently, with the counter still reporting successes.
// 2048 was sized for ~1,000 converted draws a frame. Measured 2026-08-28 in a heavier scene:
// `13.7/frame did not fit`, i.e. the table filled part way through every frame and every
// duplicate after that went unnoticed - with the counter above still reporting successes.
// The falsifier written into that line is what caught it.
constexpr unsigned kMaxSkinFrameKeys = 8192;
unsigned long long g_skinFrameKeys[kMaxSkinFrameKeys] = {};
unsigned g_skinFrameKeyCount = 0;
unsigned g_skinRepeatDraws = 0;      // identical triangles already submitted this frame
// Draws whose key did not fit the table. Non-zero means duplicates went unnoticed while the
// counter above still reported successes - the silent failure this table has to be able to report.
unsigned g_dedupKeyOverflow = 0;
// Instanced draws whose placement could not be read, so no dedup verdict was possible.
unsigned g_dedupUnjudged = 0;
// Meshes decoded WITH their morph delta applied, and morph streams that could not be read.
unsigned g_morphDecodes = 0, g_morphUnread = 0, g_morphDraws = 0;
// WHY a morph read failed. One counter for five different causes told me only that it failed,
// which is the same mistake as measuring "draws hidden" without measuring which ones.
unsigned g_morphNotSnooped = 0, g_morphStaleVerts = 0, g_morphAppliedDraws = 0;
unsigned g_boneRangeReports = 0;
// MEASURE FIRST. A rule that hides the deferred G-buffer pass is the third attempt at removing
// SR3's duplicate geometry, and the first two shipped without asking how many draws they would
// take with them. This counts the population and changes nothing.
unsigned g_mrtConverted = 0, g_mrtWouldHide = 0, g_singleTargetConverted = 0;
unsigned g_mrtSamplerReports = 0;
unsigned g_gbufferHidden = 0;
char g_mrtSamplerNames[10][28] = {};
unsigned g_morphNoStride = 0, g_morphNoDesc = 0, g_morphDynamic = 0,
         g_morphOutOfRange = 0, g_morphLockFail = 0, g_morphWhyReports = 0;
unsigned g_skinRepeatHidden = 0;     // ...and dropped because of it
unsigned g_tintedAlbedo = 0;         // fallback maps modulated by the shader's tint constant
unsigned g_blankAlbedo = 0;          // rank 0 AND no colour constant: the only truly white draws
unsigned g_hudDemoted = 0;           // authored-texture screen-space quads given an ortho projection
unsigned g_skipNoColourPass = 0;     // hidden: no colour map, no constant, no L-buffer read
unsigned g_skipScreenSpacePass = 0;  // hidden: samples the G-buffer normals (decals, AO, lights)
unsigned g_skipParticlePass = 0;     // hidden: rl_particle_* billboards built in the vertex shader
unsigned g_albedoNullRanked = 0;     // shader NAMES a colour map but nothing is bound there
unsigned g_albedoNullAfterRT = 0;    // RT exclusion left no usable texture at all
unsigned g_nullRankedReports = 0;
char g_nullRankedNames[12][28] = {};
unsigned g_skyDraws = 0;             // rfg-skybox draws, passed through so Remix can capture them
bool g_isHudDraw = false;            // set by Classify; read by the HUD probe below
bool g_uiRenderTarget = false;       // the current target is the engine's 8-bit UI surface
unsigned g_uiConverted = 0;
unsigned g_demotedToUI = 0;
// A 4x4 texture we create ourselves, bound to stage 0 for draws we need Remix to drop. It never
// reaches the screen: only shaders that sample nothing are ever marked, so the game cannot read
// it. It exists purely to give Remix's texture-hash categorisation a handle that belongs to us.
//
// Opaque magenta, with the low bits of green ramping 0-15 so the 64 bytes are unique and the
// hash cannot collide with a real 4x4 asset. Magenta is deliberate: if the marked pass ever
// renders, the duplicate world turns bright magenta and identifies itself on sight.
IDirect3DTexture9* g_marker = nullptr;
unsigned g_markedDraws = 0;    // draws given the marker texture, so Remix can be told to drop them
unsigned g_markRefused = 0;    // hidden draws whose shader DOES sample - marking would be visible
// SetTexture calls issued by marking. These deliberately bypass the state shadow, so they are
// real bridge round trips and the one number that says what marking costs. Measured, not guessed:
// the first marker build cost 6.85 ms/frame and it was not obvious how much of that was this.
unsigned g_markSetTextures = 0;
unsigned g_skippedDraws = 0;
unsigned g_skipNoAlbedo = 0;
UINT g_maxInstanceCount = 0, g_instanceTotal = 0;
// Handedness of the frame's first camera, used as the reference for spotting mirrored passes.
bool g_baseHandedness = true, g_haveBaseHandedness = false;

// The frame's MAIN camera - the one the player is actually looking through. Identified by a
// perspective projection whose aspect matches the back buffer; shadow, reflection, cubemap and
// dual-paraboloid passes generally do not match it.
//
// This matters twice over, and the second reason is the important one:
//
//  1. Geometry rendered from another viewpoint is a different pass, not the visible scene, so
//     converting it puts a second copy of the world into the path-traced result.
//
//  2. Only a small fraction of draws are converted; the rest stay shader-driven and Remix
//     reconstructs them through vertex capture using whatever D3DTS_VIEW/PROJECTION happen to
//     be set. We leave behind the last converted draw's matrices - so with several cameras per
//     frame, Remix rebuilds the entire remaining world against an arbitrary one of them. That
//     is a duplicate world at the wrong orientation. Restricting conversion to the main camera
//     means the matrices we leave set ARE the main camera, which is exactly what vertex capture
//     needs, so the leftover state becomes correct rather than arbitrary.
D3DMATRIX g_mainView{}, g_mainProj{};
bool g_haveMainCamera = false;
float g_backAspect = 0.0f;
UINT g_backBufferW = 0, g_backBufferH = 0;   // the screen's own size, for the composite test
unsigned g_skipOtherCamera = 0;

// Event trace. Three builds in a row have guessed at when the engine uploads objTM relative to
// binding a shader and issuing a draw, and each guess produced a different wrong placement.
// Rather than guess a fourth time, record the actual call order and read the protocol off it.
// Captured once, in gameplay, then dumped as a single compact string.
//   V = SetVertexShader (uppercase: the shader declares objTM)   v = one that does not
//   P = projTM (c28) upload    O = objTM (c32) upload    W = view (c48) upload
//   D = draw, converted        x = draw, not converted
constexpr unsigned kTraceMax = 400;
char g_trace[kTraceMax + 1] = {};
unsigned g_traceLen = 0;
bool g_traceDumped = false;

inline void Trace(char c) {
    if (g_traceLen < kTraceMax && g_haveCamera && !g_traceDumped) g_trace[g_traceLen++] = c;
}
// One D3D9 light slot per light, so Remix can match a light to its previous-frame self.
unsigned g_maxLightSlots = 8;        // set from D3DCAPS9::MaxActiveLights at device creation
unsigned g_lightSlot = 0;
unsigned g_lightSlotsUsedLastFrame = 0;
unsigned g_lightOverflow = 0;        // lights past the device limit, forced back onto slot 0
unsigned g_lightsEmitted = 0, g_lightShadersSeen = 0;
unsigned g_shadowSkipped = 0, g_transformWrites = 0;

// ---------------------------------------------------------------- state shadow
//
// Every D3D9 call crosses a 32->64-bit process boundary through the Remix bridge, and each
// crossing is IPC. The SR2 proxy's own profiling found this to be its single biggest
// bottleneck: ~28 state changes per draw, thousands of round-trips per frame spent re-asserting
// state that had not changed, with the GPU sitting at 37%. SR3 submits roughly three times as
// many draws per frame as SR2, so the same mistake here would be far worse. Redundant Set*
// calls are dropped locally instead.
// Signed on purpose: these are compared against D3D9's state enums, which are signed, and an
// unsigned bound would make every range check a signed/unsigned comparison.
constexpr int kRsMax = 256;
constexpr int kTssMax = 33;
constexpr int kSsMax = 14;
DWORD g_rsShadow[kRsMax] = {};      bool g_rsKnown[kRsMax] = {};
DWORD g_tssShadow[8][kTssMax] = {}; bool g_tssKnown[8][kTssMax] = {};
DWORD g_ssShadow[16][kSsMax] = {};  bool g_ssKnown[16][kSsMax] = {};

// Seed the render states we READ with D3D9's documented defaults, so a decision is never made
// from a failed query.
//
// ShadowGetRS falls back to GetRenderState and returns 0 if that fails - and D3D9 devices
// created with D3DCREATE_PUREDEVICE fail every Get* call by design. A spurious 0 from
// COLORWRITEENABLE would classify every draw in the game as the depth prepass and skip the
// entire world. The game sets these through SetRenderState, which we hook, so the seeded values
// are corrected the moment it does; seeding only removes the dependency on Get* succeeding.
void SeedRenderStateDefaults() {
    struct { D3DRENDERSTATETYPE s; DWORD v; } kDefaults[] = {
        {D3DRS_ZENABLE, TRUE},           {D3DRS_ZWRITEENABLE, TRUE},
        {D3DRS_ZFUNC, D3DCMP_LESSEQUAL}, {D3DRS_COLORWRITEENABLE, 0xF},
        {D3DRS_ALPHATESTENABLE, FALSE},  {D3DRS_ALPHAREF, 0},
        {D3DRS_ALPHABLENDENABLE, FALSE}, {D3DRS_SRCBLEND, D3DBLEND_ONE},
        {D3DRS_DESTBLEND, D3DBLEND_ZERO},{D3DRS_CULLMODE, D3DCULL_CCW},
        {D3DRS_FOGENABLE, FALSE},        {D3DRS_TEXTUREFACTOR, 0xFFFFFFFF},
    };
    for (const auto& d : kDefaults) {
        if (d.s < kRsMax) { g_rsShadow[d.s] = d.v; g_rsKnown[d.s] = true; }
    }
}

// State blocks apply device state without passing through our hooks, so the shadow cannot see
// those writes and is dropped every frame - then immediately re-seeded, because "unknown" must
// never mean "zero" for the states we make decisions from.
void ShadowInvalidate() {
    memset(g_rsKnown, 0, sizeof(g_rsKnown));
    memset(g_tssKnown, 0, sizeof(g_tssKnown));
    memset(g_ssKnown, 0, sizeof(g_ssKnown));
    SeedRenderStateDefaults();
}

HRESULT ShadowSetRS(IDirect3DDevice9* dev, D3DRENDERSTATETYPE s, DWORD v) {
    if (s < kRsMax) {
        if (g_rsKnown[s] && g_rsShadow[s] == v) { ++g_shadowSkipped; return D3D_OK; }
        g_rsShadow[s] = v;
        g_rsKnown[s] = true;
    }
    return g_origSetRenderState(dev, s, v);
}

DWORD ShadowGetRS(IDirect3DDevice9* dev, D3DRENDERSTATETYPE s) {
    if (s < kRsMax && g_rsKnown[s]) return g_rsShadow[s];
    DWORD v = 0;
    if (FAILED(g_origGetRenderState(dev, s, &v))) return 0;
    if (s < kRsMax) { g_rsShadow[s] = v; g_rsKnown[s] = true; }
    return v;
}

HRESULT ShadowSetTSS(IDirect3DDevice9* dev, DWORD stage, D3DTEXTURESTAGESTATETYPE t, DWORD v) {
    if (stage < 8 && t < kTssMax) {
        if (g_tssKnown[stage][t] && g_tssShadow[stage][t] == v) { ++g_shadowSkipped; return D3D_OK; }
        g_tssShadow[stage][t] = v;
        g_tssKnown[stage][t] = true;
    }
    return g_origSetTextureStageState(dev, stage, t, v);
}

HRESULT ShadowSetSS(IDirect3DDevice9* dev, DWORD sampler, D3DSAMPLERSTATETYPE t, DWORD v) {
    if (sampler < 16 && t < kSsMax) {
        if (g_ssKnown[sampler][t] && g_ssShadow[sampler][t] == v) { ++g_shadowSkipped; return D3D_OK; }
        g_ssShadow[sampler][t] = v;
        g_ssKnown[sampler][t] = true;
    }
    return g_origSetSamplerState(dev, sampler, t, v);
}

// ---------------------------------------------------------------- vtable patching

void NoteAtlasPtr(void* p);
void InstallTextureHooks(IDirect3DTexture9* tex);

bool PatchVTable(void* instance, int slot, void* hook, void** original) {
    void** vtable = *reinterpret_cast<void***>(instance);
    if (original) *original = vtable[slot];
    if (!hook) return true;
    DWORD old = 0;
    if (!VirtualProtect(&vtable[slot], sizeof(void*), PAGE_EXECUTE_READWRITE, &old))
        return false;
    vtable[slot] = hook;
    VirtualProtect(&vtable[slot], sizeof(void*), old, &old);
    return true;
}

// ---------------------------------------------------------------- transforms
//
// SR3 hands us VIEW directly at c48, so unlike SR2 there is nothing to decompose: the
// projection is just inverse(VIEW) * projTM. What does carry over from SR2 is the far-plane
// substitution below, which cost that project a great deal of confusion before it was found.

bool DeriveProjection(const D3DMATRIX& view, const D3DMATRIX& viewProj, D3DMATRIX& out) {
    D3DMATRIX invView;
    if (!Invert(view, invView)) return false;
    out = Multiply(invView, viewProj);   // projTM = V * P  =>  P = inverse(V) * projTM
    if (!IsFinite(out)) return false;

    // A D3D perspective projection (row-vector, left-handed) has _34 = 1 and _44 = 0, with
    // _33 = Q = far/(far-near) and _43 = -Q*near. An orthographic one has _44 = 1.
    const bool perspective = std::fabs(out._34 - 1.0f) < 0.01f && std::fabs(out._44) < 0.01f;
    if (!perspective) return true;       // still geometrically correct; caller decides

    // SR2 uses an INFINITE far plane, which drives Q to exactly 1, and Remix's CameraManager
    // rejects a projection it cannot resolve into a valid (near, far) pair. SR3 does NOT:
    // measured 2026-08-16, Q = 1.00003 with Qn = 0.15, which is exactly near=0.15 far=5000.
    // NOT DONE FOR SR3, and the measurement is the reason. SR2 ships an INFINITE far plane
    // (Q == 1) which Remix cannot resolve into a valid (near, far) pair, so that project has to
    // substitute one. SR3 does not: Q = 1.00003 with Qn = 0.15 is exactly near=0.15 far=5000, an
    // ordinary finite projection. An earlier version of this code used a 0.999 threshold, matched
    // that perfectly good projection, and "substituted" a numerically identical matrix 1,500
    // times a frame while reporting it as a fix. The corrected check then measured 0 across every
    // run, so the whole path is gone rather than left as a switch nobody varies.
    return true;
}

bool IsPerspective(const D3DMATRIX& p) {
    return std::fabs(p._34 - 1.0f) < 0.01f && std::fabs(p._44) < 0.01f;
}

// A reflection pass renders the world mirrored about the water plane, and mirroring flips
// handedness - so its view matrix has a NEGATIVE determinant while an ordinary camera's is
// positive. That makes reflection passes identifiable from the matrix alone, with no reliance
// on shader names or render-target indices. 505 shaders carry Reflection_Plane_Height, and the
// mirrored duplicate of the city hanging below the world is what converting them looks like.
float RotationDeterminant(const D3DMATRIX& m) {
    return m._11 * (m._22 * m._33 - m._23 * m._32)
         - m._12 * (m._21 * m._33 - m._23 * m._31)
         + m._13 * (m._21 * m._32 - m._22 * m._31);
}

// Every IDirect3DVertexBuffer9 shares one vtable, so patching it from the first buffer we see
// installs this for all of them. Slot 11 is Lock. We do not intercept the data - only note that
// the buffer is about to change, so any snapshot of it is dropped.
// IDirect3DTexture9: 0-2 IUnknown, 3-10 IDirect3DResource9, 11-16 IDirect3DBaseTexture9,
// then 17 GetLevelDesc, 18 GetSurfaceLevel, 19 LockRect, 20 UnlockRect, 21 AddDirtyRect.
// IDirect3DSurface9: 0-2 IUnknown, 3-10 IDirect3DResource9, then 11 GetContainer, 12 GetDesc,
// 13 LockRect, 14 UnlockRect. A SURFACE lock is a different vtable from a TEXTURE lock, and a
// game that does GetSurfaceLevel(0) then locks that surface never touches
// IDirect3DTexture9::LockRect at all - which is the gap the first fill probe left.
constexpr int kSlotSurfLockRect = 13;
constexpr int kSlotSurfUnlockRect = 14;

constexpr int kSlotTexLockRect = 19;
constexpr int kSlotTexUnlockRect = 20;
// IDirect3DDevice9, same numbering the device slots above are calibrated against
// (CreateTexture 23, SetRenderTarget 37): 30 UpdateSurface, 31 UpdateTexture, 34 StretchRect,
// 35 ColorFill.
constexpr int kSlotUpdateSurface = 30;
constexpr int kSlotUpdateTexture = 31;
constexpr int kSlotStretchRect = 34;
constexpr int kSlotColorFill = 35;

constexpr int kSlotVBLock = 11;
constexpr int kSlotVBUnlock = 12;
typedef HRESULT(WINAPI* VBLock_t)(IDirect3DVertexBuffer9*, UINT, UINT, void**, DWORD);
typedef HRESULT(WINAPI* VBUnlock_t)(IDirect3DVertexBuffer9*);
VBLock_t g_origVBLock = nullptr;
VBUnlock_t g_origVBUnlock = nullptr;

// The region the game currently has mapped for writing, so its contents can be copied out at
// Unlock. Only one buffer is tracked: SR3 does not nest locks on the instance stream.
struct PendingLock {
    IDirect3DVertexBuffer9* vb = nullptr;
    void* ptr = nullptr;
    UINT offset = 0, size = 0;
};
PendingLock g_pendingLock;

// Snoop the game's own write instead of locking the buffer ourselves.
//
// The previous version read the instance data by calling Lock(0, wholeBuffer, READONLY) once per
// frame. That is a serious mistake on a DYNAMIC buffer: a read lock forces the driver to
// synchronise with the GPU, and every one of those calls also crosses the 32->64-bit bridge. The
// result presented as the image "freezing", which cleared when the camera pointed at the sky -
// i.e. a frame-rate collapse that scaled with how much instanced geometry was in view, not a
// freeze at all.
//
// The game has to lock the buffer to fill it, so the data passes through this hook anyway.
// Copying it here costs one memcpy of the region actually written and never stalls the GPU.
// Defined with the uv conversion, further down.
void InvalidateUvBuffers(IDirect3DVertexBuffer9* vb);
void InvalidateBaseMeshes(IDirect3DVertexBuffer9* vb);

// The game locks vertex buffers from MORE THAN ONE THREAD.
//
// That is not an assumption. Crash dump 2026-08-28 00:59 faulted on memcpy(dst, NULL, 4176) in
// Hook_VBUnlock, and the only way its source pointer can be null after the null test on the line
// above it is another thread running `g_pendingLock = {}` in between. Single-threaded, the value
// cannot change there.
//
// Everything that follows from that had been running unguarded since the snoop was written:
// Hook_VBLock erases entries from g_baseMeshes and calls Release() on the buffers they pinned,
// while the render thread is holding a `const BaseMesh*` into that same map for the length of a
// skinning loop. An erase from the other thread frees the vertices out from under it. This is a
// far better candidate for the crashes recorded before ANY of this session's changes (2026-08-25,
// four dumps) than anything specific that shipped afterwards.
//
// So: nothing that a foreign thread touches may mutate a container the render thread reads. Lock
// requests are queued here and drained on the render thread at the top of the next draw.
CRITICAL_SECTION g_snoopCs;
bool g_snoopCsReady = false;
std::vector<IDirect3DVertexBuffer9*> g_pendingInvalidations;
unsigned g_deferredInvalidations = 0, g_invalidationsCoalesced = 0;
// Total bytes held by the snoop, and its ceiling. This is a 32-bit process sharing an address
// space with the game, Remix's client and a 24 MB skinning ring; the snoop does not get to
// grow without a limit just because a buffer is large.
constexpr size_t kMaxSnoopBytes = 96u << 20;
size_t g_snoopBytes = 0;
unsigned g_snoopBudgetRefusals = 0, g_snoopAllocFailures = 0;

struct SnoopGuard {
    SnoopGuard()  { if (g_snoopCsReady) EnterCriticalSection(&g_snoopCs); }
    ~SnoopGuard() { if (g_snoopCsReady) LeaveCriticalSection(&g_snoopCs); }
};

// Render thread only. Called at the top of every draw, before anything takes a pointer into the
// caches those invalidations would erase from.
void DrainPendingInvalidations() {
    if (!g_snoopCsReady) return;
    std::vector<IDirect3DVertexBuffer9*> todo;
    {
        SnoopGuard g;
        if (g_pendingInvalidations.empty()) return;
        todo.swap(g_pendingInvalidations);
    }
    for (IDirect3DVertexBuffer9* vb : todo) {
        InvalidateUvBuffers(vb);
        InvalidateBaseMeshes(vb);
        ++g_deferredInvalidations;
    }
}
// Defined with the skinning, further down - every read-lock of a game vertex buffer uses it.
bool VertexRangeFits(IDirect3DVertexBuffer9* vb, UINT streamOffset, UINT firstVertex,
                     UINT count, UINT stride);

HRESULT WINAPI Hook_VBLock(IDirect3DVertexBuffer9* self, UINT offset, UINT size, void** data,
                           DWORD flags) {
    const HRESULT hr = g_origVBLock(self, offset, size, data, flags);
    if (g_internal || FAILED(hr) || !data || !*data) return hr;
    if (flags & D3DLOCK_READONLY) return hr;   // the game is reading; nothing new to capture

    SnoopGuard guard;   // everything below touches state the render thread also reads

    // A discard invalidates everything we hold for this buffer, even the bytes it does not go on
    // to rewrite. Recorded rather than assumed away.
    if (flags & D3DLOCK_DISCARD) {
        const auto di = g_instCache.find(self);
        if (di != g_instCache.end()) {
            std::fill(di->second.fresh.begin(), di->second.fresh.end(), 0);
            ++g_instDiscards;
        }
    }

    // The converted texture coordinates for this buffer describe what it held a moment ago. The
    // buffer being refilled is exactly the case a reference on it cannot protect against, and
    // this is the only place that sees it happen.
    // The bind-pose cache has the same problem. Its key is {vb, offset, stride, minIndex, count},
    // which names a SLOT in a buffer, not a mesh - so once the game refills that slot with a
    // different character's vertices, every later draw from it is served the previous occupant's
    // geometry. That is "each character had the wrong head".
    //
    // QUEUED, not done here: both invalidations erase from containers the render thread reads,
    // and this hook does not run on the render thread. Doing it inline frees a BaseMesh while a
    // skinning loop is walking it.
    //
    // DEDUPED. Measured over 26,400 frames: 7,185,052 queued - 272 a frame - and
    // `bind-pose cache invalidated by a game write: 0 times`, so every one of them found nothing.
    // The game relocks a handful of dynamic buffers constantly; queuing the same pointer twice
    // between drains asks for the same lookup twice. The scan is over a list that is a few
    // entries long precisely because of this.
    {
        bool queued = false;
        for (IDirect3DVertexBuffer9* q : g_pendingInvalidations)
            if (q == self) { queued = true; break; }
        if (!queued) g_pendingInvalidations.push_back(self);
        else ++g_invalidationsCoalesced;
    }

    // Only buffers we have already identified as instance streams are worth snooping.
    const auto it = g_instCache.find(self);
    if (it == g_instCache.end()) return hr;

    UINT span = size;
    if (span == 0) {                    // size 0 means "to the end of the buffer"
        D3DVERTEXBUFFER_DESC d{};
        if (FAILED(self->GetDesc(&d))) return hr;
        span = (d.Size > offset) ? (d.Size - offset) : 0;
    }
    g_pendingLock = {self, *data, offset, span};
    return hr;
}

HRESULT WINAPI Hook_VBUnlock(IDirect3DVertexBuffer9* self) {
    // SNAPSHOT the pending lock and clear it BEFORE touching anything else.
    //
    // The previous version tested g_pendingLock.ptr, then called vector::resize, then passed
    // g_pendingLock.ptr to memcpy. resize is opaque to the optimiser, so the pointer is RE-READ
    // from the global after the test - and this hook is not called from one thread. Crash dump
    // 2026-08-28 00:59: ACCESS_VIOLATION at CopyUpLargeMov+0xa with esi=0, i.e.
    // memcpy(dst, NULL, 4176). 4176 is 348 vertices x the 12-byte morph stride.
    //
    // The window had always been there and never mattered: only instance streams were snooped,
    // and the game fills those on the render thread. Registering the morph buffer put a stream
    // the game writes elsewhere through the same single global.
    //
    // Locals close it completely - nothing is read from the global after the null test.
    SnoopGuard guard;
    PendingLock pl;
    if (!g_internal) {
        pl = g_pendingLock;
        if (pl.vb == self && pl.ptr) g_pendingLock = {};
    }
    if (!g_internal && pl.vb == self && pl.ptr && pl.size) {
        InstanceCache& c = g_instCache[self];
        // Grows to what the game actually writes, which is a fraction of most buffers'
        // declared size. Safe to resize here because nothing outside this lock holds a pointer
        // into it any more - readers copy out (SnoopCopy) rather than borrowing.
        //
        // Capped in total, and allocation failure is CAUGHT. The 2026-08-28 build that reserved
        // every buffer's declared size up front exhausted a 32-bit address space, and the
        // bad_alloc that came out of it was unhandled: "This application has requested the
        // Runtime to terminate it in an unusual way" is std::terminate, not a segfault. A shim
        // must degrade rather than abort the process it is a guest in.
        const UINT end = pl.offset + pl.size;
        bool grew = true;
        if (c.data.size() < end) {
            const size_t add = end - c.data.size();
            if (g_snoopBytes + add * 2 > kMaxSnoopBytes) {
                ++g_snoopBudgetRefusals;
                grew = false;
            } else {
                try {
                    c.data.resize(end);
                    c.fresh.resize(end, 0);
                    g_snoopBytes += add * 2;
                } catch (const std::bad_alloc&) {
                    ++g_snoopAllocFailures;
                    grew = false;
                }
            }
        }
        if (grew && c.data.size() >= end) {
            memcpy(c.data.data() + pl.offset, pl.ptr, pl.size);
            std::fill(c.fresh.begin() + pl.offset, c.fresh.begin() + end, 1);
        }
        c.valid = true;
        c.frame = g_frames;
        ++g_instanceLocks;
    }
    return g_origVBUnlock(self);
}

// Whole-buffer snapshot of an instance stream, refreshed once per frame.
// Pure cache lookup - this NEVER locks. Creating the map entry is what registers the buffer as
// an instance stream so Hook_VBLock/Hook_VBUnlock start snooping the game's own writes to it;
// the first draw that touches a new buffer therefore finds nothing and is not converted, and
// every draw after the game next fills it is served from the snooped copy.
// Register a buffer for snooping. Creating the map entry is what makes Hook_VBLock/Hook_VBUnlock
// start copying the game's own writes to it; the first draw that touches a new buffer therefore
// finds nothing, and every draw after the game next fills it is served from the snooped copy.
//
// This used to RETURN a pointer into the cache. It no longer does, and must not again: the
// locking thread resizes those vectors, so any pointer handed out here dangles the moment it
// does. Readers use SnoopCopy.
void RegisterSnoop(IDirect3DVertexBuffer9* vb) {
    if (!vb) return;
    SnoopGuard guard;
    const size_t before = g_instCache.size();
    g_instCache[vb];
    if (g_instCache.size() != before) vb->AddRef();
}

// Copy a range out of a snooped buffer, under the lock, reporting whether every byte of it had
// actually been written by the game since the last discard.
//
// This replaces handing callers a raw pointer into the cache. A caller keeps what it reads for
// the length of a draw, and the locking thread can resize that vector at any moment - so the
// pointer was a use-after-free waiting for the timing to line up. Copying costs the bytes a draw
// actually consumes (a head is 1,426 vertices x 12) and nothing else.
bool SnoopCopy(IDirect3DVertexBuffer9* vb, UINT offset, unsigned char* dst, UINT len,
               bool* allFresh) {
    if (!vb || !dst || !len) return false;
    SnoopGuard guard;
    const auto it = g_instCache.find(vb);
    if (it == g_instCache.end()) return false;
    const InstanceCache& c = it->second;
    if (!c.valid || offset + static_cast<size_t>(len) > c.data.size()) return false;
    memcpy(dst, c.data.data() + offset, len);
    if (allFresh) {
        *allFresh = true;
        for (size_t b = offset; b < offset + len; ++b)
            if (b >= c.fresh.size() || !c.fresh[b]) { *allFresh = false; break; }
    }
    return true;
}

// World matrix of an instanced draw, read from its instance stream. Instance count is always 1
// in SR3, so the record sits at the stream's base offset.
bool InstanceWorld(D3DMATRIX& world) {
    if (!g_curLayout.hasInstanceTransform) return false;
    const int s = g_curLayout.instStream;
    // Registering is what starts the snoop. This used to be a side effect of the accessor that
    // handed out a pointer; removing that accessor without this line would have silently stopped
    // ~1,400 instanced draws a frame from converting, with nothing failing loudly.
    RegisterSnoop(g_streamVB[s]);
    const UINT rec = g_streamOffset[s];
    float rows[12];
    bool allFresh = true;
    for (int r = 0; r < 3; ++r) {
        const UINT off = rec + static_cast<UINT>(g_curLayout.instRowOffset[r]);
        bool rowFresh = false;
        if (!SnoopCopy(g_streamVB[s], off, reinterpret_cast<unsigned char*>(&rows[r * 4]), 16,
                       &rowFresh))
            return false;
        if (!rowFresh) allFresh = false;
    }
    if (allFresh) ++g_instFreshTransform;
    else {
        ++g_instStaleTransform;
        if (g_instStaleReports < 6) {
            ++g_instStaleReports;
            Log("INSTANCE TRANSFORM STALE #%u: stream %d offset %u - these bytes were not written "
                "since the last discard. translation (%.1f %.1f %.1f)",
                g_instStaleReports, s, rec, rows[3], rows[7], rows[11]);
        }
    }
    world = FromRegisters(rows, 3);   // same row-major 3x4 convention as objTM
    return IsFinite(world);
}

// Build this draw's transforms. Returns false if the draw cannot be represented.
bool ComputeTransforms(D3DMATRIX& world, D3DMATRIX& view, D3DMATRIX& proj) {
    view = FromRegisters(&g_vsConst[kRegWorld2View][0], 3);
    const D3DMATRIX viewProj = FromRegisters(&g_vsConst[kRegProjTM][0], 4);
    if (!IsFinite(view) || !IsFinite(viewProj)) return false;

    // REVERTED 2026-08-18, and the measurement that killed it is worth keeping.
    //
    // The theory was that the UI pass never refreshes IR_World2View, so decomposing its projTM
    // against a stale c48 recovered the world's projection and planted the HUD in the world.
    // The first half is true. The conclusion was not: substituting view = identity and
    // projection = projTM did NOT put the UI on screen, and made it disappear at some camera
    // angles instead.
    //
    // The reason is in the log. The UI target's raw projTM is
    //     row0 (-0.5863 0.4811 0.9159 0.9159)  row3 (100.7555 -456.2618 -46.0588 -45.9083)
    // - a rotated basis with a large translation. That is neither a perspective projection nor
    // an orthographic one, so `rtx.orthographicIsUI` has nothing to latch onto and the draw is
    // just world geometry with an unusual transform. `perspective=0` in that log line means
    // "failed the perspective test", NOT "is orthographic", and reading it as the latter is what
    // made this look like a fix.
    //
    // So the 8-bit full-size target is not simply "the HUD". Whatever it is, it carries a full
    // world transform, and the UI problem needs a different model than "one pass, one
    // projection". Left as it was.
    if (!DeriveProjection(view, viewProj, proj)) return false;

    // Does this draw have an object matrix? The shader's own constant table is the authority:
    // if it declares objTM then the engine MUST have uploaded a valid one before the draw, or
    // the game's own rendering would be wrong too. If it does not declare objTM the geometry is
    // already in world space and identity is correct.
    //
    // This deliberately does NOT also require that we witnessed the c32 upload. That extra
    // condition was ported from SR2, whose comment explains it as "new VS may have different
    // constant layout" - true there, false here: SR3's register layout is fixed and documented
    // (docs/shader-map.md). Carrying it over meant that whenever the engine uploaded objTM
    // *before* binding the shader, the matrix was discarded and the object fell back to
    // identity. Measured effect: 1,828 converted draws sharing ~50 transform writes, i.e.
    // nearly all of the world drawn at one place.
    // ...but "the engine must have uploaded one" is only true for the draw it uploaded it FOR.
    // Dropping the freshness test entirely (previous build) meant a shader that declares objTM
    // without uploading inherited whatever was left in c32 - very often the player character's
    // matrix, which is why objects appeared at the character's origin and popped as that matrix
    // changed. c32 stays valid across the several draws of one multi-material object and is
    // invalidated at the next projTM upload following a draw, which is where a new setup starts.
    //
    // When a shader wants an object matrix and no fresh one exists we now REFUSE the draw
    // rather than substitute identity or a stale matrix. Drawing it in the wrong place is worse
    // than leaving it to the game: a skipped draw is merely not path-traced.
    // An instanced draw carries its transform in the instance stream instead of objTM, so that
    // takes precedence: for these the c32 registers are stale or irrelevant.
    if (g_instancedDraw) {
        if (!InstanceWorld(world)) return false;
        ++g_instanceConverted;
        return true;
    }

    if (g_curVS.usesObjTM) {
        if (!g_worldWritten) { ++g_objTMWithoutFreshUpload; return false; }
        world = FromRegisters(&g_vsConst[kRegObjTM][0], 3);
    } else {
        world = kIdentity;
    }
    return IsFinite(world);
}

void ApplyTransforms(IDirect3DDevice9* dev, const D3DMATRIX& world, const D3DMATRIX& view,
                     const D3DMATRIX& proj) {
    if (!g_haveAppliedWorld || !Same(world, g_appliedWorld)) {
        g_origSetTransform(dev, D3DTS_WORLD, &world);
        g_appliedWorld = world;
        g_haveAppliedWorld = true;
        ++g_transformWrites;
        ++g_worldMatrixChanges;
    }
    if (!g_haveAppliedView || !Same(view, g_appliedView)) {
        g_origSetTransform(dev, D3DTS_VIEW, &view);
        g_appliedView = view;
        g_haveAppliedView = true;
        ++g_transformWrites;
    }
    if (!g_haveAppliedProj || !Same(proj, g_appliedProj)) {
        g_origSetTransform(dev, D3DTS_PROJECTION, &proj);
        g_appliedProj = proj;
        g_haveAppliedProj = true;
        ++g_transformWrites;
    }

    // How many DIFFERENT cameras does one frame carry? Every converted draw supplies its own
    // exact view, which is correct per draw but means Remix sees each of them. If the engine
    // renders reflections or auxiliary perspective passes, Remix has to pick between cameras
    // and may pick differently from frame to frame - which looks like flickering. Counting
    // them says whether that is happening before any fix is attempted.
    if (IsPerspective(proj)) {
        bool seen = false;
        for (unsigned i = 0; i < g_frameCameraCount; ++i)
            if (Same(view, g_frameCameras[i])) { seen = true; break; }
        if (!seen && g_frameCameraCount < kMaxFrameCameras)
            g_frameCameras[g_frameCameraCount++] = view;
    }

    if (!g_cameraCaptured && IsPerspective(proj)) {
        D3DMATRIX invView;
        if (Invert(view, invView)) {
            g_cameraView = view;
            g_camX = invView._41; g_camY = invView._42; g_camZ = invView._43;
            g_cameraCaptured = true;
            g_haveCamera = true;   // persists across frames, so lights are never orphaned
            // Log only when the projection materially changes. It was logging every frame,
            // which buried the rest of the file under 2,000 identical lines.
            if (std::fabs(proj._11 - g_loggedProj11) > 0.01f ||
                std::fabs(proj._33 - g_loggedProj33) > 1e-4f) {
                g_loggedProj11 = proj._11;
                g_loggedProj33 = proj._33;
                Log("camera: pos %.1f %.1f %.1f | proj _11=%.4f _22=%.4f _33=%.5f _43=%.2f "
                    "(aspect %.3f)",
                    g_camX, g_camY, g_camZ, proj._11, proj._22, proj._33, proj._43,
                    (std::fabs(proj._11) > 1e-6f) ? proj._22 / proj._11 : 0.0f);
            }
        }
    }
}


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
        cur[i * 4 + 3] = 255;
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
    g_clothCache.clear();
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
unsigned g_clothNoPattern = 0;

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

IDirect3DBaseTexture9* HairAlbedo(IDirect3DDevice9* dev) {
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
    unsigned char pick[4] = {pat[0], pat[1], pat[2], pat[3]};
    bool texelUniform = true;
    for (size_t q = 4; q < pat.size(); q += 4)
        if (pat[q] != pat[0] || pat[q + 1] != pat[1] || pat[q + 2] != pat[2]) {
            texelUniform = false; break;
        }
    if (!texelUniform) {
        // Not uniform as a TEXTURE - but does this garment read more than one texel of it?
        float pu = 0.0f, pv = 0.0f;
        if (!ClothPatternIsSampledAtOnePoint(&pu, &pv)) {
            ++g_clothUniformNotUniform;
            g_clothCache[key] = ClothTex{nullptr, nullptr};
            return nullptr;
        }
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
    if (!TextureToBgra(diffuse, dif, dw, dh) || dif.empty() || !dw || !dh ||
        dw > 2048 || dh > 2048) {
        ++g_clothUniformNoDiffuse;
        g_clothCache[key] = ClothTex{nullptr, nullptr};
        return nullptr;
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
    const unsigned char pR = pick[2], pG = pick[1], pB = pick[0];     // stored BGRA
    float c[3];
    {
        const float fr = pR / 255.0f, fg = pG / 255.0f, fb = pB / 255.0f;
        const float sum = fr + fg + fb;
        const float mean = sum * (1.0f / 3.0f);
        const float dev = fabsf(fr - mean) + fabsf(fg - mean) + fabsf(fb - mean);
        const float test = sum - (dev * 165.016495f + 256.0f) / 255.0f;
        if (test < 0.0f) {
            const float wr = g_gammaLUT[pR], wg = g_gammaLUT[pG], wb = g_gammaLUT[pB];
            for (int k = 0; k < 3; ++k)
                c[k] = wr * col[0][k] + wg * col[1][k] + wb * col[2][k];
        } else {
            const unsigned char pk[3] = {pR, pG, pB};
            for (int k = 0; k < 3; ++k) c[k] = g_desatLUT[pk[k]];
        }
    }
    // ALREADY NORMALISED. The setting is parsed as num("clothBrightness", 100) / 100.0f, so
    // g_settings.clothBrightness is 1.0 for the default of 100 - dividing by 100 again made it
    // 0.01 and multiplied every garment down by a hundred. That is the whole "result mean 0.3 of
    // 255": the colour was right, the diffuse decoded at 204, and this line threw the product
    // away. The CPU baker beside it has always used the value directly.
    // clothTintScale, the same scale the corset is drawn with - NOT clothBrightness, which is
    // the CPU baker's separate knob. Using the wrong one is what left these garments dark even
    // after the double-division was fixed.
    const float bright = g_settings.clothTintScale;

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
    for (size_t i = 0; i < static_cast<size_t>(dw) * dh; ++i) {
        const unsigned char* q = &dif[i * 4];                 // BGRA
        // The diffuse texel is sRGB, so it goes to LINEAR before being multiplied by a colour
        // the pattern path also treats as linear. Multiplying two sRGB values and encoding the
        // product - what this did before - is a different curve, and it is why the same colour
        // that reads correctly on the corset read dull here.
        const unsigned char src[3] = {q[2], q[1], q[0]};      // R, G, B
        unsigned char o[3];
        for (int k = 0; k < 3; ++k) {
            const float v = LinearToSrgb(g_gammaLUT[src[k]] * c[k] * bright);
            const float cl = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
            o[k] = static_cast<unsigned char>(cl * 255.0f + 0.5f);
            acc += o[k];
        }
        pixels[i] = (static_cast<unsigned>(q[3]) << 24) | (static_cast<unsigned>(o[0]) << 16) |
                    (static_cast<unsigned>(o[1]) << 8) | static_cast<unsigned>(o[2]);
    }

    IDirect3DTexture9* staging = nullptr;
    IDirect3DTexture9* generated = nullptr;
    const bool wasInternal = g_internal;
    g_internal = true;
    HRESULT hr = dev->CreateTexture(dw, dh, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM,
                                    &staging, nullptr);
    if (SUCCEEDED(hr) && staging) {
        D3DLOCKED_RECT dst{};
        if (SUCCEEDED(staging->LockRect(0, &dst, nullptr, 0)) && dst.pBits) {
            for (UINT y = 0; y < dh; ++y)
                memcpy(static_cast<unsigned char*>(dst.pBits) + y * dst.Pitch,
                       &pixels[static_cast<size_t>(y) * dw], static_cast<size_t>(dw) * 4);
            staging->UnlockRect(0);
        }
        hr = dev->CreateTexture(dw, dh, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &generated,
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
            pR / 255.0f, pG / 255.0f, pB / 255.0f,
            col[0][0], col[0][1], col[0][2], col[1][0], col[1][1], col[1][2],
            col[2][0], col[2][1], col[2][2], difMean,
            acc / (3.0 * static_cast<double>(dw) * dh));

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

// ---------------------------------------------------------------- FFP conversion

// Which texture should Remix see as this surface's base colour?

// ---------------------------------------------------------------------------------------------
// How does SR3 fill its character texture? Nothing else can be built until this is known.
//
// Settled by A/B on 2026-08-29: with `d3d9.dll` renamed away, so the game runs on Windows' own
// D3D9 with no DXVK and no Remix, THE CHARACTER IS TEXTURED CORRECTLY. Under the Remix stack the
// same texture measures all zeroes. The game, the save, the character mods and SR3's own display
// settings are all cleared, and so is every configuration knob: rtx.conf restored wholesale from
// 08-18, user.conf, `d3d9.allowDiscard`, `d3d9.apitraceMode` - each verified parsed, each inert.
//
// So SR3's writes to its 2048x1024 X8R8G8B8 D3DPOOL_DEFAULT + D3DUSAGE_DYNAMIC texture land on
// native D3D9 and do not land under DXVK. The only route left for this shim is to snoop those
// writes at the CPU boundary, BEFORE DXVK loses them, and re-upload through UpdateTexture - the
// path Remix hashes. That is the architecture already proven on the morph and instance streams.
//
// It cannot be written without knowing WHICH call the game uses. This probe answers exactly that
// and nothing else: it counts, per candidate entry point, how many touch a texture of the atlas's
// shape. Whichever is non-zero is the one to snoop; if all are zero the game fills it by a route
// none of these cover, and that is worth knowing before writing a subsystem for the wrong one.
//
// Thread safety: the game locks resources from more than one thread - proven by a crash dump and
// then measured at 272 buffer locks a frame off the render thread. So this keeps a FIXED array of
// candidate pointers, written only on the render thread and scanned without a lock. No map is
// mutated from a foreign thread, which is the bug that crashed this project once already.
constexpr unsigned kMaxAtlasPtrs = 32;
void* g_atlasPtrs[kMaxAtlasPtrs] = {};
unsigned g_atlasPtrCount = 0;

unsigned g_fillLockRect = 0, g_fillUpdateTexture = 0, g_fillUpdateSurface = 0;
unsigned g_fillStretchRect = 0, g_fillColorFill = 0;
unsigned g_fillStretchFailed = 0;   // the game's OWN StretchRect into it returning an error
unsigned g_atlasPtrOverflow = 0;    // candidates that did not fit the fixed array
unsigned g_atlasCreateReports = 0;
unsigned g_bigTexReports = 0;
unsigned g_fillLockLevels = 0;        // bitmask of mip levels seen locked
DWORD    g_fillLockFlags = 0;         // OR of every lock flag seen
unsigned g_fillReports = 0;

bool IsAtlasPtr(const void* p) {
    if (!p) return false;
    for (unsigned i = 0; i < g_atlasPtrCount; ++i)
        if (g_atlasPtrs[i] == p) return true;
    return false;
}

void NoteAtlasPtr(void* p) {
    if (!p || IsAtlasPtr(p) || g_atlasPtrCount >= kMaxAtlasPtrs) { 
        if (p && !IsAtlasPtr(p) && g_atlasPtrCount >= kMaxAtlasPtrs) ++g_atlasPtrOverflow;
        return;
    }
    g_atlasPtrs[g_atlasPtrCount++] = p;
}


// Does this SURFACE belong to the tracked atlas TEXTURE?
//
// CORRECTED 2026-08-29. The first version matched any uncompressed surface >= 256x256, which also
// matches the 2560x1440 back buffer and every HDR target - so its "StretchRect 860" counted the
// whole frame's blits, not writes to the character texture. A count that does not measure what
// its name says is worse than no count, and this project has been burned by that three times.
//
// GetContainer walks the surface back to its parent texture, which is exact. It AddRefs, so the
// result is released immediately - a probe that leaks a reference per call would pin every
// texture in the game.
bool IsAtlasSurface(IDirect3DSurface9* surf) {
    if (!surf || !g_atlasPtrCount) return false;
    IDirect3DBaseTexture9* parent = nullptr;
    if (FAILED(surf->GetContainer(__uuidof(IDirect3DTexture9), reinterpret_cast<void**>(&parent))) ||
        !parent)
        return false;
    const bool hit = IsAtlasPtr(parent);
    parent->Release();
    return hit;
}

typedef HRESULT(WINAPI* SurfLockRect_t)(IDirect3DSurface9*, D3DLOCKED_RECT*, const RECT*, DWORD);
typedef HRESULT(WINAPI* SurfUnlockRect_t)(IDirect3DSurface9*);
SurfLockRect_t g_origSurfLockRect = nullptr;
SurfUnlockRect_t g_origSurfUnlockRect = nullptr;
unsigned g_fillSurfLock = 0;

// ---------------------------------------------------------------------------------------------
// The character-atlas snoop
//
// Measured 2026-08-29, after four probes that each eliminated a route:
//
//     SURFACE LockRect 18 | texture LockRect 0 (on the atlas), UpdateTexture 0, UpdateSurface 0,
//                           StretchRect 0, ColorFill 0
//     TEXTURE FILL #3: SURFACE LockRect | flags=0x0 pitch=8192 subrect=whole   <- 2048 wide, mip 0
//     #4 pitch=4096  #5 2048  #6 1024  #7 512  #8 256                          <- the mip chain
//
// SR3 fills the character texture by taking each mip level's SURFACE with GetSurfaceLevel and
// locking THAT - `IDirect3DSurface9::LockRect`, a different vtable from the texture's own method,
// which is why the first three fill probes saw nothing. The lock is whole-surface with flags 0:
// a plain preserving write.
//
// Those writes reach the game's own D3D9 correctly - with `d3d9.dll` renamed away the character
// is textured - and do not survive into the image Remix samples, which measures all zeroes
// through a GPU read whose control returned 126.7 of an expected 126.7.
//
// So the shim takes the pixels at the CPU boundary, where they demonstrably exist, and re-uploads
// them through SYSTEMMEM + UpdateTexture - the path Remix hashes, as the cloth generator's note
// says. Same architecture as the morph and instance stream snoop, applied to a texture.
//
// Only MIP 0 is captured. Remix samples the top level for a material and 2048x1024x4 is 8 MB per
// character; capturing the whole chain would add a third for nothing.
struct AtlasSnoop {
    IDirect3DBaseTexture9* tex = nullptr;   // parent texture, the key
    UINT w = 0, h = 0;
    std::vector<unsigned char> pixels;      // BGRA, mip 0
    bool fresh = false;                     // written since the copy was last built
    IDirect3DTexture9* copy = nullptr;      // what gets bound instead
    double mean = -1.0;
};
constexpr unsigned kMaxAtlasSnoop = 8;
AtlasSnoop g_atlasSnoop[kMaxAtlasSnoop];
unsigned g_atlasSnoopCount = 0;

// One pending lock per SURFACE. The game locks resources from more than one thread - proven by a
// crash dump and measured at 272 buffer locks a frame - so this is a small fixed array guarded by
// its own critical section, never a map resized from a foreign thread.
struct PendingSurfLock {
    IDirect3DSurface9* surf = nullptr;
    IDirect3DBaseTexture9* tex = nullptr;
    void* bits = nullptr;
    int pitch = 0;
    UINT w = 0, h = 0;
};
constexpr unsigned kMaxPendingSurf = 8;
PendingSurfLock g_pendingSurf[kMaxPendingSurf];
CRITICAL_SECTION g_atlasCs;
bool g_atlasCsReady = false;

unsigned g_atlasCaptured = 0;      // mip-0 captures completed
unsigned g_atlasCopyBuilt = 0;     // uploads handed to Remix
unsigned g_atlasBound = 0;         // draws/frame that bound a snooped copy
unsigned g_atlasCaptureFailed = 0;
unsigned g_atlasDumps = 0;

// ---------------------------------------------------------------------------------------------
// Where the character pixels actually are
//
// The snoop proved the game writes ZEROES into its 2048x1024 dynamic texture: captured straight
// out of the game's own mapped memory at Unlock, mean 0.0, while a 1280x768 texture captured the
// same way read 14.3. So nothing downstream was losing the pixels - they were never written.
//
// But the character IS correct without Remix. So the compositor produces zeroes only here, and
// the obvious suspect is the step before it: the game creates a 2048x1024 RENDER TARGET of the
// same size (BIG TEXTURE #3: fmt=21 usage=0x1). A GPU composite into that, read back and written
// into the dynamic texture, would emit zeroes if the READBACK is what fails under DXVK.
//
// This reads every large uncompressed render target ONCE with the path whose control already
// passed (StretchRect into our own target, then GetRenderTargetData - SelfTestCopyPath returned
// 126.7 of an expected 126.7). If one of them holds a character, the pixels exist on the GPU and
// the shim can bind THAT instead. If they are all empty, the composite never ran.
constexpr unsigned kMaxRtProbe = 12;
IDirect3DTexture9* g_rtProbe[kMaxRtProbe] = {};
UINT g_rtProbeW[kMaxRtProbe] = {}, g_rtProbeH[kMaxRtProbe] = {};
double g_rtProbeMean[kMaxRtProbe];
bool g_rtProbeDone[kMaxRtProbe] = {};
unsigned g_rtProbeAttempts[kMaxRtProbe] = {};
unsigned g_rtProbeCount = 0;
unsigned g_rtProbeRead = 0;
unsigned g_rtProbeFailed = 0, g_rtProbeFailReports = 0;

void NoteRenderTargetCandidate(IDirect3DTexture9* tex, UINT w, UINT h) {
    if (!tex || g_rtProbeCount >= kMaxRtProbe) return;
    g_rtProbe[g_rtProbeCount] = tex;
    g_rtProbeW[g_rtProbeCount] = w;
    g_rtProbeH[g_rtProbeCount] = h;
    g_rtProbeMean[g_rtProbeCount] = -1.0;
    ++g_rtProbeCount;
}

void AtlasCsInit() {
    if (!g_atlasCsReady) { InitializeCriticalSection(&g_atlasCs); g_atlasCsReady = true; }
}

AtlasSnoop* FindSnoop(IDirect3DBaseTexture9* tex) {
    for (unsigned i = 0; i < g_atlasSnoopCount; ++i)
        if (g_atlasSnoop[i].tex == tex) return &g_atlasSnoop[i];
    return nullptr;
}

typedef HRESULT(WINAPI* TexLockRect_t)(IDirect3DTexture9*, UINT, D3DLOCKED_RECT*, const RECT*, DWORD);
typedef HRESULT(WINAPI* TexUnlockRect_t)(IDirect3DTexture9*, UINT);
typedef HRESULT(WINAPI* UpdateSurface_t)(IDirect3DDevice9*, IDirect3DSurface9*, const RECT*,
                                         IDirect3DSurface9*, const POINT*);
typedef HRESULT(WINAPI* UpdateTexture_t)(IDirect3DDevice9*, IDirect3DBaseTexture9*,
                                         IDirect3DBaseTexture9*);
typedef HRESULT(WINAPI* StretchRect_t)(IDirect3DDevice9*, IDirect3DSurface9*, const RECT*,
                                       IDirect3DSurface9*, const RECT*, D3DTEXTUREFILTERTYPE);
typedef HRESULT(WINAPI* ColorFill_t)(IDirect3DDevice9*, IDirect3DSurface9*, const RECT*, D3DCOLOR);
TexLockRect_t g_origTexLockRect = nullptr;
TexUnlockRect_t g_origTexUnlockRect = nullptr;
UpdateSurface_t g_origUpdateSurface = nullptr;
UpdateTexture_t g_origUpdateTexture = nullptr;
StretchRect_t g_origStretchRect = nullptr;
ColorFill_t g_origColorFill = nullptr;

HRESULT WINAPI Hook_TexLockRect(IDirect3DTexture9* self, UINT level, D3DLOCKED_RECT* rect,
                                const RECT* r, DWORD flags) {
    const HRESULT hr = g_origTexLockRect(self, level, rect, r, flags);
    if (!g_internal && SUCCEEDED(hr) && IsAtlasPtr(self)) {
        ++g_fillLockRect;
        if (level < 32) g_fillLockLevels |= (1u << level);
        g_fillLockFlags |= flags;
        if (g_fillReports < 6) {
            ++g_fillReports;
            Log("TEXTURE FILL #%u: LockRect on the character texture | level=%u flags=0x%08lX "
                "pitch=%d subrect=%s", g_fillReports, level, flags,
                rect ? rect->Pitch : 0, r ? "yes" : "whole");
        }
    }
    return hr;
}

HRESULT WINAPI Hook_TexUnlockRect(IDirect3DTexture9* self, UINT level) {
    return g_origTexUnlockRect(self, level);
}

// The surface-level lock. IsAtlasSurface walks the surface back to its parent texture, so this
// fires only for a surface belonging to a tracked atlas.
IDirect3DBaseTexture9* AtlasParent(IDirect3DSurface9* surf) {
    if (!surf || !g_atlasPtrCount) return nullptr;
    IDirect3DBaseTexture9* parent = nullptr;
    if (FAILED(surf->GetContainer(__uuidof(IDirect3DTexture9),
                                  reinterpret_cast<void**>(&parent))) || !parent)
        return nullptr;
    const bool hit = IsAtlasPtr(parent);
    parent->Release();          // borrowed: the texture outlives the lock by construction
    return hit ? parent : nullptr;
}

HRESULT WINAPI Hook_SurfLockRect(IDirect3DSurface9* self, D3DLOCKED_RECT* rect, const RECT* r,
                                 DWORD flags) {
    const HRESULT hr = g_origSurfLockRect(self, rect, r, flags);
    if (g_internal || FAILED(hr) || !rect || !rect->pBits) return hr;
    IDirect3DBaseTexture9* parent = AtlasParent(self);
    if (!parent) return hr;

    ++g_fillSurfLock;
    g_fillLockFlags |= flags;

    // Only the TOP level is captured, identified by matching the parent's level-0 size. A
    // sub-rect lock is refused rather than partially captured: a partial copy of a texture we
    // then hand to Remix as complete would be a worse bug than the one being fixed.
    D3DSURFACE_DESC sd{};
    if (FAILED(self->GetDesc(&sd))) return hr;
    D3DSURFACE_DESC td{};
    if (FAILED(static_cast<IDirect3DTexture9*>(parent)->GetLevelDesc(0, &td))) return hr;
    const bool topLevel = (sd.Width == td.Width && sd.Height == td.Height);

    if (g_fillReports < 8) {
        ++g_fillReports;
        Log("TEXTURE FILL #%u: SURFACE LockRect on the character texture | flags=0x%08lX "
            "pitch=%d %ux%u %s", g_fillReports, flags, rect->Pitch, sd.Width, sd.Height,
            topLevel ? "TOP LEVEL" : "mip");
    }
    if (!topLevel || r) return hr;

    AtlasCsInit();
    EnterCriticalSection(&g_atlasCs);
    for (unsigned i = 0; i < kMaxPendingSurf; ++i) {
        if (g_pendingSurf[i].surf == nullptr) {
            g_pendingSurf[i] = {self, parent, rect->pBits, rect->Pitch, sd.Width, sd.Height};
            break;
        }
    }
    LeaveCriticalSection(&g_atlasCs);
    return hr;
}

// The write has landed in the game's own mapped memory by now, so this is where it is copied.
HRESULT WINAPI Hook_SurfUnlockRect(IDirect3DSurface9* self) {
    PendingSurfLock p{};
    if (!g_internal && g_atlasCsReady) {
        EnterCriticalSection(&g_atlasCs);
        for (unsigned i = 0; i < kMaxPendingSurf; ++i) {
            if (g_pendingSurf[i].surf == self) {
                p = g_pendingSurf[i];
                g_pendingSurf[i] = PendingSurfLock{};
                break;
            }
        }
        LeaveCriticalSection(&g_atlasCs);
    }
    if (!p.surf || !p.bits || p.pitch <= 0) return g_origSurfUnlockRect(self);

    // Copy BEFORE unlocking - after the unlock the pointer is no longer the game's to give.
    try {
        EnterCriticalSection(&g_atlasCs);
        AtlasSnoop* e = FindSnoop(p.tex);
        if (!e && g_atlasSnoopCount < kMaxAtlasSnoop) {
            e = &g_atlasSnoop[g_atlasSnoopCount++];
            e->tex = p.tex;
        }
        if (e) {
            e->w = p.w; e->h = p.h;
            const size_t need = static_cast<size_t>(p.w) * p.h * 4;
            if (e->pixels.size() != need) e->pixels.assign(need, 0);
            const unsigned char* src = static_cast<const unsigned char*>(p.bits);
            for (UINT y = 0; y < p.h; ++y)
                memcpy(&e->pixels[static_cast<size_t>(y) * p.w * 4],
                       src + static_cast<size_t>(y) * p.pitch, static_cast<size_t>(p.w) * 4);
            // The falsifier: what did the game actually write? A non-zero mean here proves the
            // pixels exist at this boundary, which every read further down the stack denied.
            double acc = 0.0; unsigned n = 0;
            for (UINT y = 0; y < p.h; y += 8)
                for (UINT x = 0; x < p.w; x += 8) {
                    const unsigned char* q = &e->pixels[(static_cast<size_t>(y) * p.w + x) * 4];
                    acc += q[0] + q[1] + q[2]; ++n;
                }
            e->mean = n ? acc / (3.0 * n) : 0.0;
            e->fresh = true;
            ++g_atlasCaptured;
            if (g_atlasCaptured <= 4)
                Log("ATLAS CAPTURED #%u: %ux%u from the game's own write | mean %.1f of 255",
                    g_atlasCaptured, p.w, p.h, e->mean);
            // Write the largest capture out so it can be LOOKED AT. The user reports the atlas
            // has "some stuff in it but the skin part is black", and a mean of 3.8 against a
            // fully composited 182.7 says the same thing without saying WHICH region is missing.
            // A picture does. Bounded: the two biggest captures, once each, ~6 MB apiece.
            // The character atlas is the 2048x1024 one; 1280x768 is the title screen (dumped
            // and identified 2026-08-31), so require both dimensions to be large.
            if (p.w >= 2048 && p.h >= 1024) WriteAtlasDdsOnce(p.w, p.h, e->pixels.data());
            if (p.w >= 1024 && g_atlasDumps < 2) {
                ++g_atlasDumps;
                std::vector<unsigned char> rgb;
                try {
                    rgb.resize(static_cast<size_t>(p.w) * p.h * 3);
                    for (size_t i = 0, n = static_cast<size_t>(p.w) * p.h; i < n; ++i) {
                        rgb[i * 3 + 0] = e->pixels[i * 4 + 2];   // BGRA -> RGB
                        rgb[i * 3 + 1] = e->pixels[i * 4 + 1];
                        rgb[i * 3 + 2] = e->pixels[i * 4 + 0];
                    }
                    char tag[48];
                    sprintf_s(tag, "atlas%u", g_atlasDumps);
                    DumpRaw(tag, 0, p.w, p.h, rgb.data());
                } catch (...) {
                }
            }
        }
        LeaveCriticalSection(&g_atlasCs);
    } catch (...) {
        ++g_atlasCaptureFailed;
        LeaveCriticalSection(&g_atlasCs);
    }
    return g_origSurfUnlockRect(self);
}

// Read one candidate render target per frame and record what it holds. Render thread only.
void ProbeRenderTargetContents(IDirect3DDevice9* dev) {
    if (!dev) return;
    // ROUND-ROBIN, FOREVER, keeping the MAXIMUM.
    //
    // The first version read each candidate ONCE and marked it done. It ran one per frame from
    // frame 1, so every read landed during startup and loading - before anything had been drawn -
    // and all ten reported 0.0, including the 2560x1440 scene targets that certainly hold the
    // rendered world. The measurement said nothing about the character and everything about when
    // it was taken.
    //
    // That is the third time on this bug that the right quantity was measured at the wrong TIME:
    // compositeToTexturePass counted a population in a frame that could not contain it, the fill
    // probe hooked LockRect after the fill had already happened, and now this. A one-shot read of
    // a surface whose contents change is worth nothing; only the maximum over time is.
    // BOUNDED, 2026-08-29. The first round-robin version FROZE THE GAME right after the intro
    // logos: it allocated a CreateOffscreenPlainSurface per read - 14.7 MB for each 2560x1440
    // candidate - and did a full GPU sync every four frames, forever, in a 32-bit address space
    // shared with the game, Remix's client and a 24 MB skinning ring. That is precisely the
    // failure already recorded in YOUR-INSTRUCTIONS.md, where pre-sizing the snoop buffers
    // "exhausted a 32-bit address space and killed the game".
    //
    // A shim must degrade, not abort its host, and a DIAGNOSTIC least of all. So:
    //   * a total budget of reads for the whole session, not a rate;
    //   * one read every 120 frames, roughly two seconds;
    //   * surfaces larger than the character atlas are read only a couple of times, purely as
    //     the control for whether GetRenderTargetData can see game-written surfaces at all.
    constexpr unsigned kRtProbeBudget = 60;
    constexpr unsigned kRtProbeInterval = 120;
    if (!g_rtProbeCount || g_rtProbeRead >= kRtProbeBudget) return;
    if (g_frames % kRtProbeInterval) return;
    const unsigned i = (g_frames / kRtProbeInterval) % g_rtProbeCount;
    {
        // The big scene targets cost 14.7 MB a read and are only the control; a few is plenty.
        if (g_rtProbeW[i] > 2048 && g_rtProbeAttempts[i] >= 3) return;
        ++g_rtProbeAttempts[i];
        IDirect3DTexture9* src = g_rtProbe[i];
        const UINT w = g_rtProbeW[i], h = g_rtProbeH[i];
        IDirect3DSurface9* srcSurf = nullptr;
        IDirect3DSurface9* sysSurf = nullptr;
        const bool wasInternal = g_internal;
        g_internal = true;
        // Each step reports its own failure. The previous version counted only SUCCESSFUL reads,
        // so a GetRenderTargetData that refused every surface would have printed "0 reads" -
        // indistinguishable from "the probe never ran", which is exactly the ambiguity that
        // wasted the run before this one.
        HRESULT hrSurf = E_FAIL, hrPlain = E_FAIL, hrGet = E_FAIL, hrLock = E_FAIL;
        try {
            hrSurf = src->GetSurfaceLevel(0, &srcSurf);
            if (SUCCEEDED(hrSurf) && srcSurf)
                hrPlain = dev->CreateOffscreenPlainSurface(w, h, D3DFMT_A8R8G8B8,
                                                           D3DPOOL_SYSTEMMEM, &sysSurf, nullptr);
            if (SUCCEEDED(hrPlain) && sysSurf)
                hrGet = dev->GetRenderTargetData(srcSurf, sysSurf);
            if (SUCCEEDED(hrGet)) {
                D3DLOCKED_RECT lr{};
                hrLock = sysSurf->LockRect(&lr, nullptr, D3DLOCK_READONLY);
                if (SUCCEEDED(hrLock) && lr.pBits) {
                    double acc = 0.0; unsigned n = 0;
                    for (UINT y = 0; y < h; y += 8) {
                        const unsigned char* row = static_cast<const unsigned char*>(lr.pBits) +
                                                   static_cast<size_t>(y) * lr.Pitch;
                        for (UINT x = 0; x < w; x += 8) {
                            acc += row[x * 4 + 0] + row[x * 4 + 1] + row[x * 4 + 2];
                            ++n;
                        }
                    }
                    const double mean = n ? acc / (3.0 * n) : 0.0;
                    ++g_rtProbeRead;
                    // Only an INCREASE is reported, so the log shows each surface's best content
                    // rather than thousands of repeats.
                    if (mean > g_rtProbeMean[i] + 0.5) {
                        g_rtProbeMean[i] = mean;
                        if (!g_rtProbeDone[i] || mean > 1.0) {
                            g_rtProbeDone[i] = true;
                            Log("RT CONTENT #%u: %ux%u render target | mean rose to %.1f of 255 "
                                "at frame %u  <- non-zero means the pixels exist on the GPU here",
                                i + 1, w, h, mean, g_frames);
                        }
                    }
                    sysSurf->UnlockRect();
                }
            }
        } catch (...) {
        }
        if (FAILED(hrGet) || FAILED(hrLock)) {
            ++g_rtProbeFailed;
            if (g_rtProbeFailReports < 6) {
                ++g_rtProbeFailReports;
                Log("RT CONTENT FAILED #%u: %ux%u | GetSurfaceLevel=0x%08lX "
                    "CreateOffscreenPlain=0x%08lX GetRenderTargetData=0x%08lX LockRect=0x%08lX "
                    "  <- the read tool cannot see this surface, so any zero it reports is "
                    "meaningless", g_rtProbeFailReports, w, h, hrSurf, hrPlain, hrGet, hrLock);
            }
        }
        if (sysSurf) sysSurf->Release();
        if (srcSurf) srcSurf->Release();
        g_internal = wasInternal;
    }
}

// Build (or refresh) the texture handed to Remix from the snooped pixels. Render thread only.
IDirect3DTexture9* AtlasCopyFor(IDirect3DDevice9* dev, IDirect3DBaseTexture9* tex) {
    if (!g_atlasCsReady) return nullptr;
    EnterCriticalSection(&g_atlasCs);
    AtlasSnoop* e = FindSnoop(tex);
    if (!e || (!e->fresh && !e->copy) || e->pixels.empty()) {
        IDirect3DTexture9* existing = e ? e->copy : nullptr;
        LeaveCriticalSection(&g_atlasCs);
        return existing;
    }
    if (!e->fresh) { IDirect3DTexture9* c = e->copy; LeaveCriticalSection(&g_atlasCs); return c; }

    const UINT w = e->w, h = e->h;
    std::vector<unsigned char> local;
    try { local = e->pixels; } catch (...) { LeaveCriticalSection(&g_atlasCs); return e->copy; }
    e->fresh = false;
    IDirect3DTexture9* old = e->copy;
    LeaveCriticalSection(&g_atlasCs);

    IDirect3DTexture9* staging = nullptr;
    IDirect3DTexture9* made = nullptr;
    const bool wasInternal = g_internal;
    g_internal = true;
    try {
        if (SUCCEEDED(dev->CreateTexture(w, h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM,
                                         &staging, nullptr)) && staging) {
            D3DLOCKED_RECT dst{};
            if (SUCCEEDED(staging->LockRect(0, &dst, nullptr, 0)) && dst.pBits) {
                for (UINT y = 0; y < h; ++y) {
                    unsigned char* drow = static_cast<unsigned char*>(dst.pBits) +
                                          static_cast<size_t>(y) * dst.Pitch;
                    const unsigned char* srow = &local[static_cast<size_t>(y) * w * 4];
                    for (UINT x = 0; x < w; ++x) {
                        drow[x * 4 + 0] = srow[x * 4 + 0];
                        drow[x * 4 + 1] = srow[x * 4 + 1];
                        drow[x * 4 + 2] = srow[x * 4 + 2];
                        drow[x * 4 + 3] = 255;   // the source is X8R8G8B8; alpha is undefined
                    }
                }
                staging->UnlockRect(0);
            }
            // UpdateTexture is the upload Remix hashes - the whole point of the exercise.
            if (SUCCEEDED(dev->CreateTexture(w, h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                                             &made, nullptr)) && made) {
                if (FAILED(dev->UpdateTexture(staging, made))) { made->Release(); made = nullptr; }
            }
            staging->Release();
        }
    } catch (...) { made = nullptr; }
    g_internal = wasInternal;

    if (!made) return old;
    ++g_atlasCopyBuilt;
    EnterCriticalSection(&g_atlasCs);
    e->copy = made;
    LeaveCriticalSection(&g_atlasCs);
    if (old) old->Release();
    return made;
}
// Patch the shared IDirect3DTexture9 vtable ONCE. Every texture shares it, so a second patch
// would store our own hook as the "original" and the hook would call itself.
void InstallTextureHooks(IDirect3DTexture9* tex) {
    static bool done = false;
    if (done || !tex) return;
    done = true;
    PatchVTable(tex, kSlotTexLockRect, &Hook_TexLockRect,
                reinterpret_cast<void**>(&g_origTexLockRect));
    PatchVTable(tex, kSlotTexUnlockRect, &Hook_TexUnlockRect,
                reinterpret_cast<void**>(&g_origTexUnlockRect));
    // The surface vtable is shared too, and is reached through any texture's level 0.
    IDirect3DSurface9* surf = nullptr;
    if (SUCCEEDED(tex->GetSurfaceLevel(0, &surf)) && surf) {
        PatchVTable(surf, kSlotSurfLockRect, &Hook_SurfLockRect,
                    reinterpret_cast<void**>(&g_origSurfLockRect));
        PatchVTable(surf, kSlotSurfUnlockRect, &Hook_SurfUnlockRect,
                    reinterpret_cast<void**>(&g_origSurfUnlockRect));
        surf->Release();
    }
}

HRESULT WINAPI Hook_UpdateSurface(IDirect3DDevice9* dev, IDirect3DSurface9* src, const RECT* sr,
                                  IDirect3DSurface9* dst, const POINT* dp) {
    if (!g_internal && IsAtlasSurface(dst)) ++g_fillUpdateSurface;
    return g_origUpdateSurface(dev, src, sr, dst, dp);
}

HRESULT WINAPI Hook_UpdateTexture(IDirect3DDevice9* dev, IDirect3DBaseTexture9* src,
                                  IDirect3DBaseTexture9* dst) {
    if (!g_internal && IsAtlasPtr(dst)) ++g_fillUpdateTexture;
    return g_origUpdateTexture(dev, src, dst);
}

HRESULT WINAPI Hook_StretchRect(IDirect3DDevice9* dev, IDirect3DSurface9* src, const RECT* sr,
                                IDirect3DSurface9* dst, const RECT* dr,
                                D3DTEXTUREFILTERTYPE filter) {
    const HRESULT hr = g_origStretchRect(dev, src, sr, dst, dr, filter);
    if (!g_internal && IsAtlasSurface(dst)) {
        ++g_fillStretchRect;
        if (FAILED(hr)) ++g_fillStretchFailed;
        if (g_fillReports < 8) {
            ++g_fillReports;
            D3DSURFACE_DESC sd{};
            const bool haveSrc = src && SUCCEEDED(src->GetDesc(&sd));
            Log("TEXTURE FILL #%u: StretchRect INTO the character texture | hr=0x%08lX | "
                "src %ux%u fmt=%d pool=%d usage=0x%lX | dstRect %s | filter=%d",
                g_fillReports, hr,
                haveSrc ? sd.Width : 0, haveSrc ? sd.Height : 0,
                haveSrc ? static_cast<int>(sd.Format) : 0,
                haveSrc ? static_cast<int>(sd.Pool) : -1,
                haveSrc ? static_cast<unsigned long>(sd.Usage) : 0UL,
                dr ? "sub" : "whole", static_cast<int>(filter));
        }
    }
    return hr;
}

HRESULT WINAPI Hook_ColorFill(IDirect3DDevice9* dev, IDirect3DSurface9* surf, const RECT* r,
                              D3DCOLOR c) {
    if (!g_internal && IsAtlasSurface(surf)) ++g_fillColorFill;
    return g_origColorFill(dev, surf, r, c);
}

// ---------------------------------------------------------------------------------------------
// Self-test for the copy path, because "it returned S_OK" is not evidence it moved pixels
//
// The GPU copy reports 0 StretchRect refusals, 0 failures, and a mean of 0.0 for the character
// atlas. Read literally that says the atlas is empty. This project has now been wrong three
// times in a row by believing exactly that kind of reading:
//
//   * `usage=0x200` read as D3DUSAGE_RENDERTARGET, when it is D3DUSAGE_DYNAMIC;
//   * 240 blank LockRect readbacks read as "the game never wrote it", when a READONLY lock of a
//     DYNAMIC default-pool surface has UNDEFINED contents;
//   * a black 2048x1024 texture in two Remix captures read as a date correlation, when an
//     all-zero image simply always hashes the same.
//
// Each time the missing piece was a CONTROL: a case where the answer is known in advance. So
// before concluding anything from a zero mean, run the identical path over a source this shim
// FILLED ITSELF, matched to the atlas in the properties that matter - D3DPOOL_DEFAULT,
// D3DUSAGE_DYNAMIC, uncompressed, written by LockRect and never by the GPU.
//
//   control mean ~= the value written  -> the path works, and the atlas really is empty
//   control mean == 0                  -> StretchRect cannot read a DYNAMIC default texture and
//                                         every zero this path has reported is meaningless
//
// Runs ONCE, allocates 256x256, and releases everything.
bool g_copySelfTestDone = false;

void SelfTestCopyPath(IDirect3DDevice9* dev) {
    if (g_copySelfTestDone || !dev || !g_settings.rtAlbedoCopy) return;
    g_copySelfTestDone = true;

    const UINT W = 256, H = 256;
    const unsigned char kR = 200, kG = 120, kB = 60;      // mean should land near 127
    IDirect3DTexture9* srcDyn = nullptr;
    IDirect3DTexture9* scratch = nullptr;
    IDirect3DSurface9* srcSurf = nullptr;
    IDirect3DSurface9* scratchSurf = nullptr;
    IDirect3DSurface9* sysSurf = nullptr;
    const bool wasInternal = g_internal;
    g_internal = true;
    double fillMean = -1.0, resolveMean = -1.0, stretchMean = -1.0;
    HRESULT hrStretch = E_FAIL, hrResolve = E_FAIL;
    try {
        // A source matched to the atlas: DEFAULT pool, DYNAMIC, uncompressed, CPU-written.
        if (FAILED(dev->CreateTexture(W, H, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8,
                                      D3DPOOL_DEFAULT, &srcDyn, nullptr)) || !srcDyn) goto done;
        {
            D3DLOCKED_RECT lr{};
            if (SUCCEEDED(srcDyn->LockRect(0, &lr, nullptr, D3DLOCK_DISCARD)) && lr.pBits) {
                for (UINT y = 0; y < H; ++y) {
                    unsigned char* row = static_cast<unsigned char*>(lr.pBits) +
                                         static_cast<size_t>(y) * lr.Pitch;
                    for (UINT x = 0; x < W; ++x) {
                        row[x * 4 + 0] = kB; row[x * 4 + 1] = kG;
                        row[x * 4 + 2] = kR; row[x * 4 + 3] = 255;
                    }
                }
                srcDyn->UnlockRect(0);
                fillMean = (kR + kG + kB) / 3.0;
            }
        }
        if (FAILED(dev->CreateTexture(W, H, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                                      D3DPOOL_DEFAULT, &scratch, nullptr)) || !scratch) goto done;
        if (FAILED(scratch->GetSurfaceLevel(0, &scratchSurf)) || !scratchSurf) goto done;
        if (FAILED(dev->CreateOffscreenPlainSurface(W, H, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM,
                                                    &sysSurf, nullptr)) || !sysSurf) goto done;

        auto meanOf = [&]() -> double {
            D3DLOCKED_RECT lr{};
            if (FAILED(sysSurf->LockRect(&lr, nullptr, D3DLOCK_READONLY)) || !lr.pBits) return -1.0;
            double acc = 0.0; unsigned n = 0;
            for (UINT y = 0; y < H; y += 4) {
                const unsigned char* row = static_cast<const unsigned char*>(lr.pBits) +
                                           static_cast<size_t>(y) * lr.Pitch;
                for (UINT x = 0; x < W; x += 4) {
                    acc += row[x * 4 + 0] + row[x * 4 + 1] + row[x * 4 + 2];
                    ++n;
                }
            }
            sysSurf->UnlockRect();
            return n ? acc / (3.0 * n) : -1.0;
        };

        // STEP 1 - does the RESOLVE work at all? Fill the render target with a known colour
        // through the GPU and read it back. This tests GetRenderTargetData on its own.
        if (SUCCEEDED(dev->ColorFill(scratchSurf, nullptr, D3DCOLOR_ARGB(255, kR, kG, kB)))) {
            hrResolve = dev->GetRenderTargetData(scratchSurf, sysSurf);
            if (SUCCEEDED(hrResolve)) resolveMean = meanOf();
        }
        // STEP 2 - does StretchRect actually move pixels out of a DYNAMIC default texture?
        // Clear the target first so a no-op StretchRect cannot inherit step 1's colour.
        dev->ColorFill(scratchSurf, nullptr, D3DCOLOR_ARGB(255, 0, 0, 0));
        if (SUCCEEDED(srcDyn->GetSurfaceLevel(0, &srcSurf)) && srcSurf) {
            hrStretch = dev->StretchRect(srcSurf, nullptr, scratchSurf, nullptr, D3DTEXF_NONE);
            if (SUCCEEDED(hrStretch) && SUCCEEDED(dev->GetRenderTargetData(scratchSurf, sysSurf)))
                stretchMean = meanOf();
        }
    } catch (...) {
    }
done:
    Log("COPY PATH SELF-TEST (control for the COMPOSITED ALBEDO zero):");
    Log("    wrote a DEFAULT+DYNAMIC 256x256 source by LockRect, expected mean %.1f", fillMean);
    Log("    step 1  ColorFill -> GetRenderTargetData      mean %.1f  hr=0x%08lX   "
        "(tests the RESOLVE alone)", resolveMean, hrResolve);
    Log("    step 2  StretchRect(dynamic) -> resolve       mean %.1f  hr=0x%08lX   "
        "(tests reading a DYNAMIC default texture)", stretchMean, hrStretch);
    Log("    VERDICT: step 2 near %.1f means the path WORKS and the character atlas really is "
        "empty. step 2 at 0.0 with step 1 fine means StretchRect cannot read a DYNAMIC default "
        "texture, and every zero this path has reported is meaningless.", fillMean);
    if (sysSurf) sysSurf->Release();
    if (scratchSurf) scratchSurf->Release();
    if (scratch) scratch->Release();
    if (srcSurf) srcSurf->Release();
    if (srcDyn) srcDyn->Release();
    g_internal = wasInternal;
}

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

    // First choice: the sampler the shader itself names as a colour map. This is SR3-specific
    // and stronger than anything geometry can tell us - the CTAB says which map is which.
    if (g_settings.rankAlbedo && g_curPS.albedoRank > 0 && g_curPS.albedoStage >= 0 &&
        g_curPS.albedoStage < 8) {
        albedo = g_curTexture[g_curPS.albedoStage];
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
        return nullptr;
    } else {
        // No usable shader reflection: stage 0 is taken raw, whatever it happens to hold. On a
        // material whose stage 0 is a normal map that binds a tangent-space normal map AS base
        // colour, which renders as the wrong colour with surface detail visibly present - the
        // "shoes have normals but they are the wrong colour" report. Counted because until now
        // nothing distinguished this path from a legitimate stage-0 albedo.
        albedo = g_curTexture[0];
        if (!g_curPS.isPixelShader) ++g_albedoStage0Raw;
    }

    // A render-target texture is generated data - the light buffer, the HDR scene, a shadow
    // map - never surface colour. Binding one as albedo is what makes surfaces flicker through
    // unrelated images as the frame's targets are rewritten.
    if (g_settings.excludeRTAlbedo && albedo && g_rtTextures.count(albedo)) {
        IDirect3DBaseTexture9* best = nullptr;
        UINT bestArea = 0;
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
            if (!best || area > bestArea) { best = t; bestArea = area; }
        }
        // Honest, but not free: a null here renders white just the same, and until now nothing
        // counted it. If this number is large the RT exclusion is removing real surfaces rather
        // than protecting them.
        if (!best) ++g_albedoNullAfterRT;
        albedo = best;   // may be null, which is the honest answer
    }
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

void TilingForAlbedo(int& uReg, int& vReg) {
    uReg = vReg = -1;
    if (g_curVS.tilingCount == 0 || !g_curPS.albedoSampler[0]) return;

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
    if (!best) { ++g_tilingUnmatched; return; }
    ++g_tilingMatched;
    uReg = best->uReg;
    vReg = best->vReg;
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
        ShadowSetTSS(dev, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
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
    ShadowSetTSS(dev, 0, D3DTSS_ALPHAARG1, cutout ? D3DTA_TEXTURE : D3DTA_TFACTOR);
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
    if (g_curLayout.texcoordType == D3DDECLTYPE_SHORT2) su = sv = CurrentShortUVScale();
    int tilingU = -1, tilingV = -1;
    TilingForAlbedo(tilingU, tilingV);
    if (tilingU >= 0 && tilingU < static_cast<int>(kMaxVsConst)) su *= g_vsConst[tilingU][0];
    if (tilingV >= 0 && tilingV < static_cast<int>(kMaxVsConst)) sv *= g_vsConst[tilingV][0];

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
    } else {
        ShadowSetTSS(dev, 0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
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


// ---------------------------------------------------------------------------------------------
// cameraOnly - hand Remix a camera and touch NOTHING else
//
// The user's architecture: let the game render exactly as shipped - every pass, every composite,
// every render-to-texture bake - and add only what Remix is missing, rather than removing or
// replacing the game's own draws.
//
// The thing Remix is missing is a CAMERA, and only a camera. From docs/shader-map.md:
//
//   "Remix reads worldToView/viewToProjection from SetTransform and NEVER from shader constants
//    (D3D9Rtx::processRenderState()). SR3 renders through shaders, so wherever it doesn't also
//    set the fixed-function transforms, Remix has no camera."
//
// and the fix is specified there too: watch c28 (projTM, a FUSED view-projection) and c48
// (IR_World2View, the pure view matrix), rebuild, and call SetTransform. Both registers are
// invariant across all 7,276 shaders in the corpus, so this is by construction, not heuristic.
//
// WHY THIS WAS NEVER TRIED, and it is a measurement fault rather than a design decision:
// `ApplyTransforms` - the only caller of SetTransform for the scene - is invoked from exactly one
// place, inside the conversion path. So `ffp=0` removes the geometry conversion AND the camera at
// the same time. The 2026-08-17 test that concluded
//
//   "the fixed-function conversion IS the path-traced world, entirely"
//
// could not distinguish "Remix needs our converted geometry" from "Remix needs a camera, which
// only the conversion path happens to set". That test was repeated on 2026-08-29 with the same
// confound and the same conclusion drawn.
//
// So: set the camera once per change, return PassThrough for every draw, and let vertex capture
// give Remix the geometry. Nothing is skipped, nothing is hidden, nothing is replaced - which is
// the only configuration in this project that CANNOT black out the world or break a bake,
// because it never removes anything.

// ---------------------------------------------------------------------------------------------
// WHICH camera? - the black world at certain angles, diagnosed 2026-08-30
//
// cameraOnly pushed 124,566 DISTINCT view/projection pairs over 10,800 frames - 11.5 per frame -
// straight into SetTransform, in whatever order the engine happened to draw them. Shadow cascades,
// the water reflection, cubemap faces and the main view all arrived at Remix's CameraManager,
// which then had to choose one to path trace with. The Remix log records that from its own side:
//
//     warn: [RTX] CameraManager: FOV of a camera changed between frames
//     info: Camera cut detected on frame 1416 / 2471 / 2508
//
// At some viewing angles it settles on a camera whose frustum does not contain the scene, and the
// world goes black - the symptom this project has chased since 2026-08 and blamed on the skip
// rules four separate times.
//
// It is NOT the skip rules, and that is now proven rather than argued: in cameraOnly every
// skip/hide/convert counter reads 0/frame, because Classify returns PassThrough before any of them
// runs. The shim removed nothing all run and the world still went black. Whatever picks the wrong
// camera is the only thing left that this shim does.
//
// Three discriminators, each read from state the ENGINE sets, none inferred from shader names or
// render-target indices (both recorded dead ends):
//
//   1. SIZE. The main scene passes - G-buffer MRT, lighting, material - all target a surface the
//      size of the back buffer. Shadow atlases, the light buffer and the half-res post chain are
//      smaller: the startup log lists 2560x1440 targets (the back buffer's size) alongside
//      1280x720, 2048x1024, 1280x768 and 1024x512 ones. g_rt0Width/Height already carry this from
//      the SetRenderTarget hook, so it costs no extra bridge round trip.
//   2. PERSPECTIVE. A full-resolution post-process or UI quad carries an ORTHOGRAPHIC projection;
//      the scene camera is perspective. IsPerspective() tests _34/_44 directly.
//   3. HANDEDNESS. A water reflection renders the world mirrored, which flips the determinant of
//      the view rotation negative - already documented above RotationDeterminant(), and already
//      used by skipMirrored for exactly this reason.
//
// FAILURE MODE, deliberately chosen. If all three reject every camera in a frame, nothing is set
// and Remix keeps the last camera it was given. A stale camera lags; it does not black out. That
// is a far cheaper failure than a wrong one, and it is why this returns early rather than falling
// back to "push it anyway".
bool IsMainSceneCamera(const D3DMATRIX& view, const D3DMATRIX& proj);

// The census exists because a rule that picks the WRONG camera and a rule that picks NO camera
// look identical from outside - both are a black world. Seven runs were spent on a false premise
// once already for want of a control, so the control ships with the change: every distinct camera
// the frame contains is listed with the evidence that decided it, accepted or not.
struct CamCandidate {
    D3DMATRIX view{}, proj{};
    UINT rtW = 0, rtH = 0;
    unsigned draws = 0;
    bool perspective = false;
    bool mirrored = false;
    bool accepted = false;
};
constexpr unsigned kMaxCamCandidates = 24;
CamCandidate g_camCand[kMaxCamCandidates];
unsigned g_camCandCount = 0, g_camCandOverflow = 0;
unsigned g_camDrawsOffSize = 0, g_camDrawsOrtho = 0, g_camDrawsMirrored = 0, g_camDrawsMain = 0;

unsigned NoteCamCandidate(const D3DMATRIX& view, const D3DMATRIX& proj) {
    for (unsigned i = 0; i < g_camCandCount; ++i) {
        if (memcmp(&g_camCand[i].view, &view, sizeof view) == 0 &&
            memcmp(&g_camCand[i].proj, &proj, sizeof proj) == 0) {
            ++g_camCand[i].draws;
            return i;
        }
    }
    if (g_camCandCount >= kMaxCamCandidates) { ++g_camCandOverflow; return kMaxCamCandidates; }
    const unsigned i = g_camCandCount++;
    CamCandidate& c = g_camCand[i];
    c.view = view;
    c.proj = proj;
    c.rtW = g_rt0Width;
    c.rtH = g_rt0Height;
    c.draws = 1;
    c.perspective = IsPerspective(proj);
    c.mirrored = RotationDeterminant(view) <= 0.0f;
    c.accepted = false;
    return i;
}

// Cleared on the first draw of a FRAME, not in Present. The frame report runs inside Hook_Present,
// so a reset there wipes the listing before it prints - that exact bug shipped once already for the
// command-block listing (see the note above ResetCommandBlocks), and every report read "0 blocks"
// for a whole session. This follows the corrected pattern rather than the original one, and it is
// keyed on g_frames here rather than piggybacking on NoteCommandBlock so that it still resets when
// the command-block reader is unavailable.
unsigned g_camLastFrameSeen = 0xFFFFFFFF;

bool IsMainSceneCamera(const D3DMATRIX& view, const D3DMATRIX& proj) {
    if (g_backBufferW && g_rt0Width &&
        (g_rt0Width != g_backBufferW || g_rt0Height != g_backBufferH)) {
        ++g_camDrawsOffSize;
        return false;
    }
    if (!IsPerspective(proj)) { ++g_camDrawsOrtho; return false; }
    if (RotationDeterminant(view) <= 0.0f) { ++g_camDrawsMirrored; return false; }
    ++g_camDrawsMain;
    return true;
}

D3DMATRIX g_camOnlyView{}, g_camOnlyProj{};
bool g_camOnlyHave = false;
unsigned g_camOnlySets = 0, g_camOnlyDraws = 0, g_camOnlyRefused = 0;


// ---------------------------------------------------------------------------------------------
// THE REMIX API - step 1: initialize, register the game's device, and nothing else
//
// Found 2026-08-31. The 32-bit BRIDGE CLIENT d3d9.dll - the one SR3 actually loads, in this very
// process - exports `remixapi_InitializeLibrary` and `remixapi_RegisterCallbacks`. So Remix's
// programmatic interface is callable from this shim, and does not require the 64-bit runtime to be
// addressed directly.
//
// WHY THIS MATTERS. Every hard problem in this project descends from one constraint: the only way
// to describe geometry to Remix was to fake a fixed-function draw, or let vertex capture rebuild
// one from shader output. That constraint produced the FFP conversion path and its 64-bone ceiling,
// the whole "which sampler is the albedo" apparatus (AlbedoRank, blanked materials, white
// surfaces), the SHORT2 texcoord problem, the camera reconstruction from c28/c48 - and, with
// capture ON, Remix rebuilding EVERY draw, which is the shapes around the character and the
// blocked camera. CreateMesh/CreateMaterial/DrawInstance remove the constraint outright: we would
// name the albedo instead of hoping Remix infers it, and hand over vertices we have already
// CPU-skinned instead of re-expressing them as fixed function.
//
// STEP 1 IS DELIBERATELY NOT THAT. It answers only the two questions that decide whether any of it
// is worth building, and it changes NOTHING about the image:
//
//   1. Does the bridge actually forward the API, or does it export the symbol and stub the rest?
//      Exporting a symbol is not proof of an implementation.
//   2. Does dxvk_RegisterD3D9Device accept the device Remix itself created for the game?
//
// Both are answered by return codes in the log. `remixApiCamera` is a SEPARATE key, default OFF,
// so the first run cannot alter rendering at all; flipping it needs no rebuild.
//
// Structures transcribed from bridge_api/remix/remix_c.h, API version 0.5.2. The interface struct
// must match member-for-member or every pointer past the first wrong one is garbage, so all 21
// members are declared even though step 1 calls two of them.
constexpr unsigned kRemixStructInitializeLibraryInfo = 1;
constexpr unsigned kRemixStructCameraInfo            = 16;
constexpr unsigned kRemixCameraTypeWorld             = 0;
// REMIXAPI_VERSION_MAKE(0,5,2) = major<<48 | minor<<16 | patch
constexpr unsigned long long kRemixVersion = (0ull << 48) | (5ull << 16) | 2ull;

struct RemixInitInfo {
    unsigned            sType;
    void*               pNext;
    unsigned long long  version;
};

struct RemixCameraInfo {
    unsigned sType;
    void*    pNext;
    unsigned type;
    float    view[4][4];
    float    projection[4][4];
};

struct RemixInterface {
    void* Shutdown;
    void* CreateMaterial;
    void* DestroyMaterial;
    void* CreateMesh;
    void* DestroyMesh;
    unsigned (__stdcall* SetupCamera)(const RemixCameraInfo*);
    void* DrawInstance;
    void* CreateLight;
    void* DestroyLight;
    void* DrawLightInstance;
    void* SetConfigVariable;
    void* dxvk_CreateD3D9;
    unsigned (__stdcall* dxvk_RegisterD3D9Device)(IDirect3DDevice9Ex*);
    void* dxvk_GetExternalSwapchain;
    void* dxvk_GetVkImage;
    void* dxvk_CopyRenderingOutput;
    void* dxvk_SetDefaultOutput;
    void* pick_RequestObjectPicking;
    void* pick_HighlightObjects;
    void* Startup;
    void* Present;
};

RemixInterface g_remix{};
bool g_remixReady = false;        // library initialized
bool g_remixDeviceReady = false;  // usable against the game's device (see RemixApiInit)
// Measured 2026-08-31: the bridge implements the MESH path but not SetupCamera, so these must be
// tracked apart. Lumping them made the first report claim "steps 2-4 are not possible" on the
// strength of one null pointer that steps 2-4 do not use.
bool g_remixMeshReady = false;    // CreateMesh + CreateMaterial + DrawInstance all present
bool g_remixCameraAvail = false;  // SetupCamera present
unsigned g_remixInitRc = 0xFFFFFFFF, g_remixRegisterRc = 0xFFFFFFFF;
unsigned g_remixCamCalls = 0, g_remixCamFails = 0, g_remixCamLastRc = 0;

const char* RemixErrName(unsigned rc) {
    switch (rc) {
        case 0:  return "SUCCESS";
        case 1:  return "GENERAL_FAILURE";
        case 2:  return "LOAD_LIBRARY_FAILURE";
        case 3:  return "INVALID_ARGUMENTS";
        case 4:  return "GET_PROC_ADDRESS_FAILURE";
        case 5:  return "ALREADY_EXISTS";
        case 6:  return "REGISTERING_NON_REMIX_D3D9_DEVICE";
        case 7:  return "REMIX_DEVICE_WAS_NOT_REGISTERED";
        case 8:  return "INCOMPATIBLE_VERSION";
        case 9:  return "SET_DLL_DIRECTORY_FAILURE";
        case 10: return "GET_FULL_PATH_NAME_FAILURE";
        case 11: return "NOT_INITIALIZED";
        case 0xFFFFFFFF: return "(not attempted)";
        default: return "(unknown)";
    }
}

// Called once, from HookDevice, after the vtable hooks are in.
void RemixApiInit(IDirect3DDevice9* dev) {
    if (!g_settings.remixApi || g_remixReady) return;

    // The bridge client, NOT .trex\d3d9.dll. GetModuleHandle rather than LoadLibrary: the game has
    // already loaded it, and loading a second copy of a d3d9 is how you get two runtimes.
    HMODULE lib = GetModuleHandleW(L"d3d9.dll");
    if (!lib) { Log("remix api: d3d9.dll is not loaded in this process - disabled"); return; }

    typedef unsigned (__stdcall *PFN_Init)(const RemixInitInfo*, RemixInterface*);
    const PFN_Init init =
        reinterpret_cast<PFN_Init>(GetProcAddress(lib, "remixapi_InitializeLibrary"));
    if (!init) {
        Log("remix api: d3d9.dll exports no remixapi_InitializeLibrary - this bridge does not "
            "carry the API, so the whole approach is off the table");
        return;
    }

    RemixInitInfo info{};
    info.sType = kRemixStructInitializeLibraryInfo;
    info.pNext = nullptr;
    info.version = kRemixVersion;
    g_remixInitRc = init(&info, &g_remix);
    if (g_remixInitRc != 0) {
        Log("remix api: InitializeLibrary FAILED rc=%u (%s)", g_remixInitRc,
            RemixErrName(g_remixInitRc));
        return;
    }
    g_remixReady = true;
    Log("remix api: InitializeLibrary OK. entry points: SetupCamera=%p CreateMesh=%p "
        "CreateMaterial=%p DrawInstance=%p CreateLight=%p RegisterD3D9Device=%p",
        reinterpret_cast<void*>(g_remix.SetupCamera), g_remix.CreateMesh, g_remix.CreateMaterial,
        g_remix.DrawInstance, g_remix.CreateLight,
        reinterpret_cast<void*>(g_remix.dxvk_RegisterD3D9Device));

    // Capabilities are per-function, because this bridge implements SOME of the interface.
    g_remixMeshReady = g_remix.CreateMesh && g_remix.CreateMaterial && g_remix.DrawInstance;
    g_remixCameraAvail = g_remix.SetupCamera != nullptr;
    Log("remix api: mesh path %s (CreateMesh/CreateMaterial/DrawInstance) | lights %s | "
        "SetupCamera %s",
        g_remixMeshReady ? "AVAILABLE" : "MISSING",
        g_remix.CreateLight ? "AVAILABLE" : "MISSING",
        g_remixCameraAvail ? "AVAILABLE" : "NOT IMPLEMENTED by this bridge - keep deriving the "
                                          "camera through SetTransform");

    // Registration. The bridge reports, in its own log:
    //   "[remixapi_dxvk_RegisterD3D9Device] Not yet supported. Device used by Remix API defaults
    //    to most recently created by client application."
    // So GENERAL_FAILURE here is NOT a failure to attach - the API already targets the game's
    // device implicitly, because the game's is the most recently created one. Gating the mesh
    // path on this return code would disable a working API on the strength of an unimplemented
    // convenience call.
    if (g_remix.dxvk_RegisterD3D9Device) {
        IDirect3DDevice9Ex* ex = nullptr;
        if (SUCCEEDED(dev->QueryInterface(__uuidof(IDirect3DDevice9Ex),
                                          reinterpret_cast<void**>(&ex))) && ex) {
            g_remixRegisterRc = g_remix.dxvk_RegisterD3D9Device(ex);
            ex->Release();
        } else {
            Log("remix api: the device is not an IDirect3DDevice9Ex");
        }
    }
    g_remixDeviceReady = g_remixReady && (g_remixMeshReady || g_remixCameraAvail);
    Log("remix api: RegisterD3D9Device rc=%u (%s) - explicit registration is NOT SUPPORTED by "
        "this bridge and is not required; it defaults to the most recently created device, which "
        "is the game's. Usable: %s", g_remixRegisterRc, RemixErrName(g_remixRegisterRc),
        g_remixDeviceReady ? "YES" : "NO");
}

// Step 1b, behind its own default-OFF key: hand Remix the camera through the API instead of
// through SetTransform. Uses the SAME derivation and the SAME main-view gate as ApplyCameraOnly,
// so if the picture changes it is the API doing it and not a different camera.
//
// CONVENTION UNVERIFIED: D3DMATRIX is row-major with row vectors, and remixapi_CameraInfo takes
// float[4][4] with no documented convention. Passing it straight through is the hypothesis; a
// transposed camera looks like a wildly wrong view, which is unmistakable rather than subtle.
void RemixApiSetCamera(const D3DMATRIX& view, const D3DMATRIX& proj) {
    if (!g_remixCameraAvail || !g_remixDeviceReady) return;
    RemixCameraInfo ci{};
    ci.sType = kRemixStructCameraInfo;
    ci.pNext = nullptr;
    ci.type = kRemixCameraTypeWorld;
    memcpy(ci.view, &view, sizeof(ci.view));
    memcpy(ci.projection, &proj, sizeof(ci.projection));
    const unsigned rc = g_remix.SetupCamera(&ci);
    ++g_remixCamCalls;
    g_remixCamLastRc = rc;
    if (rc != 0) ++g_remixCamFails;
}

// Derives the camera exactly as cameraOnly does and pushes it through the API. Independent of
// cameraOnly, so it works in the normal conversion configuration too.
D3DMATRIX g_remixCamView{}, g_remixCamProj{};
bool g_remixCamHave = false;
// Recorded by RemixApiCameraTick and consumed by the step-2 tick in Present, so it must be
// declared ahead of both.
D3DMATRIX g_remixLastView{};
bool g_remixLastViewValid = false;

unsigned g_remixCamUnavailable = 0;

void RemixApiCameraTick() {
    if (!g_remixReady) return;
    D3DMATRIX view = FromRegisters(&g_vsConst[kRegWorld2View][0], 3);
    const D3DMATRIX viewProj = FromRegisters(&g_vsConst[kRegProjTM][0], 4);
    D3DMATRIX proj{};
    if (!IsFinite(view) || !IsFinite(viewProj) || !DeriveProjection(view, viewProj, proj)) return;
    if (!IsMainSceneCamera(view, proj)) return;
    // Recorded whatever the camera setting says: step 2 needs the view to place its cube, and
    // SetupCamera is not implemented by this bridge anyway.
    g_remixLastView = view;
    g_remixLastViewValid = true;
    if (!g_settings.remixApiCamera) return;
    if (!g_remixCameraAvail) { ++g_remixCamUnavailable; return; }
    if (!g_remixDeviceReady) return;
    if (g_remixCamHave && memcmp(&view, &g_remixCamView, sizeof view) == 0 &&
        memcmp(&proj, &g_remixCamProj, sizeof proj) == 0)
        return;
    g_remixCamView = view;
    g_remixCamProj = proj;
    g_remixCamHave = true;
    RemixApiSetCamera(view, proj);
}


// --- step 2 types. Transcribed from remix_c.h; sizes matter, so the pads are declared. ---------
constexpr unsigned kRemixStructMaterialInfo = 2;
constexpr unsigned kRemixStructMeshInfo     = 12;
constexpr unsigned kRemixStructInstanceInfo = 13;
constexpr unsigned kRemixStructInstanceInfoBoneTransformsEXT = 14;

struct RemixFloat3D { float x, y, z; };

struct RemixHardcodedVertex {
    float    position[3];
    float    normal[3];
    float    texcoord[2];
    unsigned color;
    unsigned _pad[7];
};

struct RemixMaterialInfo {
    unsigned            sType;
    void*               pNext;
    unsigned long long  hash;
    const wchar_t*      albedoTexture;
    const wchar_t*      normalTexture;
    const wchar_t*      tangentTexture;
    const wchar_t*      emissiveTexture;
    float               emissiveIntensity;
    RemixFloat3D        emissiveColorConstant;
    unsigned char       spriteSheetRow;
    unsigned char       spriteSheetCol;
    unsigned char       spriteSheetFps;
    unsigned char       filterMode;
    unsigned char       wrapModeU;
    unsigned char       wrapModeV;
};

// The base MaterialInfo has NO albedo field - albedoConstant lives here, and without this
// chained through pNext the cube rendered grey/black on 2026-08-31. Every member is declared and
// ordered exactly as remix_c.h has it; the struct is all 4-byte members, so the only padding is
// the three bytes after alphaReferenceValue.
constexpr unsigned kRemixStructMaterialInfoOpaqueEXT = 5;

struct RemixMaterialInfoOpaqueEXT {
    unsigned       sType;
    void*          pNext;
    const wchar_t* roughnessTexture;
    const wchar_t* metallicTexture;
    float          anisotropy;
    RemixFloat3D   albedoConstant;
    float          opacityConstant;
    float          roughnessConstant;
    float          metallicConstant;
    unsigned       thinFilmThickness_hasvalue;
    float          thinFilmThickness_value;
    unsigned       alphaIsThinFilmThickness;
    const wchar_t* heightTexture;
    float          displaceIn;
    unsigned       useDrawCallAlphaState;
    unsigned       blendType_hasvalue;
    int            blendType_value;
    unsigned       invertedBlend;
    int            alphaTestType;
    unsigned char  alphaReferenceValue;
    float          displaceOut;
};

struct RemixMeshInfoSkinning {
    unsigned        bonesPerVertex;
    const float*    blendWeights_values;
    unsigned        blendWeights_count;
    const unsigned* blendIndices_values;
    unsigned        blendIndices_count;
};

struct RemixMeshInfoSurfaceTriangles {
    const RemixHardcodedVertex* vertices_values;
    unsigned long long          vertices_count;
    const unsigned*             indices_values;
    unsigned long long          indices_count;
    unsigned                    skinning_hasvalue;
    RemixMeshInfoSkinning       skinning_value;
    void*                       material;          // remixapi_MaterialHandle
};

struct RemixMeshInfo {
    unsigned                             sType;
    void*                                pNext;
    unsigned long long                   hash;
    const RemixMeshInfoSurfaceTriangles* surfaces_values;
    unsigned                             surfaces_count;
};

struct RemixTransform { float matrix[3][4]; };

struct RemixInstanceInfoBoneTransformsEXT {
    unsigned              sType;
    void*                 pNext;
    const RemixTransform* boneTransforms_values;
    unsigned              boneTransforms_count;
};

struct RemixInstanceInfo {
    unsigned       sType;
    void*          pNext;
    unsigned       categoryFlags;
    void*          mesh;                 // remixapi_MeshHandle
    RemixTransform transform;
    unsigned       doubleSided;
};

// ---------------------------------------------------------------------------------------------
// STEP 2 - one material, one mesh, one instance, submitted ENTIRELY through the Remix API.
//
// This proves the thing every later step depends on: that geometry we describe ourselves appears
// in the path-traced image, at a position we chose, without any D3D9 draw behind it. Nothing about
// the game's rendering is touched - this is pure addition, which is the whole point of the
// architecture.
//
// A glowing GREEN cube, deliberately:
//   - green because magenta already means "Remix ignored material" in this project and would be
//     ambiguous the moment it appeared;
//   - EMISSIVE because a lit-only object could be invisible for lighting reasons and would then
//     be indistinguishable from "the API did nothing", which is exactly the confusion step 2
//     exists to avoid;
//   - 3 units in front of the camera, so it cannot be missed and cannot be mistaken for a piece
//     of the world.
//
// The material carries NO textures. remixapi_Path is a wchar_t FILE PATH, not a D3D9 texture, so
// texturing API meshes from the game's own atlases is its own problem - deliberately not part of
// step 2.
unsigned char g_remixCubeVerts[24 * sizeof(RemixHardcodedVertex)];
unsigned g_remixCubeIdx[36];
void* g_remixMaterial = nullptr;
void* g_remixMesh = nullptr;
bool g_remixCubeBuilt = false, g_remixCubeFailed = false, g_remixCubeTextured = false;
bool g_remixCubeRebuilt = false;
// THE HASH IS THE IDENTITY. Remix keys registration on remixapi_MaterialInfo::hash and
// remixapi_MeshInfo::hash, and re-registering the same hash with different contents is refused -
// its log says so outright: "Ignoring repeated material registration (handle=...)", where the
// handle IS the hash. The rebuilt cube reused both hashes verbatim, so the textured material was
// never registered; the old one was handed back, and destroying "the old one" then destroyed the
// object the new handle referred to. That is why the cube disappeared entirely.
// A generation counter makes each rebuild a genuinely distinct object.
unsigned g_remixBuildGen = 0;
unsigned g_remixCubeCreateRc = 0xFFFFFFFF, g_remixCubeMeshRc = 0xFFFFFFFF;
unsigned g_remixDrawCalls = 0, g_remixDrawFails = 0, g_remixDrawLastRc = 0;
// STEP 3a handles. Declared HERE, not beside the rest of the step-3a state, because the
// per-frame DrawInstance tick lives in this block and runs long before SkinnedVertex - which the
// captured-vertex vector depends on - is even declared. The two halves of step 3a therefore sit
// on opposite sides of that type, and only the handles can come early.
bool g_apiCharBuilt = false, g_apiCharFailed = false;
void* g_apiCharMesh = nullptr;

// ---------------------------------------------------------------------------------------------
// STEP 3e - the WHOLE character, not just the body.
//
// From the 2026-08-31 frame dump the player is the body (vb=49186410, 7977 verts) PLUS about
// nineteen separate meshes on their own vertex buffers - head, hair, clothing, accessories. The
// pipeline that now works for one buffer is repeated for each: capture, build, file it away, reset,
// and pick up the next buffer on a later frame.
//
// Each finished mesh needs its OWN storage, because remixapi_MeshInfoSurfaceTriangles keeps
// POINTERS to the vertex and index arrays - the lesson that cost a run when those arrays were
// locals. Reusing one set of buffers for the next build would free the previous mesh's data out
// from under Remix in exactly the same way, one level up.
constexpr unsigned kMaxApiMeshes = 24;

// STEP 3c - LIVE. A frozen character is not a finished one.
//
// Remix skins for us: remixapi_MeshInfoSkinning carries the blend weights and indices with the
// mesh, and REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BONE_TRANSFORMS_EXT carries up to 256 bone
// transforms with the INSTANCE, per frame. SR3 needs 58. So the mesh is uploaded ONCE in its bind
// pose and only the bones and objTM move afterwards - no per-frame CreateMesh, and the shim's own
// CPU skinning becomes unnecessary for anything on the API.
//
// The layouts line up exactly, which is why this is a copy and not a conversion: SR3's bone palette
// is a row-major float3x4 in three constant registers per bone, translation in the fourth
// component of each row, and remixapi_Transform is float[3][4] with translation in the last
// column - the same thing the instance transform already proved.
constexpr unsigned kMaxApiBones = 64;    // SR3's palette is 64; the API allows 256

struct ApiDoneMesh {
    void* mesh;
    float objTM[3][4];
    unsigned tris, surfaces;
    IDirect3DVertexBuffer9* vb;
    // Refreshed every frame the game draws this part, and read at Present.
    unsigned boneCount;
    unsigned boneReg;
    float bones[kMaxApiBones][12];
    bool haveBones;
};
ApiDoneMesh g_apiDone[kMaxApiMeshes];
unsigned g_apiDoneCount = 0;
unsigned g_apiDoneTris = 0;
// The player's parts all share one objTM. Matching against the first captured mesh's translation
// is what keeps NPCs and props out of the set.
float g_apiRefPos[3] = {0, 0, 0};
bool g_apiHaveRef = false;
// Vertex buffers whose build FAILED. Without this a single bad part would set the failure flag
// permanently and the other nineteen would never be captured - one accessory with an index format
// we cannot read would cost the whole character. Failures skip the buffer and move on.
IDirect3DVertexBuffer9* g_apiSkipVB[kMaxApiMeshes];
unsigned g_apiSkipCount = 0;


// objTM, captured with the vertices. MEASURED 2026-08-31: the captured mesh's centroid is
// (-0.0 1.2 0.0) with a 0.56 x 1.86 x 1.86-tall extent while the camera sits at (90.8 145.4 21.4).
// A correctly sized character AT THE WORLD ORIGIN. So the CPU-skinned vertices are in OBJECT
// space, not world space - the claim this feature was built on was wrong. The frame dump says so
// too and I misread it: its "bind=... at(-0.00 1.19 -0.01)" is what our mesh matches, not the
// main line's world at(96.9 145.7 29.7).
//
// SR3's objTM is a row-major float3x4 held in 3 constant registers - row r is m[r*4..r*4+3] with
// the translation in the fourth component - which is EXACTLY remixapi_Transform's float[3][4]
// with translation in the last column. It maps across with a straight copy.
float g_apiCharObjTM[3][4] = {{1,0,0,0},{0,1,0,0},{0,0,1,0}};
unsigned g_apiCharDrawCalls = 0, g_apiCharDrawFails = 0, g_apiCharLastRc = 0;

// Widened for remixapi_Path, which is a wchar_t*.
wchar_t g_atlasDdsPathW[MAX_PATH] = {0};

bool RemixBuildCube() {
    ++g_remixBuildGen;
    typedef unsigned (__stdcall *PFN_CreateMaterial)(const RemixMaterialInfo*, void**);
    typedef unsigned (__stdcall *PFN_CreateMesh)(const RemixMeshInfo*, void**);
    const PFN_CreateMaterial createMaterial =
        reinterpret_cast<PFN_CreateMaterial>(g_remix.CreateMaterial);
    const PFN_CreateMesh createMesh = reinterpret_cast<PFN_CreateMesh>(g_remix.CreateMesh);
    if (!createMaterial || !createMesh) return false;

    // The opaque extension carries the SURFACE properties. Chained through pNext, because the
    // base MaterialInfo describes textures and emission only - it has no albedo constant, which
    // is why the first build of this cube came out grey/black with a perfectly working API.
    RemixMaterialInfoOpaqueEXT opaque{};
    opaque.sType = kRemixStructMaterialInfoOpaqueEXT;
    opaque.pNext = nullptr;
    opaque.albedoConstant = {0.05f, 1.0f, 0.05f};
    opaque.opacityConstant = 1.0f;
    opaque.roughnessConstant = 0.4f;
    opaque.metallicConstant = 0.0f;
    opaque.anisotropy = 0.0f;
    // ALPHA TEST. Measured 2026-08-31: with this struct zero-initialised the cube was INVISIBLE
    // while still casting green light - the material was live and emitting, but every pixel of
    // its surface was discarded. alphaTestType is a raw int in remix_c.h; Remix mirrors
    // VkCompareOp, where 0 is NEVER and 7 is ALWAYS, and the runtime carries the default string
    // "AlphaTestType alpha_test_type = Always". A zeroed field therefore meant "never pass".
    //
    // Note WHY the first cube looked better: it chained no opaque EXT at all, so Remix applied
    // its own defaults. Supplying the extension replaces those defaults wholesale - inside this
    // struct a zero is an INSTRUCTION, not an absence. Every field below is set deliberately for
    // that reason, even where the value happens to be zero.
    opaque.alphaTestType = 7;             // ALWAYS
    opaque.alphaReferenceValue = 0;
    opaque.useDrawCallAlphaState = 0;     // there is no draw call behind this mesh
    opaque.blendType_hasvalue = 0;
    opaque.blendType_value = 0;
    opaque.invertedBlend = 0;
    opaque.thinFilmThickness_hasvalue = 0;
    opaque.alphaIsThinFilmThickness = 0;
    opaque.displaceIn = 0.0f;
    opaque.displaceOut = 0.0f;
    opaque.roughnessTexture = nullptr;
    opaque.metallicTexture = nullptr;
    opaque.heightTexture = nullptr;

    RemixMaterialInfo mi{};
    mi.sType = kRemixStructMaterialInfo;
    mi.pNext = &opaque;
    mi.hash = 0x5233C0BE00000001ull + g_remixBuildGen;
    // Emission kept as well as albedo, so the two are tested at once: a LIT green cube proves
    // albedoConstant landed, a GLOWING one proves emission did, and grey again would mean neither
    // constant is honoured and the problem is not which struct carries them.
    // THE TEXTURE TEST. If the atlas DDS exists, name it as the albedo. A cube wearing the
    // character atlas proves the whole path: the game's own texture -> disk -> an API material.
    // Emission is dropped to zero in that case, because a glowing surface would wash out the very
    // thing being checked.
    if (g_atlasDdsReady) {
        MultiByteToWideChar(CP_ACP, 0, g_atlasDdsPath, -1, g_atlasDdsPathW, MAX_PATH);
        mi.albedoTexture = g_atlasDdsPathW;
        mi.emissiveIntensity = 0.0f;
        mi.emissiveColorConstant = {0.0f, 0.0f, 0.0f};
        opaque.albedoConstant = {1.0f, 1.0f, 1.0f};   // white, so the texture is not tinted
    } else {
        mi.emissiveIntensity = 200.0f;
        mi.emissiveColorConstant = {0.05f, 1.0f, 0.05f};
    }
    mi.spriteSheetRow = 1;
    mi.spriteSheetCol = 1;
    mi.spriteSheetFps = 0;
    mi.filterMode = 1;
    mi.wrapModeU = 1;
    mi.wrapModeV = 1;
    g_remixCubeCreateRc = createMaterial(&mi, &g_remixMaterial);
    if (g_remixCubeCreateRc != 0 || !g_remixMaterial) {
        Log("remix api: CreateMaterial FAILED rc=%u (%s)", g_remixCubeCreateRc,
            RemixErrName(g_remixCubeCreateRc));
        return false;
    }

    // 24 vertices - four per face, so each face gets its own normal rather than a shared one.
    static const float kFaceN[6][3] = {{0,0,-1},{0,0,1},{-1,0,0},{1,0,0},{0,-1,0},{0,1,0}};
    static const float kFaceV[6][4][3] = {
        {{-1,-1,-1},{ 1,-1,-1},{ 1, 1,-1},{-1, 1,-1}},
        {{ 1,-1, 1},{-1,-1, 1},{-1, 1, 1},{ 1, 1, 1}},
        {{-1,-1, 1},{-1,-1,-1},{-1, 1,-1},{-1, 1, 1}},
        {{ 1,-1,-1},{ 1,-1, 1},{ 1, 1, 1},{ 1, 1,-1}},
        {{-1,-1, 1},{ 1,-1, 1},{ 1,-1,-1},{-1,-1,-1}},
        {{-1, 1,-1},{ 1, 1,-1},{ 1, 1, 1},{-1, 1, 1}},
    };
    RemixHardcodedVertex* v = reinterpret_cast<RemixHardcodedVertex*>(g_remixCubeVerts);
    for (int f = 0; f < 6; ++f) {
        for (int c = 0; c < 4; ++c) {
            RemixHardcodedVertex& hv = v[f * 4 + c];
            hv.position[0] = kFaceV[f][c][0] * 0.5f;
            hv.position[1] = kFaceV[f][c][1] * 0.5f;
            hv.position[2] = kFaceV[f][c][2] * 0.5f;
            hv.normal[0] = kFaceN[f][0];
            hv.normal[1] = kFaceN[f][1];
            hv.normal[2] = kFaceN[f][2];
            hv.texcoord[0] = (c == 1 || c == 2) ? 1.0f : 0.0f;
            hv.texcoord[1] = (c >= 2) ? 1.0f : 0.0f;
            hv.color = 0xFF20FF20u;
        }
        const unsigned b = static_cast<unsigned>(f * 4);
        unsigned* ix = &g_remixCubeIdx[f * 6];
        ix[0] = b; ix[1] = b + 1; ix[2] = b + 2;
        ix[3] = b; ix[4] = b + 2; ix[5] = b + 3;
    }

    RemixMeshInfoSurfaceTriangles surf{};
    surf.vertices_values = v;
    surf.vertices_count = 24;
    surf.indices_values = g_remixCubeIdx;
    surf.indices_count = 36;
    surf.skinning_hasvalue = 0;
    surf.material = g_remixMaterial;

    RemixMeshInfo meshInfo{};
    meshInfo.sType = kRemixStructMeshInfo;
    meshInfo.hash = 0x5233C0BE10000002ull + g_remixBuildGen;
    meshInfo.surfaces_values = &surf;
    meshInfo.surfaces_count = 1;
    g_remixCubeMeshRc = createMesh(&meshInfo, &g_remixMesh);
    if (g_remixCubeMeshRc != 0 || !g_remixMesh) {
        Log("remix api: CreateMesh FAILED rc=%u (%s)", g_remixCubeMeshRc,
            RemixErrName(g_remixCubeMeshRc));
        return false;
    }
    Log("remix api: STEP 2 built gen %u - material %p, mesh %p, hashes %08X/%08X (%s)",
        g_remixBuildGen, g_remixMaterial, g_remixMesh,
        static_cast<unsigned>(mi.hash), static_cast<unsigned>(meshInfo.hash),
        g_atlasDdsReady ? "albedo = the character atlas from disk, emission off"
                        : "albedoConstant green + emissive green");
    return true;
}

// Once per frame, from Present. DrawInstance is a per-frame submission like a draw call, not a
// persistent registration, so it has to be re-issued every frame or the cube exists for one.
// NOTE, 2026-09-07: this function submits BOTH the step-2 test cube AND every built character
// mesh, and the early return below is gated on remixApiTestCube. Setting that to 0 to remove the
// floating cube therefore also stopped the character being submitted - `DrawInstance 0 calls`
// against 10 built meshes and 21,990 triangles, silently, for several runs.
//
// The cube is a test artefact and the character is not. They must not share a switch. The
// character's own gate is remixApiCharacter, and that is what decides it now.
void RemixApiTestTick() {
    if (!g_remixMeshReady) return;
    const bool wantCube = g_settings.remixApiTestCube && !g_remixCubeFailed;
    const bool wantCharacter = g_settings.remixApiCharacter && g_apiDoneCount > 0;
    if (!wantCube && !wantCharacter) return;
    // Wait for the atlas before building, so the cube can wear it. Bounded: if no atlas has
    // appeared by ~1200 frames the cube is built with the green constant anyway, so a missing
    // atlas degrades the test rather than removing it.
    //
    // AND REBUILD ONCE IF THE ATLAS ARRIVES LATER. Measured 2026-08-31: the DDS was written
    // correctly (11,184,940 bytes - exactly a 2048x1024 BGRA8 12-level mip chain plus a 128-byte
    // header) but the cube had already been built green at frame 1200, because the character
    // atlas is not captured until a character is actually on screen, which is well after the
    // loading screens the first 1200 frames are spent in. A one-shot build against a resource
    // that appears on the game's schedule is a race, and the bounded wait only hid it.
    if (!g_remixCubeBuilt) {
        if (!g_atlasDdsReady && g_frames < 1200) return;
        if (!RemixBuildCube()) { g_remixCubeFailed = true; return; }
        g_remixCubeBuilt = true;
        g_remixCubeTextured = g_atlasDdsReady;
    } else if (g_atlasDdsReady && !g_remixCubeTextured && !g_remixCubeRebuilt) {
        g_remixCubeRebuilt = true;
        // Release the untextured pair first. Safe here: this runs from Present BEFORE this
        // frame's DrawInstance, so nothing has been submitted against them yet this frame.
        // NOT destroyed. Two objects one frame apart, on a test path, are not worth the risk:
        // the previous build destroyed a handle that turned out to alias the new registration and
        // left nothing on screen at all. Leaking one material and one mesh, once, is the cheaper
        // mistake - and with distinct hashes the new pair is genuinely separate.
        void* oldMesh = g_remixMesh;
        void* oldMaterial = g_remixMaterial;
        g_remixMesh = nullptr;
        g_remixMaterial = nullptr;
        if (RemixBuildCube()) {
            g_remixCubeTextured = true;
            Log("remix api: cube REBUILT with the character atlas (it was built green at frame "
                "1200, before any character had been seen)");
        } else {
            // Put the working pair back rather than leaving no cube at all.
            g_remixMesh = oldMesh;
            g_remixMaterial = oldMaterial;
            Log("remix api: textured rebuild FAILED - keeping the green cube");
            return;
        }
    }
    if (!g_remixLastViewValid) return;

    // Camera world position and forward axis out of the inverse view.
    D3DMATRIX invView;
    if (!Invert(g_remixLastView, invView) || !IsFinite(invView)) return;
    const float px = invView._41 + invView._31 * 3.0f;
    const float py = invView._42 + invView._32 * 3.0f;
    const float pz = invView._43 + invView._33 * 3.0f;

    RemixInstanceInfo ii{};
    ii.sType = kRemixStructInstanceInfo;
    ii.categoryFlags = 0;
    ii.mesh = g_remixMesh;
    ii.doubleSided = 1;
    // remixapi_Transform is float[3][4] - three rows of four, translation in the last column.
    ii.transform.matrix[0][0] = 1.0f; ii.transform.matrix[0][3] = px;
    ii.transform.matrix[1][1] = 1.0f; ii.transform.matrix[1][3] = py;
    ii.transform.matrix[2][2] = 1.0f; ii.transform.matrix[2][3] = pz;

    typedef unsigned (__stdcall *PFN_DrawInstance)(const RemixInstanceInfo*);
    const PFN_DrawInstance drawInstance =
        reinterpret_cast<PFN_DrawInstance>(g_remix.DrawInstance);
    const unsigned rc = drawInstance(&ii);
    ++g_remixDrawCalls;
    g_remixDrawLastRc = rc;
    if (rc != 0) ++g_remixDrawFails;

    // STEP 3a. Identity transform: the captured vertices are already in world space.
    for (unsigned m = 0; m < g_apiDoneCount; ++m) {
        RemixInstanceInfo ci{};
        ci.sType = kRemixStructInstanceInfo;
        ci.categoryFlags = 0;
        ci.mesh = g_apiDone[m].mesh;
        ci.doubleSided = 1;
        memcpy(ci.transform.matrix, g_apiDone[m].objTM, sizeof(ci.transform.matrix));
        RemixInstanceInfoBoneTransformsEXT boneExt{};
        RemixTransform boneXf[kMaxApiBones];
        if (g_apiDone[m].haveBones && g_apiDone[m].boneCount) {
            const unsigned n = g_apiDone[m].boneCount;
            for (unsigned b = 0; b < n; ++b)
                memcpy(boneXf[b].matrix, g_apiDone[m].bones[b], sizeof(boneXf[b].matrix));
            boneExt.sType = kRemixStructInstanceInfoBoneTransformsEXT;
            boneExt.pNext = nullptr;
            boneExt.boneTransforms_values = boneXf;
            boneExt.boneTransforms_count = n;
            ci.pNext = &boneExt;
        }
        // STAND IT BESIDE THE REAL CHARACTER, not inside it.
        //
        // The API copy was placed at (96.9 146.9 29.7) while the game's own character stood at
        // (96.9 145.7 29.7) - the same spot. Two copies of the same surfaces occupying one volume
        // in a path traced scene z-fight, and that reads exactly as "the whole body with half the
        // polygons missing at random", which is what the user saw. Every number said the mesh was
        // complete - 36 of 36 slots, 19,359 triangles, 0 out-of-range - because it WAS.
        //
        // Offsetting is a DIAGNOSTIC, not the finished behaviour: step 3d removes the game's copy
        // entirely, at which point the offset goes back to zero. Until then the two must not
        // occupy the same space or neither can be judged.
        ci.transform.matrix[0][3] += g_settings.remixApiCharacterOffset;
        const unsigned crc = drawInstance(&ci);
        ++g_apiCharDrawCalls;
        g_apiCharLastRc = crc;
        if (crc != 0) ++g_apiCharDrawFails;
    }
}

void ApplyCameraOnly(IDirect3DDevice9* dev) {
    if (g_camLastFrameSeen != g_frames) { g_camLastFrameSeen = g_frames; g_camCandCount = 0; }
    ++g_camOnlyDraws;
    D3DMATRIX view = FromRegisters(&g_vsConst[kRegWorld2View][0], 3);
    const D3DMATRIX viewProj = FromRegisters(&g_vsConst[kRegProjTM][0], 4);
    D3DMATRIX proj{};
    if (!IsFinite(view) || !IsFinite(viewProj) || !DeriveProjection(view, viewProj, proj)) {
        ++g_camOnlyRefused;
        return;
    }
    // Census BEFORE the decision, so the log describes every camera the frame contains -
    // including, and especially, the ones declined here.
    const unsigned cand = NoteCamCandidate(view, proj);
    const bool isMain = !g_settings.cameraMainViewOnly || IsMainSceneCamera(view, proj);
    if (cand < kMaxCamCandidates && isMain) g_camCand[cand].accepted = true;
    if (!isMain) return;

    // Only on a real change. The camera is per-view, not per-draw, and a SetTransform per draw
    // would be thousands of bridge round trips a frame for no effect.
    if (g_camOnlyHave && memcmp(&view, &g_camOnlyView, sizeof view) == 0 &&
        memcmp(&proj, &g_camOnlyProj, sizeof proj) == 0)
        return;
    g_camOnlyView = view;
    g_camOnlyProj = proj;
    g_camOnlyHave = true;
    const bool wasInternal = g_internal;
    g_internal = true;
    // WORLD is identity: with vertex capture Remix reconstructs already-transformed vertices, so
    // the object matrix is baked in before it ever sees them.
    g_origSetTransform(dev, D3DTS_WORLD, &kIdentity);
    g_origSetTransform(dev, D3DTS_VIEW, &view);
    g_origSetTransform(dev, D3DTS_PROJECTION, &proj);
    g_internal = wasInternal;
    ++g_camOnlySets;
}


// ---------------------------------------------------------------------------------------------
// SR3's own render command blocks - read, never written
//
// Disassembled 2026-08-30 (docs/engine-map.md). SR3 does not call D3D9 from its game logic: a
// dedicated RENDER THREAD consumes command blocks off a ring and dispatches each command through
// a 74-entry table.
//
//   0x0049DE20  outer loop: take the next 24-byte ring descriptor, set block start/end/read ptr
//   0x0049DED2  inner loop: opcode = *readPtr; if (opcode < 74) table[opcode](); repeat
//
// The globals it keeps while doing so are the thing this shim has been approximating all along:
//
//   0x02E5D648  START of the command block currently executing   <- the draw's PASS IDENTITY
//   0x02E5D644  the last opcode dispatched
//   0x03395EA4  the engine's own draw kill-switch, tested by all four draw handlers
//
// Every D3D9 call the shim sees arrives from a handler running inside one block. Two draws in the
// same block are in the same submission unit; two draws in different blocks are not. That is a
// FACT the engine writes down, not an inference from samplers, render targets or shader outputs -
// which is what `screenSpaceMode`, `skipDeferredGBuffer`, `compositeToTexturePass` and every
// prepass test have each been trying to reconstruct.
//
// SAFETY. These are absolute addresses. The exe is RELOCS_STRIPPED with no DYNAMIC_BASE and always
// loads at 0x400000 - but docs/engine-map.md records a base mismatch that was caught by a runtime
// check before anything was written, and the rule it left behind is "verify a static map against
// the live process before trusting it". So the base is verified once, the module name is checked,
// and every address is confirmed to sit inside a committed readable page. If any of that fails the
// feature disables itself permanently. Nothing here writes.
constexpr uintptr_t kAddrBlockStart   = 0x02E5D648;
constexpr uintptr_t kAddrBlockEnd     = 0x02E5D64C;
constexpr uintptr_t kAddrLastOpcode   = 0x02E5D644;
constexpr uintptr_t kAddrDrawDisabled = 0x03395EA4;
constexpr uintptr_t kExpectedBase     = 0x00400000;

bool g_cbReady = false, g_cbChecked = false;

bool CommandBlocksAvailable() {
    if (g_cbChecked) return g_cbReady;
    g_cbChecked = true;
    g_cbReady = false;
    HMODULE h = GetModuleHandleW(nullptr);
    wchar_t path[MAX_PATH]{};
    if (!h || !GetModuleFileNameW(h, path, MAX_PATH)) return false;
    const wchar_t* leaf = wcsrchr(path, L'\\');
    leaf = leaf ? leaf + 1 : path;
    if (_wcsicmp(leaf, L"SaintsRowTheThird.exe") != 0) {
        Log("command blocks: host is not SaintsRowTheThird.exe - disabled");
        return false;
    }
    if (reinterpret_cast<uintptr_t>(h) != kExpectedBase) {
        Log("command blocks: module base 0x%08X, expected 0x%08X - DISABLED, the static map does "
            "not apply", static_cast<unsigned>(reinterpret_cast<uintptr_t>(h)),
            static_cast<unsigned>(kExpectedBase));
        return false;
    }
    for (uintptr_t a : {kAddrBlockStart, kAddrLastOpcode, kAddrDrawDisabled}) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof(mbi)) ||
            mbi.State != MEM_COMMIT ||
            !(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                             PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE))) {
            Log("command blocks: 0x%08X is not committed readable - disabled",
                static_cast<unsigned>(a));
            return false;
        }
    }
    g_cbReady = true;
    Log("command blocks: verified. base 0x%08X, block=0x%08X opcode=0x%08X killswitch=0x%08X",
        static_cast<unsigned>(kExpectedBase), static_cast<unsigned>(kAddrBlockStart),
        static_cast<unsigned>(kAddrLastOpcode), static_cast<unsigned>(kAddrDrawDisabled));
    return true;
}

struct CmdBlock {
    unsigned start = 0;
    unsigned draws = 0;
    unsigned converted = 0;
    unsigned firstOp = 0xFFFFFFFF, lastOp = 0;
};
constexpr unsigned kMaxCmdBlocks = 64;
CmdBlock g_cmdBlocks[kMaxCmdBlocks];
unsigned g_cmdBlockCount = 0;
unsigned g_cmdBlockOverflow = 0;
unsigned g_cbBlocksAccum = 0, g_cbFramesSeen = 0, g_cbDrawsAttributed = 0;
unsigned g_cbKillSwitchSeen = 0;
unsigned g_curBlockStart = 0, g_curOpcode = 0;

// Called at the top of every draw, before anything classifies it.

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
    if (g_settings.skipDeferredGBuffer && g_curPS.isPixelShader && g_curPS.rtCount > 1 &&
        g_curLayout.skinned) {
        ++g_gbufferHidden;
        return Because("deferred G-buffer pass: writes >1 render target, so it carries no colour",
                       Disp::PassThrough);
    }

    return Disp::Convert;
}

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
    g_origSetTransform(dev, D3DTS_PROJECTION, &kIdentity);
    g_internal = false;
    ++g_demotedToUI;
    return true;
}

void EndUIDemote(IDirect3DDevice9* dev, bool active) {
    if (!active) return;
    g_internal = true;
    if (g_haveAppliedProj) {
        g_origSetTransform(dev, D3DTS_PROJECTION, &g_appliedProj);
    } else {
        // Nothing known to restore yet; make sure the next converted draw writes its own.
        g_haveAppliedProj = false;
    }
    g_internal = false;
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
    if (!g_settings.remixShortUV || g_curLayout.skinned ||
        g_curLayout.texcoordType != D3DDECLTYPE_SHORT2 || !g_curDecl)
        return false;
    IDirect3DVertexBuffer9* uvBuf = UvBufferFor(dev);
    IDirect3DVertexDeclaration9* alt = uvBuf ? FloatUVDeclaration(dev, g_curDecl) : nullptr;
    if (!uvBuf || !alt) return false;
    g_origSetVertexDeclaration(dev, alt);
    g_origSetStreamSource(dev, g_uvStream, uvBuf, 0, kUvBytesPerVertex);
    // Our entries are indexed per vertex exactly as stream 0 is, so under instancing the two need
    // the same frequency. The default would read one uv per INSTANCE instead of one per vertex.
    if (g_instancedDraw && !deinstance) {
        g_origSetStreamSourceFreq(dev, g_uvStream, D3DSTREAMSOURCE_INDEXEDDATA | 1);
        scope.uvFreq = true;
    }
    scope.decl = g_curDecl;
    scope.uvStream = true;
    ++g_uvStreamDraws;
    return true;
}

Disp BeginFFP(IDirect3DDevice9* dev, FFPScope& scope) {
    const Disp d = Classify(dev);
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
        if (g_settings.cameraOnly && g_settings.cameraOnlyFloatUV)
            InstallFloatUV(dev, scope, false);
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

    scope.vs = g_lastVS;
    scope.ps = g_lastPS;
    scope.active = true;

    g_internal = true;
    g_origSetVertexShader(dev, nullptr);
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

    ApplyTransforms(dev, world, view, proj);
    SetupTextureStages(dev);
    if (!g_lightingSetUp) { SetupLighting(dev); g_lightingSetUp = true; }

    // Only stages we actually change need restoring afterwards. Restoring all eight
    // unconditionally cost eight SetTexture calls per converted draw - at ~1,500 draws a frame
    // that is 12,000 bridge round-trips the state shadow never gets to see.
    IDirect3DBaseTexture9* albedo = EffectiveAlbedo();
    // SR3's character atlas is a RENDER TARGET, and Remix hashes only what the game
    // uploads - so it receives an all-zero texture and the character renders black.
    // Re-uploaded here through UpdateTexture, which is the path Remix does hash.
    albedo = RemixReadableAlbedo(dev, albedo);

    // Keep each mesh on the texture it was first drawn with. Keyed on the vertex buffer plus
    // the base vertex, which together identify a mesh inside SR3's shared buffers; the pointer
    // alone would merge every mesh sharing a buffer onto one texture.
    // The player's chosen clothing colours, evaluated per texel into a texture of our own.
    if (IDirect3DBaseTexture9* cloth = ClothAlbedo(dev)) albedo = cloth;
    // Hair takes the same route. Its ranked albedo is the Diffuse_Map, which for hair is
    // DIRECTIONAL data rather than colour - binding it is what made hair render as a mask.
    else if (IDirect3DBaseTexture9* hair = HairAlbedo(dev)) albedo = hair;

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
    return Disp::Convert;
}

void EndFFP(IDirect3DDevice9* dev, const FFPScope& scope) {
    if (!scope.active) return;
    g_internal = true;
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
    g_origSetVertexShader(dev, scope.vs);
    g_origSetPixelShader(dev, scope.ps);
    g_ffpActive = false;
    g_internal = false;
}

// ---------------------------------------------------------------- light injection

// The light's parameters are in view space (its shader reconstructs surface positions along a
// view ray), so they are moved to world space with the inverse of the captured camera view.
// Every light reuses D3D9 slot 0: Remix accumulates game lights per draw call, so one slot
// carries an unbounded number and the 8-simultaneous-light cap never applies.
void EmitLight(IDirect3DDevice9* dev) {
    if (!g_settings.injectLights || g_curPS.light.kind == LightShader::NotLight) return;
    if (!g_haveCamera) return;

    D3DMATRIX invView;
    if (!Invert(g_cameraView, invView)) return;

    const LightShader& ls = g_curPS.light;
    if (ls.regPos < 0 || ls.regPos >= static_cast<int>(kMaxPsConst)) return;
    if (ls.regColor < 0 || ls.regColor >= static_cast<int>(kMaxPsConst)) return;

    const float* pos = g_psConst[ls.regPos];
    const float* col = g_psConst[ls.regColor];

    D3DLIGHT9 light{};
    light.Diffuse.r = col[0] * g_settings.lightScale;
    light.Diffuse.g = col[1] * g_settings.lightScale;
    light.Diffuse.b = col[2] * g_settings.lightScale;
    light.Specular = light.Diffuse;

    if (ls.kind == LightShader::Directional) {
        light.Type = D3DLIGHT_DIRECTIONAL;
        const D3DVECTOR dir = TransformDir(pos, invView);   // IR_Light_Pos holds a direction
        light.Direction = {-dir.x, -dir.y, -dir.z};
    } else {
        const float* info = (ls.regInfo >= 0) ? g_psConst[ls.regInfo] : nullptr;
        const float outer = info ? info[2] : 10.0f;         // (falloff, inner, outer, -)
        light.Position = TransformPoint(pos, invView);
        light.Range = (outer > 0.01f ? outer : 10.0f) * g_settings.lightRangeScale;
        light.Attenuation2 = 1.0f / (light.Range * light.Range * 0.25f + 1e-4f);

        if (ls.kind == LightShader::Spot && ls.regDir >= 0 && ls.regSpot >= 0) {
            light.Type = D3DLIGHT_SPOT;
            light.Direction = TransformDir(g_psConst[ls.regDir], invView);
            const float* spot = g_psConst[ls.regSpot];
            float cosOuter = spot[1];   // .y = cos(outer half-angle), .z = 1/(cosIn - cosOut)
            float cosInner = (std::fabs(spot[2]) > 1e-6f) ? cosOuter + 1.0f / spot[2] : cosOuter;
            cosOuter = max(-1.0f, min(1.0f, cosOuter));
            cosInner = max(cosOuter, min(1.0f, cosInner));
            light.Phi = 2.0f * std::acos(cosOuter);
            light.Theta = 2.0f * std::acos(cosInner);
            light.Falloff = 1.0f;
        } else {
            light.Type = D3DLIGHT_POINT;
        }
    }

    // ONE SLOT PER LIGHT, not one slot for every light.
    //
    // This used to be `SetLight(dev, 0, ...)` for all of them: every light in the frame - ~46 of
    // them - written through index 0, one after another. A D3D9 light index is the only handle
    // Remix has for correlating a light with its previous-frame self, so rewriting slot 0 forty
    // times a frame tells Remix that one light teleported forty times, and next frame it does it
    // again with different values. Nothing can be matched across frames.
    //
    // That single line explains all three reported symptoms:
    //   - "most lights, especially further from the camera, change hashes a lot" - a light's hash
    //     is derived from properties that, at slot 0, belong to a different light every call;
    //   - "the flashlight leaves a trail of lights mid air that go away after 1 or 2 seconds" -
    //     each frame's flashlight is a NEW light to Remix, and the previous one is kept alive for
    //     a few frames rather than being recognised as the same light having moved;
    //   - the flashing, at least in part, since unstable lights destabilise everything they lit.
    //
    // Emission order is the identity: the engine submits its light volumes in a consistent order
    // within a frame, so light N is the same light as light N was last frame. That is imperfect -
    // a light appearing mid-list shifts everything after it by one - but it is enormously better
    // than every light sharing one slot, and it costs nothing.
    // MEASURED 2026-08-19: checking only SetLight was not enough. D3D9 limits how many lights may
    // be simultaneously ACTIVE (D3DCAPS9::MaxActiveLights). Past that limit SetLight still
    // succeeds and LightEnable quietly fails, so the light is stored and never lit - and the
    // shotgun flashlight, which is emitted late in the frame's light list, stopped working
    // entirely while the counter still read a healthy 28 lights a frame.
    //
    // So the enable is checked too, and a light that cannot have its own slot falls back to slot
    // 0 rather than being dropped. That is the old shared-slot behaviour, but now it applies ONLY
    // to the overflow: the first N lights keep stable identities and everything beyond the
    // device limit is at least visible, which is strictly better than either extreme.
    g_internal = true;
    bool lit = false;
    if (g_lightSlot < g_maxLightSlots &&
        SUCCEEDED(g_origSetLight(dev, g_lightSlot, &light)) &&
        SUCCEEDED(g_origLightEnable(dev, g_lightSlot, TRUE))) {
        ++g_lightSlot;
        lit = true;
    } else if (SUCCEEDED(g_origSetLight(dev, 0, &light))) {
        g_origLightEnable(dev, 0, TRUE);
        ++g_lightOverflow;
        lit = true;
    }
    if (lit) ++g_lightsEmitted;
    g_internal = false;
}

// Turn off the slots this frame did not use. Without this a light that goes away stays enabled at
// its old position forever, which is the same trail by a different route.
void DisableUnusedLightSlots(IDirect3DDevice9* dev) {
    if (!g_settings.injectLights) return;
    g_internal = true;
    for (unsigned i = g_lightSlot; i < g_lightSlotsUsedLastFrame; ++i)
        g_origLightEnable(dev, i, FALSE);
    g_internal = false;
    g_lightSlotsUsedLastFrame = g_lightSlot;
    g_lightSlot = 0;
}

// ---------------------------------------------------------------- hooks

HRESULT WINAPI Hook_SetVertexShaderConstantF(IDirect3DDevice9* dev, UINT start,
                                             const float* data, UINT count) {
    const HRESULT hr = g_origSetVSConstF(dev, start, data, count);
    if (!data || !count || start + count > kMaxVsConst) return hr;

    memcpy(&g_vsConst[start][0], data, count * 4 * sizeof(float));

    const UINT end = start + count;
    if (start <= kRegProjTM && end >= kRegProjTM + 4) {
        Trace('P');
        g_viewProjDirty = true;
        // A projTM upload starts a new draw setup. Clear the world assumption so geometry that
        // does not write objTM gets identity rather than the previous object's matrix - unless
        // objTM was written earlier in this same setup, which some paths do.
        if (g_worldWritten && !g_worldWrittenSinceDraw) {
            g_worldWritten = false;
            g_worldDirty = true;
        }
        // The bone palette needs the same scoping objTM already has, and never had it.
        //
        // Measured 2026-08-25: a mesh whose objTM was IDENTICAL between two frames moved 0.75
        // units, because bone[0]'s translation went from (0.132, -0.939, -0.360) to (0, 0, 0).
        // The palette shadow persists across setups, so a draw whose own setup uploaded no
        // palette silently inherits the previous object's bones - and which object that is
        // depends on draw order, which depends on what is on screen.
        //
        // This is the identical failure the comment above describes for objTM, and the same
        // remedy applies: an object that did not publish a palette gets IDENTITY bones, so objTM
        // alone places it, rather than being posed by somebody else's skeleton.
        ++g_setupId;
        // Reject non-finite uploads: some passes reuse these registers for parameter blocks,
        // and a NaN projection poisons Remix's camera tracking for the whole frame.
        g_viewProjValid = true;
        for (int i = 0; i < 16 && g_viewProjValid; ++i)
            if (!std::isfinite(g_vsConst[kRegProjTM][i])) g_viewProjValid = false;
    }
    if (start <= kRegWorld2View && end >= kRegWorld2View + 3) {
        Trace('W');
        g_viewProjDirty = true;
    }
    if (start < kRegBonePalette + kBonesMax * kRegsPerBone && end > kRegBonePalette) {
        g_lastBoneUploadDraw = g_drawIndexThisFrame;
        g_lastBoneUploadFrame = g_frames;
        // How far the upload actually REACHED. The palette shadow persists across draws, so
        // registers past this point still hold whatever the previous object left there. A mesh
        // whose bone indices run past the upload is posed by another object's bones - which is
        // what "a car door flew to where that NPC's arm was" looks like. Recorded as a bone
        // count so it can be compared with a mesh's highest index directly.
        const unsigned reach = (end > kRegBonePalette) ? (end - kRegBonePalette) : 0u;
        g_lastBoneUploadBones = reach / kRegsPerBone;
        // Mark every bone this call touched. Partial calls accumulate instead of overwriting,
        // which is the flaw in g_lastBoneUploadBones above.
        g_paletteHistory[g_paletteHistoryPos % 24] =
            {g_drawIndexThisFrame, g_frames, start, end};
        ++g_paletteHistoryPos;
        const unsigned firstBone = (start > kRegBonePalette)
            ? (start - kRegBonePalette) / kRegsPerBone : 0u;
        const unsigned lastBone = (reach + kRegsPerBone - 1) / kRegsPerBone;
        for (unsigned b = firstBone; b < lastBone && b < static_cast<unsigned>(kBonesMax); ++b) {
            g_boneWrittenDraw[b] = g_drawIndexThisFrame;
            g_boneWrittenFrame[b] = g_frames;
            // How many bones the upload that wrote this one covered. A skeleton's size is a
            // fingerprint: a mesh that uses 1 bone but reads it from a 48-bone upload is reading
            // a CHARACTER's root bone, and every palette upload in this game starts at c52 so
            // bone 0 belongs to whoever wrote last.
            g_boneWrittenUploadBones[b] = g_lastBoneUploadBones;
            g_boneWrittenObjGen[b] = g_objGeneration;
        g_paletteSetupId = g_setupId;
        }
    }
    if (start <= kRegObjTM && end >= kRegObjTM + 3) {
        // Compared by VALUE, not merely by "a write happened": the game re-uploads an identical
        // objTM for consecutive draws of one object, and counting those as new objects would make
        // every part look foreign.
        const float t0 = g_vsConst[kRegObjTM][3], t1 = g_vsConst[kRegObjTM + 1][3],
                    t2 = g_vsConst[kRegObjTM + 2][3];
        if (t0 != g_lastObjTM[0] || t1 != g_lastObjTM[1] || t2 != g_lastObjTM[2]) {
            g_lastObjTM[0] = t0; g_lastObjTM[1] = t1; g_lastObjTM[2] = t2;
            ++g_objGeneration;
        }
        Trace('O');
        g_worldDirty = true;
        g_worldWritten = true;
        g_worldWrittenSinceDraw = true;
    }
    return hr;
}

HRESULT WINAPI Hook_SetPixelShaderConstantF(IDirect3DDevice9* dev, UINT start,
                                            const float* data, UINT count) {
    if (data)
        for (UINT i = 0; i < count && start + i < kMaxPsConst; ++i)
            memcpy(g_psConst[start + i], data + i * 4, sizeof(float) * 4);
    return g_origSetPSConstF(dev, start, data, count);
}

HRESULT WINAPI Hook_CreateVertexShader(IDirect3DDevice9* dev, const DWORD* function,
                                       IDirect3DVertexShader9** shader) {
    const HRESULT hr = g_origCreateVertexShader(dev, function, shader);
    if (SUCCEEDED(hr) && shader && *shader && function) {
        const LONGLONG t = Now();
        if (g_shaders.emplace(*shader, ReflectShader(function)).second) (*shader)->AddRef();
        g_otherMsThisFrame += MsSince(t);
        ++g_shadersCreatedThisFrame;
    }
    return hr;
}

HRESULT WINAPI Hook_SetVertexShader(IDirect3DDevice9* dev, IDirect3DVertexShader9* shader) {
    if (!g_internal) {
        const auto it = g_shaders.find(shader);
        g_curVS = (it != g_shaders.end()) ? it->second : ShaderInfo{};
        g_lastVS = shader;
        Trace(g_curVS.usesObjTM ? 'V' : 'v');
        // NOTE: g_worldWritten is deliberately NOT cleared here. See ComputeTransforms.
    }
    return g_origSetVertexShader(dev, shader);
}

HRESULT WINAPI Hook_CreatePixelShader(IDirect3DDevice9* dev, const DWORD* function,
                                      IDirect3DPixelShader9** shader) {
    const HRESULT hr = g_origCreatePixelShader(dev, function, shader);
    if (SUCCEEDED(hr) && shader && *shader && function) {
        const LONGLONG t = Now();
        ShaderInfo info = ReflectShader(function);
        info.isPixelShader = true;
        if (g_shaders.emplace(*shader, info).second) (*shader)->AddRef();
        g_otherMsThisFrame += MsSince(t);
        ++g_shadersCreatedThisFrame;
        if (info.light.kind != LightShader::NotLight && ++g_lightShadersSeen <= 6) {
            static const char* kNames[] = {"none", "point", "spot", "directional"};
            Log("light shader: %s (pos=c%d dir=c%d color=c%d info=c%d spot=c%d)",
                kNames[info.light.kind], info.light.regPos, info.light.regDir,
                info.light.regColor, info.light.regInfo, info.light.regSpot);
        }
    }
    return hr;
}

HRESULT WINAPI Hook_SetPixelShader(IDirect3DDevice9* dev, IDirect3DPixelShader9* shader) {
    if (!g_internal) {
        const auto it = g_shaders.find(shader);
        g_curPS = (it != g_shaders.end()) ? it->second : ShaderInfo{};
        g_lastPS = shader;
    }
    return g_origSetPixelShader(dev, shader);
}

HRESULT WINAPI Hook_SetTexture(IDirect3DDevice9* dev, DWORD stage,
                               IDirect3DBaseTexture9* texture) {
    if (!g_internal && stage < 8) g_curTexture[stage] = texture;
    return g_origSetTexture(dev, stage, texture);
}

HRESULT WINAPI Hook_CreateTexture(IDirect3DDevice9* dev, UINT w, UINT h, UINT levels,
                                  DWORD usage, D3DFORMAT format, D3DPOOL pool,
                                  IDirect3DTexture9** texture, HANDLE* shared) {
    const HRESULT hr = g_origCreateTexture(dev, w, h, levels, usage, format, pool, texture,
                                           shared);
    // Remember which textures are generated rather than authored, so they are never mistaken
    // for surface colour.
    if (SUCCEEDED(hr) && texture && *texture) {
        ++g_texturesCreatedThisFrame;
        // Every LARGE uncompressed texture, whatever its usage - so that if the character atlas
        // is created with parameters the filter below rejects, it still shows up here instead of
        // vanishing silently. This is the check the previous two builds did not have.
        if ((format == D3DFMT_X8R8G8B8 || format == D3DFMT_A8R8G8B8) &&
            w >= 1024 && h >= 512 && g_bigTexReports < 16) {
            ++g_bigTexReports;
            Log("BIG TEXTURE #%u created: %ux%u levels=%u fmt=%d pool=%d usage=0x%lX",
                g_bigTexReports, w, h, levels, static_cast<int>(format),
                static_cast<int>(pool), static_cast<unsigned long>(usage));
        }
        // Large uncompressed RENDER TARGETS are the candidate composite destinations - the step
        // before the dynamic texture the game fills with zeroes. Read once each, later.
        if ((format == D3DFMT_X8R8G8B8 || format == D3DFMT_A8R8G8B8) &&
            (usage & D3DUSAGE_RENDERTARGET) != 0 && w >= 1024 && h >= 512)
            NoteRenderTargetCandidate(*texture, w, h);
        // The texture vtable is patched from the FIRST texture this game creates, and candidate
        // atlases are recorded at CREATION.
        //
        // The previous version did both at the first character DRAW, and measured LockRect 0 -
        // which proved nothing, because SR3 composites a character's texture when it SPAWNS,
        // long before that character is ever drawn. The hook was being installed after the
        // event it was meant to observe. Creation is the earliest point where a texture exists
        // at all, so nothing can now be written to one before the hook is in place.
        InstallTextureHooks(*texture);
        // TIGHTENED 2026-08-29. The first version accepted DYNAMIC *or* RENDERTARGET at any
        // size >= 256, and its own overflow counter reported 955 candidates against a 32-entry
        // array - so it was tracking the first 32 render targets the game happened to create and
        // the character atlas was never among them. Every count taken through it described other
        // textures.
        //
        // The signature to match is the one the albedo report prints for the texture actually
        // bound on character draws:
        //
        //     2048x1024 fmt=22 mips=9 pool=0 usage=0x200
        //
        // fmt 22 is D3DFMT_X8R8G8B8 and 0x200 is D3DUSAGE_DYNAMIC. The render targets that
        // flooded the old test are fmt=21 with usage=0x1, so excluding RENDERTARGET and
        // requiring a large surface separates them cleanly.
        const bool uncompressed = (format == D3DFMT_X8R8G8B8 || format == D3DFMT_A8R8G8B8);
        const bool dynamicNotRT = (usage & D3DUSAGE_DYNAMIC) != 0 &&
                                  (usage & D3DUSAGE_RENDERTARGET) == 0;
        if (uncompressed && dynamicNotRT && pool == D3DPOOL_DEFAULT && w >= 1024 && h >= 512) {
            NoteAtlasPtr(*texture);
            if (g_atlasCreateReports < 8) {
                ++g_atlasCreateReports;
                Log("ATLAS CANDIDATE #%u created: %ux%u levels=%u fmt=%d pool=%d usage=0x%lX "
                    "(want 2048x1024 fmt=22 usage=0x200)",
                    g_atlasCreateReports, w, h, levels, static_cast<int>(format),
                    static_cast<int>(pool), static_cast<unsigned long>(usage));
            }
        }
    }
    if (SUCCEEDED(hr) && texture && *texture && (usage & D3DUSAGE_RENDERTARGET)) {
        // AddRef for the same reason as the mesh cache, though this set is only ever compared
        // against and never bound: a destroyed texture's address can be recycled by a newly
        // created one, which would then be permanently misclassified as generated data and
        // refused as albedo. The set holds ~55 entries, so the cost of pinning them is nil.
        if (g_rtTextures.insert(*texture).second) (*texture)->AddRef();
    }
    return hr;
}

HRESULT WINAPI Hook_SetRenderState(IDirect3DDevice9* dev, D3DRENDERSTATETYPE state, DWORD value) {
    if (!g_internal && state < kRsMax) { g_rsShadow[state] = value; g_rsKnown[state] = true; }
    return g_origSetRenderState(dev, state, value);
}

HRESULT WINAPI Hook_SetTextureStageState(IDirect3DDevice9* dev, DWORD stage,
                                         D3DTEXTURESTAGESTATETYPE type, DWORD value) {
    if (!g_internal && stage < 8 && type < kTssMax) {
        g_tssShadow[stage][type] = value;
        g_tssKnown[stage][type] = true;
    }
    return g_origSetTextureStageState(dev, stage, type, value);
}

HRESULT WINAPI Hook_SetSamplerState(IDirect3DDevice9* dev, DWORD sampler,
                                    D3DSAMPLERSTATETYPE type, DWORD value) {
    if (!g_internal && sampler < 16 && type < kSsMax) {
        g_ssShadow[sampler][type] = value;
        g_ssKnown[sampler][type] = true;
    }
    return g_origSetSamplerState(dev, sampler, type, value);
}

// The game drives everything through shader constants and never sets fixed-function
// transforms, so anything arriving here that is not ours would invalidate our cache.
HRESULT WINAPI Hook_SetTransform(IDirect3DDevice9* dev, D3DTRANSFORMSTATETYPE state,
                                 const D3DMATRIX* matrix) {
    if (!g_internal) {
        if (state == D3DTS_WORLD) g_haveAppliedWorld = false;
        else if (state == D3DTS_VIEW) g_haveAppliedView = false;
        else if (state == D3DTS_PROJECTION) g_haveAppliedProj = false;
    }
    return g_origSetTransform(dev, state, matrix);
}

VertexLayout ParseDeclaration(IDirect3DVertexDeclaration9* decl) {
    VertexLayout out;
    if (!decl) return out;
    D3DVERTEXELEMENT9 elems[64]{};   // MAXD3DDECLLENGTH; not exposed by every SDK version
    UINT count = 0;
    if (FAILED(decl->GetDeclaration(elems, &count))) return out;
    for (UINT i = 0; i < count; ++i) {
        const D3DVERTEXELEMENT9& e = elems[i];
        if (e.Stream == 0xFF) break;   // D3DDECL_END
        ++out.elements;
        // POSITION is recorded whatever stream it is in: knowing it lives outside stream 0 is
        // itself a reason not to convert, and silently skipping it would hide that.
        if (e.Usage == D3DDECLUSAGE_POSITION && e.UsageIndex == 0) {
            out.posType = e.Type;
            out.posOffset = e.Offset;
            out.posStream = e.Stream;
        }
        if (e.Usage == D3DDECLUSAGE_POSITION && e.UsageIndex == 1) {
            out.morphPosType = e.Type;
            out.morphPosOffset = e.Offset;
            out.morphStream = e.Stream;
        }
        if (e.Usage == D3DDECLUSAGE_NORMAL && e.UsageIndex == 1) {
            out.morphNrmType = e.Type;
            out.morphNrmOffset = e.Offset;
        }
        // The instance transform: POSITION 2/3/4 as float4s, all in one stream.
        if (e.Usage == D3DDECLUSAGE_POSITION && e.UsageIndex >= 2 && e.UsageIndex <= 4 &&
            e.Type == D3DDECLTYPE_FLOAT4) {
            out.instStream = e.Stream;
            out.instRowOffset[e.UsageIndex - 2] = e.Offset;
        }
        if (e.Stream != 0) continue;
        switch (e.Usage) {
            case D3DDECLUSAGE_POSITIONT: out.hasPositionT = true; break;
            case D3DDECLUSAGE_NORMAL:
                out.hasNormal = true;
                if (e.UsageIndex == 0) { out.normalType = e.Type; out.normalOffset = e.Offset; }
                break;
            case D3DDECLUSAGE_COLOR:     out.hasColor = true; break;
            case D3DDECLUSAGE_BLENDWEIGHT:
                out.skinned = true;
                out.blendWeightType = e.Type;
                out.blendWeightOffset = e.Offset;
                break;
            case D3DDECLUSAGE_BLENDINDICES:
                out.skinned = true;
                out.blendIndexType = e.Type;
                out.blendIndexOffset = e.Offset;
                break;
            case D3DDECLUSAGE_TEXCOORD:
                if (e.UsageIndex == 0) {
                    out.hasTexcoord = true;
                    out.texcoordType = e.Type;
                    out.texcoordOffset = e.Offset;
                } else if (e.UsageIndex == 1) {
                    out.texcoord1Type = e.Type;
                    out.texcoord1Offset = e.Offset;
                    out.texcoord1Stream = e.Stream;
                }
                break;
            default: break;
        }
    }
    out.hasInstanceTransform = out.instStream >= 0 && out.instStream < kMaxStreams &&
                               out.instRowOffset[0] >= 0 && out.instRowOffset[1] >= 0 &&
                               out.instRowOffset[2] >= 0;
    out.parsed = true;
    return out;
}

const char* DeclTypeName(int t) {
    switch (t) {
        case D3DDECLTYPE_FLOAT1: return "float1";
        case D3DDECLTYPE_FLOAT2: return "float2";
        case D3DDECLTYPE_FLOAT3: return "float3";
        case D3DDECLTYPE_FLOAT4: return "float4";
        case D3DDECLTYPE_D3DCOLOR: return "d3dcolor";
        case D3DDECLTYPE_UBYTE4: return "ubyte4";
        case D3DDECLTYPE_SHORT2: return "short2";
        case D3DDECLTYPE_SHORT4: return "short4";
        case D3DDECLTYPE_UBYTE4N: return "ubyte4n";
        case D3DDECLTYPE_SHORT2N: return "short2n";
        case D3DDECLTYPE_SHORT4N: return "short4n";
        case D3DDECLTYPE_FLOAT16_2: return "half2";
        case D3DDECLTYPE_FLOAT16_4: return "half4";
        default: return "?";
    }
}

unsigned g_layoutReports = 0;
// SR3 stores every world texture coordinate as D3DDECLTYPE_SHORT2, and Remix cannot read it.
// From its own log, with its own format table used to decode the number:
//
//     warn: [rtx-interleaver] Unsupported texcoord buffer format (80), skipping texcoord
//
// VkFormat 80 is VK_FORMAT_R16G16_SSCALED, which is what DXVK maps SHORT2 to. "Skipping texcoord"
// is literal: the geometry reaches the path tracer with NO UVs, so every surface samples one
// texel and renders as flat material colour. That is the whole of "the world is not textured".
//
// Re-declaring the same bytes as SHORT2N was tried first because it costs nothing, and Remix
// answered in the same words with a different number - "Unsupported texcoord buffer format (78)",
// VK_FORMAT_R16G16_SNORM. The interleaver wants real floats, and no reinterpretation of these
// four bytes can produce them.
//
// So the coordinates are CONVERTED, into a float2 buffer of our own bound as a second stream.
// float2 is not a third guess: it is the only texcoord format known to work on this exact path,
// and it is known from the two populations that are textured today - characters, whose vertices
// this shim rebuilds into a float2 FVF for skinning, and foliage, drawn from a game instance
// stream whose stream 0 is a single float2 TEXCOORD.
//
// WHOLE BUFFER AT A TIME, keyed on the source buffer rather than on the draw. Converting per draw
// would rewrite the same city block's coordinates on each of the ~450 draws a frame that need
// them. Converting the whole buffer once also makes the stream offset trivial: our entry i is
// source vertex i, so any draw from that buffer binds it unchanged at any minIndex. A per-draw
// ring would instead have to satisfy streamOffset + minIndex*stride with a UINT offset, which
// forces it to waste minIndex*8 bytes on every allocation.
DWORD g_uvStream = kUvStreamDefault;

struct UvBuffer {
    IDirect3DVertexBuffer9* src = nullptr;   // referenced: the key is its address
    IDirect3DVertexBuffer9* uv = nullptr;
    UINT vertexCount = 0;
    UINT bytes = 0;
};
std::unordered_map<unsigned long long, UvBuffer> g_uvBuffers;
size_t g_uvBytesHeld = 0;
unsigned g_uvBuffersMade = 0;
unsigned g_uvConvertFailures = 0;
unsigned g_uvFailLayout = 0;     // no stream 0, or the texcoord is outside the stride
unsigned g_uvFailDynamic = 0;    // DYNAMIC source: must not be read back, and would be stale
unsigned g_uvFailDesc = 0;       // GetDesc failed, or the buffer holds no whole vertices
unsigned g_uvFailCreate = 0;     // could not allocate our own buffer
unsigned g_uvFailLock = 0;       // could not lock one of the two buffers
unsigned g_uvDynConverted = 0;   // DYNAMIC source converted from the snooped copy
unsigned g_uvDynWaiting = 0;     // DYNAMIC source registered, snoop not filled yet
unsigned g_uvBufferFlushes = 0;
unsigned g_uvInvalidations = 0;
unsigned g_uvStreamDraws = 0;

// The range is part of the key for DYNAMIC sources only. A dynamic buffer is a RING: the game
// refills one slice of it per draw, so two draws in the same frame legitimately read different
// vertices from one buffer. Keyed on the pointer alone, the second draw is served the first
// draw's coordinates - the identical defect the bind-pose cache had ("each character had the
// wrong head"), whose key had to become {vb, offset, stride, minIndex, count} for the same
// reason. Static buffers pass 0/0 and keep their old whole-buffer key.
unsigned long long UvKey(void* vb, UINT stride, int texOffset, UINT first, UINT count) {
    unsigned long long h = 1469598103934665603ull;
    const unsigned long long parts[5] = {
        reinterpret_cast<unsigned long long>(vb), stride,
        static_cast<unsigned long long>(texOffset), first, count};
    for (int i = 0; i < 5; ++i) { h ^= parts[i]; h *= 1099511628211ull; }
    return h;
}

void ReleaseUvBuffers() {
    for (auto& kv : g_uvBuffers) {
        if (kv.second.src) kv.second.src->Release();
        if (kv.second.uv) kv.second.uv->Release();
    }
    g_uvBuffers.clear();
    g_uvBytesHeld = 0;
}

// The converted buffer for the currently bound stream 0, or null if it cannot be built.
IDirect3DVertexBuffer9* UvBufferFor(IDirect3DDevice9* dev) {
    const UINT stride = g_stream0Stride;
    const int texOffset = g_curLayout.texcoordOffset;
    if (!g_stream0 || stride == 0 || texOffset < 0 || texOffset + 4 > static_cast<int>(stride)) {
        ++g_uvFailLayout;
        ++g_uvConvertFailures;
        return nullptr;
    }

    D3DVERTEXBUFFER_DESC desc{};
    if (FAILED(g_stream0->GetDesc(&desc))) { ++g_uvFailDesc; ++g_uvConvertFailures; return nullptr; }
    // DYNAMIC source. This used to refuse outright, on two objections that are both real and
    // both already answered elsewhere in this file:
    //
    //   "a DYNAMIC buffer must not be read back"  - correct, and we do not. The VB lock hook has
    //      snooped the game's OWN writes since the morph work; RegisterSnoop/SnoopCopy read that
    //      copy, never the buffer. Same mechanism the morph stream uses.
    //   "a conversion of it would be stale immediately" - InvalidateUvBuffers(vb) is already
    //      wired into Hook_VBUnlock, so the moment the game refills the buffer our converted
    //      copy is dropped and rebuilt. The staleness guard exists; this path was written before
    //      it did.
    //
    // WHY IT MATTERS, measured 2026-08-31: this single branch accounted for EVERY ONE of the
    // 83,737 UV conversion failures in the run - 5.8 draws a frame, and 0.0/frame for all four
    // other reasons. Those draws keep raw SHORT2 texcoords, which are NOT normalised: a stored
    // 16384 is a UV of 16384, so the texture tiles into noise. That is visible with RAY TRACING
    // OFF as well, because the bad coordinates are in the rasterised draw itself - which is
    // exactly how the user described it ("the top is textured right but not shoes and other
    // stuff... also reflected with ray tracing off"). Dynamic vertex buffers are what the engine
    // uses for per-frame-written character parts.
    //
    // The first draw on a newly registered buffer finds nothing and waits - counted separately as
    // g_uvDynWaiting, NOT as a failure of the conversion itself, so the two cannot be confused
    // the way a single lumped counter hid this branch for so long.
    const bool dynamicSrc = (desc.Usage & D3DUSAGE_DYNAMIC) != 0;
    if (dynamicSrc) {
        if (!g_settings.convertDynamicUV) {
            ++g_uvFailDynamic;
            ++g_uvConvertFailures;
            return nullptr;
        }
        RegisterSnoop(g_stream0);
    }

    const UINT vertexCount = desc.Size / stride;
    if (vertexCount == 0) { ++g_uvFailDesc; ++g_uvConvertFailures; return nullptr; }
    const UINT bytes = vertexCount * kUvBytesPerVertex;

    // The slice this draw actually reads. Entries stay indexed per vertex exactly as stream 0 is
    // - the uv stream is bound at offset 0 with its own stride - so entry i always corresponds to
    // stream-0 vertex i, whether or not we filled it.
    UINT rangeFirst = 0, rangeCount = vertexCount;
    if (dynamicSrc) {
        rangeFirst = g_curDrawFirstVertex;
        rangeCount = g_curDrawVertexCount;
        if (rangeCount == 0 || rangeFirst + rangeCount > vertexCount) {
            ++g_uvFailLayout;
            ++g_uvConvertFailures;
            return nullptr;
        }
    }

    const unsigned long long key = UvKey(g_stream0, stride, texOffset,
                                         dynamicSrc ? rangeFirst : 0,
                                         dynamicSrc ? rangeCount : 0);
    const auto it = g_uvBuffers.find(key);
    if (it != g_uvBuffers.end()) return it->second.uv;

    // A cap WITH A FLUSH, not a cap that stops accepting. Two caches here have already had the
    // "stop growing" cliff, where the world progressively loses whatever the cache protects as
    // new content streams in. Flushing rebuilds within a frame or two.
    if (g_uvBytesHeld + bytes > kMaxUvBufferBytes) {
        ReleaseUvBuffers();
        ++g_uvBufferFlushes;
        Log("uv buffer arena flushed at %u MB (flush #%u)",
            static_cast<unsigned>(kMaxUvBufferBytes >> 20), g_uvBufferFlushes);
    }

    UvBuffer entry;
    entry.vertexCount = vertexCount;
    entry.bytes = bytes;
    if (FAILED(dev->CreateVertexBuffer(bytes, D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT,
                                       &entry.uv, nullptr)) || !entry.uv) {
        ++g_uvFailCreate;
        ++g_uvConvertFailures;
        return nullptr;
    }

    void* srcData = nullptr;
    void* dstData = nullptr;
    std::vector<unsigned char> staging;
    const unsigned char* src8 = nullptr;
    if (dynamicSrc) {
        // Never Lock a DYNAMIC buffer for reading. Take the copy the lock hook already made -
        // and take ONLY THE SLICE THIS DRAW READS.
        //
        // The first attempt asked for the whole declared buffer and required every byte fresh.
        // Both are wrong, and the snoop says so in its own comments: it "grows to what the game
        // actually writes, which is a fraction of most buffers' declared size", so a whole-buffer
        // request fails the `offset + len > c.data.size()` bound outright; and D3DLOCK_DISCARD
        // clears every fresh bit, so after a partial refill most of the buffer is legitimately
        // stale. Result: 0 converted, 24,772 waiting, with the report's own warning firing.
        bool fresh = false;
        const UINT sliceOff = rangeFirst * stride;
        const UINT sliceLen = rangeCount * stride;
        staging.resize(sliceLen);
        if (!SnoopCopy(g_stream0, sliceOff, staging.data(), sliceLen, &fresh) || !fresh) {
            entry.uv->Release();
            ++g_uvDynWaiting;
            return nullptr;      // not a conversion failure - the snoop has no data yet
        }
        src8 = staging.data();
    } else {
        if (FAILED(g_stream0->Lock(0, 0, &srcData, D3DLOCK_READONLY | D3DLOCK_NOSYSLOCK)) ||
            !srcData) {
            entry.uv->Release();
            ++g_uvFailLock;
            ++g_uvConvertFailures;
            return nullptr;
        }
        src8 = static_cast<const unsigned char*>(srcData);
    }
    if (FAILED(entry.uv->Lock(0, 0, &dstData, D3DLOCK_DISCARD)) || !dstData) {
        if (!dynamicSrc) g_stream0->Unlock();
        entry.uv->Release();
        ++g_uvFailLock;
        ++g_uvConvertFailures;
        return nullptr;
    }

    // RAW short values widened to float, NOT scaled. The uv texture matrix already carries the
    // 1/1024 and the per-material tiling, and it is the same convention the skinned path uses -
    // the one converted population that renders with correct textures today. Two owners for one
    // scale is how the two come to disagree.
    float* dst = static_cast<float*>(dstData);
    // Vertices outside the drawn slice are never read by this draw, but they must not be left as
    // whatever the allocator handed us: a later draw keyed to a different range would index them.
    if (dynamicSrc && rangeCount != vertexCount) memset(dst, 0, bytes);
    for (UINT n = 0; n < rangeCount; ++n) {
        const UINT i = rangeFirst + n;
        // src8 is the slice for a dynamic source and the whole buffer for a static one.
        const UINT srcVertex = dynamicSrc ? n : i;
        const short* t = reinterpret_cast<const short*>(src8 + srcVertex * stride + texOffset);
        dst[i * 2 + 0] = static_cast<float>(t[0]);
        dst[i * 2 + 1] = static_cast<float>(t[1]);
    }
    entry.uv->Unlock();
    if (!dynamicSrc) g_stream0->Unlock();
    if (dynamicSrc) ++g_uvDynConverted;

    // Referenced, because the key is its address. A freed buffer whose address is recycled would
    // otherwise hand a later mesh this one's coordinates - the defect already found in three
    // other pointer-keyed caches here.
    entry.src = g_stream0;
    entry.src->AddRef();
    g_uvBytesHeld += bytes;
    ++g_uvBuffersMade;
    g_uvBuffers[key] = entry;
    return entry.uv;
}

// The game refilled a buffer we have converted: our coordinates describe its old contents.
void InvalidateUvBuffers(IDirect3DVertexBuffer9* vb) {
    for (auto it = g_uvBuffers.begin(); it != g_uvBuffers.end();) {
        if (it->second.src == vb) {
            g_uvBytesHeld -= it->second.bytes;
            it->second.src->Release();
            if (it->second.uv) it->second.uv->Release();
            it = g_uvBuffers.erase(it);
            ++g_uvInvalidations;
        } else {
            ++it;
        }
    }
}

// A clone of the game's declaration with TEXCOORD0 moved onto our float2 stream. Position, normal
// and the other texcoord sets still come from the game's own buffers, so the geometry itself is
// untouched and only the coordinates change.
std::unordered_map<IDirect3DVertexDeclaration9*, IDirect3DVertexDeclaration9*> g_uvDecls;
unsigned g_uvDeclsMade = 0;
unsigned g_uvDeclFailures = 0;

IDirect3DVertexDeclaration9* FloatUVDeclaration(IDirect3DDevice9* dev,
                                                IDirect3DVertexDeclaration9* src) {
    if (!src) return nullptr;
    const auto it = g_uvDecls.find(src);
    if (it != g_uvDecls.end()) return it->second;

    D3DVERTEXELEMENT9 elems[64]{};
    UINT count = 0;
    IDirect3DVertexDeclaration9* clone = nullptr;
    if (SUCCEEDED(src->GetDeclaration(elems, &count)) && count > 0) {
        bool changed = false;
        for (UINT i = 0; i < count; ++i) {
            if (elems[i].Stream == 0xFF) break;
            // Only TEXCOORD0 - the set fixed function samples, since D3DTSS_TEXCOORDINDEX is 0.
            if (elems[i].Usage == D3DDECLUSAGE_TEXCOORD && elems[i].UsageIndex == 0 &&
                elems[i].Type == D3DDECLTYPE_SHORT2) {
                elems[i].Stream = static_cast<WORD>(g_uvStream);
                elems[i].Offset = 0;
                elems[i].Type = D3DDECLTYPE_FLOAT2;
                changed = true;
            }
        }
        if (changed && FAILED(dev->CreateVertexDeclaration(elems, &clone))) clone = nullptr;
    }
    if (!clone) ++g_uvDeclFailures; else ++g_uvDeclsMade;

    src->AddRef();
    g_uvDecls[src] = clone;   // null is cached too, so a failure is not retried every draw
    return clone;
}

HRESULT WINAPI Hook_SetVertexDeclaration(IDirect3DDevice9* dev,
                                         IDirect3DVertexDeclaration9* decl) {
    if (!g_internal) g_curDecl = decl;
    const auto it = g_layouts.find(decl);
    if (it != g_layouts.end()) {
        g_curLayout = it->second;
    } else {
        g_curLayout = ParseDeclaration(decl);
        // Referenced for the same reason as g_shaders: a recycled declaration address would
        // return another mesh's field offsets, and skinning reads blend weights and indices
        // through exactly those offsets.
        if (decl && g_layouts.emplace(decl, g_curLayout).second) decl->AddRef();
        // One line per distinct declaration. The texcoord type is the number that matters:
        // fixed function reads short2 as a raw integer, so anything other than float2 needs
        // the uvScale texture matrix before its textures can land correctly.
        if (g_settings.logLayouts && decl && g_layoutReports < 24) {
            ++g_layoutReports;
            Log("vertex layout #%u: elems=%d pos=%s@%d(stream %d) uv=%s normal=%d color=%d "
                "skinned=%d positionT=%d",
                g_layoutReports, g_curLayout.elements, DeclTypeName(g_curLayout.posType),
                g_curLayout.posOffset, g_curLayout.posStream,
                DeclTypeName(g_curLayout.texcoordType), g_curLayout.hasNormal,
                g_curLayout.hasColor, g_curLayout.skinned, g_curLayout.hasPositionT);
            // For a skinned declaration the summary is not enough to write a decoder against,
            // so dump every element. Only the skinned ones, and only the first few: this is the
            // input to the CPU skinning port and nothing else needs it.
            if (g_curLayout.skinned && g_skinnedDeclDumps < 4) {
                ++g_skinnedDeclDumps;
                D3DVERTEXELEMENT9 all[64]{};
                UINT n = 0;
                if (SUCCEEDED(decl->GetDeclaration(all, &n))) {
                    for (UINT i = 0; i < n; ++i) {
                        if (all[i].Stream == 0xFF) break;
                        Log("    stream=%u offset=%2u type=%-8s(%u) usage=%-12s index=%u",
                            all[i].Stream, all[i].Offset, DeclTypeName(all[i].Type),
                            all[i].Type, UsageName(all[i].Usage), all[i].UsageIndex);
                    }
                }
            }
        }
    }
    return g_origSetVertexDeclaration(dev, decl);
}

HRESULT WINAPI Hook_SetStreamSource(IDirect3DDevice9* dev, UINT stream,
                                    IDirect3DVertexBuffer9* buffer, UINT offset, UINT stride) {
    if (!g_internal) {
        if (stream == 0) {
            g_stream0 = buffer;
            g_stream0Offset = offset;
            g_stream0Stride = stride;
        }
        if (stream < kMaxStreams) {
            g_streamVB[stream] = buffer;
            g_streamOffset[stream] = offset;
            g_streamStride[stream] = stride;
        }
        // Install the shared vertex-buffer Lock/Unlock hooks the first time we see any buffer.
        // All IDirect3DVertexBuffer9 instances share one vtable, so one patch covers them all.
        if (buffer && !g_origVBLock) {
            PatchVTable(buffer, kSlotVBLock, &Hook_VBLock,
                        reinterpret_cast<void**>(&g_origVBLock));
            PatchVTable(buffer, kSlotVBUnlock, &Hook_VBUnlock,
                        reinterpret_cast<void**>(&g_origVBUnlock));
        }
    }
    return g_origSetStreamSource(dev, stream, buffer, offset, stride);
}

// Hardware instancing draws many copies of one mesh, each positioned from a per-instance
// stream. Fixed function has no equivalent, so a converted instanced draw collapses every copy
// onto a single transform - a plausible reading of "the city is bunched up". Whether SR3 uses
// it at all is unknown, so this both measures and guards.
unsigned g_freqReports = 0;
HRESULT WINAPI Hook_SetStreamSourceFreq(IDirect3DDevice9* dev, UINT stream, UINT setting) {
    if (!g_internal) {
        if (stream == 0) {
            // D3DSTREAMSOURCE_INDEXEDDATA (0x40000000) | instanceCount marks the geometry
            // stream of an instanced draw; a plain 1 resets it.
            g_instancedDraw = (setting & D3DSTREAMSOURCE_INDEXEDDATA) != 0;
            g_instanceCount = g_instancedDraw ? (setting & 0x3FFFFFFF) : 1;
        }
        if (stream < 4) {
            g_streamFreq[stream] = setting;
        }
        // Record the shapes actually used, so per-instance conversion can be written against
        // measurements rather than an assumed layout.
        if (g_freqReports < 12) {
            ++g_freqReports;
            Log("stream freq #%u: stream=%u setting=%#010x -> %s count=%u", g_freqReports,
                stream, setting,
                (setting & D3DSTREAMSOURCE_INSTANCEDATA) ? "INSTANCEDATA" :
                (setting & D3DSTREAMSOURCE_INDEXEDDATA) ? "INDEXEDDATA" : "plain",
                setting & 0x3FFFFFFF);
        }
    }
    return g_origSetStreamSourceFreq(dev, stream, setting);
}

// SR3 stores texture coordinates as short2 on essentially all world geometry. Fixed function
// reads those as raw integers, so the UVs arrive in the thousands and every texture tiles into
// noise. The divisor is not documented anywhere we have found, so measure it: read the actual
// range a few meshes occupy. A mesh whose UVs span roughly 0..N for one texture repeat gives N
// directly, and the value is almost always a power of two.
unsigned g_uvReports = 0;
void MeasureShortUVs(UINT minIndex, UINT numVertices) {
    if (g_uvReports >= 12 || !g_stream0) return;
    if (g_curLayout.texcoordType != D3DDECLTYPE_SHORT2) return;
    if (g_curLayout.texcoordOffset < 0 || g_stream0Stride < 4) return;
    if (numVertices < 3 || numVertices > 4096) return;

    const UINT first = g_stream0Offset + minIndex * g_stream0Stride;
    const UINT bytes = numVertices * g_stream0Stride;
    void* mapped = nullptr;
    if (FAILED(g_stream0->Lock(first, bytes, &mapped, D3DLOCK_READONLY | D3DLOCK_NOSYSLOCK)) ||
        !mapped)
        return;

    int minU = 32767, maxU = -32768, minV = 32767, maxV = -32768;
    const unsigned char* p = static_cast<const unsigned char*>(mapped);
    for (UINT i = 0; i < numVertices; ++i) {
        const short* uv = reinterpret_cast<const short*>(
            p + i * g_stream0Stride + g_curLayout.texcoordOffset);
        if (uv[0] < minU) minU = uv[0];
        if (uv[0] > maxU) maxU = uv[0];
        if (uv[1] < minV) minV = uv[1];
        if (uv[1] > maxV) maxV = uv[1];
    }
    g_stream0->Unlock();

    ++g_uvReports;
    Log("short2 UV range #%u: u %d..%d  v %d..%d  (verts=%u stride=%u off=%d)",
        g_uvReports, minU, maxU, minV, maxV, numVertices, g_stream0Stride,
        g_curLayout.texcoordOffset);
}

// Where does a converted draw actually land? Steelport's coordinates run to the low thousands,
// so anything an order of magnitude beyond that is geometry we have transformed wrongly - the
// "polygons stretching across the whole world". Reporting the offenders with their layout says
// WHICH class of draw is broken, instead of inferring it from shader names.
unsigned g_extentReports = 0;
void MeasureWorldExtent(UINT minIndex, UINT numVertices) {
    // Gate the LOCK, not just the logging. The report was capped at 20 lines but the buffer lock
    // was not, so this locked stream 0 on every converted draw - roughly 1,400 vertex-buffer
    // locks per frame, each one a bridge round trip and a potential GPU sync, purely to feed a
    // diagnostic that had already stopped printing. Capping the log while leaving the work in
    // place is a mistake worth remembering.
    if (g_extentReports >= 20) return;
    if (!g_stream0 || g_stream0Stride < 12) return;
    if (g_curLayout.posOffset < 0) return;
    if (numVertices < 3 || numVertices > 4096) return;

    const UINT first = g_stream0Offset + minIndex * g_stream0Stride;
    void* mapped = nullptr;
    if (FAILED(g_stream0->Lock(first, numVertices * g_stream0Stride, &mapped,
                               D3DLOCK_READONLY | D3DLOCK_NOSYSLOCK)) || !mapped)
        return;

    float worst = 0.0f;
    const unsigned char* p = static_cast<const unsigned char*>(mapped);
    const UINT step = (numVertices > 32) ? (numVertices / 32) : 1;   // sample, do not scan all
    for (UINT i = 0; i < numVertices; i += step) {
        const float* v = reinterpret_cast<const float*>(
            p + i * g_stream0Stride + g_curLayout.posOffset);
        if (!std::isfinite(v[0]) || !std::isfinite(v[1]) || !std::isfinite(v[2])) {
            worst = 1e30f;
            break;
        }
        const D3DVECTOR w = TransformPoint(v, g_appliedWorld);
        const float m = max(max(std::fabs(w.x), std::fabs(w.y)), std::fabs(w.z));
        if (m > worst) worst = m;
    }
    g_stream0->Unlock();

    if (worst > 20000.0f) {
        ++g_outlierDraws;
        if (g_extentReports < 20) {
            ++g_extentReports;
            Log("OUTLIER draw #%u: max |world| = %.0f | verts=%u stride=%u elems=%d pos=%s@%d "
                "uv=%s objTM=%d worldWritten=%d",
                g_extentReports, worst, numVertices, g_stream0Stride, g_curLayout.elements,
                DeclTypeName(g_curLayout.posType), g_curLayout.posOffset,
                DeclTypeName(g_curLayout.texcoordType), g_curVS.usesObjTM, g_worldWritten);
        }
    }
}

void RecordFrameDraw(IDirect3DDevice9* dev, UINT numVertices, UINT primitiveCount, Disp d);
void ProbeAtlasComposite(IDirect3DDevice9* dev, UINT numVertices, UINT primitiveCount, Disp d);
// Defined below, with the HUD rebuild they share their vertex type with. Hook_DrawPrimitive is
// above that point in this file, which is the ordering trap this project has hit four times.
void NotePassedThrough(UINT primitiveCount);
bool IsAtlasComposite(UINT primitiveCount, Disp d);
bool DrawCompositeFixedFunction(IDirect3DDevice9* dev);

void ClearBackBufferOnce(IDirect3DDevice9* dev);   // defined below, with its rationale

HRESULT WINAPI Hook_DrawPrimitive(IDirect3DDevice9* dev, D3DPRIMITIVETYPE type, UINT start,
                                  UINT count) {
    ++g_drawsTotal;
    ++g_drawIndexThisFrame;
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
    const bool ui = (d == Disp::Convert) ? false : BeginUIDemote(dev, d == Disp::Hide);
    const unsigned marked = (d == Disp::Mark) ? BeginMark(dev, g_markClearAllStages) : 0u;
    const HRESULT hr = g_origDrawPrimitive(dev, type, start, count);
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
unsigned g_hudConverted = 0, g_hudConvertRefused = 0, g_hudDrawFailed = 0;
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
    g_origSetVertexShader(dev, nullptr);
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

    g_internal = true;
    dev->SetFVF(oldFVF);
    g_origSetVertexDeclaration(dev, oldDecl);
    g_origSetVertexShader(dev, oldVS);
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
    g_origSetVertexShader(dev, nullptr);      // the pixel shader is deliberately left alone
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
    g_internal = false;

    const HRESULT hr = g_origDrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 2, quad,
                                             sizeof(HudFfpVertex));

    g_internal = true;
    dev->SetFVF(oldFVF);
    g_origSetVertexDeclaration(dev, oldDecl);
    g_origSetVertexShader(dev, oldVS);
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
struct PassCensusRow { const char* sampler; UINT w, h; unsigned count; };
constexpr unsigned kPassCensusRows = 40;
PassCensusRow g_passCensus[kPassCensusRows];
unsigned g_passCensusUsed = 0;

void NotePassedThrough(UINT primitiveCount) {
    if (!g_settings.passCensus) return;
    const char* name = g_curPS.firstSampler ? g_curPS.firstSampler : "";
    for (unsigned i = 0; i < g_passCensusUsed; ++i) {
        if (g_passCensus[i].sampler == name && g_passCensus[i].w == g_rt0Width &&
            g_passCensus[i].h == g_rt0Height) { ++g_passCensus[i].count; return; }
    }
    if (g_passCensusUsed >= kPassCensusRows) return;
    g_passCensus[g_passCensusUsed].sampler = name;
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

    const bool ui = BeginUIDemote(dev, hud);
    HRESULT hr = D3D_OK;
    // Rebuild it as fixed function if we can; pass the game's own draw through if we cannot.
    if (!(hud && g_settings.uiConvertUP &&
          DrawHudFixedFunction(dev, type, count, vertices, stride)))
        hr = g_origDrawPrimitiveUP(dev, type, count, vertices, stride);
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
    g_dispReason = "DrawIndexedPrimitiveUP - user-pointer draw, NOT classified by this shim";
    RecordFrameDraw(dev, numVertices, count, Disp::PassThrough);
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

unsigned g_frameDumpTarget = 0;
unsigned g_captureIndex = 0;        // 0 = the automatic first capture, then 1, 2, 3...
bool g_captureKeyWasDown = false;
bool g_captureRequested = false;

// Re-arm both dumps for the next frame. Called from Present, so no draw is half-recorded.
void RearmCaptures() {
    ++g_captureIndex;
    g_frameDumpDone = false;
    g_frameDumpTarget = 0;
    if (g_frameLog) { fclose(g_frameLog); g_frameLog = nullptr; }
    g_shapeProbeDone = false;
    g_shapeProbeTarget = 0;
    g_shapeReports = 0;
    if (g_shapeLog) { fclose(g_shapeLog); g_shapeLog = nullptr; }
    Log("CAPTURE %u armed - the next frame goes to sr3-rtx-frame-%u.log and "
        "sr3-rtx-shapes-%u.log", g_captureIndex, g_captureIndex, g_captureIndex);
}

void PollCaptureKey() {
    if (!g_settings.captureKey) return;
    const bool down = (GetAsyncKeyState(g_settings.captureKey) & 0x8000) != 0;
    if (down && !g_captureKeyWasDown) g_captureRequested = true;
    g_captureKeyWasDown = down;
    if (g_captureRequested) {
        g_captureRequested = false;
        RearmCaptures();
    }
}

// Capture 0 keeps the original filenames so nothing that reads them has to change.
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
    if (!g_frameDumpTarget) {
        if (g_lastFrameDraws < kDumpMinDraws) return;
        g_frameDumpTarget = g_frames + 1 + g_settings.dumpFrame;
    }
    if (g_frames + 1 != g_frameDumpTarget) return;
    // Check the gate AGAIN at the moment of firing, not only when the countdown started. The
    // third bad dump came from exactly this: the countdown began in gameplay and 1,800 frames
    // later landed on a 997-draw frame while the counters read 2,661. A condition tested once,
    // long before the thing it guards, guards nothing - so push the target forward instead.
    if (g_lastFrameDraws < kDumpMinDraws) {
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
    fprintf(g_frameLog,
            "%5u %s v=%-6u p=%-6u zw=%lu zt=%lu blend=%lu cw=%#lx | vs%s%s%s ps='%s' rank=%d | "
            "tex0=%p%s inst=%d at(%.1f %.1f %.1f)%s | %s\n",
            g_drawIndexThisFrame, kDisp[static_cast<int>(d)], numVertices, primitiveCount,
            ShadowGetRS(dev, D3DRS_ZWRITEENABLE), ShadowGetRS(dev, D3DRS_ZENABLE),
            ShadowGetRS(dev, D3DRS_ALPHABLENDENABLE), ShadowGetRS(dev, D3DRS_COLORWRITEENABLE),
            g_curVS.usesProjTM ? "[proj]" : "[     ]",
            g_curVS.usesObjTM ? "[obj]" : "[   ]",
            g_curVS.skinned ? "[skin]" : "[    ]",
            g_curPS.firstSampler, g_curPS.albedoRank,
            static_cast<void*>(g_curTexture[0]),
            g_rtTextures.count(g_curTexture[0]) ? "(RT)" : "    ",
            g_instancedDraw ? 1 : 0,
            // Two draws of one mesh at ONE position are a duplicate; at two positions they are two
            // objects that happen to share a texture. Nothing else in this line separates those,
            // and the head double-draw cannot be diagnosed without knowing which it is.
            g_vsConst[kRegObjTM][3], g_vsConst[kRegObjTM + 1][3], g_vsConst[kRegObjTM + 2][3],
            skinTxt, g_dispReason);
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
unsigned g_hudProbeReports = 0;

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

// ---------------------------------------------------------------- CPU skinning
//
// Why this exists: the skinned vertex buffer is STATIC and holds the bind pose (measured
// 2026-08-18, usage 0x8 = WRITEONLY, no DYNAMIC bit). Every frame of animation lives in the
// bone palette at c52 - vertex shader constants. So the geometry Remix can see never changes,
// and characters freeze in place while the converted world, which we hand over already
// transformed, moves correctly. That asymmetry is the whole bug, and it is the user's own
// observation: "the world does not freeze but the character animations and characters do".
//
// The algorithm, from the disassembly of debug_diffuse_only_c.fxo_pc:
//
//     pos' = ( SUM_i  w_i * Bone[idx_i] ) * float4(pos, 1)  /  SUM_i w_i
//
// Bone[] is row-major float3x4 at c52, 3 registers per bone. objTM (c32) is applied AFTER
// skinning, so we skin into OBJECT space and the existing WORLD/VIEW/PROJECTION path places
// the result unchanged - no transform work is duplicated here.
//
// Two measured details that a reasonable guess would have got wrong:
//   - BLENDWEIGHT is UBYTE4, not UBYTE4N. The bytes sum to 254-255, not to 1.0, which is why
//     the shader divides by their sum. Treating them as normalised would shrink every mesh.
//   - Index 255 is a sentinel for "no influence", always paired with weight 0. 255*3 = 765
//     addresses far past the 192-register palette, so influences must be rejected on WEIGHT,
//     never trusted by index.

struct SkinnedVertex {   // what we submit: plain, uncompressed, fixed-function readable
    float pos[3];
    float nrm[3];
    float uv[2];
};
constexpr DWORD kSkinnedFVF = D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX1;

// The decoded bind pose, cached per mesh. Decoding means read-locking the source buffer, which
// is a bridge round trip; doing that for ~157 skinned draws every frame is precisely the
// "state-call catastrophe" of sr2-fork.md section 6. Decode once, then only the arithmetic runs
// per frame.
struct BaseVertex {
    float pos[3];
    float nrm[3];
    float uv[2];
    // Weights kept as floats rather than raw bytes so one skinning loop serves every storage
    // format the game uses. The shader normalises by their sum, so the SCALE does not matter -
    // only that the four values are in the right order.
    float w[4];
    unsigned char idx[4];
    // TEXCOORD1, raw like uv. The pattern map is sampled with this set, not with uv - measured
    // 2026-09-03: SHORT2 at offset 32, alongside TEXCOORD0's SHORT2 at 28, same stream.
    float uv2[2];
};
struct BaseMesh {
    std::vector<BaseVertex> verts;
    // Pinned for the same reason the mesh albedo cache pins its textures, and it is the same
    // bug that caused the 2026-08-18 use-after-free: a released buffer's address can be handed
    // straight back to a newly created one, and this cache would then answer with a completely
    // different mesh's bind pose. The key contains the pointer, so without a reference the key
    // itself is unstable.
    IDirect3DVertexBuffer9* owner = nullptr;
    // Whether uv2 in these vertices is real, or just the zeroes a reduced declaration left.
    bool hasUv2 = false;
    // Highest bone index this mesh actually references, counting only components with a
    // non-zero weight - slot 255 is the "unused" sentinel and would otherwise dominate.
    // Computed once at decode so the per-draw check costs one comparison.
    unsigned maxBone = 0;
    // Which bones this mesh actually reads, one bit each. 64 bones fits exactly, and it lets the
    // per-draw check walk only the bones that matter instead of all 64.
    unsigned long long usedBones = 0;
};
std::unordered_map<unsigned long long, BaseMesh> g_baseMeshes;
// Which cached meshes came out of which buffer, so a write-lock can drop exactly those.
std::unordered_map<IDirect3DVertexBuffer9*, std::vector<unsigned long long>> g_baseMeshByVB;
unsigned g_baseMeshInvalidations = 0;
constexpr size_t kMaxBaseMeshes = 1024;

// Drop every cached bind pose decoded out of this buffer. Indexed by owner so a lock costs a

// hash lookup rather than a walk of the whole cache - the game locks buffers many times a frame

// and this runs inside that path.

void InvalidateBaseMeshes(IDirect3DVertexBuffer9* vb) {

    const auto list = g_baseMeshByVB.find(vb);

    if (list == g_baseMeshByVB.end()) return;

    for (const unsigned long long key : list->second) {

        const auto it = g_baseMeshes.find(key);

        if (it == g_baseMeshes.end()) continue;

        if (it->second.owner) it->second.owner->Release();

        g_baseMeshes.erase(it);

        ++g_baseMeshInvalidations;

    }

    g_baseMeshByVB.erase(list);

}

IDirect3DVertexBuffer9* g_skinVB = nullptr;
// The ring the CPU-skinned vertices are written into, and the one piece of shared mutable memory
// in the whole skinned path. Its write position is forced to firstVertex*stride so the game's own
// index buffer can be reused verbatim - which means the ring is consumed far faster than the
// vertex count alone suggests, because each draw skips forward to its own index base.
//
// When it fills, the wrap issues D3DLOCK_DISCARD over the WHOLE buffer, which orphans the
// allocation. Anything already submitted this frame that still refers to the old storage is fine
// for rasterisation, but it is exactly the kind of aliasing a path tracer reading the buffer later
// would not survive. How often that happens scales with how much skinned geometry is on screen -
// the one property of the reported drift that nothing else explains.
//
// Sized from the ini so the hypothesis can be tested by making wraps impossible rather than by
// argument. 8 MB was the original fixed size.
UINT g_skinRingBytes = 8u * 1024u * 1024u;
unsigned g_skinRingWraps = 0, g_skinRingDiscards = 0;
// How long the ring's own Lock takes, split by kind. A whole-buffer DISCARD asks the driver to
// hand back a FRESH allocation of the entire buffer; if its pool has none free it blocks until
// the GPU releases one. That stall is charged to whichever draw happened to be first in the
// frame, so it lands in the shim's per-draw time indistinguishable from skinning arithmetic.
// Timed separately, "is the ring's SIZE costing us frames?" stops being an inference from the
// shape of a sawtooth and becomes a number.
double g_skinLockDiscardMs = 0.0, g_skinLockKeepMs = 0.0, g_skinLockWorstMs = 0.0;
// Draws whose vertex shader reads no BLENDINDICES: not skinned by the game, whatever the
// vertex declaration says. Counted apart from g_skinC52BoneReg, which lumps "declared at c52"
// together with "declared nowhere, defaulted to c52" and so reported this bug as clean.
unsigned g_skinNoBoneDecl = 0, g_skinNoBoneDeclReports = 0;
UINT g_skinRingHighWater = 0;
UINT g_skinRingPos = 0;
bool g_skinRingFresh = true;   // next lock must DISCARD

unsigned g_skinnedConverted = 0, g_skinnedRefused = 0;
unsigned g_baseMeshDecodes = 0;
unsigned g_baseMeshCacheFlushes = 0;

// Does [firstVertex, firstVertex+count) lie inside this buffer?
//
// Every read-lock of a game vertex buffer in this shim computes its byte offset as
// `streamOffset + firstVertex * stride`, in UINT. Since baseVertex joined firstVertex on
// 2026-08-20 that product is much larger than it used to be, and an overflow would WRAP to a
// small offset that D3D accepts and that reads entirely the wrong vertices - silently, and
// looking exactly like the wrong-window bug that omitting baseVertex caused in the first place.
//
// Relying on Lock to reject an out-of-range request is not enough either: it is the caller's
// arithmetic that is unsound, and by the time Lock sees it the number is already wrong.
bool VertexRangeFits(IDirect3DVertexBuffer9* vb, UINT streamOffset, UINT firstVertex,
                     UINT count, UINT stride) {
    if (!vb || stride == 0 || count == 0) return false;
    D3DVERTEXBUFFER_DESC d{};
    if (FAILED(vb->GetDesc(&d))) return false;
    const unsigned long long begin =
        static_cast<unsigned long long>(streamOffset) +
        static_cast<unsigned long long>(firstVertex) * stride;
    const unsigned long long end = begin + static_cast<unsigned long long>(count) * stride;
    return end <= d.Size;
}

unsigned long long SkinMeshKey(void* vb, UINT offset, UINT stride, UINT minIndex, UINT count) {
    unsigned long long h = 1469598103934665603ull;
    const unsigned long long parts[5] = {
        reinterpret_cast<unsigned long long>(vb), offset, stride, minIndex, count};
    for (int i = 0; i < 5; ++i) {
        h ^= parts[i];
        h *= 1099511628211ull;
    }
    return h;
}

// Decode the bind pose once. Returns null if this mesh must not be skinned.
// A refused skinned draw falls back to PASS-THROUGH, so Remix reconstructs it as a SECOND copy
// alongside the skinned one. That makes every refusal a visible defect, not just a missed
// optimisation, and the reason has to be nameable rather than guessed at.
const char* g_skinRefuseWhy = nullptr;
#define SKIN_REFUSE(why) do { g_skinRefuseWhy = (why); return nullptr; } while (0)

// How many bytes a declaration type occupies, or 0 if this shim cannot read it.
int DeclTypeSize(int t) {
    switch (t) {
        case D3DDECLTYPE_FLOAT1:   return 4;
        case D3DDECLTYPE_FLOAT2:   return 8;
        case D3DDECLTYPE_FLOAT3:   return 12;
        case D3DDECLTYPE_FLOAT4:   return 16;
        case D3DDECLTYPE_D3DCOLOR: return 4;
        case D3DDECLTYPE_UBYTE4:   return 4;
        case D3DDECLTYPE_UBYTE4N:  return 4;
        case D3DDECLTYPE_SHORT2:   return 4;
        case D3DDECLTYPE_SHORT4:   return 8;
        case D3DDECLTYPE_SHORT2N:  return 4;
        case D3DDECLTYPE_SHORT4N:  return 8;
        default:                   return 0;
    }
}

// `firstVertex` is baseVertex + minIndex, NOT minIndex.
//
// D3D9 fetches the vertex for index i from (streamOffset + (BaseVertexIndex + i) * stride), so
// the vertices a draw actually uses start at baseVertex + MinVertexIndex. This decode ignored
// baseVertex entirely until 2026-08-20, and so did the stream bind and the cache key - all three
// consistently. Consistent is not harmless: it decodes and draws a DIFFERENT WINDOW of the shared
// character buffer, self-consistently, which renders one character's mesh with another's vertices
// and therefore another's UVs. That is the reported "each character had the wrong head", and the
// matching key collision is the "random head texture" that survived turning the mesh albedo cache
// off entirely.
//
// It is also the best remaining explanation for skinRigidSingleBone making clothing VANISH: a
// bind pose decoded from the wrong window is arbitrary geometry, and posing it by a real bone
// matrix puts it somewhere the camera never looks.
const BaseMesh* GetBaseMesh(UINT firstVertex, UINT numVertices) {
    if (!g_stream0) SKIN_REFUSE("no stream 0");
    const VertexLayout& L = g_curLayout;

    // Bounds by arithmetic, not by a magic minimum stride. The first version demanded
    // stride >= 28 - the size of the ONE layout that had been measured - and refused a perfectly
    // decodable 24-byte layout because of it, sending 23 draws a frame back to pass-through
    // where each became a second copy of a character. What actually matters is that every field
    // this decoder reads lies inside the stride.
    const int stride = static_cast<int>(g_stream0Stride);
    auto fits = [&](int off, int type) {
        const int size = DeclTypeSize(type);
        return size > 0 && off >= 0 && off + size <= stride;
    };

    if (L.posType != D3DDECLTYPE_FLOAT3 || !fits(L.posOffset, L.posType))
        SKIN_REFUSE("position is not a float3 inside the stride");
    // UBYTE4 and UBYTE4N hold the same four bytes and differ only in how the GPU scales them.
    // Since the skinning divides by the sum of the weights, that scale cancels and both decode
    // identically - so accepting UBYTE4N here costs nothing and is not a guess.
    // A declaration may legitimately carry BLENDINDICES and NO BLENDWEIGHT: that is rigid
    // single-bone attachment, where each vertex follows exactly one bone with an implicit weight
    // of 1. Measured 2026-08-19 - the refusal log printed `weights=?(-1)@-1` beside a perfectly
    // ordinary `indices=ubyte4(5)@20`, and demanding a weight element that the format never had
    // sent 55 draws a frame back to pass-through, each becoming a second copy of a character.
    // OFF by default, 2026-08-19. Enabling it took refusals from 55/frame to 0 and, in the same
    // run, "some character clothes items have vanished" - a correlation with nothing else to
    // explain it. The reasoning still looks right (BLENDINDICES with no BLENDWEIGHT is rigid
    // single-bone attachment), but "looks right" is exactly what the tint modulate and the dedup
    // also had. Something about these meshes is not what the declaration suggests - most likely
    // they are not bone-driven at all, in which case skinning them moves them somewhere the
    // camera never sees, which reads as vanishing.
    //
    // Off restores the previous behaviour: these draws are refused, pass through, and Remix
    // reconstructs them. That is the double-draw back for those items, and it is the better of
    // the two - a garment drawn twice is visible and wrong, a garment moved out of the world is
    // just gone.
    const bool rigidSingleBone = g_settings.skinRigidSingleBone && (L.blendWeightType < 0);
    if (!rigidSingleBone &&
        ((L.blendWeightType != D3DDECLTYPE_UBYTE4 && L.blendWeightType != D3DDECLTYPE_UBYTE4N &&
          L.blendWeightType != D3DDECLTYPE_D3DCOLOR && L.blendWeightType != D3DDECLTYPE_FLOAT4) ||
         !fits(L.blendWeightOffset, L.blendWeightType)))
        SKIN_REFUSE("blend weights are a type this decoder does not read");
    if ((L.blendIndexType != D3DDECLTYPE_UBYTE4 && L.blendIndexType != D3DDECLTYPE_D3DCOLOR) ||
        !fits(L.blendIndexOffset, L.blendIndexType))
        SKIN_REFUSE("blend indices are a type this decoder does not read");
    if ((L.normalType != D3DDECLTYPE_UBYTE4N && L.normalType != D3DDECLTYPE_FLOAT3) ||
        !fits(L.normalOffset, L.normalType))
        SKIN_REFUSE("normal is a type this decoder does not read");
    if ((L.texcoordType != D3DDECLTYPE_SHORT2 && L.texcoordType != D3DDECLTYPE_FLOAT2) ||
        !fits(L.texcoordOffset, L.texcoordType))
        SKIN_REFUSE("texcoord is a type this decoder does not read");

    const unsigned long long key =
        SkinMeshKey(g_stream0, g_stream0Offset, g_stream0Stride, firstVertex, numVertices);
    const auto it = g_baseMeshes.find(key);
    g_dumpSkinCacheHit = (it != g_baseMeshes.end());
    if (it != g_baseMeshes.end()) {
        // RE-DECODE IF THIS DRAW OFFERS THE SECOND UV SET AND THE CACHED COPY LACKS IT.
        //
        // The bind pose is cached from whichever draw touched the buffer FIRST, and that is
        // usually a PREPASS whose declaration is reduced - no TEXCOORD1. Every later material
        // draw then reused a cached mesh with no pattern coordinates, which is why every cloth
        // bake reported "uv2 0" even though the declaration probe had clearly seen
        // texcoord1 at offset 32.
        if (!(L.texcoord1Offset >= 0 && !it->second.hasUv2)) return &it->second;
        g_baseMeshes.erase(it);
    }
    // A full cache used to refuse every new mesh FOREVER, and a refusal falls back to
    // pass-through - so once the cap was reached, every newly seen character would silently stop
    // animating and gain a second copy, permanently, with only a counter to show for it. Clearing
    // turns that cliff into a rebuild: the meshes still on screen are re-decoded within a frame
    // or two. Releasing the pinned buffers is part of the clear, not optional.
    if (g_baseMeshes.size() >= kMaxBaseMeshes) {
        g_baseMeshByVB.clear();
        for (auto& kv : g_baseMeshes)
            if (kv.second.owner) kv.second.owner->Release();
        g_baseMeshes.clear();
        ++g_baseMeshCacheFlushes;
    }

    // The buffer is WRITEONLY. Reading it is outside what D3D9 promises, but this is DXVK
    // behind the bridge and the probe read it back correctly - and the alternative, snooping
    // every Lock the game makes, would mean holding a copy of every character mesh in the game
    // whether it is on screen or not. Verified by the values: positions land at human scale
    // (~1.6 units tall) and weight bytes sum to 255.
    D3DVERTEXBUFFER_DESC vbd{};
    if (FAILED(g_stream0->GetDesc(&vbd))) SKIN_REFUSE("GetDesc on the source buffer failed");
    if (vbd.Usage & D3DUSAGE_DYNAMIC) SKIN_REFUSE("source buffer is DYNAMIC - must not read-lock");
    if (!VertexRangeFits(g_stream0, g_stream0Offset, firstVertex, numVertices, g_stream0Stride))
        SKIN_REFUSE("vertex range lies outside the source buffer");

    void* mapped = nullptr;
    if (FAILED(g_stream0->Lock(g_stream0Offset + firstVertex * g_stream0Stride,
                               numVertices * g_stream0Stride, &mapped,
                               D3DLOCK_READONLY | D3DLOCK_NOSYSLOCK)) || !mapped)
        SKIN_REFUSE("read-lock of the source buffer failed");

    BaseMesh mesh;
    // A shim must degrade, not abort the process it is a guest in. An uncaught bad_alloc here
    // reaches the CRT as "This application has requested the Runtime to terminate it in an
    // unusual way" - which is what the user saw on 2026-08-28. Refusing the mesh costs a
    // pass-through draw; throwing costs the session.
    try {
        mesh.verts.resize(numVertices);
        // Recorded so a later draw with a fuller declaration can tell this decode was made
        // without the pattern's coordinates and ask for it again.
        mesh.hasUv2 = (L.texcoord1Offset >= 0 && fits(L.texcoord1Offset, L.texcoord1Type));
    } catch (const std::bad_alloc&) {
        g_stream0->Unlock();
        SKIN_REFUSE("out of memory decoding the bind pose");
    }
    const unsigned char* base = static_cast<const unsigned char*>(mapped);
    bool ok = true;
    for (UINT i = 0; i < numVertices && ok; ++i) {
        const unsigned char* v = base + i * g_stream0Stride;
        BaseVertex& out = mesh.verts[i];
        const float* p = reinterpret_cast<const float*>(v + L.posOffset);
        for (int k = 0; k < 3; ++k) {
            if (!std::isfinite(p[k])) { ok = false; break; }
            out.pos[k] = p[k];
        }
        if (L.normalType == D3DDECLTYPE_FLOAT3) {
            const float* n = reinterpret_cast<const float*>(v + L.normalOffset);
            for (int k = 0; k < 3; ++k) out.nrm[k] = n[k];
        } else {
            // ubyte4n arrives as 0..1; the shader expands it with v*2-1.
            const unsigned char* n = v + L.normalOffset;
            for (int k = 0; k < 3; ++k)
                out.nrm[k] = (static_cast<float>(n[k]) / 255.0f) * 2.0f - 1.0f;
        }

        out.uv2[0] = out.uv2[1] = 0.0f;
        if (L.texcoord1Offset >= 0 && fits(L.texcoord1Offset, L.texcoord1Type)) {
            if (L.texcoord1Type == D3DDECLTYPE_FLOAT2) {
                const float* t2 = reinterpret_cast<const float*>(v + L.texcoord1Offset);
                out.uv2[0] = t2[0];
                out.uv2[1] = t2[1];
            } else {
                const short* t2 = reinterpret_cast<const short*>(v + L.texcoord1Offset);
                out.uv2[0] = static_cast<float>(t2[0]);
                out.uv2[1] = static_cast<float>(t2[1]);
            }
        }

        if (L.texcoordType == D3DDECLTYPE_FLOAT2) {
            const float* t = reinterpret_cast<const float*>(v + L.texcoordOffset);
            out.uv[0] = t[0];
            out.uv[1] = t[1];
        } else {
            // Raw short2, NOT scaled. The existing conversion already installs a texture matrix
            // for short UVs, so emitting the raw values keeps one owner for that scale instead
            // of re-deriving it here and having the two disagree.
            const short* t = reinterpret_cast<const short*>(v + L.texcoordOffset);
            out.uv[0] = static_cast<float>(t[0]);
            out.uv[1] = static_cast<float>(t[1]);
        }

        // D3DCOLOR is stored BGRA, so its bytes must be reordered to match the shader's
        // .xyzw. UBYTE4 and UBYTE4N are already in order, and FLOAT4 needs no scaling because
        // the skinning divides by the sum.
        if (rigidSingleBone) {
            // One bone, full weight. The remaining three slots stay at zero and the skinning
            // loop skips them on weight, so the unused indices are never dereferenced.
            out.w[0] = 1.0f;
            out.w[1] = out.w[2] = out.w[3] = 0.0f;
        } else {
        const unsigned char* wb = v + L.blendWeightOffset;
        if (L.blendWeightType == D3DDECLTYPE_FLOAT4) {
            const float* wf = reinterpret_cast<const float*>(wb);
            for (int k = 0; k < 4; ++k) out.w[k] = std::isfinite(wf[k]) ? wf[k] : 0.0f;
        } else if (L.blendWeightType == D3DDECLTYPE_D3DCOLOR) {
            out.w[0] = static_cast<float>(wb[2]);   // R
            out.w[1] = static_cast<float>(wb[1]);   // G
            out.w[2] = static_cast<float>(wb[0]);   // B
            out.w[3] = static_cast<float>(wb[3]);   // A
        } else {
            for (int k = 0; k < 4; ++k) out.w[k] = static_cast<float>(wb[k]);
        }
        }

        const unsigned char* ib = v + L.blendIndexOffset;
        if (L.blendIndexType == D3DDECLTYPE_D3DCOLOR) {
            out.idx[0] = ib[2]; out.idx[1] = ib[1]; out.idx[2] = ib[0]; out.idx[3] = ib[3];
        } else {
            memcpy(out.idx, ib, 4);
        }
        // Only weighted components count: an unused slot carries index 255 with weight 0 and
        // would make every mesh look like it reads bone 255.
        for (int k = 0; k < 4; ++k) {
            if (out.w[k] == 0.0f || out.idx[k] >= static_cast<unsigned>(kBonesMax)) continue;
            if (out.idx[k] > mesh.maxBone) mesh.maxBone = out.idx[k];
            mesh.usedBones |= 1ull << out.idx[k];
        }
    }
    g_stream0->Unlock();
    if (!ok) return nullptr;

    ++g_baseMeshDecodes;
    mesh.owner = g_stream0;
    mesh.owner->AddRef();
    g_baseMeshByVB[g_stream0].push_back(key);
    return &(g_baseMeshes[key] = std::move(mesh));
}

// Skin into the ring buffer and point stream 0 at it. Returns false if the caller must fall
// back to passing the draw through untouched.
// The per-character morph delta, read from the game's own writes rather than by locking.
//
// Measured 2026-08-27: SR3 keeps every character's delta in ONE 5 MB D3DUSAGE_DYNAMIC buffer,
// stride 12, one contiguous block per character - the stream offset is what selects the
// character (0, 17112, 34224, ... and 17112 = 1426 verts x 12). Read-locking that is exactly
// what GetBaseMesh refuses to do and should refuse: it is write-combined memory behind a
// 32->64-bit bridge.
//
// So it is snooped, through the machinery the instance streams already use: registering the
// buffer with InstanceBufferData makes Hook_VBLock/Hook_VBUnlock copy the game's own writes,
// and the `fresh` map says which bytes have actually been written since the last discard.
//
// Applied per DRAW rather than baked into the bind pose, which is what the first attempt did.
// The bind pose is cached and the base mesh is static; the morph lives in a dynamic buffer the
// game rewrites. Baking it would have needed the cache invalidated on every write to a
// per-frame buffer - and would have served one NPC's face to the next the moment it went stale.
struct MorphSource {
    // A COPY of this draw's slice, not a pointer into the snoop cache - the locking thread can
    // resize that vector while this loop is running. Reused across draws so the allocation
    // happens once; a 7,977-vertex body slice is 96 KB and a head 17 KB.
    std::vector<unsigned char>* slice = nullptr;
    UINT stride = 0;
    int posOff = -1, nrmOff = -1;
    bool fresh = false;      // every byte of the slice was written since the last discard
    bool active = false;
};
std::vector<unsigned char> g_morphSlice;

MorphSource MorphForThisDraw(UINT firstVertex, UINT numVertices) {
    MorphSource m;
    const VertexLayout& L = g_curLayout;
    if (!g_settings.applyMorph || !g_curVS.usesMorph) return m;
    if (L.morphStream < 0 || L.morphStream >= static_cast<int>(kMaxStreams)) return m;
    if (L.morphPosType != D3DDECLTYPE_SHORT4 || L.morphPosOffset < 0) return m;
    IDirect3DVertexBuffer9* vb = g_streamVB[L.morphStream];
    if (!vb) return m;
    m.stride = g_streamStride[L.morphStream];
    if (m.stride < 8) { ++g_morphNoStride; return m; }
    RegisterSnoop(vb);
    const unsigned long long base64 =
        static_cast<unsigned long long>(g_streamOffset[L.morphStream]) +
        static_cast<unsigned long long>(firstVertex) * m.stride;
    const unsigned long long bytes64 =
        static_cast<unsigned long long>(numVertices) * m.stride;
    // A slice this draw could not possibly need is a sign the stream is not what we think it is.
    if (base64 > 0xFFFFFFFFull || bytes64 > (64ull << 20)) { ++g_morphNotSnooped; return MorphSource{}; }
    const UINT bytes = static_cast<UINT>(bytes64);
    if (g_morphSlice.size() < bytes) {
        // Growth is bounded by the cap above, and this vector is reused for every draw - so it
        // settles at the largest slice seen and then never allocates again.
        try { g_morphSlice.resize(bytes); }
        catch (const std::bad_alloc&) { ++g_morphNotSnooped; return MorphSource{}; }
    }
    if (!SnoopCopy(vb, static_cast<UINT>(base64), g_morphSlice.data(), bytes, &m.fresh)) {
        ++g_morphNotSnooped;
        return MorphSource{};
    }
    m.slice = &g_morphSlice;
    m.posOff = L.morphPosOffset;
    m.nrmOff = (L.morphNrmType == D3DDECLTYPE_UBYTE4N &&
                L.morphNrmOffset >= 0 &&
                L.morphNrmOffset + 4 <= static_cast<int>(m.stride)) ? L.morphNrmOffset : -1;
    m.active = true;
    return m;
}

// ---------------------------------------------------------------------------------------------
// STEP 3a - take ONE character mesh and submit it through the Remix API as well.
//
// Deliberately "as well", not "instead". A DOUBLED character is the proof that API-submitted
// character geometry landed; replacing the existing path in the same change would make a missing
// character mean either "the API failed" or "the old path was removed correctly", and that
// ambiguity has cost this project whole sessions before.
//
// The skinned vertices are already exactly the layout Remix wants - SkinnedVertex is
// {pos[3], nrm[3], uv[2]} and remixapi_HardcodedVertex is that plus a colour and padding - and
// they are already in WORLD space, so the instance transform is identity.
//
// Index rebasing: out[i] holds the skinned vertex for original index (baseVertex + minIndex + i),
// and the game's index buffer holds values `idx` that address (baseVertex + idx). So the index
// into our captured array is (baseVertex + idx) - (baseVertex + minIndex) = idx - minIndex.
// ---------------------------------------------------------------------------------------------
// STEP 3 TYPES, ALL IN ONE PLACE.
//
// These were spread across three regions and the declaration order caught me out FOUR times -
// each fix moved one more global earlier. The cause is structural: the capture runs in
// SkinAndBind, the build runs near the draw hook, and the per-frame DrawInstance tick sits in
// the Remix API block far above BOTH - so a type used by all three has exactly one legal home,
// which is here, after SkinnedVertex and before SkinAndBind.
//
// Only raw handles (ApiDoneMesh, the skip list, the reference position) may live earlier,
// because the draw tick needs them and they depend on no type declared below.
// ---------------------------------------------------------------------------------------------
// PRIMITIVE TYPE. SR3 draws its character slots as TRIANGLE STRIPS, and the whole of step 3b
// was built assuming lists. Proof, from the slot dump of 2026-08-31 - the ranges tile the index
// buffer exactly when a slot consumes (prims + 2) indices rather than (prims * 3):
//     slot  0: start   741, 3408 tris ->  741 + 3408 + 2 = 4151 = slot  8's start
//     slot  8: start  4151,  250 tris -> 4151 +  250 + 2 = 4403 = slot  9's start
//     slot  9: start  4403,  628 tris -> 4403 +  628 + 2 = 5033 = slot 10's start
// Reading prims*3 indices per slot therefore ran three times too far, into the NEXT slots' data,
// and then formed triangles from consecutive strip vertices as if they were independent triples.
// That is the holes, and it is why every aggregate looked healthy: the indices were all real,
// all in range, and all distinct.
//
// Hook_DrawIndexedPrimitive is handed the type as its first parameter and it was ignored.
// TINT. SR3 multiplies its diffuse result by Tint_color (c37 in the cloth shaders) - the
// shader model is documented above ShaderInfo: three Diffuse_Color_a/b/c weighted by the
// Pattern_Map channels at gamma 2.2, and then `mul oC0, r1, c37`. The API material was built with
// albedoConstant white, so that multiply was dropped and the copy rendered untinted.
//
// Captured PER SLOT, because the tint is what customisation actually varies - two slots of one
// body legitimately carry different colours, which is why remixapi_MeshInfoSurfaceTriangles has a
// material field per SURFACE rather than one per mesh.
// ALPHA STATE, read from the draw rather than assumed.
//
// Every part was given one hardcoded material - fully opaque, alphaTestType 7 (ALWAYS), one
// roughness - which flattens anything that relies on alpha. The eyes are the visible casualty:
// they use an alpha-tested or alpha-blended overlay, and forcing "always pass" makes the whole
// quad opaque.
//
// SR3's D3DCMPFUNC and Remix's AlphaTestType differ by exactly one, because Remix mirrors
// VkCompareOp: D3DCMP_NEVER=1..D3DCMP_ALWAYS=8 against NEVER=0..ALWAYS=7. So the mapping is
// (func - 1), and alpha test DISABLED means ALWAYS (7) - which is what the old hardcoded value
// happened to be, correct for the body and wrong for everything with a cutout.
struct ApiSlot {
    UINT start, prims;
    INT base;
    D3DPRIMITIVETYPE type;
    float tint[4];
    DWORD alphaTestEnable, alphaFunc, alphaRef;
    DWORD alphaBlendEnable, srcBlend, destBlend;
    // THE TEXTURE BELONGS HERE, not to the part.
    //
    // One albedo was stored per PART, taken from g_curPS.albedoStage on its first draw. Two
    // consequences, and the user saw the second: a shader with no ranked albedo gave null and the
    // part silently fell back to the body's atlas - "its just got the body albedo" - and slots
    // within one part that legitimately use DIFFERENT textures could never both be right.
    // 9 textures dumped for 10 parts was that fallback, visible in the log and not chased.
    //
    // The material is already per SURFACE, so the texture can be too.
    IDirect3DBaseTexture9* albedo;
    // CUSTOMISATION. SR3's clothing and accessories do not have a diffuse texture holding their
    // colour: the colour is three CONSTANTS weighted by the channels of a Pattern_Map, at gamma
    // 2.2, with a selector that sends some texels to a desaturated branch. The full recipe is
    // documented above ShaderInfo, read out of ir_sr3npcclothfull_c.
    //
    // Binding the raw Pattern_Map as albedo is what makes underwear, bracelets and piercings
    // WHITE - a pattern is a mask, not a colour. Three draws on the player rank a Pattern_Map at
    // 80 for exactly this reason.
    bool cloth, clothGenerated;
    float dcMul[4];             // Diffuse_Color (c11)
    int albedoRank;          // 100 = a real Diffuse_Map, 80 = a Pattern_Map, 0 = none
    char bakedPath[MAX_PATH];   // a DDS holding what this slot's PIXEL SHADER computed
    char albedoName[32];        // which sampler was chosen as this slot's base colour
    char firstName[32];         // and the first sampler its shader declares
    IDirect3DBaseTexture9* pattern;
    // HAIR. Its colour is in constants, not in a texture - the Dob_Map is white strands on a
    // green field and the Diffuse_Map is directional data, which is why hair renders white.
    // ir_sr3pchair_c shader[8] ends with  mul r0.xyz, r0, c4  then  mad r0.xyz, r2, c3, r0,
    // so Hair_Spec_Color2 (c4) is the base and Hair_Spec_Color1 (c3) the highlight.
    //
    // That shader is LIT - it samples the L-buffer - so baking its output would burn the game's
    // lighting into the albedo. The constant is taken instead and Remix lights it.
    bool hair;
    // THE DOB MAP ITSELF, not just the colour.
    //
    // Measured from the shipped asset (game-textures\clothes\cf_hair_longasym-01_dob):
    //     R  min 16  max 222   the STRANDS - fine filaments over the hair card
    //     G  min 247 max 248   flat, carries nothing
    //     B  identical to R
    // so it is one greyscale channel duplicated, and the green it appears to be is just G
    // sitting at 247 in an RGB view. The strand detail this project has been missing is in R.
    //
    // The note above is right that BAKING THE SHADER would burn the game's lighting into the
    // albedo, because that shader samples the L-buffer. This does not bake the shader: it
    // multiplies the chosen hair colour by an authored, unlit mask. Different operation, and it
    // keeps Remix doing the lighting.
    IDirect3DBaseTexture9* dob;
    float hairColor[4];
    float dcA[4], dcB[4], dcC[4];
};

// ---------------------------------------------------------------------------------------------
// EVERY PART CAPTURED IN ONE FRAME.
//
// The first version of step 3e captured one buffer per frame - build it, reset, take the next one
// on a later frame. That places each part at the objTM and in the POSE it had on ITS OWN frame, so
// the moment the player moves the parts scatter and each is frozen mid-different-animation. It
// looked like a placement bug and it is really a capture-window bug.
//
// So: one capture frame takes every part of the character, and the builds are drained afterwards,
// one per Present. The build path itself is unchanged - each pending entry is loaded into the
// globals it already reads.
struct ApiPending {
    IDirect3DVertexBuffer9* vb;
    long long firstVertex;
    float objTM[3][4];
    IDirect3DBaseTexture9* albedo;      // this part's OWN texture, not the body's atlas
    std::vector<SkinnedVertex> verts;   // BIND POSE now, not skinned output - Remix skins it
    std::vector<float> weights;         // 4 per vertex
    std::vector<unsigned> bones;        // 4 per vertex
    std::vector<float> uv2;             // 2 per vertex, raw - the PATTERN's coordinates
    unsigned boneCount, boneReg;
    // THE UV SCALE THIS PART NEEDS, captured with its vertices.
    //
    // It cannot be recomputed at build time: builds drain from Present, one per frame, long after
    // the draw that produced them, so g_curVS and g_curPS describe some unrelated draw by then.
    // The scale is a property of the captured draw and has to travel with it.
    float uvScale[2];
    int uvTilingReg[2];        // -1 when no tiling constant matched, for the report
    std::vector<ApiSlot> slots;
    std::vector<unsigned char> ib;
    UINT ibSize;
    bool ib32, ibHave;
};
ApiPending g_apiPending[kMaxApiMeshes];
unsigned g_apiPendingCount = 0, g_apiPendingNext = 0;
bool g_apiCaptureClosed = false;

std::vector<SkinnedVertex> g_apiCapVerts;
// The captured part's own uv scale, and which vertex-shader constants supplied its tiling.
// Defaulted to the bare short scale so a part captured before this existed behaves as it did.
float g_apiCapUvScale[2] = {kShortUVScale, kShortUVScale};
int g_apiCapTilingReg[2] = {-1, -1};
std::vector<unsigned> g_apiCapIndices;
UINT g_apiCapMinIndex = 0;
// Declared here rather than with the rest of the step-3b state: SkinAndBind writes them at
// capture time and it precedes the builder.
IDirect3DVertexBuffer9* g_apiCapVB = nullptr;
unsigned g_apiCapFrame = 0xFFFFFFFFu;
// The ABSOLUTE index of captured vertex 0, i.e. baseVertex + minIndex.
//
// MEASURED 2026-08-31: with 36 of 36 slots collected, every index in range (0 collapsed) and the
// placement correct, roughly half the body's polygons were still wrong. That combination rules out
// an out-of-bounds rebasing and leaves an OFF-BY-A-PER-SLOT-AMOUNT one. Splitting the builder in
// two dropped baseVertex from the collect signature, so every slot was rebased as (idx - minIndex)
// - correct only for slots whose baseVertex equals the captured draw's. The rest landed on VALID
// BUT WRONG vertices, which is exactly "the whole body with half the polygons missing at random"
// and is invisible to a range check.
//
// The correct rebasing is absolute: (slotBaseVertex + idx) - (capBaseVertex + capMinIndex).
long long g_apiCapFirstVertex = 0;
bool g_apiCapHaveVerts = false;
unsigned g_apiCharVertexCount = 0, g_apiCharTriangles = 0;
float g_apiCharAt[3] = {0, 0, 0};
const char* g_apiCharWhyNot = "not attempted yet";

bool SkinAndBind(IDirect3DDevice9* dev, INT baseVertex, UINT minIndex, UINT numVertices) {
    g_skinRefuseWhy = nullptr;
    if (!g_skinVB || numVertices == 0) { g_skinRefuseWhy = "no ring buffer"; return false; }
    // Negative baseVertex is legal in D3D9 and would address before the buffer.
    const INT first = baseVertex + static_cast<INT>(minIndex);
    if (first < 0) { g_skinRefuseWhy = "baseVertex + minIndex is negative"; return false; }
    const UINT firstVertex = static_cast<UINT>(first);
    if (baseVertex != 0) ++g_skinNonZeroBaseVertex;

    const BaseMesh* mesh = GetBaseMesh(firstVertex, numVertices);
    if (!mesh || mesh->verts.size() != numVertices) {
        if (!g_skinRefuseWhy) g_skinRefuseWhy = "cached mesh has the wrong vertex count";
        return false;
    }

    // Does the palette we are about to read belong to THIS object? Measured, not assumed - the
    // assumption in the code below is "the palette is written a few draws earlier under the same
    // objTM, so it belongs to the draw", and detached car parts posed onto NPC limbs is what it
    // looks like when that is false.
    // The draw index of the most recent palette write among the bones this mesh reads. That
    // burst is THIS object's palette; anything written materially earlier belongs to something
    // drawn before and must not pose these vertices. Zero means "no usable epoch", which leaves
    // the blend below unfiltered.
    unsigned paletteEpoch = 0;
    {
        const bool sameFrame = (g_lastBoneUploadFrame == g_frames);
        const unsigned drawsAgo =
            sameFrame && g_drawIndexThisFrame >= g_lastBoneUploadDraw
                ? g_drawIndexThisFrame - g_lastBoneUploadDraw : 0u;
        if (mesh->maxBone > g_skinMaxBoneSeen) g_skinMaxBoneSeen = mesh->maxBone;
        if (g_lastBoneUploadBones < g_skinMinReachSeen) g_skinMinReachSeen = g_lastBoneUploadBones;
        if (!sameFrame) ++g_skinPaletteStaleFrame;
        else if (mesh->maxBone >= g_lastBoneUploadBones) ++g_skinPaletteBeyondReach;
        else if (drawsAgo > 16) ++g_skinPaletteFarUpload;
        else ++g_skinPaletteOwn;

        // The measurement that does not depend on how many calls an upload took: were all the
        // bones this mesh reads written at the same time, for the same object?
        unsigned oldest = 0xFFFFFFFFu, newest = 0u;
        bool neverWritten = false;
        for (unsigned b = 0; b < static_cast<unsigned>(kBonesMax); ++b) {
            if (!(mesh->usedBones & (1ull << b))) continue;
            if (g_boneWrittenFrame[b] != g_frames) { neverWritten = true; break; }
            const unsigned w = g_boneWrittenDraw[b];
            if (w < oldest) oldest = w;
            if (w > newest) newest = w;
        }
        if (neverWritten) {
            ++g_skinBoneNeverWritten;          // reads a bone nothing wrote this frame at all
        } else if (oldest != 0xFFFFFFFFu) {
            paletteEpoch = newest;
            const unsigned spread = newest - oldest;
            if (spread > g_skinWorstSpread) g_skinWorstSpread = spread;
            // One object's palette arrives as a burst of consecutive calls, so its bones share a
            // draw index or sit within a draw or two. A wide spread means two objects' bones.
            if (spread > 4) {
                ++g_skinMixedPalette;
                // Counting these was not enough. Rejecting the older epoch stranded ~640
                // vertices per mixed draw in bind pose, which says the older bones are the
                // MAJORITY of the mesh and therefore almost certainly legitimate. So name the
                // draws instead: what they are, where they sit, and how the two epochs split.
                // If these turn out to be characters rather than vehicles, the whole palette
                // line of enquiry is finished and the car parts have another cause entirely.
                if (g_skinMixedReports < 16) {
                    ++g_skinMixedReports;
                    unsigned inNewest = 0, inOldest = 0;
                    for (unsigned b2 = 0; b2 < static_cast<unsigned>(kBonesMax); ++b2) {
                        if (!(mesh->usedBones & (1ull << b2))) continue;
                        if (newest - g_boneWrittenDraw[b2] <= kStaleBoneDraws) ++inNewest;
                        else ++inOldest;
                    }
                    const D3DMATRIX obj = FromRegisters(&g_vsConst[kRegObjTM][0], 3);
                    Log("MIXED-PALETTE #%u: ps='%s' verts=%u | objTM t=(%.1f %.1f %.1f) | "
                        "draw %u of frame %u | epochs: %u bones at draw %u (newest), "
                        "%u bones up to %u draws older | highest bone %u",
                        g_skinMixedReports, g_curPS.albedoSampler, numVertices,
                        obj._41, obj._42, obj._43, g_drawIndexThisFrame, g_frames,
                        inNewest, newest, inOldest, spread, mesh->maxBone);
                    // The uploads themselves, newest last. A contiguous block from c52 is an
                    // object publishing its whole palette; a short range starting elsewhere is
                    // a partial refresh - or another object's bones landing on top of ours.
                    for (unsigned h = 0; h < 24; ++h) {
                        const unsigned slot = (g_paletteHistoryPos + h) % 24;
                        const PaletteWrite& w = g_paletteHistory[slot];
                        if (!w.endReg || w.frame != g_frames) continue;
                        Log("        upload: draw %u regs c%u..c%u (bones %u..%u)%s",
                            w.draw, w.startReg, w.endReg,
                            w.startReg > kRegBonePalette
                                ? (w.startReg - kRegBonePalette) / kRegsPerBone : 0u,
                            (w.endReg - kRegBonePalette) / kRegsPerBone,
                            w.startReg == kRegBonePalette ? "  <- starts at c52" : "");
                    }
                }
            } else ++g_skinCleanPalette;
        }
    }

    const UINT stride = sizeof(SkinnedVertex);
    const UINT need = numVertices * stride;
    // Index i reads at (streamOffset + (baseVertex + i) * stride), so our vertex 0 must land on
    // index minIndex: the write position has to be at least (baseVertex + minIndex) * stride.
    // Meeting that here is what lets the game's own index buffer be reused verbatim - no index
    // copy and no rebasing.
    const UINT base = firstVertex * stride;
    if (static_cast<unsigned long long>(base) + need > g_skinRingBytes) {
        g_skinRefuseWhy = "mesh does not fit the ring buffer";
        return false;
    }
    if (g_skinRingPos < base) g_skinRingPos = base;
    bool discard = g_skinRingFresh;
    if (g_skinRingPos + need > g_skinRingBytes) {
        g_skinRingPos = base; discard = true; ++g_skinRingWraps;
    }
    if (discard) ++g_skinRingDiscards;
    if (g_skinRingPos + need > g_skinRingHighWater) g_skinRingHighWater = g_skinRingPos + need;

    // D3DLOCK_DISCARD is defined only for a WHOLE-buffer lock - the contract is "I am about to
    // overwrite the entire buffer", and pairing it with a sub-range is not something D3D9
    // promises anything about. So a discard locks (0, 0) and we index into the returned pointer
    // ourselves; only NOOVERWRITE takes the sub-range form.
    void* mapped = nullptr;
    const LONGLONG tLock = Now();
    const HRESULT lockHr = discard
        ? g_skinVB->Lock(0, 0, &mapped, D3DLOCK_DISCARD)
        : g_skinVB->Lock(g_skinRingPos, need, &mapped, D3DLOCK_NOOVERWRITE);
    {
        const double lockMs = MsSince(tLock);
        if (discard) g_skinLockDiscardMs += lockMs; else g_skinLockKeepMs += lockMs;
        if (lockMs > g_skinLockWorstMs) g_skinLockWorstMs = lockMs;
    }
    if (FAILED(lockHr) || !mapped) {
        g_skinRefuseWhy = "lock of the ring buffer failed";
        return false;
    }
    g_skinRingFresh = false;

    SkinnedVertex* out = reinterpret_cast<SkinnedVertex*>(
        static_cast<unsigned char*>(mapped) + (discard ? g_skinRingPos : 0u));
    // The register this shader actually declares, not the one a character shader used.
    const UINT boneBase = (g_curVS.boneReg >= 0)
        ? static_cast<UINT>(g_curVS.boneReg) : kRegBonePalette;
    if (boneBase != kRegBonePalette) ++g_skinForeignBoneReg; else ++g_skinC52BoneReg;
    if (g_curVS.usesMorph) ++g_morphDraws;
    // Name the draws whose shader reads no BLENDINDICES. These are the ones that were being
    // posed from a stranger's palette; the sampler identifies which mesh so the finding can be
    // checked against what is visibly wrong on screen rather than taken on trust.
    if (!g_curVS.usesBlendIndices && g_skinNoBoneDeclReports < 10) {
        ++g_skinNoBoneDeclReports;
        Log("NO BONE DECL #%u: ps='%s' verts=%u - this vertex shader declares no "
            "BLENDINDICES input and places the mesh by objTM alone. boneReg=%d. %s",
            g_skinNoBoneDeclReports, g_curPS.albedoSampler, numVertices, g_curVS.boneReg,
            g_settings.skinRequireBoneDecl ? "Left in its bind pose (correct)."
                                           : "STILL reading c52 (skinRequireBoneDecl=0).");
    }
    if (g_skinBoneRegSeen < 8 && boneBase != kRegBonePalette) {
        ++g_skinBoneRegSeen;
        Log("BONE REGISTER: ps='%s' declares Bone_weights at c%u, NOT c52 - every draw of this "
            "shader has been skinned from the wrong constants", g_curPS.albedoSampler, boneBase);
    }
    // GROUND TRUTH: is our shadow of the constants what the DEVICE actually holds?
    //
    // Every hypothesis so far has assumed g_vsConst mirrors the device. It is built purely from
    // SetVertexShaderConstantF calls we intercept, so anything that sets constants by another
    // route - a state block apply, a redundant-call filter of our own, a bridge-side cache -
    // leaves it stale without any of the previous probes being able to tell.
    //
    // The game renders its own cars correctly from the same c52[v.x*3] the disassembly shows, so
    // at the moment of a car draw the DEVICE must hold the car's palette. If our shadow disagrees,
    // that is the bug, and no amount of ownership heuristics on a wrong shadow would have found it.
    //
    // Read back only for the first few draws: GetVertexShaderConstantF crosses the 32-to-64-bit
    // bridge and is far too expensive to do per draw.
    if (g_shadowCheckReports < 12 && !g_curVS.usesBlendWeights && !mesh->verts.empty()) {
        const unsigned b0 = mesh->verts[0].idx[0];
        if (b0 < static_cast<unsigned>(kBonesMax)) {
            float dev4[12] = {}, obj4[12] = {};
            g_internal = true;
            const HRESULT h1 = dev->GetVertexShaderConstantF(boneBase + b0 * kRegsPerBone, dev4, 3);
            const HRESULT h2 = dev->GetVertexShaderConstantF(kRegObjTM, obj4, 3);
            g_internal = false;
            if (SUCCEEDED(h1) && SUCCEEDED(h2)) {
                const float* shadowBone = &g_vsConst[boneBase + b0 * kRegsPerBone][0];
                const float* shadowObj = &g_vsConst[kRegObjTM][0];
                float boneDiff = 0.0f, objDiff = 0.0f;
                for (int e = 0; e < 12; ++e) {
                    boneDiff = max(boneDiff, std::fabs(dev4[e] - shadowBone[e]));
                    objDiff = max(objDiff, std::fabs(obj4[e] - shadowObj[e]));
                }
                if (boneDiff > 1e-4f || objDiff > 1e-4f) {
                    ++g_shadowCheckReports;
                    ++g_shadowMismatch;
                    Log("SHADOW MISMATCH #%u: ps='%s' bone %u | bone rows differ by %.4f, objTM by "
                        "%.4f | device bone t=(%.2f %.2f %.2f) shadow t=(%.2f %.2f %.2f) | "
                        "device objTM t=(%.2f %.2f %.2f) shadow t=(%.2f %.2f %.2f)",
                        g_shadowCheckReports, g_curPS.albedoSampler, b0, boneDiff, objDiff,
                        dev4[3], dev4[7], dev4[11],
                        shadowBone[3], shadowBone[7], shadowBone[11],
                        obj4[3], obj4[7], obj4[11],
                        shadowObj[3], shadowObj[7], shadowObj[11]);
                } else {
                    ++g_shadowMatch;
                    if (g_shadowMatch == 1)
                        Log("SHADOW CHECK: device constants MATCH our shadow on the first vehicle "
                            "draw (bone %u) - the shadow is not the problem", b0);
                }
            }
        }
    }

    const MorphSource morph = MorphForThisDraw(firstVertex, numVertices);
    if (morph.active) ++g_morphAppliedDraws;
    const float* palette = &g_vsConst[boneBase][0];
    // How many bones can actually be READ from boneBase without running off g_vsConst.
    //
    // boneReg comes from the shader's own constant table as a raw 16-bit register number, and
    // nothing validated it. The palette is 64 bones x 3 registers, so anything above c64 makes
    // `palette + bone*12` index past a 1024-float array. Every shader in this game declares it
    // at c52 and the report has always said so - which is exactly why this was never noticed.
    // An out-of-bounds READ does not reliably crash; it reads whatever global follows, and poses
    // a character by it.
    const unsigned safeBones =
        (boneBase + kBonesMax * kRegsPerBone <= kMaxVsConst)
            ? static_cast<unsigned>(kBonesMax)
            : (kMaxVsConst > boneBase ? (kMaxVsConst - boneBase) / kRegsPerBone : 0u);
    if (safeBones < static_cast<unsigned>(kBonesMax) && g_boneRangeReports < 4) {
        ++g_boneRangeReports;
        Log("BONE RANGE: ps='%s' declares its palette at c%u, so only %u of %d bones fit in the "
            "%u-register shadow - influences beyond that are dropped rather than read out of "
            "bounds", g_curPS.albedoSampler, boneBase, safeBones, kBonesMax, kMaxVsConst);
    }
    double cSkin[3] = {}, cBind[3] = {};
    unsigned cN = 0;
    for (UINT i = 0; i < numVertices; ++i) {
        const BaseVertex& b = mesh->verts[i];
        // pos = base + delta/8192, and NORMAL1 replaces NORMAL0 - the order and the scale the
        // `_mc` shaders use: mad r1.xyz, v4, c0.x, v0, THEN the bone blend.
        float mpos[3] = {b.pos[0], b.pos[1], b.pos[2]};
        float mnrm[3] = {b.nrm[0], b.nrm[1], b.nrm[2]};
        if (morph.active) {
            // The slice is this draw's own copy, indexed from 0 - no cache pointer, no global
            // offset, nothing another thread can move under us.
            const size_t off = static_cast<size_t>(i) * morph.stride;
            const unsigned char* md = morph.slice->data();
            if (morph.fresh && off + morph.posOff + 8u <= morph.slice->size()) {
                const short* d =
                    reinterpret_cast<const short*>(md + off + morph.posOff);
                for (int k = 0; k < 3; ++k) mpos[k] += static_cast<float>(d[k]) * kMorphScale;
                // The normal is a DELTA too, not a replacement:
                //     mad r0.yzw, v5.xxyz, c0.y, c0.z    n_morph = v5*2 - 1
                //     mad r1,     v2,      c0.y, c0.z    n_base  = v2*2 - 1
                //     mad r0.yzw, r0,      c0.y, r1.xxyz n = n_base + 2*n_morph
                // Substituting n_morph for n_base lights the surface by a vector the game never
                // computes - the "something wrong with the normals" on heads and player skin.
                if (morph.nrmOff >= 0 &&
                    off + static_cast<size_t>(morph.nrmOff) + 4u <= morph.slice->size()) {
                    const unsigned char* n = md + off + morph.nrmOff;
                    float len = 0.0f;
                    for (int k = 0; k < 3; ++k) {
                        const float nm = (static_cast<float>(n[k]) / 255.0f) * 2.0f - 1.0f;
                        mnrm[k] = b.nrm[k] + 2.0f * nm;
                        len += mnrm[k] * mnrm[k];
                    }
                    len = std::sqrt(len);
                    if (len > 1e-8f) for (int k = 0; k < 3; ++k) mnrm[k] /= len;
                }
            } else {
                ++g_morphStaleVerts;
            }
        }
        float m[12] = {};
        float sum = 0.0f;
        // A shader that declares no blendweight input uses exactly ONE bone - index component 0,
        // implicit weight 1. Blending the other three would mix in bones the game never reads.
        // A shader with no BLENDINDICES input indexes no palette: 0 influences, so the vertex
        // falls through to the bind-pose path below and objTM places it - which is exactly and
        // only what the game's own static variant does.
        const bool reads = g_curVS.usesBlendIndices || !g_settings.skinRequireBoneDecl;
        const int influences = !reads ? 0 : (g_curVS.usesBlendWeights ? 4 : 1);
        if (i == 0) {
            if (!reads) ++g_skinNoBoneDecl;
            else if (influences == 1) ++g_skinSingleBone; else ++g_skinFourBone;
        }
        for (int k = 0; k < influences; ++k) {
            // Reject on WEIGHT, not index: slot 255 is the "unused" sentinel and would address
            // c817, far outside the 192-register palette.
            const float wk = (influences == 1) ? 1.0f : b.w[k];
            if (wk == 0.0f) continue;
            const unsigned bone = b.idx[k];
            if (bone >= safeBones) continue;
            // Reject an influence whose bone matrix belongs to a DIFFERENT object.
            //
            // The palette shadow persists across draws, so a bone this object never wrote still
            // holds the previous object's matrix. Measured 2026-08-25: 5 skinned draws a frame
            // read bones written up to 276 draws apart - two objects' palettes blended into one
            // mesh. That is the detached car part standing where an NPC's arm is.
            //
            // The surviving weights are renormalised below (the blend already divides by `sum`),
            // so dropping an influence redistributes it across this object's own bones rather
            // than shrinking the vertex toward the origin. A vertex that loses EVERY influence
            // falls through to the existing bind-pose path, which keeps it attached to its own
            // object instead of teleporting it onto someone else.
            if (g_settings.rejectStaleBones && paletteEpoch) {
                if (g_boneWrittenFrame[bone] != g_frames ||
                    paletteEpoch - g_boneWrittenDraw[bone] > kStaleBoneDraws) {
                    ++g_staleInfluencesRejected;
                    continue;
                }
            }
            // Is this bone part of THIS object's palette?
            //
            // Every upload in the game starts at c52 and runs as long as that object's skeleton
            // needs, so the palette shadow always holds a prefix belonging to the current object
            // and a tail left over from previous ones. A vertex bound to a bone in that tail is
            // posed by another object entirely.
            //
            // Multi-call uploads are handled by construction: consecutive constant writes with no
            // draw between them share a draw index, so the whole upload carries one stamp. This
            // is what the first "beyond the upload's reach" test got wrong - it compared against
            // the LAST call's length instead of the upload's stamp.
            //
            // Only some vertices of a mesh are affected, which is why the result is a car that
            // stretches as well as drifts: the bound vertices are dragged and the rest stay put.
            // The palette in the shadow belongs to a DIFFERENT draw setup, so it is not this
            // object's. Contribute nothing: the vertex keeps its bind pose and objTM places it,
            // which leaves the part attached to itself instead of posed onto another object.
            if (g_settings.paletteSetupScope && g_paletteSetupId != g_setupId) {
                ++g_paletteOutOfScope;
                continue;
            }
            // Diagnostic, and possibly an acceptable interim state: ignore the bone palette
            // entirely for single-bone (vehicle) draws and let objTM place the bind pose.
            //
            // The clamp above verifies a bone came from the MOST RECENT upload, not that the most
            // recent upload belongs to this draw's object. When a character's palette lands between
            // a car's upload and the car's draw, the character's bones carry the current stamp and
            // pass. Whether that happens depends on draw order, which depends on what is on
            // screen - which is precisely how the drift was described.
            //
            // If cars are stable and correctly placed with the palette ignored, the bones are the
            // whole problem and nothing else is involved. What is lost is whatever the bones
            // encode beyond rest position: door swing, damage deformation, wheel spin.
            if (g_settings.vehicleBonesOff && !g_curVS.usesBlendWeights) {
                ++g_vehicleBonesSkipped;
                continue;
            }
            if (g_settings.clampBonesToUpload &&
                (g_boneWrittenFrame[bone] != g_frames ||
                 g_boneWrittenDraw[bone] != g_lastBoneUploadDraw)) {
                ++g_staleBoneVerts;
                continue;   // no influence: falls through to bind pose, which objTM then places
            }
            const float* r = palette + bone * kRegsPerBone * 4;
            for (int e = 0; e < 12; ++e) m[e] += wk * r[e];
            sum += wk;
        }
        if (sum <= 0.0f) {   // no influence at all: leave the vertex in its bind pose
            ++g_skinVertsBindPose;
            memcpy(out[i].pos, mpos, sizeof(mpos));
            memcpy(out[i].nrm, mnrm, sizeof(mnrm));
            out[i].uv[0] = b.uv[0];
            out[i].uv[1] = b.uv[1];
            continue;
        }
        const float inv = 1.0f / sum;
        for (int e = 0; e < 12; ++e) m[e] *= inv;

        // Row-major float3x4: row r is m[r*4 .. r*4+3], and the shader takes dp4 against
        // (pos, 1), so the fourth component of each row is the translation.
        for (int r = 0; r < 3; ++r)
            out[i].pos[r] = m[r * 4 + 0] * mpos[0] + m[r * 4 + 1] * mpos[1] +
                            m[r * 4 + 2] * mpos[2] + m[r * 4 + 3];
        // Normals rotate only - the shader uses dp3, dropping the translation column.
        float n[3];
        for (int r = 0; r < 3; ++r)
            n[r] = m[r * 4 + 0] * mnrm[0] + m[r * 4 + 1] * mnrm[1] + m[r * 4 + 2] * mnrm[2];
        const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        const float ninv = (len > 1e-8f) ? 1.0f / len : 0.0f;
        for (int r = 0; r < 3; ++r) out[i].nrm[r] = n[r] * ninv;
        out[i].uv[0] = b.uv[0];
        out[i].uv[1] = b.uv[1];
        for (int r = 0; r < 3; ++r) { cSkin[r] += out[i].pos[r]; cBind[r] += mpos[r]; }
        ++cN;
        ++g_skinVertsTotal;
    }
    // STEP 3a capture. One draw, once: the first big skinned mesh of the run. Reading back
    // through the mapped pointer is what the centroid accumulation above already does, so it is
    // no new risk. Guarded on size so this takes a body, not an eyelash.
    // GATED ON THE ATLAS, exactly as the build is. Measured 2026-08-31: the build required
    // g_atlasDdsReady but the capture did not, so the vertices came from the FIRST big skinned
    // draw of the run - which happens during loading, long before the player character exists in
    // the world - and the mesh was then built from that stale pose at whatever position it held.
    // Submitted correctly, rendered correctly, and nowhere the player would ever look.
    // A capture and the build that consumes it must share their preconditions.
    // Capture one mesh at a time. After each is built the capture state resets, so the next
    // frame picks up another of the character's vertex buffers.
    //
    // The size floor is 64 rather than 4000 because accessories are small - 4000 was chosen to
    // find the BODY and would now exclude most of what is still missing. Membership is decided by
    // objTM translation instead: every part of one character shares it, which keeps NPCs out
    // without needing to know anything about them.
    // THE WINDOW OPENS ON THE FRAME AFTER THE BODY IS FOUND, NOT ON IT.
    //
    // The previous version opened the window at the moment the body was recognised, so every part
    // drawn EARLIER in that same frame had already gone past - and SR3 draws the body late, which
    // is why only the body was captured. A window that starts mid-frame can only ever collect the
    // tail of one.
    //
    // So the body's frame is a SCOUTING frame: it fixes the reference position and schedules the
    // real window for the next frame, which is then collected whole, body included.
    if (g_settings.remixApiCharacter && !g_apiCaptureClosed && !g_apiHaveRef &&
        g_atlasDdsReady && numVertices >= 4000) {
        g_apiRefPos[0] = g_vsConst[kRegObjTM + 0][3];
        g_apiRefPos[1] = g_vsConst[kRegObjTM + 1][3];
        g_apiRefPos[2] = g_vsConst[kRegObjTM + 2][3];
        g_apiHaveRef = true;
        g_apiCapFrame = g_frames + 1;
        Log("remix api: character found at (%.1f %.1f %.1f) - capturing every part of it on "
            "frame %u", g_apiRefPos[0], g_apiRefPos[1], g_apiRefPos[2], g_apiCapFrame);
    }

    bool wantThis = g_settings.remixApiCharacter && !g_apiCaptureClosed && g_apiHaveRef &&
                    g_atlasDdsReady && numVertices >= 64 &&
                    g_apiPendingCount < kMaxApiMeshes && g_frames == g_apiCapFrame;
    if (wantThis)
        for (unsigned d = 0; d < g_apiPendingCount; ++d)
            if (g_apiPending[d].vb == g_stream0) { wantThis = false; break; }  // already taken
    if (wantThis) {
        // Membership by objTM translation: every part of one character shares it. The tolerance is
        // wide enough to survive a frame of movement between the scouting frame and this one, and
        // the FIRST part taken re-anchors the reference so later parts match it exactly.
        const float px = g_vsConst[kRegObjTM + 0][3];
        const float py = g_vsConst[kRegObjTM + 1][3];
        const float pz = g_vsConst[kRegObjTM + 2][3];
        const float dx = px - g_apiRefPos[0], dy = py - g_apiRefPos[1], dz = pz - g_apiRefPos[2];
        if (dx * dx + dy * dy + dz * dz > 4.0f) {
            wantThis = false;                                       // a different actor
        } else if (g_apiPendingCount == 0) {
            g_apiRefPos[0] = px; g_apiRefPos[1] = py; g_apiRefPos[2] = pz;
        }
    }
    if (wantThis) {
        try {
            ApiPending& p = g_apiPending[g_apiPendingCount];
            // ------------------------------------------------------------------------------
            // UV SCALE, computed exactly as the fixed-function conversion computes it.
            //
            // The API path baked in kShortUVScale alone. The FFP path builds a texture matrix of
            // `raw * tiling / 1024` - the per-material tiling as well - and the API path never
            // learned the second half. So any part whose material tiles its albedo came out with
            // UVs wrong by exactly that factor.
            //
            // The 2026-09-04 vertex audit shows it plainly. Nine of the ten captured parts land
            // inside the unit square:
            //
            //     part 1  u[0.011 0.992]  v[0.010 0.987]
            //     part 4  u[0.005 0.992]  v[0.004 0.995]
            //     ...
            //     part 10 u[-1.072 2.787] v[-0.414 2.734]     <- the underwear
            //
            // A u span of 3.86 where every sibling spans 0.98. And the cloth bake for that same
            // part covered 3% of its texture and came out at mean 1.8 of 255 - because the baker
            // rasterises into UV space, so triangles that land outside 0..1 simply never write
            // any texels. One wrong scale, and both the geometry and the texture read as broken.
            //
            // This is not a new rule, it is the removal of an inconsistency: the FFP conversion
            // is what textures the world correctly today, and the API path now asks the same
            // question the same way. If no tiling constant matches, the answer is 1/1024 and
            // nothing changes - so a part that was right stays right.
            {
                float su = 1.0f, sv = 1.0f;
                if (g_curLayout.texcoordType == D3DDECLTYPE_SHORT2)
                    su = sv = CurrentShortUVScale();
                int tu = -1, tv = -1;
                TilingForAlbedo(tu, tv);
                if (tu >= 0 && tu < static_cast<int>(kMaxVsConst)) su *= g_vsConst[tu][0];
                if (tv >= 0 && tv < static_cast<int>(kMaxVsConst)) sv *= g_vsConst[tv][0];
                p.uvScale[0] = su;
                p.uvScale[1] = sv;
                p.uvTilingReg[0] = tu;
                p.uvTilingReg[1] = tv;
            }
            // With skinning ON the mesh must be the BIND POSE, because Remix will pose it.
            // With skinning OFF we hand over the already-skinned vertices and the result is
            // frozen - which is what step 3b shipped and what works today.
            if (!g_settings.remixApiSkinning) {
                p.verts.assign(out, out + numVertices);
                p.weights.clear();
                p.bones.clear();
                p.boneCount = 0;
                p.boneReg = 0;
            } else {
            p.verts.clear();
            p.weights.clear();
            p.bones.clear();
            p.uv2.clear();
            p.uv2.reserve(mesh->verts.size() * 2);
            p.verts.reserve(mesh->verts.size());
            p.weights.reserve(mesh->verts.size() * 4);
            p.bones.reserve(mesh->verts.size() * 4);
            for (size_t bi = 0; bi < mesh->verts.size(); ++bi) {
                const BaseVertex& bv = mesh->verts[bi];
                SkinnedVertex sv{};
                memcpy(sv.pos, bv.pos, sizeof(sv.pos));
                memcpy(sv.nrm, bv.nrm, sizeof(sv.nrm));
                sv.uv[0] = bv.uv[0];
                sv.uv[1] = bv.uv[1];
                p.verts.push_back(sv);
                p.uv2.push_back(bv.uv2[0]);
                p.uv2.push_back(bv.uv2[1]);
                // Normalised here: the game's shader divides by the sum, and Remix will not.
                float sum = bv.w[0] + bv.w[1] + bv.w[2] + bv.w[3];
                if (sum <= 0.0f) sum = 1.0f;
                for (int c = 0; c < 4; ++c) {
                    p.weights.push_back(bv.w[c] / sum);
                    const unsigned b = bv.idx[c];
                    p.bones.push_back(b < kMaxApiBones ? b : 0u);   // 255 is the unused sentinel
                }
            }
            // ALWAYS the full palette, and every bone initialised to identity at build time.
            // A blend index that exceeds boneTransforms_count would have Remix read past the
            // array, and maxBone is computed only over NON-ZERO weights - so a vertex carrying a
            // high index with a zero weight can legitimately exceed it. Sending the whole palette
            // removes the possibility rather than reasoning about it.
            p.boneCount = kMaxApiBones;
            p.boneReg = boneBase;
            }
            p.vb = g_stream0;
            p.firstVertex = static_cast<long long>(baseVertex) + minIndex;
            p.albedo = (g_curPS.albedoStage >= 0 && g_curPS.albedoStage < 8)
                           ? g_curTexture[g_curPS.albedoStage] : nullptr;
            p.slots.clear();
            p.ib.clear();
            p.ibSize = 0;
            p.ibHave = false;
            p.ib32 = false;
            // Taken at the SAME moment as the vertices, and every part is taken in the SAME
            // frame, so the whole character shares one pose and one placement.
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 4; ++c)
                    p.objTM[r][c] = g_vsConst[kRegObjTM + r][c];
            if (!g_apiHaveRef) {
                g_apiRefPos[0] = p.objTM[0][3];
                g_apiRefPos[1] = p.objTM[1][3];
                g_apiRefPos[2] = p.objTM[2][3];
                g_apiHaveRef = true;
                g_apiCapFrame = g_frames;    // the window is this frame only
            }
            g_apiCapMinIndex = minIndex;
            ++g_apiPendingCount;
        } catch (...) {
        }
    }
    g_skinVB->Unlock();

    // Only on the dump frame: measuring a bounding box over every skinned vertex of every draw
    // is exactly the per-vertex cost this path cannot afford every frame, and one frame answers
    // the question.
    g_dumpSkin = g_frameLog && !g_frameDumpDone && (g_frames + 1 == g_frameDumpTarget);
    if (g_dumpSkin) {
        g_dumpSkinFirst = firstVertex;
        g_dumpSkinVB = static_cast<void*>(g_stream0);
        for (int r = 0; r < 3; ++r) {
            g_dumpSkinBindMin[r] = g_dumpSkinBindMax[r] = mesh->verts.empty() ? 0.0f
                                                        : mesh->verts[0].pos[r];
            g_dumpSkinBindC[r]  = cN ? static_cast<float>(cBind[r] / cN) : 0.0f;
            g_dumpSkinPosedC[r] = cN ? static_cast<float>(cSkin[r] / cN) : 0.0f;
        }
        for (UINT i = 0; i < numVertices && i < mesh->verts.size(); ++i)
            for (int r = 0; r < 3; ++r) {
                const float v = mesh->verts[i].pos[r];
                if (v < g_dumpSkinBindMin[r]) g_dumpSkinBindMin[r] = v;
                if (v > g_dumpSkinBindMax[r]) g_dumpSkinBindMax[r] = v;
            }
        g_dumpSkinBones = BonesUsedCount(mesh->usedBones);
        g_dumpSkinHighBone = mesh->maxBone;
        g_dumpSkinLowBone = 0;
        for (unsigned b = 0; b < 64; ++b)
            if (mesh->usedBones & (1ull << b)) { g_dumpSkinLowBone = b; break; }
        // Newest and oldest upload behind this mesh's own bones, and how many distinct
        // objects wrote them. One object, one upload -> new == old and gens == 1.
        unsigned newest = 0, oldest = 0xFFFFFFFFu, gens = 0, seen[8] = {};
        g_dumpSkinPalNewBones = g_dumpSkinPalOldBones = 0;
        for (unsigned b = 0; b < 64; ++b) {
            if (!(mesh->usedBones & (1ull << b))) continue;
            const unsigned d = g_boneWrittenDraw[b];
            if (d >= newest) { newest = d; g_dumpSkinPalNewBones = g_boneWrittenUploadBones[b]; }
            if (d <= oldest) { oldest = d; g_dumpSkinPalOldBones = g_boneWrittenUploadBones[b]; }
            const unsigned g = g_boneWrittenObjGen[b];
            bool have = false;
            for (unsigned i = 0; i < gens; ++i) if (seen[i] == g) { have = true; break; }
            if (!have && gens < 8) seen[gens++] = g;
        }
        g_dumpSkinPalNewDraw = newest;
        g_dumpSkinPalOldDraw = (oldest == 0xFFFFFFFFu) ? 0u : oldest;
        g_dumpSkinPalGens = gens;
    }

    // THE measurement, after four runs of arguing about palettes: how far did skinning MOVE this
    // mesh from its rest position? Both centroids are in object space, so objTM cancels and this
    // is purely what the bone matrices did.
    //
    // A car part at rest sits on its own bind pose - a door swings, a wheel spins, nothing
    // travels far. A part posed by ANOTHER object's bones is displaced by that object's limb
    // positions, and the displacement changes as that object animates. That is the reported
    // "parts move around in sync with player or NPC animation", and it is a distance in units
    // rather than an interpretation of upload ordering.
    if (cN) {
        double dd = 0.0;
        for (int r = 0; r < 3; ++r) {
            const double e = (cSkin[r] - cBind[r]) / cN;
            dd += e * e;
        }
        const float d = static_cast<float>(std::sqrt(dd));
        if (d > g_skinWorstDisplace) g_skinWorstDisplace = d;
        if (d > 3.0f) {
            ++g_skinDisplaced;
            if (g_skinDisplaceReports < 16) {
                ++g_skinDisplaceReports;
                const D3DMATRIX o = FromRegisters(&g_vsConst[kRegObjTM][0], 3);
                Log("DISPLACED SKIN #%u: ps='%s' verts=%u moved %.1f units from its bind pose "
                    "| objTM t=(%.1f %.1f %.1f) | bones used %u, highest %u | draw %u frame %u",
                    g_skinDisplaceReports, g_curPS.albedoSampler, numVertices, d,
                    o._41, o._42, o._43,
                    BonesUsedCount(mesh->usedBones), mesh->maxBone,
                    g_drawIndexThisFrame, g_frames);
            }
        } else ++g_skinAtRest;

        // MEASURE THE DRIFT ITSELF, rather than assuming what "correct" looks like.
        //
        // The previous target - "a rigid part should end up centred on its origin" - was WRONG.
        // These bones legitimately place a part at its position ON the car: bone[13] is a 22
        // degree rotation plus (0, 1.121, -1.440), and the mesh is authored at the origin. An
        // offset result is normal.
        //
        // What is NOT normal is the reported symptom: the render moves while the object's real
        // position does not. So track, per mesh, the world-space centroid and the objTM it was
        // computed under. If objTM is UNCHANGED between two frames but the world centroid has
        // moved, that is the drift, in units, with no assumption about what the right answer is.
        if (!g_curVS.usesBlendWeights && cN) {
            const D3DMATRIX o = FromRegisters(&g_vsConst[kRegObjTM][0], 3);
            const float sx = static_cast<float>(cSkin[0]/cN), sy = static_cast<float>(cSkin[1]/cN),
                        sz = static_cast<float>(cSkin[2]/cN);
            const D3DVECTOR local = {sx, sy, sz};
            const D3DVECTOR w = TransformPoint(&local.x, o);
            // The cached BaseMesh pointer identifies the mesh; entries are stable until the
            // cache is invalidated, and an invalidation just restarts the comparison.
            auto& e = g_driftTrack[reinterpret_cast<unsigned long long>(mesh)];
            if (e.seen && e.frame != g_frames) {
                const bool objSame = std::fabs(e.obj[0]-o._41) < 1e-4f &&
                                     std::fabs(e.obj[1]-o._42) < 1e-4f &&
                                     std::fabs(e.obj[2]-o._43) < 1e-4f;
                const float dx = w.x-e.world[0], dy = w.y-e.world[1], dz = w.z-e.world[2];
                const float moved = std::sqrt(dx*dx + dy*dy + dz*dz);
                if (objSame) {
                    if (moved > 0.01f) {
                        ++g_driftMoved;
                        if (moved > g_driftWorst) g_driftWorst = moved;
                        if (g_driftReports < 16) {
                            ++g_driftReports;
                            const unsigned b0 = mesh->verts[0].idx[0];
                            const float* r = &g_vsConst[boneBase + b0*kRegsPerBone][0];
                            Log("DRIFT #%u: ps='%s' verts=%u bone0=%u | objTM UNCHANGED (%.2f %.2f "
                                "%.2f) but world centroid moved %.3f units",
                                g_driftReports, g_curPS.albedoSampler, numVertices, b0,
                                o._41, o._42, o._43, moved);
                            Log("    was (%8.3f %8.3f %8.3f)  now (%8.3f %8.3f %8.3f)",
                                e.world[0], e.world[1], e.world[2], w.x, w.y, w.z);
                            Log("    bone[%u] translate now (%7.3f %7.3f %7.3f), was (%7.3f %7.3f "
                                "%7.3f)  | palette upload %u bones at draw %u",
                                b0, r[3], r[7], r[11], e.bone[0], e.bone[1], e.bone[2],
                                g_lastBoneUploadBones, g_lastBoneUploadDraw);
                        }
                    } else ++g_driftStill;
                }
            }
            const unsigned b0 = mesh->verts.empty() ? 0u : mesh->verts[0].idx[0];
            const float* r = &g_vsConst[boneBase + b0*kRegsPerBone][0];
            e.seen = true; e.frame = g_frames;
            e.world[0]=w.x; e.world[1]=w.y; e.world[2]=w.z;
            e.obj[0]=o._41; e.obj[1]=o._42; e.obj[2]=o._43;
            e.bone[0]=r[3]; e.bone[1]=r[7]; e.bone[2]=r[11];
        }

        // Catch the exact failing case and dump every input to it.
        //
        // The capture gives a numeric target: a standalone rigid part should end up centred on
        // its own origin, because objTM already carries its real (physics) position. Measured
        // 2026-08-25 - a detached bumper sat at local (0, -0.085, 2.075) and a detached door at
        // (0.958, 0.515, -0.078), i.e. still at their positions ON THE CAR. The bone whose job is
        // to undo that offset is not doing it.
        //
        // Everything upstream is verified: the constants match the device (650k comparisons), the
        // register layout matches all 882 CTABs, and the transform matches the disassembly. So one
        // of the three inputs must differ for these draws specifically, and this prints all of
        // them for the draws that actually come out wrong.
        if (!g_curVS.usesBlendWeights && g_rigidOffsetReports < 20 && cN) {
            const double ox = cSkin[0]/cN, oy = cSkin[1]/cN, oz = cSkin[2]/cN;
            const double off = std::sqrt(ox*ox + oy*oy + oz*oz);
            const double bx = cBind[0]/cN, by = cBind[1]/cN, bz = cBind[2]/cN;
            if (off > 1.0) {                       // skinned result is NOT near its own origin
                ++g_rigidOffsetReports;
                const unsigned b0 = mesh->verts[0].idx[0];
                const float* r = &g_vsConst[boneBase + b0 * kRegsPerBone][0];
                const D3DMATRIX o = FromRegisters(&g_vsConst[kRegObjTM][0], 3);
                Log("OFFSET PART #%u: ps='%s' verts=%u bones=%u bone0=%u",
                    g_rigidOffsetReports, g_curPS.albedoSampler, numVertices,
                    BonesUsedCount(mesh->usedBones), b0);
                Log("    bind centroid   (%8.3f %8.3f %8.3f)", bx, by, bz);
                Log("    skinned centroid(%8.3f %8.3f %8.3f)   |offset| %.3f  <- should be ~0",
                    ox, oy, oz, off);
                Log("    bone[%u] rows    (%7.3f %7.3f %7.3f | %7.3f)", b0, r[0], r[1], r[2], r[3]);
                Log("                    (%7.3f %7.3f %7.3f | %7.3f)", r[4], r[5], r[6], r[7]);
                Log("                    (%7.3f %7.3f %7.3f | %7.3f)", r[8], r[9], r[10], r[11]);
                Log("    objTM translate (%8.3f %8.3f %8.3f) | palette upload was %u bones at "
                    "draw %u, this draw %u",
                    o._41, o._42, o._43, g_lastBoneUploadBones, g_lastBoneUploadDraw,
                    g_drawIndexThisFrame);
            }
        }

        // The population every previous test was blind to. A mesh using one or two bones is
        // posed RIGIDLY - it cannot deform, it can only translate, which is exactly "the mesh
        // matches the car, is not deformed, but moves". The spread test cannot see it because a
        // single bone has no spread to measure.
        // The single-bone population, tested against its own object rather than against a
        // skeleton-size guess.
        if (!g_curVS.usesBlendWeights) {
            const unsigned b0 = mesh->verts.empty() ? 0u : mesh->verts[0].idx[0];
            if (b0 < static_cast<unsigned>(kBonesMax)) {
                if (g_boneWrittenObjGen[b0] == g_objGeneration) {
                    ++g_rigidOwnObject;
                } else {
                    ++g_rigidForeignObject;
                    if (g_rigidForeignReports < 20) {
                        ++g_rigidForeignReports;
                        const D3DMATRIX o = FromRegisters(&g_vsConst[kRegObjTM][0], 3);
                        Log("FOREIGN BONE #%u: ps='%s' verts=%u bone %u was written under objTM "
                            "generation %u but this draw is generation %u (%u objects later) | "
                            "objTM t=(%.1f %.1f %.1f) | moved %.2f | draw %u frame %u",
                            g_rigidForeignReports, g_curPS.albedoSampler, numVertices, b0,
                            g_boneWrittenObjGen[b0], g_objGeneration,
                            g_objGeneration - g_boneWrittenObjGen[b0],
                            o._41, o._42, o._43, d, g_drawIndexThisFrame, g_frames);
                    }
                }
            }
        }

        const unsigned used = BonesUsedCount(mesh->usedBones);
        if (used <= 2) {
            ++g_skinFewBones;
            const unsigned b0 = mesh->maxBone;   // with <=2 bones this is the meaningful one
            const unsigned upl = g_boneWrittenUploadBones[b0];
            // A skeleton's size is a fingerprint. A 1-bone part reading from a 40-bone upload is
            // reading a character's palette, not its own.
            if (upl > used + 4) {
                ++g_skinFewBonesForeign;
                if (g_skinFewBoneReports < 20) {
                    ++g_skinFewBoneReports;
                    const D3DMATRIX o = FromRegisters(&g_vsConst[kRegObjTM][0], 3);
                    Log("RIGID PART #%u: ps='%s' verts=%u uses %u bone(s) (idx %u) but that bone "
                        "came from a %u-BONE upload at draw %u | moved %.2f units | "
                        "objTM t=(%.1f %.1f %.1f) | draw %u frame %u",
                        g_skinFewBoneReports, g_curPS.albedoSampler, numVertices, used, b0,
                        upl, g_boneWrittenDraw[b0], d, o._41, o._42, o._43,
                        g_drawIndexThisFrame, g_frames);
                }
            } else ++g_skinFewBonesOwn;
        }
    }

    g_internal = true;
    g_origSetStreamSource(dev, 0, g_skinVB, g_skinRingPos - base, stride);
    g_origSetVertexDeclaration(dev, nullptr);
    dev->SetFVF(kSkinnedFVF);
    g_internal = false;

    g_skinRingPos += need;
    ++g_skinnedConverted;

    return true;
}

void UnbindSkinned(IDirect3DDevice9* dev) {
    g_internal = true;
    g_origSetStreamSource(dev, 0, g_stream0, g_stream0Offset, g_stream0Stride);
    g_origSetVertexDeclaration(dev, g_curDecl);
    g_internal = false;
}

bool g_skinVBTried = false;
void CreateSkinBuffer(IDirect3DDevice9* dev) {
    // Tried ONCE. Without this flag a failed creation is retried on every draw - thousands of
    // CreateVertexBuffer calls a frame, each one a round trip across the 32->64 bit bridge, on
    // the exact path that must stay cheap.
    if (g_skinVB || g_skinVBTried || !g_settings.convertSkinned) return;
    g_skinVBTried = true;
    // Clamped: too small and every draw wraps, too large and a DEFAULT-pool allocation this big
    // may simply fail, which would refuse every skinned draw.
    {
        const int mb = (g_settings.skinRingMB < 4) ? 4
                     : (g_settings.skinRingMB > 256) ? 256 : g_settings.skinRingMB;
        g_skinRingBytes = static_cast<UINT>(mb) * 1024u * 1024u;
    }
    // DEFAULT + DYNAMIC + WRITEONLY: the pool D3D9Ex accepts, and the usage that lets
    // NOOVERWRITE/DISCARD locks avoid stalling on the GPU. Not DrawIndexedPrimitiveUP - that
    // path is recorded in sr2-fork.md as a null-pointer crash inside the Remix bridge server.
    g_internal = true;
    const HRESULT hr = dev->CreateVertexBuffer(g_skinRingBytes, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY,
                                               0, D3DPOOL_DEFAULT, &g_skinVB, nullptr);
    g_internal = false;
    Log("skinning: vertex buffer %s (%u KB, %u-byte vertices)",
        SUCCEEDED(hr) ? "created" : "FAILED", g_skinRingBytes / 1024,
        static_cast<unsigned>(sizeof(SkinnedVertex)));
}

unsigned g_shapeReports = 0;
bool g_shapeProbeDone = false;

void ProbeShapes(UINT minIndex, UINT numVertices, UINT primitiveCount, Disp d) {
    if (!g_settings.shapeProbe || g_shapeProbeDone || !g_haveCamera) return;
    if (!g_shapeProbeTarget) {
        if (g_lastFrameDraws < kDumpMinDraws) return;       // wait until we are in the world
        g_shapeProbeTarget = g_frames + 1;
    }
    if (g_frames + 1 != g_shapeProbeTarget) {
        if (g_frames + 1 > g_shapeProbeTarget && g_shapeLog) {
            fprintf(g_shapeLog, "%u\n", g_shapeReports);
            fclose(g_shapeLog);
            g_shapeLog = nullptr;
            g_shapeProbeDone = true;
            Log("shape probe written to sr3-rtx-shapes.log (%u shapes)", g_shapeReports);
        }
        return;
    }
    // Only draws Remix can actually SEE. A marked draw is dropped by the ignore list and a
    // skipped one never reaches the device, so neither can be a shape the user is looking at -
    // and the first run proved the point the expensive way: the prepass is submitted first, so
    // 148 of the 200 slots went to geometry that is already invisible and the probe never
    // reached the material pass at all. Filtering by what is visible is both the right question
    // and a large saving on locks.
    if (d == Disp::Mark || d == Disp::Skip) return;
    if (g_shapeReports >= 400) return;
    if (!g_stream0 || g_stream0Stride < 12 || g_curLayout.posOffset < 0) return;
    if (numVertices < 3 || numVertices > 8192) return;

    // Never read-lock a dynamic buffer - it is the one rule this file already learned the hard
    // way. A DYNAMIC buffer is written by the CPU every frame and locking it for reading forces
    // a sync; the instance stream is snooped through the game's own Lock/Unlock for this reason.
    D3DVERTEXBUFFER_DESC vbd{};
    if (FAILED(g_stream0->GetDesc(&vbd)) || (vbd.Usage & D3DUSAGE_DYNAMIC)) return;

    const UINT first = g_stream0Offset + minIndex * g_stream0Stride;
    void* mapped = nullptr;
    if (FAILED(g_stream0->Lock(first, numVertices * g_stream0Stride, &mapped,
                               D3DLOCK_READONLY | D3DLOCK_NOSYSLOCK)) || !mapped)
        return;

    // Object space unless the shader says otherwise: a vertex shader with no objTM emits world
    // or clip space itself, so its positions are already where they will appear.
    D3DMATRIX world = kIdentity;
    // Instanced draws carry their transform in the instance stream, not c32, so ask the same
    // helper the conversion path uses rather than reading stale registers.
    if (g_instancedDraw) { if (!InstanceWorld(world)) world = kIdentity; }
    else if (g_curVS.usesObjTM) world = FromRegisters(&g_vsConst[kRegObjTM][0], 3);
    if (!IsFinite(world)) world = kIdentity;

    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    bool finite = true;
    const unsigned char* base = static_cast<const unsigned char*>(mapped);
    const UINT step = (numVertices > 256) ? (numVertices / 256) : 1;
    for (UINT i = 0; i < numVertices; i += step) {
        const float* v = reinterpret_cast<const float*>(
            base + i * g_stream0Stride + g_curLayout.posOffset);
        if (!std::isfinite(v[0]) || !std::isfinite(v[1]) || !std::isfinite(v[2])) { finite = false; break; }
        const D3DVECTOR w = TransformPoint(v, world);
        const float c[3] = {w.x, w.y, w.z};
        for (int k = 0; k < 3; ++k) {
            if (c[k] < lo[k]) lo[k] = c[k];
            if (c[k] > hi[k]) hi[k] = c[k];
        }
    }
    g_stream0->Unlock();
    if (!finite) return;

    const float sx = hi[0] - lo[0], sy = hi[1] - lo[1], sz = hi[2] - lo[2];
    const float cx = (lo[0] + hi[0]) * 0.5f, cy = (lo[1] + hi[1]) * 0.5f, cz = (lo[2] + hi[2]) * 0.5f;
    const float dx = cx - g_camX, dy = cy - g_camY, dz = cz - g_camZ;
    const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);

    // "Around me" - compact, and close. The world is thousands of units across, so a shape that
    // is both small and near the player is exactly the population being described.
    const float biggest = max(max(sx, sy), sz);
    if (biggest > 400.0f || dist > 300.0f) return;

    if (!g_shapeLog) {
        char sname[64];
        CaptureFileName(sname, sizeof(sname), "shapes");
        g_shapeLog = _fsopen(sname, "w", _SH_DENYNO);
        if (!g_shapeLog) { g_shapeProbeDone = true; return; }
        fprintf(g_shapeLog,
                "sr3-rtx shape probe - world-space bounds of every draw near the camera, one frame\n"
                "frame %u, camera (%.1f %.1f %.1f)\n"
                "flat = smallest axis relative to the largest; a dish, cap or ring is very flat\n\n"
                "%5s %-8s %-6s %-6s %-22s %-22s %7s %5s  %s\n",
                g_shapeProbeTarget, g_camX, g_camY, g_camZ,
                "draw", "disp", "verts", "prims", "size x/y/z", "centre", "dist", "flat", "shader / why");
    }
    ++g_shapeReports;
    static const char* kD[] = {"CONVERT", "HIDE", "SKIP", "PASS", "MARK"};
    char sizeStr[32], centreStr[32];
    _snprintf_s(sizeStr, sizeof(sizeStr), _TRUNCATE, "%.1f/%.1f/%.1f", sx, sy, sz);
    _snprintf_s(centreStr, sizeof(centreStr), _TRUNCATE, "%.0f/%.0f/%.0f", cx, cy, cz);
    const float smallest = min(min(sx, sy), sz);
    fprintf(g_shapeLog, "%5u %-8s %-6u %-6u %-22s %-22s %7.1f %5.2f  ps='%s' rank=%d | %s\n",
            g_drawIndexThisFrame, kD[static_cast<int>(d)], numVertices, primitiveCount,
            sizeStr, centreStr, dist, (biggest > 1e-4f) ? (smallest / biggest) : 0.0f,
            g_curPS.firstSampler, g_curPS.albedoRank, g_dispReason);
}


// ---------------------------------------------------------------------------------------------
// STEP 3b - ALL of the character's slots, as surfaces of one mesh.
//
// SR3's body is ONE 7,977-vertex buffer drawn 35 times with different INDEX RANGES, one per
// clothing/body slot (frame dump 2026-08-31: same vb, same bone palette, primitive counts from 84
// to 3408). Step 3a took a single range and produced "some polygons", correctly.
//
// remixapi_MeshInfo::surfaces_count takes an array of surfaces, each with its own indices and its
// own material - which is exactly the shape of the problem. One mesh, N surfaces, one shared
// vertex array.
//
// Two phases, because a slot list is only complete at the END of the frame that produced it:
//   COLLECT  during the capture frame, note every draw that indexes the captured vertex buffer,
//            and copy the game's index buffer ONCE (a character IB is ~90 KB, so copying it whole
//            costs one lock instead of one per slot).
//   BUILD    on the first frame AFTER that, when no further slots can arrive.

std::vector<unsigned char> g_apiCapIB;
UINT g_apiCapIBSize = 0;
bool g_apiCapIB32 = false;
bool g_apiCapIBHave = false;
std::vector<ApiSlot> g_apiSlots;

// THE MESH DATA MUST OUTLIVE THE BUILDER.
//
// remixapi_MeshInfoSurfaceTriangles holds POINTERS to the vertices and indices - vertices_values,
// indices_values - and nothing in the header says CreateMesh copies them. The step-2 cube has
// worked from the first attempt precisely because its arrays are static globals
// (g_remixCubeVerts / g_remixCubeIdx). The character built its arrays as LOCAL std::vectors, so
// they were freed the instant RemixBuildCharacter returned and every DrawInstance afterwards read
// released memory - which is a complete, sound, correctly rebased mesh that renders as holes and
// smeared texture, exactly as reported.
//
// The vertex audit is what made this findable: 0 zeroed vertices, a proper character bounding box
// and clean 0..1 uvs proved the DATA was right, which left only the DESCRIPTION.
// One set per finished mesh, indexed by g_apiDoneCount. See the note on ApiDoneMesh: a shared
// set would be overwritten by the next build while Remix still points into it.
struct ApiMeshStore {
    std::vector<RemixHardcodedVertex> verts;
    std::vector<float> weights;
    std::vector<unsigned> bones;
    std::vector<float> uv2;
    std::vector<std::vector<unsigned>> slotIndices;
    std::vector<RemixMeshInfoSurfaceTriangles> surfaces;
};
ApiMeshStore g_apiStore[kMaxApiMeshes];
unsigned g_apiSlotsSeen = 0, g_apiOutOfRange = 0, g_apiSurfaces = 0;
IDirect3DBaseTexture9* g_apiCapAlbedo = nullptr;
std::vector<float> g_apiCapWeights;
std::vector<unsigned> g_apiCapBones;
std::vector<float> g_apiCapUv2;
unsigned g_apiCapBoneCount = 0, g_apiCapBoneReg = 0;
unsigned g_apiSlotsDuplicate = 0, g_apiDegenerate = 0, g_apiStripSlots = 0;
unsigned g_apiDegeneratePrev = 0;   // so each part reports its OWN degenerate count
unsigned g_apiClothGenerated = 0, g_apiClothApprox = 0, g_apiClothBlackConst = 0;
// Diagnostics for the two generators that fired zero times on 2026-09-07.
unsigned g_hairSlotReports = 0, g_clothSlotReports = 0;
unsigned g_uv2Reports = 0;
unsigned g_psDumped = 0;
IDirect3DPixelShader9* g_psDumpedPtr[8] = {};
constexpr unsigned kMaxApiSlots = 64;

// Called from the draw hook for every draw, during the capture frame only.
// ---------------------------------------------------------------------------------------------
// BAKING WHAT THE PIXEL SHADER COMPUTES
//
// SR3's clothing and accessories have no texture holding their colour. The colour is computed in
// the PIXEL SHADER from three constants weighted per texel by a Pattern_Map at gamma 2.2, with a
// selector branch, then multiplied by Tint_color. Remix never runs a pixel shader - it picks one
// bound texture as the albedo and applies its own PBR model - so that colour cannot survive by any
// amount of choosing a better texture or a better constant. Approximating it with Diffuse_Color_a
// is what blacked the top.
//
// The answer is to RUN the game's own pixel shader and keep the result: render the item into an
// offscreen target whose SCREEN SPACE IS UV SPACE. Every texel of the output then holds exactly
// the colour that shader computes for that texel, and the result is a texture Remix can sample.
// This is not an approximation of the recipe - it IS the recipe, executed by the same shader with
// the same constants and the same textures.
//
// The one piece the game does not provide is a vertex shader that maps UV to clip space. We build
// it. In SM3.0 vertex inputs bind by DECLARATION USAGE rather than register number, so a
// standalone shader that declares dcl_texcoord v0 receives the texture coordinates from the game's
// own vertex declaration without touching the game's shader at all.
//
// The bytecode is assembled here directly. It is checked by D3D9 itself: CreateVertexShader
// validates the token stream, so a mistake in this assembler is reported as a failed HRESULT and
// the feature disables itself, rather than corrupting a draw.

namespace sm3 {
constexpr DWORD kVersionVs30 = 0xFFFE0300u;
constexpr DWORD kEnd         = 0x0000FFFFu;

// Register types (D3DSHADER_PARAM_REGISTER_TYPE). The type is split across two bit ranges in a
// parameter token, which is the usual source of hand-assembly mistakes, so it is done in one place.
constexpr unsigned kRegTemp = 0, kRegInput = 1, kRegConst = 2, kRegOutput = 3;

constexpr unsigned kOpMov = 1, kOpMad = 4, kOpDcl = 31, kOpDef = 81;

// Swizzles, 2 bits per component starting at bit 16 of a source token.
constexpr unsigned kSwzXYZW = 0xE4, kSwzX = 0x00, kSwzY = 0x55, kSwzZ = 0xAA, kSwzW = 0xFF;
// Write masks, bits 16..19 of a destination token.
constexpr unsigned kMaskX = 0x1, kMaskY = 0x2, kMaskZ = 0x4, kMaskW = 0x8;
constexpr unsigned kMaskXY = kMaskX | kMaskY, kMaskAll = 0xF;

inline DWORD Instr(unsigned opcode, unsigned lengthTokens) {
    return opcode | (lengthTokens << 24);
}
inline DWORD Param(unsigned type, unsigned reg) {
    // bits 28..30 hold the low three bits of the type, bits 11..12 the high two.
    return 0x80000000u | reg | ((type & 7u) << 28) | (((type >> 3) & 3u) << 11);
}
inline DWORD Dest(unsigned type, unsigned reg, unsigned mask) {
    return Param(type, reg) | (mask << 16);
}
inline DWORD Source(unsigned type, unsigned reg, unsigned swizzle) {
    return Param(type, reg) | (swizzle << 16);
}
inline DWORD AsBits(float f) {
    DWORD d;
    memcpy(&d, &f, sizeof(d));
    return d;
}
}  // namespace sm3

// D3DDECLUSAGE_TEXCOORD
constexpr unsigned kUsageTexcoord = 5;
constexpr unsigned kUsagePosition = 0;

// Constant registers for our two literals. Chosen at the very top of the file's range: SR3's bone
// palette runs c52..c243 and projTM/objTM sit at c28..c34, so c250+ cannot collide with anything
// the game uploads.
constexpr unsigned kBakeConstA = 250, kBakeConstB = 251;

// vs_3_0: position = the texture coordinate, mapped from [0,1] to clip space, with v flipped
// because texture space runs downwards and clip space upwards. Texture coordinates reach us as RAW
// SHORTS - SR3 scales them by 1/1024 in its own shaders - so that scale is folded into the
// literal: 2/1024 = 0.001953125.
std::vector<DWORD> BuildUvBakeVs(unsigned passthroughCount) {
    using namespace sm3;
    std::vector<DWORD> t;
    t.push_back(kVersionVs30);

    // dcl_texcoord0 v0
    t.push_back(Instr(kOpDcl, 2));
    t.push_back(0x80000000u | kUsageTexcoord | (0u << 16));
    t.push_back(Dest(kRegInput, 0, kMaskAll));

    // dcl_position o0
    t.push_back(Instr(kOpDcl, 2));
    t.push_back(0x80000000u | kUsagePosition | (0u << 16));
    t.push_back(Dest(kRegOutput, 0, kMaskAll));

    // dcl_texcoord<i> o<1+i> - the pixel shader reads several interpolants and we do not know
    // which one carries the pattern's coordinates, so every one is given the same uv. A wrong
    // guess here shows as a mis-sampled bake, not a crash.
    for (unsigned i = 0; i < passthroughCount; ++i) {
        t.push_back(Instr(kOpDcl, 2));
        t.push_back(0x80000000u | kUsageTexcoord | (i << 16));
        t.push_back(Dest(kRegOutput, 1 + i, kMaskAll));
    }

    // def c250, 2/1024, -1, -2/1024, 1
    t.push_back(Instr(kOpDef, 5));
    t.push_back(Dest(kRegConst, kBakeConstA, kMaskAll));
    t.push_back(AsBits(0.001953125f));
    t.push_back(AsBits(-1.0f));
    t.push_back(AsBits(-0.001953125f));
    t.push_back(AsBits(1.0f));

    // def c251, 0, 0, 0, 1
    t.push_back(Instr(kOpDef, 5));
    t.push_back(Dest(kRegConst, kBakeConstB, kMaskAll));
    t.push_back(AsBits(0.0f));
    t.push_back(AsBits(0.0f));
    t.push_back(AsBits(0.0f));
    t.push_back(AsBits(1.0f));

    // mad o0.x, v0.x, c250.x, c250.y      ->  u*2/1024 - 1
    t.push_back(Instr(kOpMad, 4));
    t.push_back(Dest(kRegOutput, 0, kMaskX));
    t.push_back(Source(kRegInput, 0, kSwzX));
    t.push_back(Source(kRegConst, kBakeConstA, kSwzX));
    t.push_back(Source(kRegConst, kBakeConstA, kSwzY));

    // mad o0.y, v0.y, c250.z, c250.w      ->  1 - v*2/1024
    t.push_back(Instr(kOpMad, 4));
    t.push_back(Dest(kRegOutput, 0, kMaskY));
    t.push_back(Source(kRegInput, 0, kSwzY));
    t.push_back(Source(kRegConst, kBakeConstA, kSwzZ));
    t.push_back(Source(kRegConst, kBakeConstA, kSwzW));

    // mov o0.zw, c251   -> z = 0, w = 1
    t.push_back(Instr(kOpMov, 2));
    t.push_back(Dest(kRegOutput, 0, kMaskZ | kMaskW));
    t.push_back(Source(kRegConst, kBakeConstB, kSwzXYZW));

    for (unsigned i = 0; i < passthroughCount; ++i) {
        t.push_back(Instr(kOpMov, 2));
        t.push_back(Dest(kRegOutput, 1 + i, kMaskAll));
        t.push_back(Source(kRegInput, 0, kSwzXYZW));
    }

    t.push_back(kEnd);
    return t;
}

IDirect3DVertexShader9* g_bakeVs = nullptr;
bool g_bakeTried = false, g_bakeDead = false;
unsigned g_bakeOk = 0, g_bakeFail = 0;

// Built once. If D3D9 rejects the bytecode the whole feature retires itself and says so - the
// assembler above is hand-written and this is the check that it is right.
bool BakeShaderReady(IDirect3DDevice9* dev) {
    if (g_bakeDead) return false;
    if (g_bakeVs) return true;
    if (g_bakeTried) return false;
    g_bakeTried = true;
    const std::vector<DWORD> code = BuildUvBakeVs(7);
    const bool wasInternal = g_internal;
    g_internal = true;
    const HRESULT hr = dev->CreateVertexShader(code.data(), &g_bakeVs);
    g_internal = wasInternal;
    if (FAILED(hr) || !g_bakeVs) {
        g_bakeDead = true;
        Log("bake: D3D9 REJECTED the hand-assembled uv-bake vertex shader (hr=0x%08lX, %u tokens)"
            " - the token stream is wrong and baking is disabled", hr,
            static_cast<unsigned>(code.size()));
        return false;
    }
    Log("bake: uv-bake vertex shader accepted by D3D9 (%u tokens). Screen space is now UV space, "
        "so the game's own pixel shader can be run into an offscreen target and its computed "
        "albedo kept.", static_cast<unsigned>(code.size()));
    return true;
}


// ---------------------------------------------------------------------------------------------
// The bake pass itself.
//
// Re-issues the game's draw with OUR vertex shader and the game's own pixel shader, into an
// offscreen target. Everything else is left exactly as the game set it: the same vertex and index
// buffers, the same declaration, the same pixel-shader constants, the same textures. Only where
// the triangles LAND changes, and they land in UV space.
//
// Two things are neutralised for the duration:
//   depth and culling, because a UV unwrap has no meaningful depth and its winding is arbitrary;
//   alpha test and blending, because the point is to capture the colour the shader COMPUTES, not
//   the subset of it that would have survived a depth-tested composite.
//
// The whole thing runs with g_internal set, so none of the shim's own hooks react to it.
// 512, not 1024. A clothing item's unwrap does not need more, and this is a THIRTY-TWO BIT
// process already holding a skinning ring, a 96 MB vertex snoop, a 48 MB uv arena and Remix's own
// client. The target, its system-memory readback surface and the transient copy are 1 MB each at
// this size instead of 4.
constexpr UINT kBakeSize = 512;
IDirect3DTexture9* g_bakeTex = nullptr;
IDirect3DSurface9* g_bakeSurf = nullptr;
IDirect3DSurface9* g_bakeSys = nullptr;
// HIDE THE BAKE DRAW FROM REMIX.
//
// Remix crashed its 64-bit server on the first bake (0xc0000005, 2026-09-03). Vertex capture is on,
// so it tries to capture the output of our hand-assembled shader and falls over.
//
// Remix declines to raytrace any draw issued inside an OCCLUSION QUERY - its own log says
// "Trying to raytrace an occlusion query. Ignoring." - while D3D9 still rasterises it normally.
// That is precisely what a bake needs: the pixel shader runs, the target fills, and Remix never
// looks at it. The query result is never read.
IDirect3DQuery9* g_bakeQuery = nullptr;
unsigned g_bakeDumped = 0, g_bakeAttempts = 0, g_bakeThisFrame = 0;
unsigned g_bakeFrame = 0xFFFFFFFFu;

bool BakeTargetsReady(IDirect3DDevice9* dev) {
    if (g_bakeTex && g_bakeSurf && g_bakeSys) return true;
    const bool wasInternal = g_internal;
    g_internal = true;
    bool ok = true;
    if (!g_bakeTex &&
        FAILED(dev->CreateTexture(kBakeSize, kBakeSize, 1, D3DUSAGE_RENDERTARGET,
                                  D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g_bakeTex, nullptr)))
        ok = false;
    if (ok && !g_bakeSurf && FAILED(g_bakeTex->GetSurfaceLevel(0, &g_bakeSurf))) ok = false;
    // SYSTEMMEM, because GetRenderTargetData is the only sanctioned way back from a render target
    // and it requires one. Reading the GAME's targets this way froze SR3 twice; reading OUR OWN,
    // which we created and filled, was proved to work by the 2026-08-29 copy self-test.
    if (ok && !g_bakeSys &&
        FAILED(dev->CreateOffscreenPlainSurface(kBakeSize, kBakeSize, D3DFMT_A8R8G8B8,
                                                D3DPOOL_SYSTEMMEM, &g_bakeSys, nullptr)))
        ok = false;
    if (ok && !g_bakeQuery &&
        FAILED(dev->CreateQuery(D3DQUERYTYPE_OCCLUSION, &g_bakeQuery)))
        ok = false;
    g_internal = wasInternal;
    if (!ok) {
        g_bakeDead = true;
        Log("bake: could not create the 1024x1024 target or its readback surface - disabled");
    }
    return ok;
}

// Returns a path to a written DDS, or nullptr.
const char* BakeSlotAlbedo(IDirect3DDevice9* dev, D3DPRIMITIVETYPE type, INT baseVertex,
                           UINT minIndex, UINT numVertices, UINT startIndex, UINT primitiveCount) {
    if (!g_settings.bakeShaderAlbedo || g_bakeDead) return nullptr;
    if (!BakeShaderReady(dev) || !BakeTargetsReady(dev)) return nullptr;

    // CAP ON ATTEMPTS, NOT SUCCESSES.
    //
    // This read `if (g_bakeDumped >= 16)`, and g_bakeDumped only increments on SUCCESS - so every
    // failing path returned without advancing anything and the bake was retried for every
    // customised slot, every frame, each attempt a full draw plus a GetRenderTargetData readback
    // across the 32-to-64-bit bridge. That is an unbounded loop in a 32-bit address space, and it
    // exhausted memory.
    //
    // A budget that only counts the good outcomes is not a budget.
    ++g_bakeAttempts;
    if (g_bakeAttempts > 24) {
        if (!g_bakeDead) {
            g_bakeDead = true;
            Log("bake: attempt budget spent (%u attempts, %u written, %u failed) - baking off",
                g_bakeAttempts, g_bakeOk, g_bakeFail);
        }
        return nullptr;
    }
    if (g_bakeFail >= 3) {
        if (!g_bakeDead) {
            g_bakeDead = true;
            Log("bake: %u failures - baking off rather than retrying", g_bakeFail);
        }
        return nullptr;
    }
    // At most two a frame. Eleven 512x512 readbacks and DDS writes in ONE frame is a stall even
    // when every one of them succeeds.
    if (g_bakeFrame == g_frames && g_bakeThisFrame >= 2) return nullptr;
    if (g_bakeFrame != g_frames) { g_bakeFrame = g_frames; g_bakeThisFrame = 0; }
    ++g_bakeThisFrame;

    const bool wasInternal = g_internal;
    g_internal = true;

    IDirect3DSurface9* oldRt = nullptr;
    IDirect3DSurface9* oldDs = nullptr;
    IDirect3DVertexShader9* oldVs = nullptr;
    D3DVIEWPORT9 oldVp{};
    dev->GetRenderTarget(0, &oldRt);
    dev->GetDepthStencilSurface(&oldDs);
    dev->GetVertexShader(&oldVs);
    dev->GetViewport(&oldVp);
    const DWORD oldZ = ShadowGetRS(dev, D3DRS_ZENABLE);
    const DWORD oldZW = ShadowGetRS(dev, D3DRS_ZWRITEENABLE);
    const DWORD oldCull = ShadowGetRS(dev, D3DRS_CULLMODE);
    const DWORD oldAT = ShadowGetRS(dev, D3DRS_ALPHATESTENABLE);
    const DWORD oldAB = ShadowGetRS(dev, D3DRS_ALPHABLENDENABLE);
    const DWORD oldCW = ShadowGetRS(dev, D3DRS_COLORWRITEENABLE);

    bool ok = SUCCEEDED(dev->SetRenderTarget(0, g_bakeSurf));
    if (ok) {
        dev->SetDepthStencilSurface(nullptr);
        D3DVIEWPORT9 vp{0, 0, kBakeSize, kBakeSize, 0.0f, 1.0f};
        dev->SetViewport(&vp);
        dev->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_ARGB(0, 0, 0, 0), 1.0f, 0);
        dev->SetVertexShader(g_bakeVs);
        dev->SetRenderState(D3DRS_ZENABLE, FALSE);
        dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
        // Inside the query, so Remix ignores it.
        g_bakeQuery->Issue(D3DISSUE_BEGIN);
        ok = SUCCEEDED(g_origDrawIndexedPrimitive(dev, type, baseVertex, minIndex, numVertices,
                                                  startIndex, primitiveCount));
        g_bakeQuery->Issue(D3DISSUE_END);
    }

    dev->SetVertexShader(oldVs);
    if (oldVs) oldVs->Release();
    dev->SetRenderState(D3DRS_ZENABLE, oldZ);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, oldZW);
    dev->SetRenderState(D3DRS_CULLMODE, oldCull);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, oldAT);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, oldAB);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, oldCW);
    if (oldRt) { dev->SetRenderTarget(0, oldRt); oldRt->Release(); }
    dev->SetDepthStencilSurface(oldDs);
    if (oldDs) oldDs->Release();
    dev->SetViewport(&oldVp);

    static char path[MAX_PATH];
    path[0] = 0;
    if (ok && SUCCEEDED(dev->GetRenderTargetData(g_bakeSurf, g_bakeSys))) {
        D3DLOCKED_RECT lr{};
        if (SUCCEEDED(g_bakeSys->LockRect(&lr, nullptr, D3DLOCK_READONLY)) && lr.pBits) {
            std::vector<unsigned char> bgra;
            try {
                bgra.resize(static_cast<size_t>(kBakeSize) * kBakeSize * 4);
                for (UINT y = 0; y < kBakeSize; ++y)
                    memcpy(&bgra[static_cast<size_t>(y) * kBakeSize * 4],
                           static_cast<const unsigned char*>(lr.pBits) +
                               static_cast<size_t>(y) * lr.Pitch,
                           static_cast<size_t>(kBakeSize) * 4);
                // What did the shader actually produce? A mean of zero means the draw landed
                // outside the target - the uv mapping - rather than the shader misbehaving.
                double acc = 0.0;
                unsigned n = 0, opaque = 0;
                for (UINT i = 0; i < kBakeSize * kBakeSize; i += 97) {
                    acc += bgra[i * 4] + bgra[i * 4 + 1] + bgra[i * 4 + 2];
                    if (bgra[i * 4 + 3]) ++opaque;
                    ++n;
                }
                char dir[MAX_PATH] = {0};
                GetModuleFileNameA(GetModuleHandleW(nullptr), dir, MAX_PATH);
                char* sl = strrchr(dir, '\\');
                if (sl) *(sl + 1) = 0;
                ++g_bakeDumped;
                sprintf_s(path, "%ssr3-remix-bake-%u.dds", dir, g_bakeDumped);
                if (WriteBgraDds(path, kBakeSize, kBakeSize, bgra.data())) {
                    ++g_bakeOk;
                    Log("bake #%u: wrote %s | mean %.1f of 255 over %u samples, %u%% covered",
                        g_bakeDumped, path, n ? acc / (3.0 * n) : 0.0, n,
                        n ? (opaque * 100 / n) : 0);
                } else {
                    path[0] = 0;
                    ++g_bakeFail;
                }
            } catch (...) { path[0] = 0; ++g_bakeFail; }
            g_bakeSys->UnlockRect();
        } else {
            ++g_bakeFail;
        }
    } else if (ok) {
        ++g_bakeFail;
        Log("bake: GetRenderTargetData failed on our own target");
    } else {
        ++g_bakeFail;
    }
    g_internal = wasInternal;
    return path[0] ? path : nullptr;
}

void RemixCollectCharacterSlot(IDirect3DDevice9* dev, D3DPRIMITIVETYPE type, INT baseVertex,
                               UINT minIndex, UINT numVertices, UINT startIndex,
                               UINT primitiveCount) {
    if (!g_settings.remixApiCharacter || g_apiCharBuilt || g_apiCharFailed) return;
    if (!g_apiPendingCount || g_frames != g_apiCapFrame) return;
    unsigned pi = kMaxApiMeshes;
    for (unsigned d = 0; d < g_apiPendingCount; ++d)
        if (g_apiPending[d].vb == g_stream0) { pi = d; break; }
    if (pi == kMaxApiMeshes) return;
    ApiPending& P = g_apiPending[pi];
    // minIndex deliberately NOT required to match: the rebasing is absolute now, so a slot
    // with a different window is handled correctly rather than skipped. The vertex COUNT still
    // has to match, as a cheap "is this the same body mesh" test.
    (void)minIndex;
    if (numVertices != P.verts.size()) return;
    if (primitiveCount == 0 || P.slots.size() >= kMaxApiSlots) return;

    // Copy the whole index buffer once, on the first slot. Every later slot reads out of this
    // copy, so 35 slots cost one lock rather than 35 bridge round trips.
    if (!P.ibHave) {
        IDirect3DIndexBuffer9* ib = nullptr;
        g_internal = true;
        const HRESULT hr = dev->GetIndices(&ib);
        g_internal = false;
        if (FAILED(hr) || !ib) return;
        D3DINDEXBUFFER_DESC d{};
        void* mapped = nullptr;
        if (SUCCEEDED(ib->GetDesc(&d)) && d.Size &&
            SUCCEEDED(ib->Lock(0, 0, &mapped, D3DLOCK_READONLY)) && mapped) {
            try {
                P.ib.assign(static_cast<const unsigned char*>(mapped),
                            static_cast<const unsigned char*>(mapped) + d.Size);
                P.ibSize = d.Size;
                P.ib32 = (d.Format == D3DFMT_INDEX32);
                P.ibHave = true;
            } catch (...) { P.ib.clear(); }
            ib->Unlock();
        }
        ib->Release();
        if (!P.ibHave) return;
    }
    // DEDUP. SR3 submits the same skinned geometry more than once a frame - the shim's own
    // skinning report has said "19 exact duplicates dropped/frame" all along, and its FFP path
    // drops them for exactly this reason. Collected without that filter, an identical index range
    // enters the mesh twice, and two coplanar copies of a surface z-fight against each other. The
    // result is a correctly built, correctly textured mesh full of holes - which is what was on
    // screen, and what no counter could show, because every slot is individually valid.
    ++g_apiSlotsSeen;
    if (type != D3DPT_TRIANGLESTRIP && type != D3DPT_TRIANGLELIST) return;
    for (size_t i = 0; i < P.slots.size(); ++i)
        if (P.slots[i].start == startIndex && P.slots[i].prims == primitiveCount &&
            P.slots[i].base == baseVertex) {
            ++g_apiSlotsDuplicate;
            return;
        }
    ApiSlot slot{};
    slot.start = startIndex;
    slot.prims = primitiveCount;
    slot.base = baseVertex;
    slot.type = type;
    slot.tint[0] = slot.tint[1] = slot.tint[2] = slot.tint[3] = 1.0f;
    if (g_curPS.tintColorReg >= 0 &&
        g_curPS.tintColorReg < static_cast<int>(kMaxPsConst))
        memcpy(slot.tint, g_psConst[g_curPS.tintColorReg], sizeof(slot.tint));
    // The ranked albedo stage if the shader has one, otherwise whatever is actually bound at
    // stage 0. Falling back to the BODY's atlas - which is what happened - is never right for a
    // part that simply has no ranked sampler.
    slot.albedo = nullptr;
    // If the shim can GENERATE the customised albedo - pattern combined with the three colours -
    // that is the correct texture and beats anything bound. It only handles the family where the
    // pattern IS the albedo; the player's own clothing samples a Diffuse_Map as well through a
    // second UV set, which cannot be folded into one texture, so it falls through.
    slot.cloth = false;
    slot.clothGenerated = false;
    slot.dcMul[0] = slot.dcMul[1] = slot.dcMul[2] = slot.dcMul[3] = 1.0f;
    slot.albedoRank = g_curPS.albedoRank;
    slot.bakedPath[0] = 0;
    // DUMP THE ACTUAL PIXEL SHADER.
    //
    // The recipe has been read from files picked out of the corpus by name, and twice it was the
    // WRONG file - the NPC cloth shader has a sum/deviation branch the player's does not. The only
    // way to stop guessing is to take the bytecode the device is actually running and disassemble
    // that. One file per distinct shader, capped.
    if (g_curPS.diffuseColorReg[0] >= 0 && g_psDumped < 6) {
        IDirect3DPixelShader9* ps = nullptr;
        const bool wi = g_internal;
        g_internal = true;
        if (SUCCEEDED(dev->GetPixelShader(&ps)) && ps) {
            bool seen = false;
            for (unsigned k = 0; k < g_psDumped; ++k)
                if (g_psDumpedPtr[k] == ps) { seen = true; break; }
            if (!seen) {
                UINT sz = 0;
                if (SUCCEEDED(ps->GetFunction(nullptr, &sz)) && sz) {
                    std::vector<unsigned char> code;
                    try {
                        code.resize(sz);
                        if (SUCCEEDED(ps->GetFunction(code.data(), &sz))) {
                            char dd[MAX_PATH] = {0};
                            GetModuleFileNameA(GetModuleHandleW(nullptr), dd, MAX_PATH);
                            char* dsl = strrchr(dd, 0x5C);
                            if (dsl) *(dsl + 1) = 0;
                            char pth[MAX_PATH];
                            sprintf_s(pth, "%ssr3-ps-%u.fxo", dd, g_psDumped);
                            FILE* f = nullptr;
                            if (fopen_s(&f, pth, "wb") == 0 && f) {
                                fwrite(code.data(), 1, sz, f);
                                fclose(f);
                                Log("dumped the LIVE cloth pixel shader to %s (%u bytes, "
                                    "albedo '%s')", pth, sz, g_curPS.albedoSampler);
                            }
                        }
                    } catch (...) {}
                }
                g_psDumpedPtr[g_psDumped++] = ps;
            }
            ps->Release();
        }
        g_internal = wi;
    }
    strncpy_s(slot.albedoName, g_curPS.albedoSampler, _TRUNCATE);
    strncpy_s(slot.firstName, g_curPS.firstSampler, _TRUNCATE);
    slot.hair = false;
    slot.dob = nullptr;
    slot.hairColor[0] = slot.hairColor[1] = slot.hairColor[2] = slot.hairColor[3] = 1.0f;
    if (g_curPS.hairColorReg[1] >= 0 &&
        g_curPS.hairColorReg[1] < static_cast<int>(kMaxPsConst)) {
        slot.hair = true;
        memcpy(slot.hairColor, g_psConst[g_curPS.hairColorReg[1]], sizeof(slot.hairColor));
        // WIDENED, and it reports itself. "Dob_Map" was matched with a 7-character prefix
        // compare, which is exact and therefore brittle - a sampler called Dob_Map_1 or dob_map
        // would miss. A case-insensitive substring cannot miss those, and if it still finds
        // nothing the log says what the stages WERE, so the next step is reading rather than
        // another guess.
        for (int st = 0; st < 8; ++st) {
            const char* nm = g_curPS.samplerName[st];
            if (g_curTexture[st] && nm[0] && StrStrIA(nm, "dob")) {
                slot.dob = g_curTexture[st];
                break;
            }
        }
        if (g_hairSlotReports < 4) {
            ++g_hairSlotReports;
            char stages[256] = {0};
            int p = 0;
            for (int st = 0; st < 8 && p < 200; ++st)
                if (g_curTexture[st] || g_curPS.samplerName[st][0])
                    p += _snprintf_s(stages + p, sizeof(stages) - p, _TRUNCATE, "s%d='%s'%s ",
                                     st, g_curPS.samplerName[st][0] ? g_curPS.samplerName[st]
                                                                    : "(unnamed)",
                                     g_curTexture[st] ? "" : "(NO TEXTURE)");
            Log("HAIR SLOT #%u: Dob_Map %s | stages: %s", g_hairSlotReports,
                slot.dob ? "FOUND" : "NOT FOUND - the strand generator cannot run", stages);
        }
    } else if (g_curPS.hairColorReg[0] >= 0 &&
               g_curPS.hairColorReg[0] < static_cast<int>(kMaxPsConst)) {
        slot.hair = true;
        memcpy(slot.hairColor, g_psConst[g_curPS.hairColorReg[0]], sizeof(slot.hairColor));
    }
    // WHICH PATTERNS THE API PATH SEES.
    //
    // The 2026-09-07 run put ten garments through the texture-space generator and every pattern
    // it saw was 32x32 - yet the CPU bake on this side handled a 64x64 one, the heart. So the
    // underwear's draw reaches HERE and does not reach ClothAlbedo, which runs only for CONVERTED
    // draws. Everything built for that garment sits downstream of a gate it never passes, and
    // this says which gate by naming the pattern each slot carries.
    if (g_clothSlotReports < 16 && g_curPS.patternStage >= 0 && g_curPS.patternStage < 8 &&
        g_curTexture[g_curPS.patternStage]) {
        UINT pw2 = 0, ph2 = 0;
        if (g_curTexture[g_curPS.patternStage]->GetType() == D3DRTYPE_TEXTURE) {
            D3DSURFACE_DESC sd{};
            if (SUCCEEDED(static_cast<IDirect3DTexture9*>(g_curTexture[g_curPS.patternStage])
                              ->GetLevelDesc(0, &sd))) { pw2 = sd.Width; ph2 = sd.Height; }
        }
        if (pw2 != 32 || ph2 != 32) {
            ++g_clothSlotReports;
            // AND WHAT THE CLASSIFIER DID WITH IT. ClothAlbedo runs only for a draw that
            // Classify returns Convert for, so the disposition IS the gate. g_dispReason is set
            // by Classify and this runs later in the same hook, so it still describes this draw.
            char uvmap[160] = {0};
            {
                int q = 0;
                for (int st = 0; st < 8 && q < 130; ++st)
                    if (g_curPS.samplerName[st][0] || g_curTexture[st])
                        q += _snprintf_s(uvmap + q, sizeof(uvmap) - q, _TRUNCATE, "s%d<-%s ", st,
                                         g_curPS.samplerUv[st] < 0
                                             ? "computed"
                                             : (g_curPS.samplerUv[st] == 0 ? "TEXCOORD0"
                                                : g_curPS.samplerUv[st] == 1 ? "TEXCOORD1"
                                                : g_curPS.samplerUv[st] == 6 ? "TEXCOORD6"
                                                : "TEXCOORDn"));
            }
            Log("      UV SET PER SAMPLER (from the shader's own texld): %s", uvmap);
            Log("CLOTH SLOT (API) #%u: pattern %ux%u at stage %d, albedo stage %d, "
                "blend %lu (src %lu dst %lu) | CLASSIFIED AS: %s",
                g_clothSlotReports, pw2, ph2, g_curPS.patternStage, g_curPS.albedoStage,
                ShadowGetRS(dev, D3DRS_ALPHABLENDENABLE), ShadowGetRS(dev, D3DRS_SRCBLEND),
                ShadowGetRS(dev, D3DRS_DESTBLEND),
                g_dispReason && g_dispReason[0] ? g_dispReason : "(no reason recorded)");
        }
    }
    slot.pattern = (g_curPS.patternStage >= 0 && g_curPS.patternStage < 8)
                       ? g_curTexture[g_curPS.patternStage] : nullptr;

    // FILL THE PART'S SECOND UV SET FROM *THIS* DRAW.
    //
    // The part was captured from the FIRST skinned draw on its vertex buffer, and that draw's
    // declaration has no TEXCOORD1 - so uv2 came back empty every time. The draw that DOES carry
    // it is this one, the cloth material draw, arriving later as a slot. Re-decoding here, with
    // the current declaration, is the only point where both the buffer and a declaration
    // containing TEXCOORD1 are in hand at once.
    if (P.uv2.size() != P.verts.size() * 2 && g_curLayout.texcoord1Offset >= 0) {
        const INT first = baseVertex + static_cast<INT>(minIndex);
        if (first >= 0) {
            const BaseMesh* m2 = GetBaseMesh(static_cast<UINT>(first), numVertices);
            if (m2 && m2->hasUv2 && m2->verts.size() == P.verts.size()) {
                try {
                    P.uv2.clear();
                    P.uv2.reserve(m2->verts.size() * 2);
                    for (size_t k = 0; k < m2->verts.size(); ++k) {
                        P.uv2.push_back(m2->verts[k].uv2[0]);
                        P.uv2.push_back(m2->verts[k].uv2[1]);
                    }
                    Log("cloth uv2: filled %u coordinates for a part from its material draw "
                        "(the capture draw had no TEXCOORD1)", (unsigned)m2->verts.size());
                } catch (...) { P.uv2.clear(); }
            }
        }
    }
    // Does this customised draw actually carry the second UV set the pattern needs? Reported once
    // per distinct answer, because the CPU bake depends entirely on it being there.
    if (g_curPS.diffuseColorReg[0] >= 0 && g_uv2Reports < 4) {
        ++g_uv2Reports;
        Log("cloth uv2: texcoord1 type=%d offset=%d stream=%d (texcoord0 type=%d offset=%d) - "
            "the pattern map is sampled with TEXCOORD1, so a CPU bake needs this present",
            g_curLayout.texcoord1Type, g_curLayout.texcoord1Offset, g_curLayout.texcoord1Stream,
            g_curLayout.texcoordType, g_curLayout.texcoordOffset);
    }
    // BAKE. Only for slots that carry the customisation recipe, because those are the ones whose
    // colour exists nowhere but in the pixel shader. Run here, immediately after the game's draw,
    // because every piece of state the bake needs - textures, ps constants, buffers - is still
    // exactly as the game left it.
    if (g_settings.bakeShaderAlbedo && g_curPS.diffuseColorReg[0] >= 0 &&
        g_curPS.diffuseColorReg[1] >= 0 && g_curPS.diffuseColorReg[2] >= 0) {
        const char* baked = BakeSlotAlbedo(dev, type, baseVertex, minIndex, numVertices,
                                           startIndex, primitiveCount);
        if (baked) strncpy_s(slot.bakedPath, baked, _TRUNCATE);
    }
    for (int c = 0; c < 4; ++c) slot.dcA[c] = slot.dcB[c] = slot.dcC[c] = 1.0f;
    // NOT gated on remixApiClothAlbedo. That switch exists to stop ClothAlbedo() REPLACING a
    // working texture - it was never meant to stop the slot being RECOGNISED as customised. It
    // did, so slot.cloth and the three colours were never set and the CPU baker could not fire.
    // A guard placed on the wrong side of a condition.
    if (g_curPS.diffuseColorReg[0] >= 0 && g_curPS.diffuseColorReg[1] >= 0 &&
        g_curPS.diffuseColorReg[2] >= 0) {
        slot.cloth = true;
        const int* r = g_curPS.diffuseColorReg;
        if (r[0] < static_cast<int>(kMaxPsConst)) memcpy(slot.dcA, g_psConst[r[0]], 16);
        if (r[1] < static_cast<int>(kMaxPsConst)) memcpy(slot.dcB, g_psConst[r[1]], 16);
        if (r[2] < static_cast<int>(kMaxPsConst)) memcpy(slot.dcC, g_psConst[r[2]], 16);
        if (g_curPS.diffuseColorMulReg >= 0 &&
            g_curPS.diffuseColorMulReg < static_cast<int>(kMaxPsConst))
            memcpy(slot.dcMul, g_psConst[g_curPS.diffuseColorMulReg], 16);
        slot.albedo = g_settings.remixApiClothAlbedo ? ClothAlbedo(dev) : nullptr;
        slot.clothGenerated = (slot.albedo != nullptr);
        if (slot.clothGenerated) ++g_apiClothGenerated;
        else ++g_apiClothApprox;
    }
    if (!slot.albedo && g_curPS.albedoStage >= 0 && g_curPS.albedoStage < 8)
        slot.albedo = g_curTexture[g_curPS.albedoStage];
    if (!slot.albedo) slot.albedo = g_curTexture[0];
    slot.alphaTestEnable  = ShadowGetRS(dev, D3DRS_ALPHATESTENABLE);
    slot.alphaFunc        = ShadowGetRS(dev, D3DRS_ALPHAFUNC);
    slot.alphaRef         = ShadowGetRS(dev, D3DRS_ALPHAREF);
    slot.alphaBlendEnable = ShadowGetRS(dev, D3DRS_ALPHABLENDENABLE);
    slot.srcBlend         = ShadowGetRS(dev, D3DRS_SRCBLEND);
    slot.destBlend        = ShadowGetRS(dev, D3DRS_DESTBLEND);
    P.slots.push_back(slot);
}


// ---------------------------------------------------------------------------------------------
// Write ANY game texture to a DDS, so an API material can name it.
//
// The character atlas had its own path because the snoop already held its pixels. Every OTHER part
// of the character - head, hair, clothing, accessories - carries its own texture, and pointing all
// of them at the atlas is why they came out wrong. From the frame dump those are 4923C980,
// 16381958, 4923F2A0 and about sixteen more; only the body uses 3BD233F0.
//
// Compressed formats are copied through UNCHANGED rather than decoded: a DDS can carry DXT blocks
// directly, so the fourCC is simply set and the bytes written. That avoids writing a block decoder
// and keeps the texture bit-exact. Uncompressed BGRA is written as before, with alpha forced to
// 255 for X8R8G8B8 whose alpha byte is undefined.
//
// Cached by texture pointer: each is dumped once and the path reused.
struct DumpedTex { IDirect3DBaseTexture9* tex; char path[MAX_PATH]; };
std::vector<DumpedTex> g_dumpedTex;
unsigned g_texDumpOk = 0, g_texDumpFail = 0;

bool DumpTextureDds(IDirect3DBaseTexture9* base, const char** outPath) {
    if (!base) return false;
    for (size_t i = 0; i < g_dumpedTex.size(); ++i)
        if (g_dumpedTex[i].tex == base) {
            if (!g_dumpedTex[i].path[0]) return false;      // known-bad, do not retry
            *outPath = g_dumpedTex[i].path;
            return true;
        }

    DumpedTex entry{};
    entry.tex = base;
    g_dumpedTex.push_back(entry);
    DumpedTex& slot = g_dumpedTex.back();

    IDirect3DTexture9* tex = nullptr;
    if (base->GetType() != D3DRTYPE_TEXTURE ||
        FAILED(base->QueryInterface(__uuidof(IDirect3DTexture9),
                                    reinterpret_cast<void**>(&tex))) || !tex) {
        ++g_texDumpFail;
        return false;
    }

    D3DSURFACE_DESC d{};
    if (FAILED(tex->GetLevelDesc(0, &d)) || !d.Width || !d.Height) {
        tex->Release(); ++g_texDumpFail; return false;
    }
    const DWORD levels = tex->GetLevelCount();
    unsigned blockBytes = 0, fourCC = 0;
    switch (d.Format) {
        case D3DFMT_DXT1: blockBytes = 8;  fourCC = MAKEFOURCC('D','X','T','1'); break;
        case D3DFMT_DXT3: blockBytes = 16; fourCC = MAKEFOURCC('D','X','T','3'); break;
        case D3DFMT_DXT5: blockBytes = 16; fourCC = MAKEFOURCC('D','X','T','5'); break;
        case D3DFMT_A8R8G8B8:
        case D3DFMT_X8R8G8B8: break;
        default:
            Log("remix api: texture %p has format %u, which this dumper does not write - the part "
                "using it keeps the atlas", static_cast<void*>(base),
                static_cast<unsigned>(d.Format));
            tex->Release(); ++g_texDumpFail; return false;
    }

    char dir[MAX_PATH] = {0};
    if (!GetModuleFileNameA(GetModuleHandleW(nullptr), dir, MAX_PATH)) {
        tex->Release(); ++g_texDumpFail; return false;
    }
    char* sl = strrchr(dir, '\\');
    if (sl) *(sl + 1) = 0;
    char path[MAX_PATH];
    sprintf_s(path, "%ssr3-remix-tex-%08X.dds", dir, static_cast<unsigned>(
        reinterpret_cast<uintptr_t>(base)));

    FILE* f = nullptr;
    if (fopen_s(&f, path, "wb") != 0 || !f) { tex->Release(); ++g_texDumpFail; return false; }

    unsigned hdr[32] = {0};
    hdr[0] = 124;
    hdr[1] = 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | (fourCC ? 0x80000u : 0x8u);
    hdr[2] = d.Height;
    hdr[3] = d.Width;
    hdr[4] = fourCC ? ((d.Width + 3) / 4) * ((d.Height + 3) / 4) * blockBytes : d.Width * 4;
    hdr[6] = levels;
    hdr[18] = 32;
    if (fourCC) {
        hdr[19] = 0x4;              // DDPF_FOURCC
        hdr[20] = fourCC;
    } else {
        hdr[19] = 0x1 | 0x40;       // ALPHAPIXELS | RGB
        hdr[21] = 32;
        hdr[22] = 0x00FF0000;
        hdr[23] = 0x0000FF00;
        hdr[24] = 0x000000FF;
        hdr[25] = 0xFF000000;
    }
    hdr[26] = 0x1000 | (levels > 1 ? (0x400000 | 0x8) : 0);
    const unsigned magic = 0x20534444;
    fwrite(&magic, 4, 1, f);
    fwrite(hdr, 4, 31, f);

    bool ok = true;
    // Is level 0 actually READABLE? A DEFAULT-pool or render-target texture can return SUCCESS
    // from LockRect and hand back zeroes, and we then wrote a fully black DDS and used it as an
    // albedo. That is worse than not dumping at all, because a black texture looks like a
    // deliberate result. Measured 2026-09-03: a 512x512 dump was black from end to end.
    double lvl0 = 0.0;
    unsigned lvl0n = 0;
    const bool forceAlpha = (d.Format == D3DFMT_X8R8G8B8);
    for (DWORD lv = 0; lv < levels && ok; ++lv) {
        D3DSURFACE_DESC ld{};
        D3DLOCKED_RECT lr{};
        if (FAILED(tex->GetLevelDesc(lv, &ld)) ||
            FAILED(tex->LockRect(lv, &lr, nullptr, D3DLOCK_READONLY)) || !lr.pBits) {
            ok = false;
            break;
        }
        const unsigned rows = fourCC ? ((ld.Height + 3) / 4) : ld.Height;
        const unsigned rowBytes = fourCC ? (((ld.Width + 3) / 4) * blockBytes) : (ld.Width * 4);
        std::vector<unsigned char> line;
        try { line.resize(rowBytes); } catch (...) { ok = false; }
        for (unsigned y = 0; y < rows && ok; ++y) {
            const unsigned char* src =
                static_cast<const unsigned char*>(lr.pBits) + static_cast<size_t>(y) * lr.Pitch;
            memcpy(line.data(), src, rowBytes);
            if (forceAlpha)
                for (unsigned x = 3; x < rowBytes; x += 4) line[x] = 255;
            if (lv == 0)
                for (unsigned x = 0; x < rowBytes; x += 16) { lvl0 += line[x]; ++lvl0n; }
            if (fwrite(line.data(), 1, rowBytes, f) != rowBytes) ok = false;
        }
        tex->UnlockRect(lv);
    }
    fclose(f);
    tex->Release();
    if (ok && lvl0n && (lvl0 / lvl0n) < 0.5) {
        remove(path);
        ++g_texDumpFail;
        Log("remix api: texture %p read back EMPTY (%ux%u) - it cannot be locked for real, so the "
            "dump is discarded rather than shipping a black albedo", static_cast<void*>(base),
            d.Width, d.Height);
        return false;
    }
    if (!ok) {
        ++g_texDumpFail;
        Log("remix api: could not read texture %p (a DEFAULT-pool texture cannot be locked) - the "
            "part using it keeps the atlas", static_cast<void*>(base));
        return false;
    }
    strncpy_s(slot.path, path, _TRUNCATE);
    *outPath = slot.path;
    ++g_texDumpOk;
    Log("remix api: dumped texture %p as %s (%ux%u, %u levels, %s)", static_cast<void*>(base),
        path, d.Width, d.Height, levels, fourCC ? "compressed, copied through" : "BGRA8");
    return true;
}



// ---------------------------------------------------------------------------------------------
// CPU BAKE of SR3's customisation recipe.
//
// The GPU route is closed: driving a render target through the Remix device crashed its server
// twice at the same address, with the bake draw hidden inside an occlusion query and everything
// restored. So the recipe is evaluated here instead.
//
// The recipe, read out of ir_sr3npcclothfull_c shader[8] and recorded above ShaderInfo:
//
//     p    = Pattern_Map sampled with TEXCOORD1        (NOT TEXCOORD0 - that is the diffuse set)
//     sum  = p.r + p.g + p.b
//     dev  = |p.r-sum/3| + |p.g-sum/3| + |p.b-sum/3|
//     test = sum - (dev*165.016495 + 256)/255
//     test <  0 -> albedo = p.r^2.2*Diffuse_Color_a + p.g^2.2*_b + p.b^2.2*_c
//     test >= 0 -> albedo = saturate((p - 0.372549) * 1.59375)^2.2
//     result    = albedo * Tint_color
//
// The second branch is what lets trim and skin escape being tinted: a texel that is roughly grey
// takes its own colour instead of the three customisation colours.
//
// Output is in TEXCOORD0 space, because that is the space the mesh's albedo is sampled in.

// --- DXT decode. Pattern maps are compressed, so the blocks have to be expanded to read them. ---
void DecodeDxtBlockColour(const unsigned char* b, unsigned char out[16][4], bool dxt1) {
    const unsigned short c0 = static_cast<unsigned short>(b[0] | (b[1] << 8));
    const unsigned short c1 = static_cast<unsigned short>(b[2] | (b[3] << 8));
    unsigned char col[4][3];
    auto expand = [](unsigned short c, unsigned char* o) {
        o[0] = static_cast<unsigned char>(((c >> 11) & 31) * 255 / 31);   // r
        o[1] = static_cast<unsigned char>(((c >> 5) & 63) * 255 / 63);    // g
        o[2] = static_cast<unsigned char>((c & 31) * 255 / 31);           // b
    };
    expand(c0, col[0]);
    expand(c1, col[1]);
    const bool fourColour = !dxt1 || c0 > c1;
    for (int k = 0; k < 3; ++k) {
        if (fourColour) {
            col[2][k] = static_cast<unsigned char>((2 * col[0][k] + col[1][k]) / 3);
            col[3][k] = static_cast<unsigned char>((col[0][k] + 2 * col[1][k]) / 3);
        } else {
            col[2][k] = static_cast<unsigned char>((col[0][k] + col[1][k]) / 2);
            col[3][k] = 0;
        }
    }
    const unsigned bits = static_cast<unsigned>(b[4]) | (static_cast<unsigned>(b[5]) << 8) |
                          (static_cast<unsigned>(b[6]) << 16) | (static_cast<unsigned>(b[7]) << 24);
    for (int i = 0; i < 16; ++i) {
        const unsigned sel = (bits >> (i * 2)) & 3u;
        out[i][2] = col[sel][0];   // stored BGRA
        out[i][1] = col[sel][1];
        out[i][0] = col[sel][2];
        out[i][3] = 255;
    }
}

// Whole texture level 0 to BGRA. Handles DXT1/3/5 and the uncompressed 32-bit formats.
bool TextureToBgra(IDirect3DBaseTexture9* base, std::vector<unsigned char>& out,
                   UINT& w, UINT& h) {
    if (!base || base->GetType() != D3DRTYPE_TEXTURE) return false;
    IDirect3DTexture9* tex = nullptr;
    if (FAILED(base->QueryInterface(__uuidof(IDirect3DTexture9),
                                    reinterpret_cast<void**>(&tex))) || !tex)
        return false;
    D3DSURFACE_DESC d{};
    D3DLOCKED_RECT lr{};
    bool ok = SUCCEEDED(tex->GetLevelDesc(0, &d)) && d.Width && d.Height &&
              SUCCEEDED(tex->LockRect(0, &lr, nullptr, D3DLOCK_READONLY)) && lr.pBits;
    if (!ok) { tex->Release(); return false; }
    w = d.Width;
    h = d.Height;
    try {
        out.assign(static_cast<size_t>(w) * h * 4, 0);
    } catch (...) { tex->UnlockRect(0); tex->Release(); return false; }

    const unsigned char* src = static_cast<const unsigned char*>(lr.pBits);
    if (d.Format == D3DFMT_A8R8G8B8 || d.Format == D3DFMT_X8R8G8B8) {
        for (UINT y = 0; y < h; ++y)
            memcpy(&out[static_cast<size_t>(y) * w * 4], src + static_cast<size_t>(y) * lr.Pitch,
                   static_cast<size_t>(w) * 4);
    } else if (d.Format == D3DFMT_DXT1 || d.Format == D3DFMT_DXT3 || d.Format == D3DFMT_DXT5) {
        const bool dxt1 = (d.Format == D3DFMT_DXT1);
        const unsigned blockBytes = dxt1 ? 8u : 16u;
        const unsigned bw = (w + 3) / 4, bh = (h + 3) / 4;
        for (unsigned by = 0; by < bh; ++by)
            for (unsigned bx = 0; bx < bw; ++bx) {
                const unsigned char* blk = src + static_cast<size_t>(by) * lr.Pitch +
                                           static_cast<size_t>(bx) * blockBytes;
                unsigned char texels[16][4];
                DecodeDxtBlockColour(dxt1 ? blk : blk + 8, texels, dxt1);
                // ALPHA. Skipping this wrote 255 everywhere, which is what made a decal's
                // surround opaque. DXT3 stores 4 bits per texel; DXT5 stores two endpoints and
                // a 3-bit index per texel.
                if (d.Format == D3DFMT_DXT3) {
                    for (int i = 0; i < 16; ++i) {
                        const unsigned char n = blk[i >> 1];
                        const unsigned a4 = (i & 1) ? (n >> 4) : (n & 0xF);
                        texels[i][3] = static_cast<unsigned char>(a4 * 17);
                    }
                } else if (d.Format == D3DFMT_DXT5) {
                    const unsigned char a0 = blk[0], a1 = blk[1];
                    unsigned char av[8];
                    av[0] = a0; av[1] = a1;
                    if (a0 > a1)
                        for (int k = 1; k < 7; ++k)
                            av[k + 1] = static_cast<unsigned char>(((7 - k) * a0 + k * a1) / 7);
                    else {
                        for (int k = 1; k < 5; ++k)
                            av[k + 1] = static_cast<unsigned char>(((5 - k) * a0 + k * a1) / 5);
                        av[6] = 0; av[7] = 255;
                    }
                    unsigned long long bits = 0;
                    for (int k = 0; k < 6; ++k)
                        bits |= static_cast<unsigned long long>(blk[2 + k]) << (8 * k);
                    for (int i = 0; i < 16; ++i)
                        texels[i][3] = av[(bits >> (i * 3)) & 7u];
                }
                for (int i = 0; i < 16; ++i) {
                    const unsigned px = bx * 4 + (i & 3), py = by * 4 + (i >> 2);
                    if (px >= w || py >= h) continue;
                    memcpy(&out[(static_cast<size_t>(py) * w + px) * 4], texels[i], 4);
                }
            }
    } else {
        ok = false;
    }
    tex->UnlockRect(0);
    tex->Release();
    return ok;
}

// The device, kept for work that happens at Present rather than inside a draw. The character
// builds drain from Present and had no device to hand, which is why the CPU baker could only ever
// write a FILE - and a file is no use to the game's own converted draw, which needs a bound
// D3D texture.
IDirect3DDevice9* g_presentDevice = nullptr;

constexpr UINT kCpuBakeSize = 512;
unsigned g_cpuBakeCount = 0, g_cpuBakeFail = 0;
// Triangles that reached outside the unit square, and triangles so far outside that the box had
// to be bounded. The first is normal for a tiling garment; the second means the UVs are wrong
// after all, and the two must not be confused for each other.
unsigned g_cpuBakeWrapped = 0, g_cpuBakeClipped = 0;
unsigned g_clothBakedRegistered = 0;
unsigned g_hairGenerated = 0;

inline float SrgbPow22(float v) { return powf(v < 0.0f ? 0.0f : v, 2.2f); }

// Rasterise the slot's triangles into TEXCOORD0 space, evaluating the recipe per texel.
// THE RECIPE, read from the PLAYER's own cloth shader (ir_at_sr3pccloth_bs shader[8]):
//
//     texld r4, r4, s0            ; Diffuse_Map
//     mul   r1.yzw, r4.xxyz, c11  ; diffuse * Diffuse_Color
//     texld r5, v1, s2            ; Pattern_Map, at TEXCOORD1
//     r7/r8/r9 = pow(Diffuse_Color_a/b/c, c12.w)    with c12.w = 2.2
//     lrp r6, r5.z, r9, 1.0       ; start at WHITE, blend toward C by pattern.b
//     lrp r9, r5.y, r8, r6        ;                 toward B by pattern.g
//     lrp r6, r5.x, r7, r9        ;                 toward A by pattern.r
//     mul r5.xyz, r1.yzw, r6      ; DIFFUSE * that
//
// Two things the earlier implementation got wrong, both because it came from a DIFFERENT shader
// (ir_sr3npcclothfull_c, which has a sum/deviation branch this one does not):
//   it never multiplied by the DIFFUSE MAP, and
//   it treated a black mask region as "all three colours at zero" - i.e. black - when the lerp
//     chain leaves such a region at WHITE, meaning "keep the diffuse unchanged".
// That is exactly the black bands and the cyan block seen on the underwear.
const char* CpuBakeCloth(const std::vector<RemixHardcodedVertex>& verts,
                         const std::vector<float>& uv2,
                         const std::vector<unsigned>& indices,
                         IDirect3DBaseTexture9* pattern,
                         IDirect3DBaseTexture9* diffuse,
                         const float* dcA, const float* dcB, const float* dcC,
                         const float* dcMul) {
    if (verts.empty() || indices.size() < 3 || uv2.size() != verts.size() * 2) return nullptr;
    std::vector<unsigned char> pat;
    UINT pw = 0, ph = 0;
    if (!TextureToBgra(pattern, pat, pw, ph)) { ++g_cpuBakeFail; return nullptr; }
    // The diffuse map is sampled in the SAME space we are baking into, so its texel for an output
    // pixel is simply that pixel's own uv - no interpolation needed.
    std::vector<unsigned char> dif;
    UINT dw = 0, dh = 0;
    const bool haveDiffuse = TextureToBgra(diffuse, dif, dw, dh);

    // Dump the INPUTS of every bake, not only the first one's pattern.
    //
    // Three hypotheses about the underwear have now died - a per-material tiling factor, a
    // different UV divide in the shader, and (only partly) the baker clipping tiled UVs away.
    // Each was reasoned from summary numbers: a mean, a coverage percentage, a uv range. The two
    // times this project has actually converged on a rendering fault quickly, it was by LOOKING
    // at the thing - the slot table that showed triangle strips, and the contact sheet that
    // showed cloth-9 was empty rather than scrambled.
    //
    // So every bake now writes what it was GIVEN alongside what it produced. A pattern that is
    // uniform means the customisation colour is uniform, and a uniform colour times a diffuse map
    // does not need a UV-space rasterisation at all - it can be done in the diffuse's own texture
    // space, where no UV assumption can break it. That is a different fix from anything tried so
    // far, and whether it applies is a question about these two images.
    {
        char pdir[MAX_PATH] = {0};
        GetModuleFileNameA(GetModuleHandleW(nullptr), pdir, MAX_PATH);
        char* psl = strrchr(pdir, 0x5C);
        if (psl) *(psl + 1) = 0;
        char ppath[MAX_PATH];
        sprintf_s(ppath, "%ssr3-remix-in-%u-pattern.dds", pdir, g_cpuBakeCount + 1);
        if (WriteBgraDds(ppath, pw, ph, pat.data()))
            Log("cloth bake #%u INPUT pattern -> %s (%ux%u)", g_cpuBakeCount + 1, ppath, pw, ph);
        if (haveDiffuse) {
            sprintf_s(ppath, "%ssr3-remix-in-%u-diffuse.dds", pdir, g_cpuBakeCount + 1);
            if (WriteBgraDds(ppath, dw, dh, dif.data()))
                Log("cloth bake #%u INPUT diffuse -> %s (%ux%u)", g_cpuBakeCount + 1, ppath,
                    dw, dh);
        }
        // Is the pattern UNIFORM? If every texel is the same, the lerp chain produces one colour
        // for the whole garment and the rasterisation is doing no work that a texture-space
        // multiply could not do more safely. Measured, not eyeballed, because it decides which
        // fix is the right one.
        bool uniform = true;
        if (pat.size() >= 4)
            for (size_t q = 4; q < pat.size(); q += 4)
                if (pat[q] != pat[0] || pat[q + 1] != pat[1] || pat[q + 2] != pat[2]) {
                    uniform = false; break;
                }
        Log("      pattern is %s (first texel BGRA %u %u %u %u)",
            uniform ? "UNIFORM - one colour for the whole garment"
                    : "spatially varying - the rasterisation is doing real work",
            pat.size() >= 4 ? pat[0] : 0, pat.size() >= 4 ? pat[1] : 0,
            pat.size() >= 4 ? pat[2] : 0, pat.size() >= 4 ? pat[3] : 0);
    }

    std::vector<unsigned char> img;
    try { img.assign(static_cast<size_t>(kCpuBakeSize) * kCpuBakeSize * 4, 0); }
    catch (...) { ++g_cpuBakeFail; return nullptr; }

    auto samplePattern = [&](float u, float v, float* rgb, float* a) {
        int x = static_cast<int>(u * static_cast<float>(pw)) % static_cast<int>(pw);
        int y = static_cast<int>(v * static_cast<float>(ph)) % static_cast<int>(ph);
        if (x < 0) x += pw;
        if (y < 0) y += ph;
        const unsigned char* p = &pat[(static_cast<size_t>(y) * pw + x) * 4];
        rgb[0] = p[2] / 255.0f;   // stored BGRA
        rgb[1] = p[1] / 255.0f;
        rgb[2] = p[0] / 255.0f;
        if (a) *a = p[3] / 255.0f;
    };

    for (size_t t = 0; t + 2 < indices.size(); t += 3) {
        const unsigned ia = indices[t], ib = indices[t + 1], ic = indices[t + 2];
        if (ia >= verts.size() || ib >= verts.size() || ic >= verts.size()) continue;
        // TEXCOORD0 is the destination space; both sets are raw shorts, so 1/1024 applies to both.
        // verts already carry TEXCOORD0 scaled to 0..1; uv2 is still raw shorts.
        const float ax = verts[ia].texcoord[0], ay = verts[ia].texcoord[1];
        const float bx = verts[ib].texcoord[0], by = verts[ib].texcoord[1];
        const float cx = verts[ic].texcoord[0], cy = verts[ic].texcoord[1];
        const float area = (bx - ax) * (cy - ay) - (cx - ax) * (by - ay);
        if (std::fabs(area) < 1e-12f) continue;
        const float inv = 1.0f / area;

        float lo0 = min(ax, min(bx, cx)), hi0 = max(ax, max(bx, cx));
        float lo1 = min(ay, min(by, cy)), hi1 = max(ay, max(by, cy));
        int x0 = static_cast<int>(lo0 * kCpuBakeSize) - 1, x1 = static_cast<int>(hi0 * kCpuBakeSize) + 1;
        int y0 = static_cast<int>(lo1 * kCpuBakeSize) - 1, y1 = static_cast<int>(hi1 * kCpuBakeSize) + 1;
        // THE DESTINATION BOX IS NOT CLAMPED TO THE TEXTURE. It used to be, and that is why the
        // underwear baked at 3% coverage while every other garment reached 29-98%.
        //
        // Its UVs run u[-1.072 2.787] v[-0.414 2.734] - roughly four tiles - and the mesh is not
        // broken for that. A tiling garment is ordinary, the game samples it with ADDRESS_WRAP,
        // and the pattern lookup in this very function has always wrapped its source with `% pw`.
        // Only the WRITE clamped, so every triangle outside the unit square was clipped away and
        // simply never contributed. 96% of this garment was discarded by four lines.
        //
        // Two hypotheses died before this one - a per-material tiling factor (no tiling constant
        // matched any part) and a different UV divide in the shader (reading it out of the
        // bytecode picked 1/8192 for the BODY and broke it, while leaving the underwear at
        // 1/1024). Both assumed the UVs were wrong. They are not: the baker's assumption that
        // UVs live in 0..1 is what was wrong.
        //
        // The barycentric test below still runs in UNWRAPPED space, so the triangle keeps its
        // true shape and a triangle straddling a tile boundary is rasterised correctly; only the
        // address it writes to wraps. That is what makes this different from taking frac() of
        // each vertex, which would tear those triangles apart.
        //
        // Bounded, because a genuinely broken UV could otherwise ask for millions of pixels: at
        // most four tiles in each direction, which is more than any real garment tiles and still
        // finite if the data is nonsense.
        const int kMaxTiles = 4;
        if (x1 - x0 > static_cast<int>(kCpuBakeSize) * kMaxTiles) {
            x1 = x0 + static_cast<int>(kCpuBakeSize) * kMaxTiles;
            ++g_cpuBakeClipped;
        }
        if (y1 - y0 > static_cast<int>(kCpuBakeSize) * kMaxTiles) {
            y1 = y0 + static_cast<int>(kCpuBakeSize) * kMaxTiles;
            ++g_cpuBakeClipped;
        }
        if (x0 < 0 || y0 < 0 || x1 > static_cast<int>(kCpuBakeSize) ||
            y1 > static_cast<int>(kCpuBakeSize))
            ++g_cpuBakeWrapped;

        for (int py = y0; py < y1; ++py)
            for (int px = x0; px < x1; ++px) {
                const float fx = (px + 0.5f) / kCpuBakeSize, fy = (py + 0.5f) / kCpuBakeSize;
                const float w0 = ((bx - fx) * (cy - fy) - (cx - fx) * (by - fy)) * inv;
                const float w1 = ((cx - fx) * (ay - fy) - (ax - fx) * (cy - fy)) * inv;
                const float w2 = 1.0f - w0 - w1;
                if (w0 < -0.002f || w1 < -0.002f || w2 < -0.002f) continue;

                // uv2 arrives already scaled to 0..1 - see the call site, which also supplies
                // the fallback for parts whose shader samples the pattern with TEXCOORD0.
                const float pu = w0 * uv2[ia * 2] + w1 * uv2[ib * 2] + w2 * uv2[ic * 2];
                const float pv = w0 * uv2[ia * 2 + 1] + w1 * uv2[ib * 2 + 1] +
                                 w2 * uv2[ic * 2 + 1];
                float p[3], pa = 1.0f;
                samplePattern(pu, pv, p, &pa);

                // Start WHITE and blend toward each customisation colour by its own channel.
                // A black mask region therefore stays white and leaves the diffuse untouched -
                // which is what the black bands should have been all along.
                float alb[3];
                for (int k = 0; k < 3; ++k) {
                    // The colours are used AS AUTHORED, not raised to 2.2.
                    //
                    // The shader does pow(colour, 2.2) to move an sRGB constant into linear light,
                    // and its result is later written out through the game's own gamma. We do the
                    // same conversion on the way OUT (the sRGB encode below), so applying it here
                    // as well counts it twice: colour a(0.40 0.00 0.21) came back as roughly
                    // (96 0 48) - the dark maroon in bake 3 - where the game shows bright pink.
                    //
                    // The diffuse texel is likewise already sRGB and is used as-is.
                    const float ca = dcA[k], cb = dcB[k], cc = dcC[k];
                    float v = 1.0f + (cc - 1.0f) * p[2];   // lrp toward C by pattern.b
                    v = v + (cb - v) * p[1];               // then toward B by pattern.g
                    v = v + (ca - v) * p[0];               // then toward A by pattern.r
                    alb[k] = v;
                }
                // ... times the diffuse map and Diffuse_Color.
                if (haveDiffuse && g_settings.clothUseDiffuse) {
                    const int dx = static_cast<int>(fx * dw) % static_cast<int>(dw);
                    const int dy = static_cast<int>(fy * dh) % static_cast<int>(dh);
                    const unsigned char* dp =
                        &dif[(static_cast<size_t>(dy < 0 ? dy + dh : dy) * dw +
                              (dx < 0 ? dx + dw : dx)) * 4];
                    alb[0] *= (dp[2] / 255.0f) * dcMul[0];
                    alb[1] *= (dp[1] / 255.0f) * dcMul[1];
                    alb[2] *= (dp[0] / 255.0f) * dcMul[2];
                    pa = dp[3] / 255.0f;
                }
                // TINT ONLY IF IT IS A COLOUR.
                //
                // The first bake produced a correct garment unwrap in flat magenta. Working it
                // back: the pattern is solid BLUE, so the recipe takes its first branch and the
                // albedo is Diffuse_Color_c = (0.40, 0.00, 0.21), a dark maroon - and then
                // Tint_color = 5.0 multiplies it to (2.0, 0, 1.05), which clamps to pure magenta.
                //
                // A value of 5 is not a colour. The same 5.0 turned up on every blended material
                // earlier. Components above 1 are therefore treated as "no tint" rather than
                // scaled, so a mis-read constant cannot destroy a correct albedo.
                // Wrap the ADDRESS, not the geometry. C++ leaves the sign of % on a negative
                // operand implementation-defined for our purposes here, so it is folded up
                // explicitly rather than trusted.
                int wx = px % static_cast<int>(kCpuBakeSize);
                int wy = py % static_cast<int>(kCpuBakeSize);
                if (wx < 0) wx += static_cast<int>(kCpuBakeSize);
                if (wy < 0) wy += static_cast<int>(kCpuBakeSize);
                unsigned char* dst = &img[(static_cast<size_t>(wy) * kCpuBakeSize + wx) * 4];
                for (int k = 0; k < 3; ++k) {
                    // A TUNING KNOB, and labelled as one.
                    //
                    // The recipe is verified correct: the exe divides the swatch by 255, the
                    // shader lerps from white toward each colour by a pattern channel, and
                    // r1.x is c6.x = 1.0 so that lerp is exactly what it looks like. Bake #2
                    // computes (84 0 0) from colour C = 0.40 and a diffuse of 0.82, which is
                    // what the shader would produce.
                    //
                    // It still looks too dark in the path traced image, which means Remix lights
                    // this mesh differently from how the game lit it - not that the albedo is
                    // wrong. That is a lighting question, not a recipe one, and it is NOT solved
                    // here. This scales the baked albedo so the result can be made usable while
                    // that stays open.
                    float c = alb[k] * g_settings.clothBrightness;
                    c = c < 0.0f ? 0.0f : (c > 1.0f ? 1.0f : c);
                    // BACK TO sRGB BEFORE STORING.
                    //
                    // The recipe works in LINEAR light - both its branches raise the pattern to
                    // the 2.2 - but this DDS is an 8-bit texture that Remix samples as sRGB.
                    // Writing the linear value straight in applies the 2.2 a SECOND time, which
                    // is why the clothes came out too dark with visible noise in the shadows: an
                    // 8-bit encoding of a linear value has almost no precision left down there.
                    // No gamma on the way out either, now that none was applied on the way in -
                    // the values are sRGB from end to end.
                    dst[2 - k] = static_cast<unsigned char>(c * 255.0f + 0.5f);   // BGRA
                }
                // The pattern's own alpha, so a decal keeps its cut-out. Writing 255 here is
                // what made the logo's surround a solid rectangle.
                dst[3] = static_cast<unsigned char>(pa * 255.0f + 0.5f);
            }
    }

    double acc = 0.0;
    unsigned n = 0, covered = 0;
    for (UINT i = 0; i < kCpuBakeSize * kCpuBakeSize; i += 37) {
        acc += img[i * 4] + img[i * 4 + 1] + img[i * 4 + 2];
        if (img[i * 4 + 3]) ++covered;
        ++n;
    }
    char dir[MAX_PATH] = {0};
    GetModuleFileNameA(GetModuleHandleW(nullptr), dir, MAX_PATH);
    char* sl = strrchr(dir, '\\');
    if (sl) *(sl + 1) = 0;
    static char path[MAX_PATH];
    ++g_cpuBakeCount;
    sprintf_s(path, "%ssr3-remix-cloth-%u.dds", dir, g_cpuBakeCount);
    if (!WriteBgraDds(path, kCpuBakeSize, kCpuBakeSize, img.data())) {
        ++g_cpuBakeFail;
        return nullptr;
    }
    // ------------------------------------------------------------------ ONE BAKER, BOTH COPIES
    //
    // The underwear is the one garment neither texture-space generator can do. Its pattern is a
    // heart-shaped LOGO that varies across the surface, and it is sampled through a SECOND uv set
    // while the diffuse is sampled through the first - so a per-texel answer genuinely needs the
    // mesh to relate the two, which is exactly what this rasteriser has. ClothAlbedoUniform
    // declines it correctly; there is nothing to fix there.
    //
    // What was missing is that this result only ever became a FILE. The API copy reads that file;
    // the game's own converted draw cannot, so it fell back to the raw Diffuse_Map - a mostly
    // BLACK 256x128 map with two pale panels and a waistband on it. Black underwear with a pale
    // patch is not a colour bug, it is that map shown untinted.
    //
    // So the bake is registered into the very cache ClothAlbedo consults, under the key
    // ClothAlbedoUniform builds. The first draws of a new outfit still miss and bind the raw map
    // for a frame or two, then the build lands and every later draw gets the baked albedo.
    //
    // ONLY when this slot has a SEPARATE diffuse map. Without that guard the key collides.
    //
    // ClothAlbedoUniform builds its key as ClothKey(pattern, col) ^ diffuse*prime. When a slot
    // has no separate diffuse - the corset and the shoes, where the Pattern_Map IS the albedo -
    // that XOR is against nullptr, so the key collapses to exactly ClothKey(pattern, col): the
    // key the ORIGINAL pattern-space path uses. Registering here then OVERWROTE those garments
    // correct pattern-space texture with a UV-space bake, which is why the shoes went black (bake
    // #3 covers 11% of its texture) and why the corset colour transition moved - a resample of a
    // 512x512 rasterisation has softer edges in different places than the 32x32 pattern it
    // replaced.
    //
    // A slot with no separate diffuse never needed this bridge in the first place: the
    // pattern-space generator already handles it, and handles it better.
    if (g_presentDevice && !img.empty() && diffuse) {
        float col[3][4]{};
        for (int k = 0; k < 4; ++k) {
            col[0][k] = dcA[k];
            col[1][k] = dcB[k];
            col[2][k] = dcC[k];
        }
        unsigned long long bkey = ClothKey(pattern, col);
        bkey ^= reinterpret_cast<uintptr_t>(diffuse) * 1099511628211ull;
        IDirect3DTexture9* staging = nullptr;
        IDirect3DTexture9* baked = nullptr;
        const bool wasInternal = g_internal;
        g_internal = true;
        if (SUCCEEDED(g_presentDevice->CreateTexture(kCpuBakeSize, kCpuBakeSize, 1, 0,
                                                     D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM,
                                                     &staging, nullptr)) && staging) {
            D3DLOCKED_RECT dst{};
            if (SUCCEEDED(staging->LockRect(0, &dst, nullptr, 0)) && dst.pBits) {
                for (UINT y = 0; y < kCpuBakeSize; ++y)
                    memcpy(static_cast<unsigned char*>(dst.pBits) + y * dst.Pitch,
                           &img[static_cast<size_t>(y) * kCpuBakeSize * 4],
                           static_cast<size_t>(kCpuBakeSize) * 4);
                staging->UnlockRect(0);
            }
            if (SUCCEEDED(g_presentDevice->CreateTexture(kCpuBakeSize, kCpuBakeSize, 1, 0,
                                                         D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                                                         &baked, nullptr)) && baked &&
                FAILED(g_presentDevice->UpdateTexture(staging, baked))) {
                baked->Release();
                baked = nullptr;
            }
            staging->Release();
        }
        g_internal = wasInternal;
        if (baked) {
            // Overwrites the "declined, do not retry" entry ClothAlbedoUniform left behind, which
            // is what that entry is for: it stops the decode running every draw without claiming
            // the answer is permanently nothing.
            // BOTH old references, not just the texture. ReleaseClothCache releases pattern
            // AND generated, so an entry that already held a pattern owns a reference to it -
            // dropping only the generated one and then AddRef'ing the pattern again leaks a
            // reference every time an outfit is re-baked. The common case is overwriting the
            // {nullptr, nullptr} entry the decline path leaves, where this is a no-op; it is the
            // second bake of the same garment that would have leaked.
            const auto old = g_clothCache.find(bkey);
            if (old != g_clothCache.end()) {
                if (old->second.generated) old->second.generated->Release();
                if (old->second.pattern) old->second.pattern->Release();
            }
            if (pattern) pattern->AddRef();
            g_clothCache[bkey] = ClothTex{pattern, baked};
            ++g_clothBakedRegistered;
            Log("      registered this bake as the albedo for the GAME's own draw too "
                "(key %016llX) - %u so far", bkey, g_clothBakedRegistered);
        }
    }

    Log("cloth bake #%u: %s | pattern %ux%u diffuse %ux%u%s | mean %.1f of 255, %u%% covered | "
        "colours a(%.2f %.2f %.2f) b(%.2f %.2f %.2f) c(%.2f %.2f %.2f) | %u triangles TILED "
        "outside 0..1 (wrapped, not discarded), %u had to be bounded at 4 tiles",
        g_cpuBakeCount, path, pw, ph, dw, dh, haveDiffuse ? "" : " MISSING",
        n ? acc / (3.0 * n) : 0.0, n ? covered * 100 / n : 0,
        dcA[0], dcA[1], dcA[2], dcB[0], dcB[1], dcB[2], dcC[0], dcC[1], dcC[2],
        g_cpuBakeWrapped, g_cpuBakeClipped);
    {
        // The two inputs that decide brightness, and the pattern's own level. If the pattern is
        // mostly black the lerp chain should leave the diffuse ALONE, so a dark result then means
        // the multiply is at fault rather than the colours.
        double pm = 0.0; unsigned pn = 0;
        for (UINT i = 0; i < pw * ph; i += 7) {
            pm += pat[i * 4] + pat[i * 4 + 1] + pat[i * 4 + 2]; ++pn;
        }
        double dm = 0.0; unsigned dn = 0;
        if (haveDiffuse)
            for (UINT i = 0; i < dw * dh; i += 13) {
                dm += dif[i * 4] + dif[i * 4 + 1] + dif[i * 4 + 2]; ++dn;
            }
        Log("      Diffuse_Color (c11) = (%.3f %.3f %.3f) | pattern mean %.1f | diffuse mean %.1f",
            dcMul[0], dcMul[1], dcMul[2], pn ? pm / (3.0 * pn) : 0.0,
            dn ? dm / (3.0 * dn) : 0.0);
        if (g_cpuBakeCount == 1 && haveDiffuse) {
            char dd[MAX_PATH] = {0};
            GetModuleFileNameA(GetModuleHandleW(nullptr), dd, MAX_PATH);
            char* dsl = strrchr(dd, 0x5C);
            if (dsl) *(dsl + 1) = 0;
            char dpath[MAX_PATH];
            sprintf_s(dpath, "%ssr3-remix-diffuse.dds", dd);
            if (WriteBgraDds(dpath, dw, dh, dif.data()))
                Log("      dumped the diffuse map used by this bake to %s", dpath);
        }
    }
    return path;
}

// Abandon the buffer being captured and remember not to try it again. Used everywhere the
// builder used to set a permanent failure flag.
void ApiSkipCurrentCapture() {
    if (g_apiCapVB && g_apiSkipCount < kMaxApiMeshes)
        g_apiSkipVB[g_apiSkipCount++] = g_apiCapVB;
    Log("remix api: skipping a character mesh - %s (%u skipped so far)", g_apiCharWhyNot,
        g_apiSkipCount);
    g_apiCapVerts.clear();
    g_apiCapIndices.clear();
    g_apiCapIB.clear();
    g_apiSlots.clear();
    g_apiCapHaveVerts = false;
    g_apiCapIBHave = false;
    g_apiCharBuilt = false;
    g_apiCapVB = nullptr;
    // g_apiCapFrame is deliberately NOT reset here. It was, and that one line stopped the drain
    // dead after the first part: RemixBuildCharacter returns early on g_apiCapFrame == 0xFFFFFFFF,
    // so finishing one mesh disabled every later call - no skip, no failure, no log, which is
    // exactly how it presented. Correct when each build began a fresh capture; with the pending
    // table the window frame is history that has to be kept.
}

// Called from Present, on the first frame after the capture frame.
// Load the next captured part into the globals the builder already reads. Nothing about the
// build path changes; only where its input comes from.
bool ApiLoadNextPending() {
    if (g_apiCapHaveVerts) return true;                       // one already loaded
    if (g_apiPendingNext >= g_apiPendingCount) return false;   // all drained
    ApiPending& p = g_apiPending[g_apiPendingNext++];
    g_apiCapVerts = p.verts;
    g_apiCapUvScale[0] = p.uvScale[0];
    g_apiCapUvScale[1] = p.uvScale[1];
    g_apiCapTilingReg[0] = p.uvTilingReg[0];
    g_apiCapTilingReg[1] = p.uvTilingReg[1];
    g_apiSlots = p.slots;
    g_apiCapIB = p.ib;
    g_apiCapIBSize = p.ibSize;
    g_apiCapIB32 = p.ib32;
    g_apiCapIBHave = p.ibHave;
    g_apiCapVB = p.vb;
    g_apiCapFirstVertex = p.firstVertex;
    g_apiCapAlbedo = p.albedo;
    g_apiCapWeights = p.weights;
    g_apiCapBones = p.bones;
    g_apiCapUv2 = p.uv2;
    g_apiCapBoneCount = p.boneCount;
    g_apiCapBoneReg = p.boneReg;
    memcpy(g_apiCharObjTM, p.objTM, sizeof(g_apiCharObjTM));
    g_apiCapHaveVerts = !g_apiCapVerts.empty() && !g_apiSlots.empty() && p.ibHave;
    if (!g_apiCapHaveVerts) {
        Log("remix api: pending part %u has no usable slots - skipped", g_apiPendingNext - 1);
        return ApiLoadNextPending();
    }
    return true;
}

void RemixBuildCharacter() {
    if (!g_settings.remixApiCharacter || g_apiCharBuilt) return;
    if (!g_remixMeshReady) return;
    if (g_apiCapFrame == 0xFFFFFFFFu || g_frames <= g_apiCapFrame) return;   // window still open
    if (!g_apiCaptureClosed) {
        g_apiCaptureClosed = true;
        Log("remix api: capture window closed - %u parts of the character taken in one frame",
            g_apiPendingCount);
    }
    if (!ApiLoadNextPending()) return;
    if (!g_atlasDdsReady) { g_apiCharWhyNot = "no character atlas"; return; }
    if (g_apiSlots.empty() || !g_apiCapIBHave) {
        g_apiCharWhyNot = "no index ranges were collected";
        ApiSkipCurrentCapture();
        return;
    }

    const UINT idxSize = g_apiCapIB32 ? 4u : 2u;
    const size_t vertCount = g_apiCapVerts.size();

    // One index array per slot, all rebased by (idx - minIndex). Held together so the surface
    // array can point into them - a surface keeps a POINTER, so these must outlive CreateMesh.
    ApiMeshStore& store = g_apiStore[g_apiDoneCount];
    std::vector<std::vector<unsigned>>& slotIndices = store.slotIndices;
    try {
        slotIndices.clear();
        slotIndices.resize(g_apiSlots.size());
    } catch (...) { g_apiCharWhyNot = "out of memory"; ApiSkipCurrentCapture(); return; }

    g_apiOutOfRange = 0;
    for (size_t sIdx = 0; sIdx < g_apiSlots.size(); ++sIdx) {
        const ApiSlot& sl = g_apiSlots[sIdx];
        const bool strip = (sl.type == D3DPT_TRIANGLESTRIP);
        // A strip of N triangles is N+2 indices; a list of N triangles is N*3.
        const UINT wanted = strip ? (sl.prims + 2) : (sl.prims * 3);
        std::vector<unsigned>& out = slotIndices[sIdx];

        std::vector<unsigned> raw;
        try { raw.reserve(wanted); } catch (...) { continue; }
        for (UINT k = 0; k < wanted; ++k) {
            const size_t at = static_cast<size_t>(sl.start + k) * idxSize;
            if (at + idxSize > g_apiCapIBSize) break;
            const unsigned v = g_apiCapIB32
                ? *reinterpret_cast<const unsigned*>(&g_apiCapIB[at])
                : *reinterpret_cast<const unsigned short*>(&g_apiCapIB[at]);
            // Absolute: where this index lands in the buffer, minus where our vertex 0 is.
            const long long abs = static_cast<long long>(sl.base) + v;
            const long long rel = abs - g_apiCapFirstVertex;
            if (rel < 0 || rel >= static_cast<long long>(vertCount)) {
                raw.push_back(0u);
                ++g_apiOutOfRange;
            } else {
                raw.push_back(static_cast<unsigned>(rel));
            }
        }

        if (!strip) {
            out.swap(raw);
            if (out.size() % 3) out.resize(out.size() - (out.size() % 3));
        } else {
            // Strip -> list. Winding alternates, and a repeated index is a degenerate join used
            // to stitch strips together; those must be dropped rather than emitted as slivers.
            try { out.reserve(sl.prims * 3); } catch (...) {}
            for (size_t t = 0; t + 2 < raw.size(); ++t) {
                unsigned a = raw[t], b = raw[t + 1], c = raw[t + 2];
                if (a == b || b == c || a == c) { ++g_apiDegenerate; continue; }
                if (t & 1) { const unsigned tmp = b; b = c; c = tmp; }
                out.push_back(a);
                out.push_back(b);
                out.push_back(c);
            }
        }
    }

    store.weights = g_apiCapWeights;
    store.bones = g_apiCapBones;
    store.uv2 = g_apiCapUv2;
    std::vector<RemixHardcodedVertex>& verts = store.verts;
    try { verts.assign(vertCount, RemixHardcodedVertex{}); }
    catch (...) { g_apiCharWhyNot = "out of memory"; ApiSkipCurrentCapture(); return; }
    // UV SCALE. The bind-pose uv values are RAW SHORTS, not 0..1 - SR3's vertex shaders end with
    // `mul o1.xy, r0, 0.0009765625`, i.e. x1/1024, and the fixed-function path reproduces that with
    // a uv texture matrix instead of touching the vertices (the report's "last scale 0.00098").
    // An API mesh has no texture matrix, so the scale has to be baked into the vertex - otherwise
    // the texture tiles about a thousand times over and reads as untextured.
    //
    // DIAGNOSTIC, because "missing polygons" and "wrong texture" have so far both been read off
    // the picture and both been guessed at. These numbers say whether the captured VERTEX DATA is
    // sound, independently of anything Remix does with it.
    unsigned zeroVerts = 0;
    float pLo[3] = {1e30f, 1e30f, 1e30f}, pHi[3] = {-1e30f, -1e30f, -1e30f};
    float uLo[2] = {1e30f, 1e30f}, uHi[2] = {-1e30f, -1e30f};
    for (size_t i = 0; i < vertCount; ++i) {
        const SkinnedVertex& sv = g_apiCapVerts[i];
        RemixHardcodedVertex& hv = verts[i];
        memcpy(hv.position, sv.pos, sizeof(sv.pos));
        memcpy(hv.normal, sv.nrm, sizeof(sv.nrm));
        hv.texcoord[0] = sv.uv[0] * g_apiCapUvScale[0];
        hv.texcoord[1] = sv.uv[1] * g_apiCapUvScale[1];
        hv.color = 0xFFFFFFFFu;
        if (sv.pos[0] == 0.0f && sv.pos[1] == 0.0f && sv.pos[2] == 0.0f &&
            sv.nrm[0] == 0.0f && sv.nrm[1] == 0.0f && sv.nrm[2] == 0.0f)
            ++zeroVerts;
        for (int a = 0; a < 3; ++a) {
            if (sv.pos[a] < pLo[a]) pLo[a] = sv.pos[a];
            if (sv.pos[a] > pHi[a]) pHi[a] = sv.pos[a];
        }
        for (int a = 0; a < 2; ++a) {
            if (hv.texcoord[a] < uLo[a]) uLo[a] = hv.texcoord[a];
            if (hv.texcoord[a] > uHi[a]) uHi[a] = hv.texcoord[a];
        }
    }
    Log("remix api: captured vertex audit - %u of %u vertices are all-zero (never written by the "
        "skinning loop) | pos x[%.2f %.2f] y[%.2f %.2f] z[%.2f %.2f] | uv after x1/1024 "
        "u[%.3f %.3f] v[%.3f %.3f]",
        zeroVerts, static_cast<unsigned>(vertCount),
        pLo[0], pHi[0], pLo[1], pHi[1], pLo[2], pHi[2],
        uLo[0], uHi[0], uLo[1], uHi[1]);
    if (zeroVerts * 4 > vertCount)
        Log("      MORE THAN A QUARTER of the captured vertices are zero. The capture is "
            "incomplete, not the index rebasing - every triangle touching one of these collapses "
            "to the origin and reads as a hole.");

    ++g_remixBuildGen;
    typedef unsigned (__stdcall *PFN_CreateMaterial)(const RemixMaterialInfo*, void**);
    typedef unsigned (__stdcall *PFN_CreateMesh)(const RemixMeshInfo*, void**);
    const PFN_CreateMaterial createMaterial =
        reinterpret_cast<PFN_CreateMaterial>(g_remix.CreateMaterial);
    const PFN_CreateMesh createMesh = reinterpret_cast<PFN_CreateMesh>(g_remix.CreateMesh);

    RemixMaterialInfoOpaqueEXT opaque{};
    opaque.sType = kRemixStructMaterialInfoOpaqueEXT;
    opaque.albedoConstant = {1.0f, 1.0f, 1.0f};
    opaque.opacityConstant = 1.0f;
    opaque.roughnessConstant = 0.6f;
    opaque.metallicConstant = 0.0f;
    opaque.alphaTestType = 7;          // ALWAYS. Zero means NEVER - that cost a run.
    // This part's OWN texture if it can be written; the atlas only as a fallback. Pointing every
    // part at the atlas is what made the hair and clothes come out wrong.
    const char* texPath = g_atlasDdsPath;
    if (g_apiCapAlbedo) {
        const char* own = nullptr;
        if (DumpTextureDds(g_apiCapAlbedo, &own) && own) texPath = own;
    }
    MultiByteToWideChar(CP_ACP, 0, texPath, -1, g_atlasDdsPathW, MAX_PATH);

    RemixMaterialInfo mi{};
    mi.sType = kRemixStructMaterialInfo;
    mi.pNext = &opaque;
    mi.hash = 0x5233C0DE00000000ull + g_remixBuildGen;
    mi.albedoTexture = g_atlasDdsPathW;
    mi.spriteSheetRow = 1;
    mi.spriteSheetCol = 1;
    mi.filterMode = 1;
    mi.wrapModeU = 1;
    mi.wrapModeV = 1;
    // One material per DISTINCT tint. Building one per slot would be 36 registrations of what
    // are mostly the same colour; building one for the whole mesh would throw the customisation
    // away, which is what the untinted copy was.
    // One material per distinct (tint, ALPHA STATE). Alpha is part of a material's identity, not
    // a property of the mesh: two slots of one part legitimately differ only in whether they are
    // cut out, and giving them a shared opaque material is what loses the eyes.
    struct TintMat {
        float c[3];
        int aTest;
        unsigned aRef;
        int blend;
        IDirect3DBaseTexture9* tex;
        void* handle;
        DWORD src, dst;
    };
    std::vector<TintMat> tintMats;
    size_t sIdxForBake = 0;
    auto materialFor = [&](const ApiSlot& sl) -> void* {
        // D3DCMPFUNC 1..8 -> Remix AlphaTestType 0..7 (it mirrors VkCompareOp). Alpha test off
        // means ALWAYS.
        int aTest = 7;
        unsigned aRef = 0;
        if (sl.alphaTestEnable && sl.alphaFunc >= 1 && sl.alphaFunc <= 8) {
            aTest = static_cast<int>(sl.alphaFunc) - 1;
            aRef = sl.alphaRef & 0xFF;
        }
        // TRANSLUCENT ONLY IF THE FACTORS ACTUALLY BLEND.
        //
        // Measured 2026-09-02: 11 of the 12 slots with D3DRS_ALPHABLENDENABLE set use
        // src=D3DBLEND_ONE(2), dst=D3DBLEND_ZERO(1) - the IDENTITY blend, result = source, i.e.
        // an ordinary opaque write with the enable flag left on. Treating the enable flag as
        // "translucent" gave all of them opacityConstant 0.85 and thinned the whole character.
        // Exactly one slot is genuinely blended: src=SRCALPHA(5), dst=INVSRCALPHA(6).
        //
        // So the flag is necessary but not sufficient; the FACTORS decide.
        const bool identityBlend = (sl.srcBlend == 2 /*ONE*/ && sl.destBlend == 1 /*ZERO*/);
        const int blend = (sl.alphaBlendEnable && !identityBlend) ? 1 : 0;
        for (size_t i = 0; i < tintMats.size(); ++i)
            if (std::fabs(tintMats[i].c[0] - sl.tint[0]) < 0.004f &&
                std::fabs(tintMats[i].c[1] - sl.tint[1]) < 0.004f &&
                std::fabs(tintMats[i].c[2] - sl.tint[2]) < 0.004f &&
                tintMats[i].aTest == aTest && tintMats[i].aRef == aRef &&
                tintMats[i].blend == blend && tintMats[i].tex == sl.albedo)
                return tintMats[i].handle;
        if (tintMats.size() >= 32) return tintMats.empty() ? nullptr : tintMats[0].handle;

        // This slot's OWN texture. The part-level texture is only a fallback now, and the atlas
        // only a fallback to that.
        // A baked texture is the shader's own output and beats anything bound, because it IS
        // what the game draws rather than an input to it.
        // CPU BAKE. A customised slot's colour is computed by the pixel shader and exists in no
        // texture, so it is evaluated here from the pattern and the three colours and written out.
        // Cached per material key, so each distinct item is baked once.
        const char* clothTex = nullptr;
        // Blended slots ARE baked again. Skipping them turned the underwear white, because it
        // is itself a blended slot and lost its baked colour. It needs BOTH - the recipe's colour
        // and the texture's alpha - so the bake now carries alpha through instead of writing 255.
        if (sl.cloth && g_cpuBakeCount < 12 && sIdxForBake < slotIndices.size()) {
            if (!sl.pattern) {
                Log("cloth bake skipped: no Pattern_Map bound on this slot");
            } else {
                // Pattern coordinates, scaled to 0..1 here so the baker takes one convention.
                //
                // FALLBACK TO TEXCOORD0. Three parts - 416, 130 and 1128 vertices - carry no
                // TEXCOORD1 on any draw, and one of them is the corset, which is visibly showing
                // its segmentation map raw. Those shader variants sample the pattern with the
                // FIRST uv set. Using set 0 there is better than not baking at all, and a wrong
                // guess shows as a mis-sampled bake in the dump rather than as a silent skip.
                std::vector<float> puv;
                bool fromSet1 = (store.uv2.size() == verts.size() * 2);
                try {
                    puv.resize(verts.size() * 2);
                    for (size_t k = 0; k < verts.size(); ++k) {
                        if (fromSet1) {
                            // The pattern's own coordinates take the same scale. When a part
                            // has no TEXCOORD1 the baker falls back to TEXCOORD0, which is
                            // exactly the stream this scale was measured on - so a wrong scale
                            // here is what drove the underwear's bake to 3% coverage.
                            puv[k * 2] = store.uv2[k * 2] * g_apiCapUvScale[0];
                            puv[k * 2 + 1] = store.uv2[k * 2 + 1] * g_apiCapUvScale[1];
                        } else {
                            puv[k * 2] = verts[k].texcoord[0];
                            puv[k * 2 + 1] = verts[k].texcoord[1];
                        }
                    }
                } catch (...) { puv.clear(); }
                if (puv.size() == verts.size() * 2) {
                    if (!fromSet1)
                        Log("cloth bake: this part has no TEXCOORD1, sampling the pattern with "
                            "TEXCOORD0 instead (%u verts)", (unsigned)verts.size());
                    // ONLY pass a diffuse map if the slot HAS one.
                    //
                    // For a slot whose ranked albedo is the Pattern_Map itself - the corset is
                    // one - sl.albedo and sl.pattern are the same texture, and multiplying the
                    // pattern by the colours reproduced the segmentation map. Those slots have no
                    // diffuse: the lerp chain alone is their colour.
                    IDirect3DBaseTexture9* dif =
                        (sl.albedo && sl.albedo != sl.pattern) ? sl.albedo : nullptr;
                    clothTex = CpuBakeCloth(verts, puv, slotIndices[sIdxForBake], sl.pattern,
                                            dif, sl.dcA, sl.dcB, sl.dcC, sl.dcMul);
                }
            }
        }

        const char* slotTex = texPath;
        if (sl.hair) {
            // STRANDS, when the Dob_Map is available.
            //
            // albedo = chosen hair colour * (dob.R / 255). The mask is authored and unlit, so
            // this adds the strand variation the flat constant could never have while leaving
            // every photon to Remix.
            if (g_settings.hairStrandsFromDob && sl.dob) {
                std::vector<unsigned char> dob;
                UINT hw = 0, hh = 0;
                if (TextureToBgra(sl.dob, dob, hw, hh) && hw && hh &&
                    dob.size() >= static_cast<size_t>(hw) * hh * 4) {
                    std::vector<unsigned char> out;
                    bool ok = true;
                    try { out.assign(static_cast<size_t>(hw) * hh * 4, 0); }
                    catch (...) { ok = false; }
                    if (ok) {
                        double acc = 0.0;
                        for (size_t i = 0; i < static_cast<size_t>(hw) * hh; ++i) {
                            const float m = dob[i * 4 + 2] / 255.0f;   // BGRA -> R is the strands
                            for (int k = 0; k < 3; ++k) {
                                float v = sl.hairColor[k] * m * g_settings.clothTintScale;
                                v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
                                const unsigned char q =
                                    static_cast<unsigned char>(v * 255.0f + 0.5f);
                                out[i * 4 + (2 - k)] = q;      // write B,G,R
                                acc += q;
                            }
                            out[i * 4 + 3] = 255;
                        }
                        char hdir[MAX_PATH] = {0};
                        GetModuleFileNameA(GetModuleHandleW(nullptr), hdir, MAX_PATH);
                        char* hsl = strrchr(hdir, 0x5C);
                        if (hsl) *(hsl + 1) = 0;
                        static char hairPath[MAX_PATH];
                        ++g_hairGenerated;
                        sprintf_s(hairPath, "%ssr3-remix-hair-%u.dds", hdir, g_hairGenerated);
                        if (WriteBgraDds(hairPath, hw, hh, out.data())) {
                            Log("HAIR STRANDS #%u: %ux%u = colour (%.3f %.3f %.3f) * the Dob_Map "
                                "strand channel | mean %.1f of 255 -> %s",
                                g_hairGenerated, hw, hh, sl.hairColor[0], sl.hairColor[1],
                                sl.hairColor[2],
                                acc / (3.0 * static_cast<double>(hw) * hh), hairPath);
                            slotTex = hairPath;
                            goto hairDone;
                        }
                        --g_hairGenerated;
                    }
                }
            }
            // NO TEXTURE FOR HAIR.
            //
            // The hair slot's chosen albedo is its Diffuse_Map, and for hair that map is smooth
            // DIRECTIONAL data - the blue/green/purple image the user sees. It is not a colour,
            // so binding it wins over the colour constant and the hair renders as the mask.
            // Hair_Spec_Color2 carries the chosen colour; a flat constant with no texture is
            // closer to right than a mask, and the strand detail is a later problem.
            slotTex = nullptr;
            hairDone: ;
        } else if (clothTex) {
            slotTex = clothTex;
        } else if (sl.bakedPath[0]) {
            slotTex = sl.bakedPath;
        } else if (sl.albedo) {
            const char* own = nullptr;
            if (DumpTextureDds(sl.albedo, &own) && own) slotTex = own;
        }
        if (slotTex) {
            MultiByteToWideChar(CP_ACP, 0, slotTex, -1, g_atlasDdsPathW, MAX_PATH);
            mi.albedoTexture = g_atlasDdsPathW;
        } else {
            mi.albedoTexture = nullptr;
        }
        ++g_remixBuildGen;
        // ALBEDO IS A COLOUR: 0..1. The captured Tint_color came back as 5.0 on every part's
        // blended slot, which is not a base colour - it is a multiplier, and handing it to
        // albedoConstant would coat the character in blown-out white. Clamped, and the raw value
        // is kept in the log so the difference stays visible.
        // Which constant colours this surface. For a customised item whose albedo could NOT be
        // generated, Tint_color is not it - the colour lives in Diffuse_Color_a/b/c, and the
        // dominant term is _a (the pattern's red channel). Using it makes the item its CHOSEN
        // colour instead of the pattern's white.
        //
        // This is an APPROXIMATION and is labelled as one: the real recipe weights all three by
        // the pattern channels at gamma 2.2 per texel, which a single constant cannot express.
        // It is the difference between a white bracelet and a roughly right one, not correctness.
        // ONLY where the chosen albedo is a PATTERN.
        //
        // The first version applied Diffuse_Color_a to EVERY slot carrying the cloth recipe,
        // including the top - which already had a real Diffuse_Map at rank 100 and rendered
        // correctly. Its constant is near-black, so the top went black. A regression caused by
        // widening a fix past the case it was for.
        //
        // The white items are the ones whose chosen albedo ranks 80, i.e. a Pattern_Map bound as
        // base colour. A slot with a genuine diffuse map keeps it and keeps its tint.
        //
        // And a near-black constant is never an improvement on white: if the colour reads as
        // unset, leave the surface alone rather than making it black.
        const float* src = sl.tint;
        if (sl.hair) {
            src = sl.hairColor;
        } else if (sl.cloth && !sl.clothGenerated && sl.albedoRank <= 80) {
            const float lum = sl.dcA[0] + sl.dcA[1] + sl.dcA[2];
            if (lum > 0.02f) src = sl.dcA;
            else ++g_apiClothBlackConst;
        }
        float alb[3];
        for (int c = 0; c < 3; ++c)
            alb[c] = (src[c] < 0.0f) ? 0.0f : (src[c] > 1.0f ? 1.0f : src[c]);
        opaque.albedoConstant = {alb[0], alb[1], alb[2]};
        // A GENUINELY BLENDED SLOT MUST BE CUT OUT, NOT DRAWN AS A TRANSLUCENT QUAD.
        //
        // The one slot with real alpha blending (src SRCALPHA / dst INVSRCALPHA) is a decal layer
        // - the logo on the underwear. Giving it alphaTestType ALWAYS drew its whole quad, so the
        // transparent area around the logo appeared as a solid rectangle over the garment.
        //
        // Discarding only texels whose alpha is essentially zero keeps every visible part of the
        // logo, including its soft edges, while removing the empty area around it. A high
        // reference value would eat the logo's own edges, so it is deliberately low.
        if (blend && aTest == 7) {
            opaque.alphaTestType = 6;              // GREATER_OR_EQUAL
            opaque.alphaReferenceValue = 8;        // only fully transparent texels are dropped
            opaque.opacityConstant = 1.0f;
        } else {
            opaque.alphaTestType = aTest;
            opaque.alphaReferenceValue = static_cast<unsigned char>(aRef);
        }
        // A blended draw is translucent; say so through opacityConstant rather than guessing at
        // blendType_value, whose enumeration is not in the header.
        opaque.opacityConstant = 1.0f;
        mi.hash = 0x5233C0DE00000000ull + g_remixBuildGen;
        void* h = nullptr;
        const unsigned rc2 = createMaterial(&mi, &h);
        if (rc2 != 0 || !h) {
            Log("remix api: material failed rc=%u (%s)", rc2, RemixErrName(rc2));
            return tintMats.empty() ? nullptr : tintMats[0].handle;
        }
        tintMats.push_back(
            TintMat{{sl.tint[0], sl.tint[1], sl.tint[2]}, aTest, aRef, blend, sl.albedo, h,
                    sl.srcBlend, sl.destBlend});
        return h;
    };

    // A white fallback, and the first entry, so a slot with no Tint_color still has a material.
    ApiSlot whiteSlot{};
    whiteSlot.tint[0] = whiteSlot.tint[1] = whiteSlot.tint[2] = whiteSlot.tint[3] = 1.0f;
    whiteSlot.alphaFunc = 8;   // D3DCMP_ALWAYS
    whiteSlot.albedo = g_apiCapAlbedo;
    void* material = materialFor(whiteSlot);
    if (!material) {
        g_apiCharWhyNot = "CreateMaterial failed";
        ApiSkipCurrentCapture();
        return;
    }

    // Every slot shares the one vertex array and the one material; only the indices differ.
    // Built only AFTER every inner index vector is final: taking a pointer into a vector that
    // is later resized is the same class of mistake as the one above, one level down.
    std::vector<RemixMeshInfoSurfaceTriangles>& surfaces = store.surfaces;
    surfaces.clear();
    unsigned tris = 0;
    try { surfaces.reserve(slotIndices.size()); } catch (...) {}
    for (size_t sIdx = 0; sIdx < slotIndices.size(); ++sIdx) {
        if (slotIndices[sIdx].size() < 3) continue;
        RemixMeshInfoSurfaceTriangles su{};
        su.vertices_values = verts.data();
        su.vertices_count = verts.size();
        su.indices_values = slotIndices[sIdx].data();
        su.indices_count = slotIndices[sIdx].size();
        // Remix does the skinning. Both arrays are 4 per vertex and live in the same store as
        // the vertices, so they outlive CreateMesh for the same reason.
        if (g_settings.remixApiSkinning && !store.weights.empty() &&
            store.weights.size() == verts.size() * 4 && !store.bones.empty() &&
            store.bones.size() == verts.size() * 4) {
            su.skinning_hasvalue = 1;
            su.skinning_value.bonesPerVertex = 4;
            su.skinning_value.blendWeights_values = store.weights.data();
            su.skinning_value.blendWeights_count =
                static_cast<unsigned>(store.weights.size());
            su.skinning_value.blendIndices_values = store.bones.data();
            su.skinning_value.blendIndices_count = static_cast<unsigned>(store.bones.size());
        } else {
            su.skinning_hasvalue = 0;
        }
        sIdxForBake = sIdx;
        su.material = materialFor(g_apiSlots[sIdx]);
        if (!su.material) su.material = material;
        surfaces.push_back(su);
        tris += static_cast<unsigned>(slotIndices[sIdx].size() / 3);
    }
    if (surfaces.empty()) {
        g_apiCharWhyNot = "every slot was empty after rebasing";
        ApiSkipCurrentCapture();
        return;
    }

    // ---------------------------------------------------------------------------------------
    // SLOT STRUCTURE, logged in full.
    //
    // Four theories for the holes have now been proposed from the picture and all four are dead:
    // overlap with the game's own character (offsetting changed nothing), a baseVertex mistake
    // (the fix was a no-op), out-of-range indices (0), and duplicate index ranges (0 dropped).
    // Every aggregate number describes a healthy mesh. So stop inferring and print the thing
    // itself: what each slot actually covers, and how much of the vertex set anything references.
    //
    // What to look for:
    //   ranges that OVERLAP without being identical - LOD variants or nested submissions would
    //     z-fight exactly like duplicates while defeating an equality test;
    //   a referenced-vertex count well below 7977 - then the slots simply do not cover the body
    //     and the holes are geometry that was never submitted.
    {
        std::vector<unsigned char> used;
        unsigned referenced = 0, overlapping = 0;
        try {
            used.assign(vertCount, 0);
            for (size_t a = 0; a < slotIndices.size(); ++a)
                for (size_t k = 0; k < slotIndices[a].size(); ++k) {
                    const unsigned v = slotIndices[a][k];
                    if (v < vertCount && !used[v]) { used[v] = 1; ++referenced; }
                }
        } catch (...) {}
        // A SLOT'S INDEX RANGE DEPENDS ON ITS TOPOLOGY, and this used prims*3 for every slot.
        //
        // The builder has known since the strip discovery that a STRIP of N triangles occupies
        // N+2 indices, not N*3 - but this diagnostic never learned it, so it reported every strip
        // slot as three times too long, found them all "overlapping" their neighbours, and then
        // stated a conclusion as fact: "coplanar duplicates ... they z-fight exactly like
        // identical ones."
        //
        // Every one of those warnings was false. Read with the right formula the slots TILE
        // exactly - part 2 of the 2026-09-04 run reported slot 0 as [0..5715) and slot 1 starting
        // at 1907, and 1907 is precisely 0 + 1905 + 2. That is a perfect strip boundary, not an
        // overlap.
        //
        // Kept as a warning rather than deleted, because a genuine overlap is worth catching -
        // but it now has to be genuine. A diagnostic that cries wolf on healthy data is worse
        // than none: it sends the next reader chasing a duplicate that was never there.
        auto slotIndexCount = [](const ApiSlot& sl) -> UINT {
            return (sl.type == D3DPT_TRIANGLESTRIP) ? (sl.prims + 2) : (sl.prims * 3);
        };
        // Per PART, not since the process started. These were cumulative globals printed as
        // though they described the part being built, so part 6 read "13 triangle STRIPS" for a
        // 3-slot mesh and "3914 degenerate joins" for one that dropped 636.
        unsigned stripsHere = 0;
        for (size_t a = 0; a < g_apiSlots.size(); ++a)
            if (g_apiSlots[a].type == D3DPT_TRIANGLESTRIP) ++stripsHere;
        g_apiStripSlots += stripsHere;
        const unsigned degenerateHere = g_apiDegenerate - g_apiDegeneratePrev;
        g_apiDegeneratePrev = g_apiDegenerate;
        Log("remix api: SLOT STRUCTURE - %u slots (%u triangle STRIPS), %u of %u vertices "
            "referenced, %u degenerate strip joins dropped in this part",
            static_cast<unsigned>(g_apiSlots.size()), stripsHere, referenced,
            static_cast<unsigned>(vertCount), degenerateHere);
        // Say what scale this part was built with and where it came from. A uv range outside
        // 0..1 with tiling reg -1 means no tiling constant matched and the fault is elsewhere -
        // which is a different next step from "the tiling was found and applied".
        Log("      uv scale %.8f x %.8f  (bare 1/1024 is %.8f) | tiling from vs const u=%d v=%d "
            "(-1 = no tiling constant matched this albedo)",
            g_apiCapUvScale[0], g_apiCapUvScale[1], kShortUVScale,
            g_apiCapTilingReg[0], g_apiCapTilingReg[1]);
        for (size_t a = 0; a < g_apiSlots.size(); ++a) {
            const UINT aLo = g_apiSlots[a].start, aHi = aLo + slotIndexCount(g_apiSlots[a]);
            unsigned hits = 0;
            for (size_t b = 0; b < g_apiSlots.size(); ++b) {
                if (b == a) continue;
                const UINT bLo = g_apiSlots[b].start, bHi = bLo + slotIndexCount(g_apiSlots[b]);
                if (aLo < bHi && bLo < aHi) ++hits;
            }
            if (hits) ++overlapping;
            if (a < 40)
                Log("      slot %2u: startIndex %6u, %5u tris %s, base %d, indices [%u..%u)%s",
                    static_cast<unsigned>(a), g_apiSlots[a].start, g_apiSlots[a].prims,
                    g_apiSlots[a].type == D3DPT_TRIANGLESTRIP ? "STRIP" : "list ",
                    g_apiSlots[a].base, aLo, aHi,
                    hits ? "  <- OVERLAPS another slot" : "");
        }
        if (overlapping)
            Log("      %u of %u slots share index ranges with another, read with each slot's OWN "
                "topology. This one is real: overlapping ranges are coplanar duplicates that an "
                "equality test cannot see, and they z-fight exactly like identical ones.",
                overlapping, static_cast<unsigned>(g_apiSlots.size()));
    }

    RemixMeshInfo meshInfo{};
    meshInfo.sType = kRemixStructMeshInfo;
    meshInfo.hash = 0x5233C0DE10000000ull + g_remixBuildGen;
    meshInfo.surfaces_values = surfaces.data();
    meshInfo.surfaces_count = static_cast<unsigned>(surfaces.size());
    const unsigned rc = createMesh(&meshInfo, &g_apiCharMesh);
    if (rc != 0 || !g_apiCharMesh) {
        Log("remix api: character CreateMesh failed rc=%u (%s)", rc, RemixErrName(rc));
        g_apiCharWhyNot = "CreateMesh failed";
        ApiSkipCurrentCapture();
        return;
    }
    g_apiCharVertexCount = static_cast<unsigned>(vertCount);
    g_apiCharTriangles = tris;
    g_apiSurfaces = static_cast<unsigned>(surfaces.size());
    g_apiCharBuilt = true;
    g_apiCharWhyNot = "built";

    // File it, then RESET so the next frame captures another of the character's buffers. The
    // store this mesh used is not touched again - the next build takes the following slot.
    ApiDoneMesh& done = g_apiDone[g_apiDoneCount];
    done.mesh = g_apiCharMesh;
    done.tris = tris;
    done.surfaces = g_apiSurfaces;
    done.vb = g_apiCapVB;
    done.boneCount = g_apiCapBoneCount;
    done.boneReg = g_apiCapBoneReg;
    done.haveBones = false;
    for (unsigned b = 0; b < kMaxApiBones; ++b) {
        memset(done.bones[b], 0, sizeof(done.bones[b]));
        done.bones[b][0] = done.bones[b][5] = done.bones[b][10] = 1.0f;   // identity 3x4
    }
    memcpy(done.objTM, g_apiCharObjTM, sizeof(done.objTM));
    ++g_apiDoneCount;
    g_apiDoneTris += tris;

    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f}, mid[3] = {0, 0, 0};
    for (size_t i = 0; i < verts.size(); ++i)
        for (int a = 0; a < 3; ++a) {
            const float v = verts[i].position[a];
            if (v < lo[a]) lo[a] = v;
            if (v > hi[a]) hi[a] = v;
            mid[a] += v;
        }
    if (!verts.empty()) for (int a = 0; a < 3; ++a) mid[a] /= static_cast<float>(verts.size());
    for (int r = 0; r < 3; ++r)
        g_apiCharAt[r] = g_apiCharObjTM[r][0] * mid[0] + g_apiCharObjTM[r][1] * mid[1] +
                         g_apiCharObjTM[r][2] * mid[2] + g_apiCharObjTM[r][3];
    for (size_t hi = 0; hi < g_apiSlots.size(); ++hi)
        if (g_apiSlots[hi].hair) {
            Log("hair: slot %u uses Hair_Spec_Color (%.3f %.3f %.3f) as its albedo - the Dob_Map "
                "holds strands, not colour", (unsigned)hi, g_apiSlots[hi].hairColor[0],
                g_apiSlots[hi].hairColor[1], g_apiSlots[hi].hairColor[2]);
            break;
        }
    if (g_apiClothGenerated || g_apiClothApprox)
        Log("remix api: CUSTOMISED items - %u albedos GENERATED, %u could NOT be generated "
            "(the player's own clothing samples a Diffuse_Map through a second UV set). Of those, "
            "only slots whose albedo ranks <= 80 (a Pattern_Map) take Diffuse_Color_a; a slot with "
            "a real Diffuse_Map keeps its texture - applying it to those is what blacked the top.",
            g_apiClothGenerated, g_apiClothApprox);
    if (g_apiClothBlackConst)
        Log("      %u customised slots had a near-BLACK Diffuse_Color_a and were left with their "
            "own texture - applying it would have blacked the surface, which is what happened to "
            "the top when this was applied to every cloth slot", g_apiClothBlackConst);
    // What each slot IS, by name. Identifying a part from the picture and then guessing which
    // shader it uses has cost several runs; this says it outright.
    for (size_t li = 0; li < g_apiSlots.size() && li < 16; ++li)
        Log("      slot %u: albedo '%s', first '%s', %s%s%u tris", (unsigned)li,
            g_apiSlots[li].albedoName, g_apiSlots[li].firstName,
            g_apiSlots[li].cloth ? "CLOTH " : "", g_apiSlots[li].hair ? "HAIR " : "",
            g_apiSlots[li].prims);
    Log("remix api: %u distinct materials across %u slots (tint x alpha state)",
        static_cast<unsigned>(tintMats.size()), static_cast<unsigned>(g_apiSlots.size()));
    static const char* kCmp[8] = {"NEVER", "LESS", "EQUAL", "LESS_EQ", "GREATER", "NOT_EQ",
                                  "GREATER_EQ", "ALWAYS"};
    for (size_t i = 0; i < tintMats.size() && i < 12; ++i)
        // D3DBLEND: 1 ZERO, 2 ONE, 3 SRCCOLOR, 4 INVSRCCOLOR, 5 SRCALPHA, 6 INVSRCALPHA,
        // 9 DESTCOLOR. src ONE / dst ONE is additive; SRCALPHA / INVSRCALPHA is ordinary alpha.
        Log("      mat %u: tex %p, tint (%.2f %.2f %.2f)%s, alphaTest %s ref %u, blend %s "
            "(src %lu dst %lu)%s",
            static_cast<unsigned>(i), static_cast<void*>(tintMats[i].tex),
            tintMats[i].c[0], tintMats[i].c[1], tintMats[i].c[2],
            (tintMats[i].c[0] > 1.0f || tintMats[i].c[1] > 1.0f || tintMats[i].c[2] > 1.0f)
                ? " CLAMPED to 1" : "",
            (tintMats[i].aTest >= 0 && tintMats[i].aTest < 8) ? kCmp[tintMats[i].aTest] : "?",
            tintMats[i].aRef, tintMats[i].blend ? "ON" : "off",
            tintMats[i].src, tintMats[i].dst,
            tintMats[i].tex ? "" : "  <- NO texture of its own, using the fallback");
    Log("remix api: STEP 3b - character mesh built: %u surfaces from %u slots (%u duplicate "
        "index ranges dropped), %u verts, %u triangles, placed at (%.1f %.1f %.1f), extent "
        "%.2f x %.2f x %.2f",
        g_apiSurfaces, g_apiSlotsSeen, g_apiSlotsDuplicate, g_apiCharVertexCount,
        g_apiCharTriangles,
        g_apiCharAt[0], g_apiCharAt[1], g_apiCharAt[2],
        hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]);
    // Reset the capture so another buffer can be taken. Everything the finished mesh needs
    // lives in g_apiStore[its index] and g_apiDone[its index] from here on.
    g_apiCapVerts.clear();
    g_apiCapIndices.clear();
    g_apiCapIB.clear();
    g_apiSlots.clear();
    g_apiCapHaveVerts = false;
    g_apiCapIBHave = false;
    g_apiCharBuilt = false;
    g_apiCapVB = nullptr;
    // g_apiCapFrame is deliberately NOT reset here. It was, and that one line stopped the drain
    // dead after the first part: RemixBuildCharacter returns early on g_apiCapFrame == 0xFFFFFFFF,
    // so finishing one mesh disabled every later call - no skip, no failure, no log, which is
    // exactly how it presented. Correct when each build began a fresh capture; with the pending
    // table the window frame is history that has to be kept.

    if (g_apiOutOfRange)
        Log("      %u indices fell outside the captured vertex window and were collapsed to "
            "degenerates - if this is a large fraction of %u, the missing geometry is those, not "
            "a partial capture", g_apiOutOfRange, g_apiCharTriangles * 3);
}

HRESULT WINAPI Hook_DrawIndexedPrimitive(IDirect3DDevice9* dev, D3DPRIMITIVETYPE type,
                                         INT baseVertex, UINT minIndex, UINT numVertices,
                                         UINT startIndex, UINT primitiveCount) {
    const LONGLONG tShim = Now();
    ++g_drawsTotal;
    ++g_drawIndexThisFrame;
    NoteCommandBlock();
    // Before anything takes a pointer into the caches these would erase from.
    DrainPendingInvalidations();
    CreateSkinBuffer(dev);
    ProbeSkyOrder(dev, numVertices, primitiveCount);
    g_meshKeyBaseVertex = static_cast<unsigned>(baseVertex) + minIndex;
    g_curDrawFirstVertex = static_cast<UINT>(max(0, baseVertex + static_cast<INT>(minIndex)));
    g_curDrawVertexCount = numVertices;
    EmitLight(dev);
    FFPScope scope;
    Disp d = BeginFFP(dev, scope);

    // An EXACT duplicate: same buffer, same vertex range, same index range, same triangle count,
    // already converted earlier in this frame. The rasteriser blends such passes into one
    // surface; a path tracer gets two surfaces occupying the same space, which is what "drawn
    // twice" looks like. Only the first is converted - the rest are hidden, not skipped, because
    // the engine still needs them for its own buffers.
    bool skinned = false;
    // Skinned draws need the pose in the key; rigid ones do not have one. Everything else about
    // the test is identical, and the duplicates measured on 2026-08-27 were overwhelmingly NOT
    // skinned - 25 world draws against 4 character draws in one frame - so restricting this to
    // skinned geometry left most of the coincident surfaces in place.
    const bool dedupThis = (d == Disp::Convert) &&
                           (g_curLayout.skinned ? g_settings.dedupSkinned : g_settings.dedupAll);
    if (dedupThis) {
        unsigned long long dk = SkinMeshKey(g_stream0, g_stream0Offset, g_stream0Stride,
                                            minIndex, numVertices);
        dk ^= (static_cast<unsigned long long>(startIndex) << 32) ^ primitiveCount;
        dk *= 1099511628211ull;
        // The POSE, folded in. Without it this key is identical for two different NPCs wearing
        // the same garment - they differ only in their bone palette - and the first attempt at
        // this hid the second character's clothing, which the user saw as parts of NPCs
        // disappearing and coming back. Geometry alone does not identify an object.
        //
        // Eight bones is enough to separate two characters and cheap enough to run per draw:
        // 96 floats against ~94 skinned draws a frame. A full-palette hash would be 768.
        //
        // objTM is hashed TOO, and it is not redundant with the palette. The bones are in OBJECT
        // space - objTM is applied after the blend - so two idle NPCs holding the same pose have
        // byte-identical palettes and differ only in where objTM puts them. Without this the
        // pose key merges them and the second one loses parts, which is the third time this key
        // has been too small: first no index range, then no pose, then no position.
        for (int b = 0; g_curLayout.skinned && b < 8; ++b) {
            const float* r = &g_vsConst[kRegBonePalette + b * kRegsPerBone][0];
            for (int e = 0; e < 4; ++e) {
                unsigned bits;
                memcpy(&bits, &r[e], sizeof(bits));
                dk ^= bits;
                dk *= 1099511628211ull;
            }
        }
        for (int r = 0; r < kRegsPerBone; ++r) {
            for (int e = 0; e < 4; ++e) {
                unsigned bits;
                memcpy(&bits, &g_vsConst[kRegObjTM + r][e], sizeof(bits));
                dk ^= bits;
                dk *= 1099511628211ull;
            }
        }
        // The ALBEDO, and this is the FOURTH thing this key has been missing. Measured on a real
        // frame before switching the dedup on: of 68 groups the key would have merged, 67 shared
        // one texture and one did not -
        //
        //     v=1080 p=499 at(96.9 145.7 29.7) -> textures 16484C98, 16484E58, 16484F38
        //
        // three draws of one mesh, one pose and one position carrying three DIFFERENT textures.
        // They are separate material layers on the same geometry, and merging them would have
        // dropped two of the three - which is exactly how the previous four attempts failed, in a
        // form no amount of staring at the key would have revealed.
        //
        // Keyed on what was actually bound rather than on stage 0, because that is what Remix
        // receives and what makes two draws genuinely the same surface.
        dk ^= reinterpret_cast<uintptr_t>(g_lastBoundAlbedo);
        dk *= 1099511628211ull;
        // For an INSTANCED draw objTM is identical across the whole batch - the placement
        // lives in the instance stream instead, and the frame dump shows every one of them
        // reporting at(0.0 0.0 -1024.0). Without this the key cannot tell two instances of a
        // street prop apart, and the second would be dropped as a duplicate of the first.
        // InstanceWorld is a cache lookup and never locks, so this costs no bridge traffic.
        bool canJudge = true;
        if (g_instancedDraw) {
            D3DMATRIX iw{};
            if (InstanceWorld(iw)) {
                const float* m = &iw._11;
                for (int e = 0; e < 16; ++e) {
                    unsigned bits;
                    memcpy(&bits, &m[e], sizeof(bits));
                    dk ^= bits;
                    dk *= 1099511628211ull;
                }
            } else {
                // Instanced, but its placement could not be read. Then nothing in the key
                // separates two instances of the same prop, and merging them would delete
                // one. Refuse to judge rather than guess: a missed duplicate is a second
                // surface, a wrong merge is a missing object.
                canJudge = false;
                ++g_dedupUnjudged;
            }
        }
        bool dup = false;
        for (unsigned i = 0; canJudge && i < g_skinFrameKeyCount; ++i)
            if (g_skinFrameKeys[i] == dk) { dup = true; break; }
        if (dup) {
            ++g_skinRepeatDraws;
            ++g_skinRepeatHidden;
            EndFFP(dev, scope);
            scope = FFPScope{};
            d = Because(g_curLayout.skinned
                            ? "skinned duplicate: identical triangles already converted this frame"
                            : "duplicate: identical triangles already converted this frame",
                        HiddenDisp(true));
        } else if (canJudge && g_skinFrameKeyCount < kMaxSkinFrameKeys) {
            g_skinFrameKeys[g_skinFrameKeyCount++] = dk;
        } else {
            ++g_dedupKeyOverflow;
        }
    }
    // A skinned draw converted WITHOUT skinning would be worse than not converting it: fixed
    // function cannot read the bone palette, so the mesh would render as a rigid bind pose - a
    // T-posed statue instead of a stuttering character. So the substitution has to succeed
    // before the conversion is allowed to stand, and if it fails the draw goes back to passing
    // through exactly as it did before.
    if (d == Disp::Convert && g_curLayout.skinned) {
        skinned = SkinAndBind(dev, baseVertex, minIndex, numVertices);
        if (!skinned) {
            ++g_skinnedRefused;
            ProbeRigidSkinned(static_cast<UINT>(max(0, baseVertex + static_cast<INT>(minIndex))),
                              numVertices);
            // Name each distinct reason once. A refusal is not a missed optimisation: the draw
            // falls back to pass-through, so Remix reconstructs it as a SECOND copy beside the
            // skinned one - which is what "my character looks like it is drawn twice" would
            // look like. The reason decides the fix, so it must be measured, not assumed.
            if (g_skinRefuseWhy && g_skinRefuseReports < 8) {
                bool seen = false;
                for (unsigned i = 0; i < g_skinRefuseReports; ++i)
                    if (!strcmp(g_skinRefuseNames[i], g_skinRefuseWhy)) { seen = true; break; }
                if (!seen) {
                    strncpy_s(g_skinRefuseNames[g_skinRefuseReports], g_skinRefuseWhy, 63);
                    // Numeric D3DDECLTYPE alongside the name: the previous run printed "?" for
                    // the weights, which says only that the type is outside DeclTypeName's
                    // table and not WHICH type it is. A number can be looked up; a "?" cannot.
                    Log("skinning REFUSED #%u: %s | verts=%u stride=%u decl: pos=%s(%d)@%d "
                        "normal=%s(%d)@%d weights=%s(%d)@%d indices=%s(%d)@%d uv=%s(%d)@%d",
                        g_skinRefuseReports + 1, g_skinRefuseWhy, numVertices, g_stream0Stride,
                        DeclTypeName(g_curLayout.posType), g_curLayout.posType,
                        g_curLayout.posOffset,
                        DeclTypeName(g_curLayout.normalType), g_curLayout.normalType,
                        g_curLayout.normalOffset,
                        DeclTypeName(g_curLayout.blendWeightType), g_curLayout.blendWeightType,
                        g_curLayout.blendWeightOffset,
                        DeclTypeName(g_curLayout.blendIndexType), g_curLayout.blendIndexType,
                        g_curLayout.blendIndexOffset,
                        DeclTypeName(g_curLayout.texcoordType), g_curLayout.texcoordType,
                        g_curLayout.texcoordOffset);
                    ++g_skinRefuseReports;
                }
            }
            EndFFP(dev, scope);
            scope = FFPScope{};
            d = Because("skinned, but the bind pose could not be read", Disp::PassThrough);
        }
    }

    // Population of the deferred G-buffer pass, measured against what is actually converted.
    // Purely observational until the numbers say what hiding it would cost.
    if (g_curPS.isPixelShader && g_curPS.rtCount > 1) {
        ++g_mrtWouldHide;
        if (d == Disp::Convert) {
            ++g_mrtConverted;
            if (g_mrtSamplerReports < 10 && g_curPS.albedoSampler[0]) {
                bool seen = false;
                for (unsigned i = 0; i < g_mrtSamplerReports; ++i)
                    if (!strcmp(g_mrtSamplerNames[i], g_curPS.albedoSampler)) { seen = true; break; }
                if (!seen) {
                    strncpy_s(g_mrtSamplerNames[g_mrtSamplerReports], g_curPS.albedoSampler, 27);
                    ++g_mrtSamplerReports;
                }
            }
        }
    } else if (g_curPS.isPixelShader && d == Disp::Convert) {
        ++g_singleTargetConverted;
    }

    const bool ffp = (d == Disp::Convert);
    if (ffp) {
        NoteConvertedTarget();
        Trace('D');
        MeasureShortUVs(minIndex, numVertices);
        MeasureWorldExtent(minIndex, numVertices);
    } else {
        Trace(d == Disp::Hide ? 'h' : (d == Disp::Mark ? 'm' : 'x'));
    }
    ProbeDraw(dev, numVertices, primitiveCount, ffp);
    ClearBackBufferOnce(dev);
    ProbeSkyDraw(minIndex, numVertices, d);
    ProbeHudDraw(minIndex, numVertices, d);
    ProbeShapes(minIndex, numVertices, primitiveCount, d);
    ProbeSkinned(minIndex, numVertices, d);
    ProbeDecal(dev, numVertices, d);
    ProbeBoundAlbedo(d);
    ProbeAtlasComposite(dev, numVertices, primitiveCount, d);
    RecordFrameDraw(dev, numVertices, primitiveCount, d);

    // Skip: never reaches the device, so Remix cannot capture it. Reporting success is correct -
    // the engine ignores the return value of a draw, and pretending it failed could send it down
    // an error path.
    if (d == Disp::Skip) {
        ++g_skippedDraws;
        g_worldWrittenSinceDraw = false;
        g_shimMsThisFrame += MsSince(tShim);
        return D3D_OK;
    }

    if (d == Disp::PassThrough) NotePassedThrough(primitiveCount);
    // The character atlas composite, re-issued as fixed function so Remix executes it. On
    // success the game's own draw is NOT also issued - that would composite twice.
    if (IsAtlasComposite(primitiveCount, d) && DrawCompositeFixedFunction(dev)) {
        g_shimMsThisFrame += MsSince(tShim);
        g_worldWrittenSinceDraw = false;
        return D3D_OK;
    }
    const bool ui = ffp ? false : BeginUIDemote(dev, d == Disp::Hide);
    const unsigned marked = (d == Disp::Mark) ? BeginMark(dev, g_markClearAllStages) : 0u;
    // Everything up to here is ours; the draw call itself is not, so the timer stops before it
    // and resumes after. Otherwise the measurement would blame us for the bridge's own cost.
    g_shimMsThisFrame += MsSince(tShim);
    const HRESULT hr = g_origDrawIndexedPrimitive(dev, type, baseVertex, minIndex, numVertices,
                                                  startIndex, primitiveCount);
    const LONGLONG tShim2 = Now();
    // AFTER the draw, not before it.
    //
    // This ran at the TOP of the hook, which is before Classify -> SkinAndBind has captured the
    // part. So the FIRST draw of every new vertex buffer found no pending entry and its slot was
    // dropped; a part drawn only once therefore ended up with NO slots at all and was skipped with
    // "no usable slots". Four of ten parts died that way.
    //
    // Here the capture has already happened, and g_stream0 / g_curPS still describe the game draw -
    // EndFFP below is what restores state, and it has not run yet.
    RemixCollectCharacterSlot(dev, type, baseVertex, minIndex, numVertices, startIndex,
                              primitiveCount);
    // LIVE. Every frame the game draws one of our captured parts, take its objTM and bone palette.
    // This is the whole of the animation: the mesh never changes, only these.
    for (unsigned m = 0; m < g_apiDoneCount; ++m) {
        if (g_apiDone[m].vb != g_stream0) continue;
        ApiDoneMesh& dm = g_apiDone[m];
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c)
                dm.objTM[r][c] = g_vsConst[kRegObjTM + r][c];
        const unsigned reg = dm.boneReg;
        unsigned n = dm.boneCount;
        if (n > kMaxApiBones) n = kMaxApiBones;
        if (reg + n * kRegsPerBone <= kMaxVsConst) {
            for (unsigned b = 0; b < n; ++b)
                memcpy(dm.bones[b], &g_vsConst[reg + b * kRegsPerBone][0], 12 * sizeof(float));
            dm.haveBones = (n > 0);
        }
        break;
    }
    if (skinned) UnbindSkinned(dev);
    EndMark(dev, marked);
    EndUIDemote(dev, ui);
    EndFFP(dev, scope);
    g_worldWrittenSinceDraw = false;
    g_shimMsThisFrame += MsSince(tShim2);
    return hr;
}

HRESULT WINAPI Hook_SetRenderTarget(IDirect3DDevice9* dev, DWORD index,
                                    IDirect3DSurface9* target) {
    // GetDesc below is a bridge round trip, so this hook is a candidate for the hitch even
    // though it runs only on a target CHANGE. Timed for that reason.
    const LONGLONG tRT = Now();
    if (!g_internal && index < kMaxRTSlots) {
        // ONE GetDesc, feeding both questions. Two would double the bridge round trips on the
        // hook this file already names as a hitch candidate.
        D3DSURFACE_DESC d{};
        const bool haveDesc = target && SUCCEEDED(target->GetDesc(&d));
        g_rtBound[index] = (target != nullptr);
        g_rtChannels[index] = haveDesc ? FormatColourChannels(d.Format) : 4u;
        if (index == 0) {
            g_curRenderTarget = target;
            g_rt0Width = haveDesc ? d.Width : 0;
            g_rt0Height = haveDesc ? d.Height : 0;
            // Is this the UI pass? The test is the target's own description, not its position
            // in a list: SR3 composites its UI into a back-buffer-sized 8-BIT surface while the
            // scene stays in A16B16G16R16F, so format and size say what the pass is FOR.
            // Selecting passes by render-target index is a recorded dead end because indices are
            // not stable; format and size are stable descriptions.
            g_uiRenderTarget = haveDesc &&
                               (d.Format == D3DFMT_A8R8G8B8 || d.Format == D3DFMT_X8R8G8B8) &&
                               d.Width >= 1000;
        }
    }
    g_otherMsThisFrame += MsSince(tRT);
    return g_origSetRenderTarget(dev, index, target);
}

void ProbeRenderTargetContents(IDirect3DDevice9* dev);

// Set before anything Present-time runs, so the cloth baker can register its result as a real
// texture instead of only a file on disk.
HRESULT WINAPI Hook_Present(IDirect3DDevice9* dev, const RECT* src, const RECT* dst, HWND window,
                            const RGNDATA* dirty) {
    g_presentDevice = dev;
    ++g_frames;
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
        if (ms < 10000.0) {   // ignore alt-tab and loading pauses
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
    // The skinning ring restarts every frame. DISCARD on the first lock of a frame tells the
    // driver the old contents are dead, so it can hand back fresh memory instead of waiting
    // for the GPU to finish reading last frame's vertices.
    g_skinRingPos = 0;
    g_skinRingFresh = true;
    g_skinFrameKeyCount = 0;
    g_shimMsThisFrame = 0.0;
    g_otherMsThisFrame = 0.0;
    g_shadersCreatedThisFrame = 0;
    g_texturesCreatedThisFrame = 0;
    if (g_frames == 60 || g_frames % 600 == 0) {
        const double f = static_cast<double>(g_frames);
        Log("frame %u | draws %.0f/frame | FFP converted %.0f/frame (%.1f%%)",
            g_frames, g_drawsTotal / f, g_ffpConverted / f,
            g_drawsTotal ? 100.0 * g_ffpConverted / g_drawsTotal : 0.0);
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
        Log("        DEDUP: %.1f draws/frame hidden as exact duplicates already converted this "
            "frame (%.0f keys held, %.1f/frame did not fit - non-zero means duplicates were "
            "MISSED) | skinned %s, all %s",
            g_skinRepeatHidden / f, static_cast<double>(g_skinFrameKeyCount),
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
        // The morph is SNOOPED now, not locked: the buffer is DYNAMIC and read-locking it is
        // what the previous attempt got wrong. "not snooped yet" is expected on the first
        // draws of a new character and should stay near zero afterwards; stale vertices mean
        // the game had not written those bytes since its last discard.
        Log("        VB LOCK HOOK: %u invalidations deferred to the render thread, %u "
            "coalesced (the game locks vertex buffers from more than one thread - see the "
            "2026-08-28 crash) | snoop holds %.1f MB of %d MB, %u writes refused on budget, "
            "%u allocations failed",
            g_deferredInvalidations, g_invalidationsCoalesced, g_snoopBytes / 1048576.0,
            static_cast<int>(kMaxSnoopBytes >> 20), g_snoopBudgetRefusals,
            g_snoopAllocFailures);
        Log("        MORPH APPLIED: %.1f draws/frame skinned WITH the delta | %u draws found "
            "no snooped buffer, %u stride refusals, %.0f vertices/frame fell back (not "
            "written yet)",
            g_morphAppliedDraws / f, g_morphNotSnooped, g_morphNoStride,
            g_morphStaleVerts / f);
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
            "%.1f/frame (%u total)",
            g_upDrawsTotal / f, g_upDrawsTotal, g_upIndexedTotal / f, g_upIndexedTotal);
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
                "uploaded, %u capture failures | %.1f draws/frame bound a snooped copy - gate %s",
                g_atlasCaptured, g_atlasCopyBuilt, g_atlasCaptureFailed, g_atlasBound / f,
                g_settings.atlasSnoop ? "ON" : "OFF");
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
            "RT textures known %u | lights %.1f/frame (%u shaders)",
            g_albedoMoved / f, g_albedoBlanked / f, g_albedoRestored / f,
            static_cast<unsigned>(g_meshAlbedo.size()), g_meshAlbedoFlushes,
            static_cast<unsigned>(g_rtTextures.size()), g_lightsEmitted / f, g_lightShadersSeen);
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
            "%u declarations cloned | failures: %u convert, %u declaration",
            g_uvStreamDraws / f, g_uvBuffersMade,
            static_cast<double>(g_uvBytesHeld) / (1024.0 * 1024.0), g_uvBufferFlushes,
            g_uvInvalidations, g_uvDeclsMade, g_uvConvertFailures, g_uvDeclFailures);
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
    g_clearedThisFrame = false;
    g_lastFrameDraws = g_drawIndexThisFrame;
    g_drawIndexThisFrame = 0;
    if (g_frameCameraCount > g_maxFrameCameras) g_maxFrameCameras = g_frameCameraCount;
    g_frameCameraCount = 0;
    if (g_frames % 600 == 0) InstallCrashFilter();
    ShadowInvalidate();
    g_haveAppliedWorld = g_haveAppliedView = g_haveAppliedProj = false;
    g_lightingSetUp = false;
    DisableUnusedLightSlots(dev);

    // Split our own end-of-frame block from Remix's. Both land in the NEXT interval, which is
    // why they are carried in "Last" variables rather than accumulated here.
    g_ourPresentMsLast = MsSince(tPresentEntry);
    const LONGLONG tRealPresent = Now();
    const HRESULT hr = g_origPresent(dev, src, dst, window, dirty);
    g_presentMsLast = MsSince(tRealPresent);
    return hr;
}

// ---------------------------------------------------------------- setup

// ---------------------------------------------------------------- crash reporting
//
// The user reports the game dying perhaps one time in three when moving fast or freefalling from
// the penthouse roof. The log flushes every line, so it ends mid-sentence on a crash and says
// nothing about why. This makes the next one produce evidence instead of a hypothesis.
//
// Two things are recorded: where the fault was (module + offset, and for an access violation the
// address that was touched), and what the shim was doing (frame, draw index, the albedo last
// bound and whether the mesh cache supplied it). If the faulting address is the cached albedo,
// the diagnosis is finished.
//
// dbghelp is loaded at crash time rather than imported, so a missing dbghelp.dll costs the dump
// and not the process.
LPTOP_LEVEL_EXCEPTION_FILTER g_prevFilter = nullptr;

LONG WINAPI CrashFilter(EXCEPTION_POINTERS* info) {
    if (!info || !info->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;
    const EXCEPTION_RECORD& er = *info->ExceptionRecord;

    char module[MAX_PATH] = "?";
    uintptr_t offset = 0;
    HMODULE mod = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(er.ExceptionAddress), &mod) && mod) {
        GetModuleFileNameA(mod, module, MAX_PATH);
        offset = reinterpret_cast<uintptr_t>(er.ExceptionAddress) - reinterpret_cast<uintptr_t>(mod);
    }

    Log("*** CRASH: code 0x%08lX at %p (%s +0x%IX)", er.ExceptionCode,
        er.ExceptionAddress, module, offset);
    if (er.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er.NumberParameters >= 2) {
        Log("    access violation %s address %p",
            er.ExceptionInformation[0] ? "WRITING" : "reading",
            reinterpret_cast<void*>(er.ExceptionInformation[1]));
    }
    Log("    shim state: frame %u draw %u | last albedo %p (from mesh cache: %s) | "
        "%u meshes cached, %u RT textures | marker %p",
        g_frames, g_drawIndexThisFrame, static_cast<void*>(g_lastBoundAlbedo),
        g_lastAlbedoFromCache ? "YES" : "no",
        static_cast<unsigned>(g_meshAlbedo.size()), static_cast<unsigned>(g_rtTextures.size()),
        static_cast<void*>(g_marker));

    if (HMODULE dbghelp = LoadLibraryA("dbghelp.dll")) {
        typedef BOOL(WINAPI * WriteDump_t)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
                                           PMINIDUMP_EXCEPTION_INFORMATION, PVOID, PVOID);
        if (auto write = reinterpret_cast<WriteDump_t>(
                GetProcAddress(dbghelp, "MiniDumpWriteDump"))) {
            char path[MAX_PATH]{};
            GetModuleFileNameA(nullptr, path, MAX_PATH);
            if (char* slash = strrchr(path, '\\')) strcpy_s(slash + 1, 32, "sr3-rtx-crash.dmp");
            const HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                            FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file != INVALID_HANDLE_VALUE) {
                MINIDUMP_EXCEPTION_INFORMATION mei{};
                mei.ThreadId = GetCurrentThreadId();
                mei.ExceptionPointers = info;
                mei.ClientPointers = FALSE;
                const BOOL ok = write(GetCurrentProcess(), GetCurrentProcessId(), file,
                                      MiniDumpWithIndirectlyReferencedMemory, &mei, nullptr,
                                      nullptr);
                CloseHandle(file);
                Log("    minidump %s: %s", ok ? "written" : "FAILED", path);
            }
        }
    }
    return g_prevFilter ? g_prevFilter(info) : EXCEPTION_CONTINUE_SEARCH;
}

// Re-installed periodically: whoever calls SetUnhandledExceptionFilter last wins, and the game
// and the Remix bridge both install their own well after we start. Chaining to the previous
// filter means nobody's handling is lost.
void InstallCrashFilter() {
    LPTOP_LEVEL_EXCEPTION_FILTER prev = SetUnhandledExceptionFilter(CrashFilter);
    if (prev != CrashFilter) g_prevFilter = prev;
}

// ---------------------------------------------------------------- occlusion queries
//
// SR3 does its own GPU occlusion culling: it draws bounding volumes, wraps them in a
// D3DQUERYTYPE_OCCLUSION query, reads the visible-pixel count back, and skips submitting an
// object whose count came back zero. This is the readback the mode-2 comment above refers to -
// "a draw whose RESULT the engine reads can never be skipped, only hidden".
//
// `rtx.useVertexCapture = False` is a global skip of every shader draw, the depth prepass
// included, so the depth buffer those queries test against is never written. Measured
// 2026-08-23 from two frame dumps taken in the same area (camera y~147 both times):
//
//     capture ON   5330 draws/frame   (MARK 3930, CONVERT 1260, PASS 140)
//     capture OFF  1426 draws/frame   (MARK 1182, CONVERT  220, PASS  24)
//
// A 73% collapse in the game's OWN submission, matching the 77% collapse recorded for
// hiddenPassMode=2. That is the whole "objects enter the frustum and are dropped" symptom: the
// game asks whether an object is visible, is told no, and stops drawing it. Objects clipped by
// a frustum plane survive because an occlusion test on a clipped bounding box is unreliable and
// engines skip the query for those - which is exactly why only the periphery rendered.
//
// The fix does not need anything from Remix. The queries are D3D9 objects and we are the D3D9
// layer: answer them "fully visible" and the engine stops culling itself, whatever the depth
// buffer contains.
//
// ONLY D3DQUERYTYPE_OCCLUSION is touched. D3DQUERYTYPE_EVENT is a fence the game uses for frame
// pacing, and lying about one of those would break synchronisation rather than culling.
typedef HRESULT(WINAPI* CreateQuery_t)(IDirect3DDevice9*, D3DQUERYTYPE, IDirect3DQuery9**);
typedef HRESULT(WINAPI* QueryGetData_t)(IDirect3DQuery9*, void*, DWORD, DWORD);
CreateQuery_t g_origCreateQuery = nullptr;
QueryGetData_t g_origQueryGetData = nullptr;

// A count large enough that no visibility threshold can read it as "occluded". Not 0xFFFFFFFF:
// an engine that scales a coverage fraction by the count would overflow on that.
constexpr DWORD kVisiblePixels = 1u << 20;

HRESULT WINAPI Hook_QueryGetData(IDirect3DQuery9* self, void* data, DWORD size, DWORD flags) {
    if (self) {
        // GetType is a plain accessor - no GPU work, no bridge round trip beyond the call
        // itself - so the type is asked per call rather than cached against a pointer that
        // could be recycled by a later query of a different type. Asked even when the override
        // is off, because "how many occlusion queries does SR3 read back per frame" is the
        // baseline that says whether this mechanism is worth anything at all.
        if (self->GetType() == D3DQUERYTYPE_OCCLUSION) {
            ++g_occlusionQueriesSeen;
            if (!g_settings.forceOcclusionVisible)
                return g_origQueryGetData(self, data, size, flags);
            ++g_occlusionForced;
            // The real query is deliberately NOT drained. Calling through with the game's flags
            // can carry D3DGETDATA_FLUSH, which stalls on the GPU across the 32-to-64-bit
            // bridge - the same mistake that made the instance-buffer read lock present as a
            // frame-rate collapse. Fabricating the answer costs nothing and never blocks.
            if (data && size >= sizeof(DWORD)) *static_cast<DWORD*>(data) = kVisiblePixels;
            return D3D_OK;   // always "ready", so the engine never spins waiting
        }
    }
    return g_origQueryGetData(self, data, size, flags);
}

HRESULT WINAPI Hook_CreateQuery(IDirect3DDevice9* dev, D3DQUERYTYPE type,
                                IDirect3DQuery9** returned) {
    const HRESULT hr = g_origCreateQuery(dev, type, returned);
    // D3D9 allows a null out-pointer as a "is this type supported" probe, which creates nothing.
    if (SUCCEEDED(hr) && returned && *returned) {
        ++g_queriesCreated;
        // Every IDirect3DQuery9 shares one vtable, so patching from the first one we are handed
        // covers all of them - the same pattern the vertex-buffer Lock hook uses.
        if (!g_origQueryGetData) {
            PatchVTable(*returned, kSlotQueryGetData, &Hook_QueryGetData,
                        reinterpret_cast<void**>(&g_origQueryGetData));
            Log("query vtable patched from the first %s query (GetData slot %d), "
                "forceOcclusionVisible=%d",
                type == D3DQUERYTYPE_OCCLUSION ? "OCCLUSION" : "non-occlusion",
                kSlotQueryGetData, g_settings.forceOcclusionVisible ? 1 : 0);
        }
    }
    return hr;
}

void HookDevice(void* device, const char* origin, const D3DPRESENT_PARAMETERS* params) {
    static bool hooked = false;
    if (hooked || !device) return;
    hooked = true;
    g_createTick = GetTickCount();
    QueryPerformanceFrequency(&g_qpcFreq);
    SeedRenderStateDefaults();
    if (params && params->BackBufferHeight) {
        g_backAspect = static_cast<float>(params->BackBufferWidth) /
                       static_cast<float>(params->BackBufferHeight);
        g_backBufferW = params->BackBufferWidth;
        g_backBufferH = params->BackBufferHeight;
    }

    Log("%s -> hooking device (backbuffer %ux%u, aspect %.3f)", origin,
        params ? params->BackBufferWidth : 0, params ? params->BackBufferHeight : 0,
        g_backAspect);
    RemixApiInit(static_cast<IDirect3DDevice9*>(device));

    struct { int slot; void* hook; void** orig; } kHooks[] = {
        {kSlotPresent,                  &Hook_Present,                  reinterpret_cast<void**>(&g_origPresent)},
        {kSlotCreateTexture,            &Hook_CreateTexture,            reinterpret_cast<void**>(&g_origCreateTexture)},
        {kSlotSetRenderTarget,          &Hook_SetRenderTarget,          reinterpret_cast<void**>(&g_origSetRenderTarget)},
        // Candidate entry points for filling the character texture - see the fill probe.
        {kSlotUpdateSurface,            &Hook_UpdateSurface,            reinterpret_cast<void**>(&g_origUpdateSurface)},
        {kSlotUpdateTexture,            &Hook_UpdateTexture,            reinterpret_cast<void**>(&g_origUpdateTexture)},
        {kSlotStretchRect,              &Hook_StretchRect,              reinterpret_cast<void**>(&g_origStretchRect)},
        {kSlotColorFill,                &Hook_ColorFill,                reinterpret_cast<void**>(&g_origColorFill)},
        {kSlotSetTransform,             &Hook_SetTransform,             reinterpret_cast<void**>(&g_origSetTransform)},
        {kSlotSetRenderState,           &Hook_SetRenderState,           reinterpret_cast<void**>(&g_origSetRenderState)},
        {kSlotSetTexture,               &Hook_SetTexture,               reinterpret_cast<void**>(&g_origSetTexture)},
        {kSlotSetTextureStageState,     &Hook_SetTextureStageState,     reinterpret_cast<void**>(&g_origSetTextureStageState)},
        {kSlotSetSamplerState,          &Hook_SetSamplerState,          reinterpret_cast<void**>(&g_origSetSamplerState)},
        {kSlotDrawPrimitive,            &Hook_DrawPrimitive,            reinterpret_cast<void**>(&g_origDrawPrimitive)},
        {kSlotDrawIndexedPrimitive,     &Hook_DrawIndexedPrimitive,     reinterpret_cast<void**>(&g_origDrawIndexedPrimitive)},
        {kSlotDrawPrimitiveUP,          &Hook_DrawPrimitiveUP,          reinterpret_cast<void**>(&g_origDrawPrimitiveUP)},
        {kSlotDrawIndexedPrimitiveUP,   &Hook_DrawIndexedPrimitiveUP,   reinterpret_cast<void**>(&g_origDrawIndexedPrimitiveUP)},
        {kSlotSetVertexDeclaration,     &Hook_SetVertexDeclaration,     reinterpret_cast<void**>(&g_origSetVertexDeclaration)},
        {kSlotSetStreamSource,          &Hook_SetStreamSource,          reinterpret_cast<void**>(&g_origSetStreamSource)},
        {kSlotSetStreamSourceFreq,      &Hook_SetStreamSourceFreq,      reinterpret_cast<void**>(&g_origSetStreamSourceFreq)},
        {kSlotCreateVertexShader,       &Hook_CreateVertexShader,       reinterpret_cast<void**>(&g_origCreateVertexShader)},
        {kSlotSetVertexShader,          &Hook_SetVertexShader,          reinterpret_cast<void**>(&g_origSetVertexShader)},
        {kSlotSetVertexShaderConstantF, &Hook_SetVertexShaderConstantF, reinterpret_cast<void**>(&g_origSetVSConstF)},
        {kSlotCreatePixelShader,        &Hook_CreatePixelShader,        reinterpret_cast<void**>(&g_origCreatePixelShader)},
        {kSlotSetPixelShader,           &Hook_SetPixelShader,           reinterpret_cast<void**>(&g_origSetPixelShader)},
        {kSlotSetPixelShaderConstantF,  &Hook_SetPixelShaderConstantF,  reinterpret_cast<void**>(&g_origSetPSConstF)},
        {kSlotCreateQuery,              &Hook_CreateQuery,              reinterpret_cast<void**>(&g_origCreateQuery)},
    };
    for (const auto& h : kHooks) PatchVTable(device, h.slot, h.hook, h.orig);

    // After the vtable is patched, so the creation goes through the same path everything else
    // does and Hook_CreateTexture sees it (it only records render targets, which this is not).
    CreateMarkerTexture(static_cast<IDirect3DDevice9*>(device));
    {
        IDirect3DDevice9* d = static_cast<IDirect3DDevice9*>(device);
        // How many lights may be lit at once. Asked rather than assumed, because assuming is what
        // broke the flashlight: lights past this limit are silently stored and never enabled.
        // D3D9 defines MaxActiveLights == 0 as "no limit".
        D3DCAPS9 caps{};
        if (SUCCEEDED(d->GetDeviceCaps(&caps))) {
            g_maxLightSlots = (caps.MaxActiveLights == 0) ? 64u
                                                          : min(64u, caps.MaxActiveLights);
            Log("device reports MaxActiveLights = %lu, using %u light slots",
                static_cast<unsigned long>(caps.MaxActiveLights), g_maxLightSlots);
            // The converted texture coordinates need a stream the game is not using. SR3 binds
            // 0-3; the highest the device allows is furthest from anything it might add.
            if (caps.MaxStreams > 1)
                g_uvStream = min(kUvStreamDefault, static_cast<DWORD>(caps.MaxStreams - 1));
            Log("device reports MaxStreams = %lu, converted texcoords go on stream %lu",
                static_cast<unsigned long>(caps.MaxStreams),
                static_cast<unsigned long>(g_uvStream));
            // Fixed-function vertex blending is the alternative to CPU skinning: put the c52
            // palette in D3DTS_WORLDMATRIX(0..63), enable indexed blending, and let the draw
            // keep its own BLENDINDICES/BLENDWEIGHT. That hands Remix real bones, which is what
            // dispatchSkinning needs before a skinned USD REPLACEMENT can attach to a character.
            // SR3 needs 64 matrices and 4 influences per vertex. Asked rather than assumed -
            // D3D9 permits a device to report 0 blend matrices and no indexed blending at all.
            Log("device reports MaxVertexBlendMatrices = %lu, MaxVertexBlendMatrixIndex = %lu, "
                "VertexProcessingCaps = 0x%08lX (SR3 needs 64 matrices, 4 influences)",
                static_cast<unsigned long>(caps.MaxVertexBlendMatrices),
                static_cast<unsigned long>(caps.MaxVertexBlendMatrixIndex),
                static_cast<unsigned long>(caps.VertexProcessingCaps));
        }
        if (SUCCEEDED(d->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &g_backBufferSurface)))
            Log("back buffer surface %p (cleared once a frame; the composite no longer writes it)",
                static_cast<void*>(g_backBufferSurface));   // reference deliberately kept
        else
            Log("GetBackBuffer failed - the back-buffer clear is disabled");
    }
    InstallCrashFilter();

    // Captured but not intercepted: called by us, never filtered.
    PatchVTable(device, kSlotGetRenderState, nullptr, reinterpret_cast<void**>(&g_origGetRenderState));
    PatchVTable(device, kSlotSetMaterial, nullptr, reinterpret_cast<void**>(&g_origSetMaterial));
    PatchVTable(device, kSlotSetLight, nullptr, reinterpret_cast<void**>(&g_origSetLight));
    PatchVTable(device, kSlotLightEnable, nullptr, reinterpret_cast<void**>(&g_origLightEnable));
    PatchVTable(device, kSlotSetIndices, nullptr, reinterpret_cast<void**>(&g_origSetIndices));
}

HRESULT WINAPI Hook_CreateDevice(IDirect3D9* self, UINT adapter, D3DDEVTYPE type, HWND focus,
                                 DWORD flags, D3DPRESENT_PARAMETERS* params,
                                 IDirect3DDevice9** returned) {
    const HRESULT hr = g_origCreateDevice(self, adapter, type, focus, flags, params, returned);
    if (SUCCEEDED(hr) && returned && *returned) HookDevice(*returned, "CreateDevice", params);
    else Log("CreateDevice FAILED hr=0x%08lX", hr);
    return hr;
}

HRESULT WINAPI Hook_CreateDeviceEx(IDirect3D9Ex* self, UINT adapter, D3DDEVTYPE type, HWND focus,
                                   DWORD flags, D3DPRESENT_PARAMETERS* params,
                                   D3DDISPLAYMODEEX* mode, IDirect3DDevice9Ex** returned) {
    const HRESULT hr = g_origCreateDeviceEx(self, adapter, type, focus, flags, params, mode,
                                            returned);
    if (SUCCEEDED(hr) && returned && *returned) HookDevice(*returned, "CreateDeviceEx", params);
    else Log("CreateDeviceEx FAILED hr=0x%08lX", hr);
    return hr;
}

DWORD WINAPI Init(LPVOID) {
    g_log = _fsopen("sr3-rtx.log", "w", _SH_DENYNO);   // shared so it can be tailed live
    LoadSettings();
    Log("sr3-rtx loaded (pid %lu) - FFP conversion, ported from sr2-rtx-remix-proxy",
        GetCurrentProcessId());
    Log("settings: ffp=%d convertSkinned=%d hiddenPassMode=%d skipUntextured=%d "
        "screenSpaceMode=%d hideLightVolumes=%d skipMirrored=%d dumpFrame=%d",
        g_settings.ffp, g_settings.convertSkinned, g_settings.hiddenPassMode,
        g_settings.skipUntextured, g_settings.screenSpaceMode, g_settings.hideLightVolumes,
        g_settings.skipMirrored, g_settings.dumpFrame);
    Log("          rankAlbedo=%d excludeRTAlbedo=%d cacheMeshAlbedo=%d injectLights=%d",
        g_settings.rankAlbedo, g_settings.excludeRTAlbedo, g_settings.cacheMeshAlbedo,
        g_settings.injectLights);

    // d3d9.dll here is the Remix bridge client. Wait for it if the game has not loaded it yet.
    HMODULE d3d9 = nullptr;
    for (int attempt = 0; attempt < 200 && !d3d9; ++attempt) {
        d3d9 = GetModuleHandleA("d3d9.dll");
        if (!d3d9) Sleep(50);
    }
    if (!d3d9) { Log("d3d9.dll never appeared - giving up"); return 0; }

    // Our probe object shares its vtable with whatever the game creates, so patching here
    // installs the hook for the game's instance too. Nothing is released: dropping the last
    // reference before the game creates its own could tear down the Remix bridge underneath us.
    auto create = reinterpret_cast<IDirect3D9*(WINAPI*)(UINT)>(
        GetProcAddress(d3d9, "Direct3DCreate9"));
    if (create) {
        if (IDirect3D9* probe = create(D3D_SDK_VERSION)) {
            const bool ok = PatchVTable(probe, kSlotCreateDevice, &Hook_CreateDevice,
                                        reinterpret_cast<void**>(&g_origCreateDevice));
            Log(ok ? "CreateDevice hook installed" : "failed to patch IDirect3D9 vtable");
        } else {
            Log("Direct3DCreate9 returned null");
        }
    }

    auto createEx = reinterpret_cast<HRESULT(WINAPI*)(UINT, IDirect3D9Ex**)>(
        GetProcAddress(d3d9, "Direct3DCreate9Ex"));
    if (createEx) {
        IDirect3D9Ex* probeEx = nullptr;
        if (SUCCEEDED(createEx(D3D_SDK_VERSION, &probeEx)) && probeEx) {
            const bool ok = PatchVTable(probeEx, kSlotCreateDeviceEx, &Hook_CreateDeviceEx,
                                        reinterpret_cast<void**>(&g_origCreateDeviceEx));
            Log(ok ? "CreateDeviceEx hook installed" : "failed to patch IDirect3D9Ex vtable");
        }
    }
    return 0;
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        InitializeCriticalSection(&g_snoopCs);
        g_snoopCsReady = true;
        CreateThread(nullptr, 0, Init, nullptr, 0, nullptr);
    }
    return TRUE;
}

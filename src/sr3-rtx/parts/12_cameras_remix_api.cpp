// parts/12_cameras_remix_api.cpp - main-camera detection and Remix API initialisation.
// NOT a standalone translation unit: #included, in order, by ../sr3rtx.cpp inside its anonymous
// namespace. Originally sr3rtx.cpp lines 8267-8750 (old file:line citations map here).
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
    // All three consumers gate together here, BEFORE the derivation below (a 4x4 Invert plus a
    // Multiply inside DeriveProjection) runs for every one of ~3,000 draws/frame. remixApiCamera
    // reads the result to call SetupCamera further down; remixApiTestCube and remixApiCharacter
    // do not touch this function directly, but RemixApiTestTick (their own tick, once/frame from
    // Present) reads g_remixLastView/g_remixLastViewValid, which this function records
    // unconditionally below regardless of remixApiCamera. With all three off there is no
    // consumer left for this work at all - added 2026-09-17.
    if (!g_settings.remixApiCamera && !g_settings.remixApiTestCube && !g_settings.remixApiCharacter)
        return;
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


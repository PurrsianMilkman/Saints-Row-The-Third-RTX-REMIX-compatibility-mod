// parts/13_remix_api_ticks.cpp - Remix API camera/test ticks and camera-only mode.
// NOT a standalone translation unit: #included, in order, by ../sr3rtx.cpp inside its anonymous
// namespace. Originally sr3rtx.cpp lines 8751-9244 (old file:line citations map here).
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
    g_internalThread = GetCurrentThreadId();
    // WORLD is identity: with vertex capture Remix reconstructs already-transformed vertices, so
    // the object matrix is baked in before it ever sees them.
    g_origSetTransform(dev, D3DTS_WORLD, &kIdentity);
    g_origSetTransform(dev, D3DTS_VIEW, &view);
    g_origSetTransform(dev, D3DTS_PROJECTION, &proj);
    g_curDeviceProj = proj;   // see g_curDeviceProj's own comment - kept exact everywhere
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


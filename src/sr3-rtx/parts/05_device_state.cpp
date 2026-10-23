// parts/05_device_state.cpp - tracked device state, rasterized-frame invariant, state shadow, vtable patching, transforms.
// NOT a standalone translation unit: #included, in order, by ../sr3rtx.cpp inside its anonymous
// namespace. Originally sr3rtx.cpp lines 2019-3057 (old file:line citations map here).
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
    // CHANGE 3, 2026-09-18: WHICH STREAM the usage-index-0 TEXCOORD actually lives on, read
    // the same way texcoord1Stream already is (below) - BEFORE the stream-0 filter this loop
    // applies to texcoordType/texcoordOffset. Without this there is no way to tell "this
    // shader declares no TEXCOORD0 at all" apart from "it declares one, but ParseDeclaration
    // skipped it because it sits on a stream other than 0": both left texcoordType at -1.
    // The frame dump's tc0type/tc0stream fields exist to make that declaration-side blind
    // spot visible: tc0type=-1 together with a non-zero tc0stream names it outright.
    int texcoordStream = -1;
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

// The chosen albedo's format and colour-channel count, cached by POINTER exactly the way
// g_rtCopies and g_vbDescCache above are: GetLevelDesc is a bridge round trip, so it must be
// paid once per distinct texture and never once per draw. Read by SetupTextureStages, which
// uses the channel count to decide whether stage 0 may take its COLOUR from this texture at
// all (see albedoRejectColourlessTexture), and by RecordFrameDraw, which prints it so a frame
// dump shows how many bound albedos are colourless across the WHOLE frame rather than only the
// eight decal-probe slots. A texture's format cannot change over its own lifetime, so unlike a
// render target's contents this needs no invalidation - only the same pointer-reuse caution
// g_rtCopies already accepts for its own non-render-target entries: a freed texture's address
// handed to an unrelated new texture would read this cache's stale answer, which is wrong but
// not unsafe, since the worst it does is pick the wrong side of a channel-count comparison and
// nothing here ever dereferences the pointer itself.
struct AlbedoFormatInfo {
    D3DFORMAT fmt = D3DFMT_UNKNOWN;
    unsigned channels = 4;   // unknown defaults to 4: this test may only ever REMOVE a draw's
                             // texture colour, so anything it cannot identify must be left alone
};
std::unordered_map<IDirect3DBaseTexture9*, AlbedoFormatInfo> g_albedoFormatCache;

const AlbedoFormatInfo& AlbedoFormatFor(IDirect3DBaseTexture9* tex) {
    static const AlbedoFormatInfo kUnknown{};
    if (!tex) return kUnknown;
    const auto it = g_albedoFormatCache.find(tex);
    if (it != g_albedoFormatCache.end()) return it->second;
    AlbedoFormatInfo info;
    if (tex->GetType() == D3DRTYPE_TEXTURE) {
        D3DSURFACE_DESC d{};
        if (SUCCEEDED(static_cast<IDirect3DTexture9*>(tex)->GetLevelDesc(0, &d))) {
            info.fmt = d.Format;
            info.channels = FormatColourChannels(d.Format);
        }
    }
    return g_albedoFormatCache.emplace(tex, info).first->second;
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
    // THE SHAPE BAKE: per-buffer write/discard generation counters, and whether this buffer is
    // known to be a MORPH stream (set by MorphForThisDraw, never by RegisterSnoop itself - see
    // MorphForThisDraw's own comment). writeSeq increments on every real write this shim actually
    // captures (Hook_VBUnlock's copy branch) AND on every DISCARD (Hook_VBLock), so a bake's
    // cached snoopWriteSeq can tell "this buffer was touched since I baked" without re-hashing.
    // discardSeq increments only on DISCARD, for the probe's "changed after a repack" split.
    unsigned writeSeq = 0;
    unsigned discardSeq = 0;
    bool isMorph = false;
};
std::unordered_map<IDirect3DVertexBuffer9*, InstanceCache> g_instCache;
unsigned g_instanceConverted = 0, g_instanceLocks = 0, g_instCacheInvalidations = 0;
unsigned g_instFreshTransform = 0;   // instanced draws whose transform bytes were freshly written
unsigned g_instStaleTransform = 0;   // ...and those reading bytes left over from a previous fill
unsigned g_instDiscards = 0;         // whole-buffer discards seen on an instance stream
unsigned g_instStaleReports = 0;
// THE SHAPE BAKE: the morph VB's own write traffic - declared here (rather than beside the rest
// of the bake machinery, much further down) because Hook_VBLock/Hook_VBUnlock, which bump these,
// are defined before that point in the file. Not gated on morphProbe: MorphBakeFor's validation
// needs writeSeq (InstanceCache::writeSeq, beside it) whether or not the probe is on.
unsigned g_morphVbWrites = 0, g_morphVbDiscards = 0;
unsigned long long g_morphVbBytes = 0;

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
// The rest of the indexed draw's parameters. [firstVertex, +vertexCount) is a RANGE HINT -
// D3D only requires that every referenced vertex fall inside it, not that every vertex inside
// it be referenced. A probe that walks the range walks whatever else shares the buffer. These
// let a probe walk the INDEX BUFFER instead and see only what the draw actually uses.
bool g_curDrawIndexed = false;
INT g_curDrawBaseVertex = 0;
UINT g_curDrawMinIndex = 0, g_curDrawStartIndex = 0, g_curDrawPrimCount = 0;
D3DPRIMITIVETYPE g_curDrawPrimType = D3DPT_TRIANGLELIST;
float g_appliedU = 0.0f, g_appliedV = 0.0f;   // cached texture-matrix scale
unsigned g_uvMatrixWrites = 0;
// 2026-09-18 audit fix counter: how often SetupTextureStages' corrected uvSrcType (the set FIX A
// actually used) disagrees with the old texcoordType-only test - see SetupTextureStages.
unsigned long long g_decalUvScaleFixChanged = 0;
// Published by SetupTextureStages, one draw at a time, for ProbeDecalDraw (defined far
// below, after TextureToBgra): the su/sv THIS draw's texture matrix was set to, and whether
// D3DTSS_TEXTURETRANSFORMFLAGS ended up COUNT2 or DISABLE. The probe exists because the
// applied scale and the bound texture have never actually been measured on the building
// decals that render as a stripe pattern instead of their image.
float g_decalProbeSu = 1.0f, g_decalProbeSv = 1.0f;
bool g_decalProbeCount2 = false;   // true: D3DTTFF_COUNT2 was set; false: D3DTTFF_DISABLE
unsigned g_samplerProbeReports = 0;
// A second, separate cap: the population above (INHERITED sampler0 before conversion) armed
// on whichever draw happened to convert first, and measured 2026-09-18 that was six reports
// of 'Diffuse_MapSampler' - ordinary wall materials - and never once a decal, which is the
// population whose measured UVs run from -0.95 to +1.24 (see ProbeDecalDraw) and would
// actually be visible under CLAMP versus WRAP. Gated on the CHOSEN albedo sampler naming a
// Decal map so the next run answers the question for the population that matters.
unsigned g_samplerProbeDecalReports = 0;
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
// Counts every ACTUAL round trip (win or lose), unlike g_shadowCheckReports below, which only
// ever incremented in the mismatch branch - see the 2026-09-18 audit comment at the probe site.
unsigned g_shadowCheckRuns = 0;
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

// injectProbe/injectControl (see EvaluateInjectTrigger, below the marker texture section):
// whatever D3DTS_PROJECTION ACTUALLY holds on the device right now. The game never calls
// SetTransform at all (see Hook_SetTransform's own comment), so every write to this state
// comes from this shim, at exactly four sites - ApplyTransforms, ApplyCameraOnly,
// BeginUIDemote, EndUIDemote - each of which keeps this shadow exact with no extra bridge
// call. Starts identity: D3D9's own default before this shim ever calls SetTransform.
D3DMATRIX g_curDeviceProj = kIdentity;

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
unsigned g_tfactorReset = 0;   // resetTFactorOnTexturedDraws: normal textured draws given opaque white
// FIX B: draws routed to alpha-from-texture in SetupTextureStages because the chosen albedo
// sampler names a Decal map AND the draw is genuinely blended by its FACTORS - not merely
// alpha-test cutouts (shaderCutout already counts those separately). See decalAlphaFromTexture.
unsigned g_decalAlphaFromTexture = 0;
// What EffectiveAlbedo() actually returned for the current draw, published so downstream code
// (the churn probe below, the frame dump, the rolling record) can see the texture that was
// CHOSEN rather than re-deriving it from g_curTexture[0], which is not necessarily the same
// thing - that is the whole point of AlbedoRank existing. Stage -1 means nothing was chosen
// (the null/blanked returns).
IDirect3DBaseTexture9* g_lastEffectiveAlbedo = nullptr;
int g_lastEffectiveAlbedoStage = -1;
// albedoChurnProbe: converted draws whose EffectiveAlbedo() pick differs from what the SAME
// draw (by vertex window, index range and placement, never by texture) picked last frame. See
// the churn-detection block in BeginFFP, right after EffectiveAlbedo() is called.
unsigned g_albedoChurnDraws = 0;
unsigned g_albedoChurnReports = 0;   // ALBEDO CHURN log lines written so far, capped at 10
// Per-draw breakdown of which texcoord set the CHOSEN albedo sampler reads (g_curPS.samplerUv at
// g_lastEffectiveAlbedoStage), counted only when a stage was actually chosen (0..7). The three
// buckets are mutually exclusive and cover every counted draw: samplerUv==0 is "set 0" (what the
// shim currently forces via D3DTSS_TEXCOORDINDEX), samplerUv>0 is "non-zero set" (texturing would
// be wrong), samplerUv==-1 is "unknown/computed in the shader" (traced, not a plain input read -
// neither provably right nor provably wrong). Filled in the albedoChurnProbe block in BeginFFP.
unsigned g_albedoUvSet0Draws = 0;
unsigned g_albedoUvNonZeroDraws = 0;
unsigned g_albedoUvUnknownDraws = 0;
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
// Diagnostic for the skinFFHonourStreamOffset/uvHonourStreamOffset fixes: the denominator over
// ALL converted draws, not only the skinned or SHORT2-uv populations those two switches touch.
unsigned g_convertedNonZeroStreamOffset = 0;
unsigned g_gbufferHidden = 0;
// CHANGE 1, 2026-09-18: VFX mask materials (ir_vfxmask / ir_vfxmasktod) whose opacity lives
// entirely in Alpha_MaskSampler - a sampler AlbedoRank scores 0 and SetupTextureStages never
// binds - caught by Classify's rule just before its final return. The distinct-shader set is
// what would show an over-broad match: shader_constants.csv names only four files in this
// family, so more than a handful of distinct shaders here means the test is catching
// something else and needs to be narrowed.
unsigned g_vfxMaskHidden = 0;
std::unordered_set<IDirect3DPixelShader9*> g_vfxMaskShaders;
char g_mrtSamplerNames[10][28] = {};
unsigned g_morphNoStride = 0, g_morphNoDesc = 0, g_morphDynamic = 0,
         g_morphOutOfRange = 0, g_morphLockFail = 0, g_morphWhyReports = 0;
unsigned g_skinRepeatHidden = 0;     // ...and dropped because of it
unsigned g_tintedAlbedo = 0;         // fallback maps modulated by the shader's tint constant
unsigned g_blankAlbedo = 0;          // rank 0 AND no colour constant: the only truly white draws
// albedoRejectColourlessTexture: converted draws whose CHOSEN albedo has fewer than 3 colour
// channels (D3DFMT_A8 and the like - measured on the decal probe's fmt=28 report, 2026-09-18).
// SetupTextureStages used to select this texture's colour anyway, which for an alpha-only
// format is always RGB=0 in fixed function - a mask rendered as solid black. Counted here so
// the size of that population is known; the distinct-texture set is what tells whether this is
// one shared mask atlas or many unrelated ones.
unsigned g_albedoColourlessRejected = 0;
std::unordered_set<IDirect3DBaseTexture9*> g_albedoColourlessTextures;
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

// ---------------------------------------------------------------- the rasterized-frame invariant
//
// THE USER'S STATED TOP PRIORITY, verbatim: "at some camera angles, the rasterized render of the
// game replaces the scene. we have had this issue before. we must find a permanent solution for
// this no matter what." This project has fixed that symptom before, each time on ONE path, and it
// has come back through another. CompositesEngineOutputToScreen is the fix as an INVARIANT
// instead of one more per-path rule: called from Classify (the primary gate, ahead of every rule
// that can Convert, Hide or Mark a draw) and from the two USER-POINTER draw hooks, which bypass
// Classify entirely and so need their own call.
//
// The gap that made this necessary: the HUD test in Hook_DrawPrimitiveUP is "no projection matrix
// and depth test off" and NEVER asks what the quad SAMPLES. A real HUD icon samples an authored
// texture; a full-screen post effect (sun glare, a lens flare, bloom, anything that reads the
// game's own rendered frame) meets the identical test and rides the SAME rebuild that keeps the
// HUD alive with vertex capture off - straight to Remix, rasterised as a UI overlay ON TOP of the
// path-traced world (rtx.orthographicIsUI). "At some camera angles" is exactly what a
// camera-angle-dependent effect like glare looks like from the player's chair.
//
// THE INVARIANT: a draw that SAMPLES a render target on ANY bound stage (0-7, not only stage 0 -
// a post effect can read the scene from a secondary stage) AND WRITES a screen-sized target
// (render target 0 exactly the back buffer's own size) is compositing the engine's own rendered
// output onto the screen, whichever path it arrived by.
//
// Both halves matter, and it is their CONJUNCTION that keeps this from also catching the
// character atlas composite: the atlas legitimately samples render targets (two DXT5 layers, see
// DrawCompositeFixedFunction above) but writes an off-screen 2048x1024/1024x512 texture, never a
// target the size of the screen. Only the rasterized frame painted onto the screen matches both
// halves at once.
unsigned g_rtInvariantIndexedPass = 0, g_rtInvariantIndexedSkip = 0;      // Classify, DrawIndexedPrimitive
unsigned g_rtInvariantDrawPass = 0, g_rtInvariantDrawSkip = 0;            // Classify, DrawPrimitive (non-indexed)
unsigned g_rtInvariantUPPass = 0, g_rtInvariantUPSkip = 0;                // Hook_DrawPrimitiveUP - the confirmed gap
unsigned g_rtInvariantIndexedUPPass = 0, g_rtInvariantIndexedUPSkip = 0;  // Hook_DrawIndexedPrimitiveUP - unclassified before this
unsigned g_rtInvariantFallbackHits = 0;   // caught live, off the texture's own usage flags, not g_rtTextures
unsigned g_rtInvariantProbeReports = 0;   // the capped report below; re-armed by the capture key

bool CompositesEngineOutputToScreen(IDirect3DDevice9* dev, int* rtStage, UINT* rtW, UINT* rtH) {
    (void)dev;
    if (rtStage) *rtStage = -1;
    if (rtW) *rtW = 0;
    if (rtH) *rtH = 0;
    if (!g_settings.blockEngineOutputToScreen) return false;

    // HALF ZERO, added 2026-09-21 before this ever shipped: SCREEN-SPACE draws only. A vertex
    // shader that transforms by projTM is WORLD GEOMETRY, and SR3's world materials legitimately
    // sample render targets while writing the screen-sized main target: ir_sr3fauxinterior
    // declares Reflection_MapSampler at s4, car paint samples a reflection map that is very
    // likely a dynamically rendered cube target, and the deferred material pass writes the
    // screen-sized main RT on every draw. Without this gate the check runs in Classify BEFORE
    // conversion, so a world draw that is converted and visible today would be passed through
    // instead and would simply vanish. The rasterised frame is painted by a SCREEN-SPACE quad,
    // and this is the file's own definition of one: the screen-space branch in Classify opens
    // with `if (!g_curVS.usesProjTM)`, a shader that emits clip-space positions itself. A draw
    // with NO vertex shader (pretransformed, already fixed function) reads usesProjTM false and
    // is still checked, which is right: those are the ones Remix accepts whatever the capture
    // setting says, so they are the most dangerous of all.
    if (g_curVS.usesProjTM) return false;

    // HALF TWO, tested first because it is free: render target 0's own size against the back
    // buffer's, both already tracked (Hook_SetRenderTarget, CreateDevice/Reset) with no bridge
    // call here. Not the screen means not this bug, whatever any stage samples - which is what
    // keeps the character atlas composite working: it writes an off-screen 2048x1024/1024x512
    // texture and never reaches half one below.
    if (!g_backBufferW || !g_backBufferH || !g_rt0Width || !g_rt0Height) return false;
    if (g_rt0Width != g_backBufferW || g_rt0Height != g_backBufferH) return false;

    // HALF ONE: does any stage this draw can read hold a render target? Every bound stage, 0
    // through 7 - the confirmed gap this closes (Hook_DrawPrimitiveUP's HUD test) checked NO
    // stage at all, and a post effect can sample the scene on any of them, not only stage 0. When
    // the pixel shader is reflected and names at least one sampler, only the stages it actually
    // declares are checked, so an unrelated leftover texture on another stage cannot trip this by
    // accident; with no reflection (fixed-function pixel pipeline, or a shader this shim never
    // saw) every bound stage is checked, because there is nothing narrower to trust.
    const bool haveReflection = g_curPS.isPixelShader && g_curPS.anySampler;
    for (int st = 0; st < 8; ++st) {
        IDirect3DBaseTexture9* const t = g_curTexture[st];
        if (!t) continue;
        if (haveReflection && !g_curPS.samplerName[st][0]) continue;
        if (g_rtTextures.count(t)) {
            if (rtStage) *rtStage = st;
            if (t->GetType() == D3DRTYPE_TEXTURE) {
                D3DSURFACE_DESC sd{};
                if (SUCCEEDED(static_cast<IDirect3DTexture9*>(t)->GetLevelDesc(0, &sd))) {
                    if (rtW) *rtW = sd.Width;
                    if (rtH) *rtH = sd.Height;
                }
            }
            return true;
        }
        // FALLBACK for a render target g_rtTextures never saw: created before the CreateTexture
        // hook was installed, or through CreateCubeTexture/CreateVolumeTexture, neither of which
        // this shim hooks - the coverage gap the build report names plainly. Asked LIVE off the
        // texture's own usage flags rather than trusting a creation-time record, and only ever
        // reached here, after half two has already proven render target 0 is screen-sized, so it
        // adds no bridge call to the other ~1,700 draws a frame that never reach this line.
        DWORD usage = 0;
        UINT w = 0, h = 0;
        switch (t->GetType()) {
            case D3DRTYPE_TEXTURE: {
                D3DSURFACE_DESC sd{};
                if (SUCCEEDED(static_cast<IDirect3DTexture9*>(t)->GetLevelDesc(0, &sd))) {
                    usage = sd.Usage; w = sd.Width; h = sd.Height;
                }
                break;
            }
            case D3DRTYPE_CUBETEXTURE: {
                D3DSURFACE_DESC sd{};
                if (SUCCEEDED(static_cast<IDirect3DCubeTexture9*>(t)->GetLevelDesc(0, &sd))) {
                    usage = sd.Usage; w = sd.Width; h = sd.Height;
                }
                break;
            }
            case D3DRTYPE_VOLUMETEXTURE: {
                D3DVOLUME_DESC vd{};
                if (SUCCEEDED(static_cast<IDirect3DVolumeTexture9*>(t)->GetLevelDesc(0, &vd))) {
                    usage = vd.Usage; w = vd.Width; h = vd.Height;
                }
                break;
            }
            default: break;
        }
        if (usage & D3DUSAGE_RENDERTARGET) {
            ++g_rtInvariantFallbackHits;
            if (rtStage) *rtStage = st;
            if (rtW) *rtW = w;
            if (rtH) *rtH = h;
            return true;
        }
    }
    return false;
}

// Capped at 12 and re-armed by the capture key, exactly like ProbeDecalDraw/ProbeHudDraw
// (RearmCaptures resets g_rtInvariantProbeReports to 0 alongside them). Prints numbers, not a
// verdict: which path, which stage held the render target and its own dimensions, render target
// 0's dimensions, the back buffer's, the pixel shader's declared sampler names if reflected, the
// primitive count, and whether a vertex shader was bound - everything the next run needs to prove
// which draw was painting the rasterized frame.
void LogRtInvariantBlock(const char* path, int rtStage, UINT rtW, UINT rtH, UINT primitiveCount,
                         bool hasVS) {
    if (g_rtInvariantProbeReports >= 12) return;
    ++g_rtInvariantProbeReports;
    char samplers[256] = {}; int p = 0;
    for (int st = 0; st < 8; ++st) {
        if (!g_curTexture[st]) continue;
        const char* name = g_curPS.samplerName[st][0] ? g_curPS.samplerName[st] : "(unreflected)";
        p += _snprintf_s(samplers + p, sizeof(samplers) - p, _TRUNCATE, "%s%d:%s", p ? " " : "",
                         st, name);
    }
    Log("BLOCKED (rasterized-frame invariant) #%u: path=%s stage=%d holds the render target "
        "(%ux%u) | target0=%ux%u backBuffer=%ux%u | prims=%u | vertex shader %s | bound stages: %s",
        g_rtInvariantProbeReports, path, rtStage, rtW, rtH, g_rt0Width, g_rt0Height, g_backBufferW,
        g_backBufferH, primitiveCount,
        hasVS ? "present - passed through raw" : "NULL - already fixed function, not issued",
        samplers[0] ? samplers : "(none bound)");
}

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

const HostApi* g_host = nullptr;   // set by Sr3Logic_Attach

// Phase 1 host/logic split: the vtable is patched by the HOST, which points the slot at a thunk
// in its own memory; nothing in the game points into this DLL. Same contract as before:
// *original gets the true original entry, hook==nullptr only queries it.
bool PatchVTable(void* instance, int slot, void* hook, void** original) {
    void** vtable = *reinterpret_cast<void***>(instance);
    if (!g_host) return false;
    return g_host->PatchVtableSlot(vtable, slot, hook, original) != 0;
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
    // 2026-09-18 AUDIT FIX: which thread's Lock call populated this entry, so Hook_VBUnlock can
    // tell "the thread that locked this is the one unlocking it" from "some OTHER thread's
    // Unlock landed while this entry was still waiting" - see g_settings.vbUnlockOwnerThread.
    DWORD owner = 0;
};
PendingLock g_pendingLock;
// Snoop refusals on the copy below: a stale/unmapped source range rejected before copying, or
// an exception caught during the copy itself despite passing that check. One counter for both
// (unlike the atlas snoop's two), since this hook never had either defence before - any nonzero
// count here says the same thing the atlas snoop's crash did, just caught instead of crashing.
unsigned g_vbCopyRefused = 0;
// Forward-declared: defined with the atlas snoop's validated copy (AtlasSourceRangeReadable /
// CopyAtlasRowsGuarded), far below - this hook shares both the VirtualQuery range check and
// the guarded memcpy with that code rather than duplicating either. MSVC forbids __try in a
// function that also requires object unwinding (error C2712), and this function has plenty
// (InstanceCache&, several vectors), so the __try could not live here even if it were the only
// copy of it in the file.
static bool MemoryRangeReadable(const void* src, size_t len, const void** onFailAddr,
                                 MEMORY_BASIC_INFORMATION* onFailInfo);
static bool CopyRangeGuarded(void* dst, const void* src, size_t len);

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
// Defined with the GPU skinning side stream, further down (after CreateSkinBuffer) - it needs
// SkinFFBuffer, declared there.
void InvalidateSkinFFBuffers(IDirect3DVertexBuffer9* vb);
void InvalidateOccluders(IDirect3DVertexBuffer9* vb, UINT offset = 0, UINT size = 0);   // size 0 = whole buffer
void InvalidateMorphBakesBySrc(IDirect3DVertexBuffer9* vb);   // THE SHAPE BAKE - defined with the rest of the bake machinery, after MorphForThisDraw

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
// What the game locked: the buffer, and the byte range when it was not a DISCARD / to-the-end
// lock. The uv and bind-pose caches invalidate per buffer as before; the occluder cache keeps
// the meshes the range does not touch.
struct PendingInvalidation { IDirect3DVertexBuffer9* vb; UINT offset, size; bool whole; };
std::vector<PendingInvalidation> g_pendingInvalidations;
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


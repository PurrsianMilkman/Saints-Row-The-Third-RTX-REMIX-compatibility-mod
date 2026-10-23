// parts/01_core.cpp - constants, logging, timing primitives.
// NOT a standalone translation unit: #included, in order, by ../sr3rtx.cpp inside its anonymous
// namespace. Originally sr3rtx.cpp lines 63-161 (old file:line citations map here).

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
constexpr int kSlotQueryIssue = 6;

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


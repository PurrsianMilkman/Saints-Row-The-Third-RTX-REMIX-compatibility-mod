// parts/03_math_originals.cpp - matrix maths and the saved original D3D9 entry points.
// NOT a standalone translation unit: #included, in order, by ../sr3rtx.cpp inside its anonymous
// namespace. Originally sr3rtx.cpp lines 1152-1350 (old file:line citations map here).
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
IDirect3DIndexBuffer9* g_curIB = nullptr;   // what the game last bound; no bridge call to ask
// A vertex buffer's description never changes for its lifetime, and asking the bridge for it is
// a synchronous round trip. UvBufferFor asked on EVERY converted draw, before its own cache
// lookup - 4 ms of a city frame. Cleared with the uv cache when the game rewrites the buffer.
std::unordered_map<IDirect3DVertexBuffer9*, D3DVERTEXBUFFER_DESC> g_vbDescCache;
UINT g_uvBindOffset = 0;   // set by UvBufferFor (the uv ring's offset for this entry), used by InstallFloatUV
CreatePixelShader_t g_origCreatePixelShader = nullptr;
SetPixelShader_t g_origSetPixelShader = nullptr;
SetPSConstF_t g_origSetPSConstF = nullptr;

bool g_internal = false;   // guards against re-entering our own hooks
// Which thread SET g_internal true, so a read of it can tell "I am the render thread doing my
// own internal work" from "some other thread happened to read this while the render thread was
// internal". The render thread holds g_internal true across the whole of BeginFFP - cloth
// generation, uv buffer creation and locks, the atlas re-upload, the decal probe's DDS writes -
// while the game does its own atlas and vertex-buffer lock/unlock pairs on its MAIN thread. A
// main-thread call landing in that window used to read the bare global, see true, and wrongly
// treat itself as internal: Hook_SurfUnlockRect skipped consuming its own pending atlas entry,
// which left it with a bits pointer the real Unlock had already invalidated, and the next lock
// of the same surface appended a SECOND entry that Hook_SurfUnlockRect later found first and
// copied from - the September 14 crash (memcpy source 0x59ADF000, size 0x2000, one atlas row,
// inside the game's own CRT memmove). Only the render thread ever writes g_internal true, so
// this pair is read and written without a lock: it can only ever name that one thread or none,
// and a torn read of a DWORD cannot invent a thread id that thread never had.
volatile DWORD g_internalThread = 0;
// True only when the CURRENT thread is the one that actually set g_internal - see
// g_internalThread just above. snoopThreadScopedInternal is the kill switch back to a bare read
// of g_internal (every race this exists to fix, restored) for an A/B if this is ever wrong.
inline bool InternalHere() {
    return g_internal && (!g_settings.snoopThreadScopedInternal ||
                          g_internalThread == GetCurrentThreadId());
}
// What crosses the bridge, per call type, so the frame report can say where the traffic is.
unsigned long long g_callVsConst = 0, g_callVsConstRegs = 0, g_callPsConst = 0, g_callPsConstRegs = 0,
                   g_callSetVS = 0, g_callSetPS = 0, g_callSetTex = 0, g_callRS = 0, g_callTSS = 0,
                   g_callSS = 0, g_callStream = 0, g_callDecl = 0, g_callTransform = 0,
                   g_dietVsConstDropped = 0, g_dietVsBindDropped = 0, g_bridgeVsSyncs = 0;
// The vertex shader the BRIDGE currently has bound - distinct from g_lastVS, which is what the
// game last asked for. Every bind that reaches the bridge goes through here, so the two can
// differ only by design (the diet), never by accident.
IDirect3DVertexShader9* g_bridgeVS = nullptr;
inline void SyncBridgeVS(IDirect3DDevice9* dev, IDirect3DVertexShader9* want) {
    if (g_bridgeVS == want) return;
    g_origSetVertexShader(dev, want);
    g_bridgeVS = want;
    ++g_bridgeVsSyncs;
}
HRESULT WINAPI Hook_SetIndices(IDirect3DDevice9* dev, IDirect3DIndexBuffer9* ib) {
    if (!InternalHere()) g_curIB = ib;
    return g_origSetIndices(dev, ib);
}


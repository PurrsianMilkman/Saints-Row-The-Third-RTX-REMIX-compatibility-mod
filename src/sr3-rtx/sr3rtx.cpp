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
#include <algorithm>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <memory>
#include <atomic>
#include <intrin.h>
#include "../sr3-rtx-host/host_api.h"

namespace {

// ---------------------------------------------------------------- source layout
// The shim is ONE translation unit split across parts/ for manageability: everything below is
// textually #included, IN THIS ORDER, inside this anonymous namespace, exactly as when it was a
// single file. Order matters (later parts use earlier definitions). Each part's header gives the
// line range it occupied in the old monolithic sr3rtx.cpp, so old file:line citations still resolve.
#include "parts/01_core.cpp"
#include "parts/02_settings.cpp"
#include "parts/03_math_originals.cpp"
#include "parts/04_shader_reflection.cpp"
#include "parts/05_device_state.cpp"
#include "parts/06_buffer_writes.cpp"
#include "parts/07_clothing.cpp"
#include "parts/08_hair.cpp"
#include "parts/09_atlas_textures.cpp"
#include "parts/10_albedo_materials.cpp"
#include "parts/11_texture_stages.cpp"
#include "parts/12_cameras_remix_api.cpp"
#include "parts/13_remix_api_ticks.cpp"
#include "parts/14_classify.cpp"
#include "parts/15_ui_inject_marker.cpp"
#include "parts/16_time_accounting.cpp"
#include "parts/17_lights_state_hooks.cpp"
#include "parts/18_occlusion_cpu.cpp"
#include "parts/19_draw_primitive_hud.cpp"
#include "parts/20_recorders_probes.cpp"
#include "parts/21_cpu_skinning.cpp"
#include "parts/22_index_window_tighten.cpp"
#include "parts/23_gpu_skinning.cpp"
#include "parts/24_decal_probe.cpp"
#include "parts/25_asset_hash_probe.cpp"
#include "parts/26_stretch_index_window.cpp"
#include "parts/27_draw_indexed.cpp"
#include "parts/28_orphan_sweep.cpp"
#include "parts/29_present.cpp"
#include "parts/30_setup_device.cpp"

}  // namespace

// Phase 1 host/logic split: the host (sr3-rtx.asi) loads this DLL and calls Sr3Logic_Attach from
// ITS DllMain, so this does exactly what the old DllMain's PROCESS_ATTACH did.
extern "C" __declspec(dllexport) int __cdecl Sr3Logic_Attach(const HostApi* host) {
    g_host = host;
    InitializeCriticalSection(&g_snoopCs);
    g_snoopCsReady = true;
    CreateThread(nullptr, 0, Init, nullptr, 0, nullptr);
    return 1;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(module);
    return TRUE;
}

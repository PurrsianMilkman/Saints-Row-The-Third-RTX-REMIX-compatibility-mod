// Probe: is hardware ray tracing reachable from a 32-bit process on this machine?
// Answers two independent questions, so one failing does not hide the other:
//   1. Vulkan  - does any physical device expose VK_KHR_ray_tracing_pipeline / acceleration_structure / ray_query?
//   2. D3D12   - does any adapter report D3D12_RAYTRACING_TIER >= 1_0?
// Everything is loaded dynamically, so no SDK and no import libs are needed.
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <d3d12.h>
#include <dxgi1_4.h>

// ---------------- minimal Vulkan ----------------
typedef void*    VkInstance;
typedef void*    VkPhysicalDevice;
typedef int      VkResult;

struct VkApplicationInfo {
    uint32_t sType; const void* pNext;
    const char* pApplicationName; uint32_t applicationVersion;
    const char* pEngineName;      uint32_t engineVersion;
    uint32_t apiVersion;
};
struct VkInstanceCreateInfo {
    uint32_t sType; const void* pNext; uint32_t flags;
    const VkApplicationInfo* pApplicationInfo;
    uint32_t enabledLayerCount;     const char* const* ppEnabledLayerNames;
    uint32_t enabledExtensionCount; const char* const* ppEnabledExtensionNames;
};
struct VkExtensionProperties { char extensionName[256]; uint32_t specVersion; };

typedef void* (__stdcall *PFN_vkGetInstanceProcAddr)(VkInstance, const char*);
typedef VkResult (__stdcall *PFN_vkCreateInstance)(const VkInstanceCreateInfo*, const void*, VkInstance*);
typedef VkResult (__stdcall *PFN_vkEnumeratePhysicalDevices)(VkInstance, uint32_t*, VkPhysicalDevice*);
typedef void     (__stdcall *PFN_vkGetPhysicalDeviceProperties)(VkPhysicalDevice, void*);
typedef VkResult (__stdcall *PFN_vkEnumerateDeviceExtensionProperties)(VkPhysicalDevice, const char*, uint32_t*, VkExtensionProperties*);

static const char* kWanted[] = {
    "VK_KHR_acceleration_structure",
    "VK_KHR_ray_tracing_pipeline",
    "VK_KHR_ray_query",
    "VK_KHR_deferred_host_operations",
    "VK_KHR_buffer_device_address",
    "VK_EXT_opacity_micromap",
    "VK_KHR_external_memory_win32",
};

static void probeVulkan() {
    printf("=== VULKAN ===\n");
    HMODULE lib = LoadLibraryA("vulkan-1.dll");
    if (!lib) { printf("  vulkan-1.dll: NOT LOADABLE in this bitness\n\n"); return; }
    printf("  vulkan-1.dll: loaded\n");

    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)GetProcAddress(lib, "vkGetInstanceProcAddr");
    if (!gipa) { printf("  no vkGetInstanceProcAddr\n\n"); return; }

    PFN_vkCreateInstance createInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    if (!createInstance) { printf("  no vkCreateInstance\n\n"); return; }

    VkApplicationInfo app = {};
    app.sType = 0;                      // VK_STRUCTURE_TYPE_APPLICATION_INFO
    app.pApplicationName = "rtprobe";
    app.pEngineName = "rtprobe";
    app.apiVersion = (1u << 22) | (3u << 12);   // 1.3

    VkInstanceCreateInfo ci = {};
    ci.sType = 1;                       // VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO
    ci.pApplicationInfo = &app;

    VkInstance inst = NULL;
    VkResult r = createInstance(&ci, NULL, &inst);
    if (r != 0) {
        // retry at 1.1 in case the loader/ICD refuses 1.3
        app.apiVersion = (1u << 22) | (1u << 12);
        r = createInstance(&ci, NULL, &inst);
    }
    if (r != 0) { printf("  vkCreateInstance FAILED, VkResult=%d\n\n", r); return; }

    PFN_vkEnumeratePhysicalDevices enumPd =
        (PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties getProps =
        (PFN_vkGetPhysicalDeviceProperties)gipa(inst, "vkGetPhysicalDeviceProperties");
    PFN_vkEnumerateDeviceExtensionProperties enumExt =
        (PFN_vkEnumerateDeviceExtensionProperties)gipa(inst, "vkEnumerateDeviceExtensionProperties");
    if (!enumPd || !getProps || !enumExt) { printf("  missing entry points\n\n"); return; }

    uint32_t n = 0;
    enumPd(inst, &n, NULL);
    printf("  physical devices: %u\n", n);
    if (!n) { printf("\n"); return; }

    VkPhysicalDevice* pds = (VkPhysicalDevice*)calloc(n, sizeof(void*));
    enumPd(inst, &n, pds);

    for (uint32_t i = 0; i < n; i++) {
        // VkPhysicalDeviceProperties is ~824 bytes; deviceName starts at byte 20.
        unsigned char props[4096] = {};
        getProps(pds[i], props);
        const char* name = (const char*)(props + 20);
        uint32_t api = *(uint32_t*)props;
        printf("  [%u] %s  (api %u.%u.%u)\n", i, name,
               (api >> 22) & 0x7f, (api >> 12) & 0x3ff, api & 0xfff);

        uint32_t ec = 0;
        enumExt(pds[i], NULL, &ec, NULL);
        VkExtensionProperties* exts = (VkExtensionProperties*)calloc(ec ? ec : 1, sizeof(VkExtensionProperties));
        enumExt(pds[i], NULL, &ec, exts);
        printf("      %u device extensions\n", ec);
        for (int w = 0; w < (int)(sizeof(kWanted)/sizeof(kWanted[0])); w++) {
            bool found = false;
            for (uint32_t e = 0; e < ec; e++)
                if (strcmp(exts[e].extensionName, kWanted[w]) == 0) { found = true; break; }
            printf("      %-34s %s\n", kWanted[w], found ? "YES" : "no");
        }
        free(exts);
    }
    printf("\n");
}

// ---------------- D3D12 ----------------
typedef HRESULT (WINAPI *PFN_D3D12CreateDevice_t)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
typedef HRESULT (WINAPI *PFN_CreateDXGIFactory1_t)(REFIID, void**);

static void probeD3D12() {
    printf("=== D3D12 / DXR ===\n");
    HMODULE d3d12 = LoadLibraryA("d3d12.dll");
    HMODULE dxgi  = LoadLibraryA("dxgi.dll");
    if (!d3d12 || !dxgi) { printf("  d3d12.dll/dxgi.dll: NOT LOADABLE in this bitness\n\n"); return; }

    PFN_D3D12CreateDevice_t createDevice = (PFN_D3D12CreateDevice_t)GetProcAddress(d3d12, "D3D12CreateDevice");
    PFN_CreateDXGIFactory1_t createFactory = (PFN_CreateDXGIFactory1_t)GetProcAddress(dxgi, "CreateDXGIFactory1");
    if (!createDevice || !createFactory) { printf("  missing entry points\n\n"); return; }

    IDXGIFactory1* factory = NULL;
    if (FAILED(createFactory(__uuidof(IDXGIFactory1), (void**)&factory))) {
        printf("  CreateDXGIFactory1 failed\n\n"); return;
    }

    IDXGIAdapter1* adapter = NULL;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 desc = {};
        adapter->GetDesc1(&desc);
        printf("  [%u] %ls%s\n", i, desc.Description,
               (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? "  (software)" : "");

        ID3D12Device* dev = NULL;
        HRESULT hr = createDevice(adapter, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), (void**)&dev);
        if (FAILED(hr)) {
            printf("      D3D12CreateDevice failed hr=0x%08lX\n", (unsigned long)hr);
        } else {
            D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5 = {};
            hr = dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &o5, sizeof(o5));
            if (FAILED(hr)) printf("      OPTIONS5 query failed hr=0x%08lX\n", (unsigned long)hr);
            else            printf("      RaytracingTier = %d  (0=none, 10=1.0, 11=1.1)\n", (int)o5.RaytracingTier);
            dev->Release();
        }
        adapter->Release();
    }
    factory->Release();
    printf("\n");
}

int main() {
    printf("rtprobe - %d-bit process\n\n", (int)(sizeof(void*) * 8));
    probeVulkan();
    probeD3D12();
    return 0;
}

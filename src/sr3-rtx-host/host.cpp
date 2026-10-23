// host.cpp - persistent HOST for the SR3 RTX shim (sr3-rtx.asi). Phase 1: loads
// sr3-rtx-logic.dll from its own directory and offers vtable-thunk services. Kernel32 only.
#include <windows.h>
#include "host_api.h"

namespace {

constexpr int kMaxThunks = 256;
constexpr int kThunkStride = 8;   // 6 bytes of code + 2 bytes int3 padding

struct SlotRec { void** slotAddr; void* original; };

void*           g_table[kMaxThunks];      // g_table[i] = logic hook for thunk i (jmp [&g_table[i]])
SlotRec         g_slots[kMaxThunks];      // which vtable slot thunk i serves + its true original
int             g_count = 0;
unsigned char*  g_pool = nullptr;         // executable thunk pool
SRWLOCK         g_lock = SRWLOCK_INIT;
HostApi         g_api;
wchar_t         g_dir[MAX_PATH];

void HostLog(const char* msg, DWORD code) {
    wchar_t path[MAX_PATH + 40];
    lstrcpyW(path, g_dir);
    lstrcatW(path, L"sr3-rtx-host.log");
    HANDLE f = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    char line[256];
    lstrcpynA(line, msg, 200);
    int n = lstrlenA(line);
    static const char hex[] = "0123456789ABCDEF";
    line[n++] = ' '; line[n++] = '('; line[n++] = '0'; line[n++] = 'x';
    for (int s = 28; s >= 0; s -= 4) line[n++] = hex[(code >> s) & 15];
    line[n++] = ')'; line[n++] = '\r'; line[n++] = '\n';
    DWORD w = 0;
    WriteFile(f, line, n, &w, nullptr);
    CloseHandle(f);
}

int __cdecl PatchVtableSlot(void** vtable, int index, void* logicFn, void** outOriginal) {
    void** slot = &vtable[index];
    AcquireSRWLockExclusive(&g_lock);
    int found = -1;
    for (int i = 0; i < g_count; ++i)
        if (g_slots[i].slotAddr == slot) { found = i; break; }

    if (found >= 0) {                       // already ours: the true original is on record
        if (outOriginal) *outOriginal = g_slots[found].original;
        if (logicFn) g_table[found] = logicFn;
        ReleaseSRWLockExclusive(&g_lock);
        return 1;
    }
    if (outOriginal) *outOriginal = *slot;
    if (!logicFn) { ReleaseSRWLockExclusive(&g_lock); return 1; }

    if (g_count >= kMaxThunks || !g_pool) { ReleaseSRWLockExclusive(&g_lock); return 0; }
    const int i = g_count;
    unsigned char* t = g_pool + i * kThunkStride;
    t[0] = 0xFF; t[1] = 0x25;                                   // jmp dword ptr [imm32]
    *reinterpret_cast<unsigned long*>(t + 2) = reinterpret_cast<unsigned long>(&g_table[i]);
    t[6] = 0xCC; t[7] = 0xCC;
    g_table[i] = logicFn;                                       // table before vtable
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) {
        g_table[i] = nullptr;
        ReleaseSRWLockExclusive(&g_lock);
        return 0;
    }
    g_slots[i].slotAddr = slot;
    g_slots[i].original = *slot;
    ++g_count;
    *slot = t;
    VirtualProtect(slot, sizeof(void*), old, &old);
    ReleaseSRWLockExclusive(&g_lock);
    return 1;
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        DWORD n = GetModuleFileNameW(module, g_dir, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) return TRUE;
        wchar_t* slash = g_dir + n;
        while (slash > g_dir && slash[-1] != L'\\' && slash[-1] != L'/') --slash;
        *slash = 0;

        g_pool = static_cast<unsigned char*>(VirtualAlloc(nullptr, kMaxThunks * kThunkStride,
                                             MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (!g_pool) { HostLog("host: VirtualAlloc for thunk pool failed", GetLastError()); return TRUE; }

        wchar_t logic[MAX_PATH + 32];
        lstrcpyW(logic, g_dir);
        lstrcatW(logic, L"sr3-rtx-logic.dll");
        HMODULE lib = LoadLibraryW(logic);
        if (!lib) { HostLog("host: LoadLibrary sr3-rtx-logic.dll failed", GetLastError()); return TRUE; }
        Sr3LogicAttachFn attach = reinterpret_cast<Sr3LogicAttachFn>(GetProcAddress(lib, SR3_LOGIC_ATTACH_NAME));
        if (!attach) { HostLog("host: Sr3Logic_Attach export not found", GetLastError()); return TRUE; }

        g_api.version = SR3_HOST_API_VERSION;
        g_api.size = sizeof(HostApi);
        g_api.PatchVtableSlot = &PatchVtableSlot;
        attach(&g_api);
    }
    return TRUE;
}

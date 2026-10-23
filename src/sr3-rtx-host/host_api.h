// host_api.h - the contract between the persistent HOST (sr3-rtx.asi) and the LOGIC DLL
// (sr3-rtx-logic.dll). Included by both sides. Append new members at the END and bump
// SR3_HOST_API_VERSION; never reorder.
#pragma once
#include <stdint.h>

#define SR3_HOST_API_VERSION 1u

struct HostApi {
    uint32_t version;   // SR3_HOST_API_VERSION the host was built with
    uint32_t size;      // sizeof(HostApi) as built by the host

    // Patch vtable[index] so it points at a host-owned thunk that jumps through a table to
    // logicFn. Nothing in the game's memory ever points into the logic DLL directly.
    //  - *outOriginal receives the TRUE original entry (never a thunk), also on a repeat patch.
    //  - logicFn == nullptr: query only, writes outOriginal and leaves the vtable alone.
    //  - Patching the same slot again reuses its thunk and just updates the table entry.
    // Returns 1 on success, 0 on failure (pool full / VirtualProtect failed).
    int (__cdecl* PatchVtableSlot)(void** vtable, int index, void* logicFn, void** outOriginal);
};

typedef int (__cdecl* Sr3LogicAttachFn)(const HostApi*);
#define SR3_LOGIC_ATTACH_NAME "Sr3Logic_Attach"

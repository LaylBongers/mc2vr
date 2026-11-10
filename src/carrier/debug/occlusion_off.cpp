// debug_occlusion_off — see occlusion_off.hpp.

#include "debug/occlusion_off.hpp"

#include <windows.h>

#include <cstdint>
#include <cstring>

#include "game/addresses.h"
#include "log.hpp"

namespace mc2vr::occlusion_off {

namespace {

bool g_enabled = false;

} // namespace

bool set_enabled(const char *value)
{
    if (strcmp(value, "on") == 0) g_enabled = true;
    else if (strcmp(value, "off") == 0) g_enabled = false;
    else return false;
    MC2VR_LOG("occlusion: debug_occlusion_off=%s", value);
    return true;
}

void install()
{
    if (!g_enabled) {
        return;
    }
    static const uintptr_t sites[2] = {MC2_OCCLUSION_BOX_CALL_A, MC2_OCCLUSION_BOX_CALL_B};
    for (uintptr_t site : sites) {
        uint8_t *p = reinterpret_cast<uint8_t *>(site);
        int32_t rel;
        memcpy(&rel, p + 1, 4);
        if (p[0] != 0xE8 || (uintptr_t)(site + 5 + rel) != MC2_OCCLUSION_BOX_EMIT) {
            MC2VR_LOG("occlusion: site %p does not hold `call OcclusionBox_EmitPrimRecord` — "
                      "NOT patched", (void *)site);
            continue;
        }
        DWORD old;
        if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old)) {
            MC2VR_LOG("occlusion: VirtualProtect failed @ %p", (void *)site);
            continue;
        }
        memset(p, 0x90, 5);
        VirtualProtect(p, 5, old, &old);
        FlushInstructionCache(GetCurrentProcess(), p, 5);
        MC2VR_LOG("occlusion: debug_occlusion_off: box call @ %p NOPed", (void *)site);
    }
}

} // namespace mc2vr::occlusion_off

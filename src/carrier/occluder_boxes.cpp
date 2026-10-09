// Occluder boxes — see occluder_boxes.hpp for the design and evidence.

#include "occluder_boxes.hpp"

#include <windows.h>

#include <cstdint>
#include <cstring>

#include "log.hpp"

namespace mc2vr::occluder_boxes {

namespace {

bool g_skip = true;

void patch()
{
    static const uintptr_t sites[2] = {0x00468eedu, 0x00468f12u};
    const uintptr_t target = 0x0046edf0u;
    for (uintptr_t site : sites) {
        uint8_t *p = reinterpret_cast<uint8_t *>(site);
        int32_t rel;
        memcpy(&rel, p + 1, 4);
        if (p[0] != 0xE8 || (uintptr_t)(site + 5 + rel) != target) {
            MC2VR_LOG("occluders: site %p does not hold `call 0x46edf0` — NOT patched",
                      (void *)site);
            continue;
        }
        DWORD old;
        if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old)) {
            MC2VR_LOG("occluders: VirtualProtect failed @ %p", (void *)site);
            continue;
        }
        memset(p, 0x90, 5);
        VirtualProtect(p, 5, old, &old);
        FlushInstructionCache(GetCurrentProcess(), p, 5);
        MC2VR_LOG("occluders: box call @ %p NOPed", (void *)site);
    }
}

} // namespace

bool set_skip(const char *value)
{
    if (strcmp(value, "on") == 0) {
        g_skip = true;
    } else if (strcmp(value, "off") == 0) {
        g_skip = false;
    } else {
        return false;
    }
    return true;
}

void install()
{
    if (g_skip) {
        patch();
    }
}

} // namespace mc2vr::occluder_boxes

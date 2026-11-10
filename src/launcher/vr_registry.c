// The Wine VR registry — see vr_registry.h.

#include "vr_registry.h"

#include <stddef.h>

#include "xr_runtime.h"

#define VR_KEY "Software\\Wine\\VR"

// The values wineopenxr_init_registry publishes.
static const char *const kPublished[] = {
    "openxr_vulkan_instance_extensions",
    "openxr_vulkan_device_extensions",
    "openxr_vulkan_device_vid",
    "openxr_vulkan_device_pid",
};
#define PUBLISHED_COUNT (sizeof(kPublished) / sizeof(kPublished[0]))

static BOOL has_value(const char *name)
{
    HKEY k;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, VR_KEY, 0, KEY_READ, &k) != ERROR_SUCCESS) {
        return FALSE;
    }
    const BOOL ok = RegQueryValueExA(k, name, NULL, NULL, NULL, NULL) == ERROR_SUCCESS;
    RegCloseKey(k);
    return ok;
}

// Publish and check (see vr_registry.h): 0 = published, 2 = no wineopenxr
// (not under Proton), 3 = published nothing (the earlier values, if any,
// restored).
static int publish(void)
{
    HMODULE m = LoadLibraryW(L"wineopenxr.dll");
    void (*init)(void) = m != NULL ? (void (*)(void))GetProcAddress(m, "wineopenxr_init_registry") : NULL;
    if (init == NULL) {
        return 2;
    }
    HKEY k;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, VR_KEY, 0, KEY_ALL_ACCESS, &k) != ERROR_SUCCESS) {
        init();  // the key is created first; without it there is nothing to check
        return 3;
    }
    static struct {
        BOOL present;
        DWORD type, size;
        BYTE data[8192];
    } saved[PUBLISHED_COUNT];
    for (size_t i = 0; i < PUBLISHED_COUNT; i++) {
        saved[i].size = sizeof(saved[i].data);
        saved[i].present = RegQueryValueExA(k, kPublished[i], NULL, &saved[i].type, saved[i].data,
                                            &saved[i].size) == ERROR_SUCCESS;
        RegDeleteValueA(k, kPublished[i]);
    }
    init();
    BOOL all = TRUE;
    for (size_t i = 0; i < PUBLISHED_COUNT; i++) {
        all = all && RegQueryValueExA(k, kPublished[i], NULL, NULL, NULL, NULL) == ERROR_SUCCESS;
    }
    if (!all) {
        for (size_t i = 0; i < PUBLISHED_COUNT; i++) {
            if (saved[i].present) {
                RegSetValueExA(k, kPublished[i], 0, saved[i].type, saved[i].data, saved[i].size);
            }
        }
    }
    RegCloseKey(k);
    return all ? 0 : 3;
}

void vr_registry_init(VrRegistryLog log)
{
    if (!xr_under_wine()) {
        return;  // native Windows: no wineopenxr, nothing to publish
    }

    // state: normally published by Proton's vrclient for an OpenVR app;
    // without it wineopenxr negotiation fails (-6), and state=2 fails too.
    // Only filled in if absent.
    HKEY k;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, VR_KEY, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &k, NULL) ==
        ERROR_SUCCESS) {
        DWORD v = 0, sz = sizeof(v), type = 0;
        if (RegQueryValueExA(k, "state", NULL, &type, (BYTE *)&v, &sz) != ERROR_SUCCESS) {
            v = 1;
            RegSetValueExA(k, "state", 0, REG_DWORD, (const BYTE *)&v, sizeof(v));
            log("vr registry: set " VR_KEY "\\state=1 (was absent)");
        }
        RegCloseKey(k);
    } else {
        log("warning: vr registry: cannot create HKCU\\" VR_KEY);
    }

    const int rc = publish();
    const BOOL lists = has_value("openxr_vulkan_instance_extensions") &&
                       has_value("openxr_vulkan_device_extensions");
    if (rc == 0) {
        log("vr registry: runtime Vulkan requirements published by this run");
    } else if (rc == 2) {
        log("vr registry: wineopenxr not available — nothing published");
    } else if (rc == 3 && lists) {
        log("warning: vr registry: wineopenxr published nothing (SteamVR up? 32-bit runtime "
            "registered?) — an earlier run's runtime Vulkan requirements stay in effect");
    } else if (rc == 3) {
        log("warning: vr registry: wineopenxr published nothing (SteamVR up? 32-bit runtime "
            "registered?) — no runtime Vulkan requirements, so DXVK won't prepare its device for OpenXR");
    }
}

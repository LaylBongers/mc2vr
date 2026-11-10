// The Wine VR registry — see vr_registry.h.

#include "vr_registry.h"

#include <stdio.h>
#include <string.h>

#include "xr_runtime.h"

#define VR_KEY "Software\\Wine\\VR"
#define CHILD_TIMEOUT_MS (15 * 1000)

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

void vr_registry_init(const wchar_t *host_exe, VrRegistryLog log)
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

    // The runtime's Vulkan requirements, published by wineopenxr in the 64-bit
    // host's helper mode.
    if (GetFileAttributesW(host_exe) == INVALID_FILE_ATTRIBUTES) {
        log("warning: vr registry: %ls not found — the runtime Vulkan requirements are not "
            "refreshed (an earlier run's stay in effect)", host_exe);
        return;
    }
    wchar_t cmd[MAX_PATH + 64];
    _snwprintf(cmd, sizeof(cmd) / sizeof(cmd[0]), L"\"%ls\" --vr-registry-init", host_exe);
    cmd[sizeof(cmd) / sizeof(cmd[0]) - 1] = L'\0';
    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));
    if (!CreateProcessW(host_exe, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        log("warning: vr registry: cannot start the host's init helper (%lu)", GetLastError());
        return;
    }
    DWORD rc = 1;
    if (WaitForSingleObject(pi.hProcess, CHILD_TIMEOUT_MS) == WAIT_OBJECT_0) {
        GetExitCodeProcess(pi.hProcess, &rc);
    } else {
        log("warning: vr registry: init helper still running after %us — continuing",
            CHILD_TIMEOUT_MS / 1000);
        rc = WAIT_TIMEOUT;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    // Exit codes: see mc2vr_host's vr_registry_init (src/host/main.cpp).
    const BOOL lists = has_value("openxr_vulkan_instance_extensions") &&
                       has_value("openxr_vulkan_device_extensions");
    if (rc == 0) {
        log("vr registry: runtime Vulkan requirements published by this run");
    } else if (rc == 2) {
        log("vr registry: wineopenxr not available — nothing published");
    } else if (rc == 3 && lists) {
        log("warning: vr registry: wineopenxr published nothing (SteamVR up?) — an earlier run's "
            "runtime Vulkan requirements stay in effect");
    } else if (rc == 3) {
        log("warning: vr registry: wineopenxr published nothing (SteamVR up?) — no runtime Vulkan "
            "requirements, so DXVK won't prepare its device for OpenXR");
    } else if (rc != WAIT_TIMEOUT) {
        log("warning: vr registry: init helper exited with %lu", rc);
    }
}

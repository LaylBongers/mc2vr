// mc2vr_host: 64-bit OpenXR/D3D11 host. See docs/host.md.
//   mc2vr_host.exe [--mock] [--frames N] [--xr-debug] [--ipc-name NAME]
//   mc2vr_host.exe --vr-registry-init   (the launcher's helper; see below)
// Logs to mc2vr_host.log beside the exe; the launcher waits for the
// "mc2vr_host: ready" line there. The IPC section is created BEFORE the
// session comes up, so a carrier injected right after the ready line finds it.
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "ipc.hpp"
#include "log.hpp"
#include "mock.hpp"
#include "xr_session.hpp"

// --vr-registry-init: publish the Wine VR registry (the OpenXR runtime's Vulkan
// requirements, read by DXVK in every process) for the launcher, then exit
// (src/launcher/vr_registry.h, docs/openxr_proton.md item 2). A 64-bit process
// always finds a runtime: the system registers SteamVR's 64-bit one.
//
// wineopenxr_init_registry returns TRUE even when it publishes nothing, and
// Wine skips writing a value whose data is unchanged without touching the
// key, so neither tells whether this run published. Instead the published
// values are snapshotted and cleared first: whatever is there afterwards was
// written by this run. If they don't come back, the snapshot is restored, so a
// failure never leaves less than before. Exit code 0 = published,
// 2 = no wineopenxr (not under Proton), 3 = published nothing (the earlier
// values, if any, restored). Writes no log.
static int vr_registry_init() {
    HMODULE m = LoadLibraryW(L"wineopenxr.dll");
    auto fn = m ? (void (*)())GetProcAddress(m, "wineopenxr_init_registry") : nullptr;
    if (fn == nullptr) return 2;
    HKEY k;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Wine\\VR", 0, KEY_ALL_ACCESS, &k) != ERROR_SUCCESS) {
        fn();  // the launcher creates the key first; without it there is nothing to check
        return 3;
    }
    static const char* const kValues[] = {"openxr_vulkan_instance_extensions", "openxr_vulkan_device_extensions",
                                          "openxr_vulkan_device_vid", "openxr_vulkan_device_pid"};
    struct Saved {
        bool present = false;
        DWORD type = 0, size = 0;
        BYTE data[8192];
    };
    static Saved saved[4];
    for (int i = 0; i < 4; ++i) {
        saved[i].size = sizeof(saved[i].data);
        saved[i].present =
            RegQueryValueExA(k, kValues[i], nullptr, &saved[i].type, saved[i].data, &saved[i].size) == ERROR_SUCCESS;
        RegDeleteValueA(k, kValues[i]);
    }
    fn();
    bool all = true;
    for (int i = 0; i < 4; ++i) {
        all = all && RegQueryValueExA(k, kValues[i], nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    }
    if (!all) {
        for (int i = 0; i < 4; ++i) {
            if (saved[i].present) RegSetValueExA(k, kValues[i], 0, saved[i].type, saved[i].data, saved[i].size);
        }
    }
    RegCloseKey(k);
    return all ? 0 : 3;
}

int main(int argc, char** argv) {
    if (argc == 2 && !strcmp(argv[1], "--vr-registry-init")) return vr_registry_init();
    bool isMock = false, xrDebug = false;
    const char* ipcName = MC2VR_IPC_DEFAULT_NAME;
    xrs::Options opt;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--mock")) isMock = true;
        else if (!strcmp(argv[i], "--xr-debug")) xrDebug = true;
        else if (!strcmp(argv[i], "--ipc-name") && i + 1 < argc) ipcName = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) opt.maxFrames = atoi(argv[++i]);
        else {
            fprintf(stderr, "usage: mc2vr_host [--mock] [--frames N] [--xr-debug] [--ipc-name NAME]\n");
            return 2;
        }
    }

    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    wchar_t* slash = wcsrchr(path, L'\\');
    wcscpy(slash ? slash + 1 : path, L"mc2vr_host.log");
    hostlog::open(path);
    if (xrDebug) {
        // OpenXR loader tracing goes to stderr; capture it beside the log.
        wcscpy(slash ? slash + 1 : path, L"mc2vr_host_xrloader.log");
        _wfreopen(path, L"w", stderr);
        _putenv("XR_LOADER_DEBUG=all");
        wcscpy(slash ? slash + 1 : path, L"mc2vr_host.log");
    }
    hostlog::write("mc2vr_host starting (%s, ipc '%s')", isMock ? "mock" : "openxr",
                   ipcName);

    // IPC first: the carrier may connect as soon as we
    // are ready. Failure is non-fatal for --frames-limited mock runs but the
    // carrier path depends on it, so log loudly.
    if (!ipc::server_init(ipcName)) {
        hostlog::write("mc2vr_host: ipc unavailable — exiting");
        return 3;
    }

    const int rc = isMock ? mock::run(opt.maxFrames) : xrs::run(opt);
    ipc::mark_exiting();
    return rc;
}

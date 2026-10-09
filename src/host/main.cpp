// mc2vr_host: 64-bit OpenXR/D3D11 host (S4). See docs/stereo.md §S4.
//   mc2vr_host.exe [--mock] [--frames N] [--xr-debug] [--ipc-name NAME]
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

int main(int argc, char** argv) {
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

    // IPC first: the carrier (or selftest probe) may connect as soon as we
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

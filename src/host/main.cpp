// mc2vr_host: 64-bit OpenXR/D3D11 host (S4). See docs/s4_handover.md.
//   mc2vr_host.exe [--mock] [--frames N]
// Logs to mc2vr_host.log beside the exe; prints "mc2vr_host: ready" on
// stdout/log once the session (or mock device) is up.
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "log.hpp"
#include "mock.hpp"
#include "xr_session.hpp"

int main(int argc, char** argv) {
    bool isMock = false, xrDebug = false;
    xrs::Options opt;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--mock")) isMock = true;
        else if (!strcmp(argv[i], "--xr-debug")) xrDebug = true;
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) opt.maxFrames = atoi(argv[++i]);
        else {
            fprintf(stderr, "usage: mc2vr_host [--mock] [--frames N] [--xr-debug]\n");
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
    hostlog::write("mc2vr_host starting (%s)", isMock ? "mock" : "openxr");

    return isMock ? mock::run(opt.maxFrames) : xrs::run(opt);
}

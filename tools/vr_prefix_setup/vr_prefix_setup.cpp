// One-shot prefix VR setup (S4-1 run-5 forensics; docs/s4_handover.md).
// DIAGNOSTIC ONLY as of run 6b — launch.sh does NOT use this (the carrier
// writes the registry values itself inside attach→init2 and deletes them
// after; writing them outside that window arms DXVK's boot-time interop,
// which hung the game pre-main-loop in run 6a). Kept for RE/forensics:
// demonstrates the heavy path of vrclient_init_registry in a throwaway
// process.
//
// Calls ONLY vrclient_init_registry and exits. In vrclient_init_registry's
// "heavy" path (the HKCU Software\Wine\VR key absent), the spawned
// initialize_vr_data thread connects SteamVR, enumerates the Vulkan
// extensions the compositor requires, caches them in the registry, then
// FreeLibraryAndExitThread's the native vrclient — a process that goes on to
// USE vrclient afterwards runs on freed memory (garbage queries, hard hang;
// that is exactly what killed run 5 in-game). This tool deliberately never
// touches vrclient after the call, so the heavy path is SAFE here: the
// registry state gets written and the process exits cleanly.
//
// launch.sh runs this BEFORE the game so the game's own bootstrap finds the
// complete registry state and NEVER needs the in-game call (which stays
// openvr_init_registry=off, live-proven poison).
//
// Build: i686-w64-mingw32-g++ -static -mconsole -o vr_prefix_setup.exe vr_prefix_setup.cpp
// Run:   <proton>/proton run vr_prefix_setup.exe   (SteamVR must be up)
#include <windows.h>
#include <cstdio>
#include <cstring>

// Wine console stdout is unreliable under `proton run` — log to a file next
// to the exe (cwd of the run).
static FILE *g_out;
#define printf(...) fprintf(g_out, __VA_ARGS__)

int main()
{
    g_out = fopen("vr_prefix_setup_out.txt", "w");
    if (g_out != nullptr) {
        setvbuf(g_out, nullptr, _IONBF, 0);
    } else {
        g_out = stderr;
    }
    printf("=== vr_prefix_setup (pid=%lu) ===\n", GetCurrentProcessId());

    HMODULE vrc = LoadLibraryW(L"vrclient.dll");
    if (vrc == nullptr) {
        printf("FATAL: vrclient.dll load failed (%lu)\n", GetLastError());
        return 1;
    }
    typedef int(__cdecl *init_registry_t)(void *);
    auto p_initreg = (init_registry_t)GetProcAddress(vrc, "vrclient_init_registry");
    if (p_initreg == nullptr) {
        printf("FATAL: vrclient_init_registry export missing\n");
        return 1;
    }
    // The export ignores its argument in this build (RE-proven 2026-10-04);
    // pass a zeroed buffer like the probe does.
    static unsigned char params[256];
    int rc = p_initreg(params);
    printf("vrclient_init_registry -> %d\n", rc);
    // Give the spawned initialize_vr_data thread time to finish its registry
    // writes before exit (observed: completes well within ~1 s; 3 s margin).
    Sleep(3000);
    printf("done\n");
    return 0;
}

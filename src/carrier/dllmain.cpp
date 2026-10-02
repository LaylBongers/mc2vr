// Carrier DLL entry point. DllMain only spawns the init thread — nothing
// that can touch loader lock happens here (no LoadLibrary, no CreateProcess,
// no COM, no CRT atexit tricks). All work, including hook installation, runs
// on the init thread (docs/launcher_plan.md step 5).
#include <windows.h>

#include "init.hpp"

static DWORD WINAPI init_thread(LPVOID unused)
{
    (void)unused;
    mc2vr::init();
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    (void)reserved;

    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        // The returned handle is intentionally not stored: the init thread
        // locates the module via GetModuleHandleExW on its own address.
        HANDLE thread = CreateThread(nullptr, 0, init_thread, nullptr, 0, nullptr);
        if (thread) {
            CloseHandle(thread);
        }
        // If CreateThread fails we still return TRUE — failing DllMain here
        // would make LoadLibraryW fail and unload us mid-call, which is
        // worse than a silently idle carrier (visible in the log's absence).
    }

    return TRUE;
}

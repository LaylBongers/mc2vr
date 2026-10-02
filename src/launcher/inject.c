#include "inject.h"

#include <stdint.h>

DWORD mc2_inject_dll(HANDLE hProcess, const wchar_t *dll_path, DWORD timeout_ms)
{
    SIZE_T path_bytes = (wcslen(dll_path) + 1) * sizeof(wchar_t);

    // Remote buffer for the UTF-16 path.
    void *remote_path = VirtualAllocEx(hProcess, NULL, path_bytes,
                                       MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote_path) {
        return GetLastError();
    }

    DWORD result = 0;

    if (!WriteProcessMemory(hProcess, remote_path, dll_path, path_bytes, NULL)) {
        result = GetLastError();
        goto out;
    }

    // LoadLibraryW is resolved in the launcher's kernel32 and called in the
    // game's. Within one Wine prefix the system DLL layout is shared, so the
    // address is valid in both processes. (Known caveat on real Windows x64:
    // HMODULE truncation in the exit code — we only test it for non-zero.)
    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    if (!kernel32) {
        result = GetLastError();
        goto out;
    }
    FARPROC load_library_w = GetProcAddress(kernel32, "LoadLibraryW");
    if (!load_library_w) {
        result = GetLastError();
        goto out;
    }

    HANDLE thread = CreateRemoteThread(hProcess, NULL, 0,
                                       (LPTHREAD_START_ROUTINE)load_library_w,
                                       remote_path, 0, NULL);
    if (!thread) {
        result = GetLastError();
        goto out;
    }

    if (WaitForSingleObject(thread, timeout_ms) != WAIT_OBJECT_0) {
        TerminateThread(thread, 0); // last resort; shouldn't happen
        CloseHandle(thread);
        result = ERROR_TIMEOUT;
        goto out;
    }

    DWORD exit_code = 0;
    GetExitCodeThread(thread, &exit_code);
    CloseHandle(thread);

    if (exit_code == 0) {
        // LoadLibraryW failed in the remote process (module not loaded).
        result = ERROR_INVALID_FUNCTION;
    }

out:
    VirtualFreeEx(hProcess, remote_path, 0, MEM_RELEASE);
    return result;
}

#include "boot_wait.h"

#include <tlhelp32.h>

DWORD mc2_remote_module_base(HANDLE hProcess, const wchar_t *module_name,
                             uintptr_t *base, DWORD retries)
{
    *base = 0;

    for (DWORD attempt = 0; attempt < retries; attempt++) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetProcessId(hProcess));
        if (snap == INVALID_HANDLE_VALUE) {
            Sleep(100);
            continue;
        }

        MODULEENTRY32W me;
        me.dwSize = sizeof(me);

        if (Module32FirstW(snap, &me)) {
            do {
                if (_wcsicmp(me.szModule, module_name) == 0) {
                    *base = (uintptr_t)me.modBaseAddr;
                    CloseHandle(snap);
                    return 0;
                }
            } while (Module32NextW(snap, &me));
        }

        // Not listed yet (loader still mapping modules) or snapshot raced.
        CloseHandle(snap);
        Sleep(100);
    }

    return ERROR_NOT_FOUND;
}

DWORD mc2_wait_boot_counter(HANDLE hProcess, uintptr_t address,
                            DWORD poll_ms, DWORD timeout_ms,
                            uint64_t *out_initial, uint64_t *out_final)
{
    uint8_t initial[8];
    SIZE_T got = 0;

    // First sample can be taken while the image is still being set up;
    // keep trying until the page is readable.
    DWORD elapsed = 0;
    for (;;) {
        if (ReadProcessMemory(hProcess, (LPCVOID)address, initial, sizeof(initial), &got)
            && got == sizeof(initial)) {
            break;
        }
        if (elapsed >= timeout_ms) {
            return ERROR_TIMEOUT;
        }
        Sleep(poll_ms);
        elapsed += poll_ms;
    }

    // Poll until the value changes twice. The counter is bumped every
    // GameTimeAccumulate_Update, so two distinct changes mean a
    // continuously ticking loop — and, by construction, the SecuROM startup
    // stub has finished (it runs before the first tick).
    uint8_t previous[8];
    memcpy(previous, initial, sizeof(previous));
    int changes = 0;

    for (;;) {
        if (elapsed >= timeout_ms) {
            return ERROR_TIMEOUT;
        }
        Sleep(poll_ms);
        elapsed += poll_ms;

        uint8_t current[8];
        got = 0;
        if (!ReadProcessMemory(hProcess, (LPCVOID)address, current, sizeof(current), &got)) {
            DWORD err = GetLastError();
            if (err != ERROR_PARTIAL_COPY) {
                return err;
            }
            continue; // transient: page not fully available yet
        }
        if (got != sizeof(current)) {
            continue;
        }
        if (memcmp(current, previous, sizeof(current)) != 0) {
            memcpy(previous, current, sizeof(previous));
            if (++changes >= 2) {
                if (out_initial) {
                    memcpy(out_initial, initial, sizeof(initial));
                }
                if (out_final) {
                    memcpy(out_final, previous, sizeof(previous));
                }
                return 0;
            }
        }
    }
}

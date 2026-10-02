// Remote-process helpers: verify the game module's load base and wait for
// the main-loop frame counter to start moving (boot complete marker).
#pragma once

#include <windows.h>
#include <stdint.h>

// Look up the load base of `module_name` (e.g. L"Mercenaries2.exe") in the
// process `hProcess`. Returns 0 and sets *base on failure.
// Retries briefly: the toolhelp snapshot can fail transiently (ERROR_BAD_LENGTH)
// right after CreateProcess while the remote loader is busy.
DWORD mc2_remote_module_base(HANDLE hProcess, const wchar_t *module_name,
                             uintptr_t *base, DWORD retries);

// Poll 8 bytes at `address` until they change TWICE (two distinct values after
// the initial sample), proving a continuously ticking counter — the game main
// loop is alive. Reads use ReadProcessMemory — cross-process *reads* are
// safe; all *writes* stay in-process in the carrier.
//
// Two changes rather than one: a one-off write during early init would pass a
// single-change test; a frame counter keeps moving. The observed samples are
// returned for logging — the values double as evidence that the address is
// really the per-frame counter (M1+ install gate).
//
// Returns 0 on success, non-zero on error/timeout.
DWORD mc2_wait_boot_counter(HANDLE hProcess, uintptr_t address,
                            DWORD poll_ms, DWORD timeout_ms,
                            uint64_t *out_initial, uint64_t *out_final);

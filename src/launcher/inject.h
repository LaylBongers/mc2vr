// DLL injection: CreateRemoteThread + LoadLibraryW. This is the chosen
// mechanism (docs/launcher_plan.md step 4); fallbacks (manual mapping,
// boot-time import stub) only if this proves flaky under Proton.
#pragma once

#include <windows.h>

// Inject `dll_path` (absolute) into `hProcess` by having the remote thread
// call LoadLibraryW on it. Blocks until the remote LoadLibraryW returns.
// Returns 0 on success, non-zero on failure.
DWORD mc2_inject_dll(HANDLE hProcess, const wchar_t *dll_path, DWORD timeout_ms);

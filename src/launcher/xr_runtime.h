// 32-bit OpenXR runtime selection check (docs/openxr_proton.md item 4).
// Linux (Wine/Proton) only; a no-op on native Windows.
//
// The game is 32-bit, so an OpenXR loader inside it (wineopenxr's i386 Unix
// side, used by in-process OpenXR) wants a 32-bit runtime. It looks for
// openxr/1/active_runtime.i686.json, then active_runtime.json, in
// $XDG_CONFIG_HOME (default $HOME/.config), each $XDG_CONFIG_DIRS entry
// (default /etc/xdg), then /etc; first hit wins. SteamVR can register only its
// 64-bit runtime there while shipping a 32-bit one next to it
// (steamxr_linux32.json); the 32-bit loader then fails ("wrong ELF class").
// This repeats the loader's search and says how to register the 32-bit
// runtime when it's missing. It can't fix it per process: Wine gives a child
// its parent's Unix environment, not the environment block passed to
// CreateProcess, so an XR_RUNTIME_JSON for the game alone never arrives.
#pragma once

#include <windows.h>

#include <stddef.h>

// TRUE: a 32-bit process finds a usable runtime (or there's nothing to check).
// FALSE: it would land on a 64-bit runtime; `fix` = the shell command that
// registers SteamVR's 32-bit runtime, or empty when none is known. Either way
// `*why` is a short static reason for the log.
BOOL xr_runtime_32_check(char *fix, size_t cap, const char **why);

// TRUE when running under Wine (ntdll exports wine_get_version).
BOOL xr_under_wine(void);

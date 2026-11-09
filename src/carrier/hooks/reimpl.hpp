// Reimplemented functions (src/carrier/reimpl/) replacing their originals.
//
// Each one is an inline hook on the original's entry. The table also gives
// each original's code range: hooks/site.hpp refuses other hooks inside a
// replaced original. With debug_reimpl_shadow=on nothing is replaced (the
// originals run; debug/shadow verifies ours), and nothing is refused.
#pragma once

#include <cstdint>

namespace mc2vr::hooks {

struct Replacement {
    const char *name;  // Ghidra name of the original
    uintptr_t lo, hi;  // the original's code, [lo, hi)
    void *ours;        // same convention as the original
};

// From conf, before any hook is installed.
void set_reimpl_shadow(bool on);

// The replaced original whose code contains va, or nullptr.
const Replacement *replaced_at(uintptr_t va);

// Installs every replacement (no-op with debug_reimpl_shadow=on).
void install_reimpl();

} // namespace mc2vr::hooks

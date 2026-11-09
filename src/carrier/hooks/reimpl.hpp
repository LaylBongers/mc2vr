// Reimplemented functions (src/carrier/reimpl/) replacing their originals.
//
// Each one is an inline hook on the original's entry, jumping to an entry
// stub that adapts the original's convention to ours (hooks/entry.hpp). The
// table also gives each original's code range: hooks/site.hpp refuses other
// hooks inside a replaced original.
#pragma once

#include <cstdint>

namespace mc2vr::hooks {

namespace entry_abi {
struct EntryInfo;
}

struct Replacement {
    const char *name;                    // Ghidra name of the original
    uintptr_t lo, hi;                    // the original's code, [lo, hi)
    const entry_abi::EntryInfo *entry;   // ours, behind the original's convention (hooks/entry.hpp)
};

// The replaced original whose code contains va, or nullptr.
const Replacement *replaced_at(uintptr_t va);

// Installs every replacement.
void install_reimpl();

} // namespace mc2vr::hooks

// Lti engine-core helpers (profiling names, debug output).
#pragma once

#include <cstdint>

#include "game/function.hpp"

namespace mc2vr::game {

// Lower-cased FNV-1a of a C string (0 for NULL/empty); profiler name ids.
inline constexpr Function<uint32_t(const char *name)> Lti_LazyNameHash{"Lti_LazyNameHash", 0x008244a0u, {EDX}};

// Debug printf (cdecl, varargs). Through a runtime-patched slot into the
// plaintext body. Declared per arity used.
inline constexpr Function<void(const char *fmt)> Lti_DebugPrintf{"Lti_DebugPrintf", 0x0074d520u, {STK}};
inline constexpr Function<void(const char *fmt, uint32_t a)> Lti_DebugPrintf_1{"Lti_DebugPrintf", 0x0074d520u,
                                                                               {STK, STK}};

} // namespace mc2vr::game

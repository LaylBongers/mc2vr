// Creating hooks on game code. Every hook in the carrier (hooks/ and debug/)
// goes through these, so the rules have one choke point (docs/hooks.md):
// they refuse any site inside the code of an original that a
// reimplementation replaces (hooks/reimpl.hpp). Such a hook would silently
// never fire; move it into the reimplementation as a direct call instead.
// Failures and refusals are logged with `what`.
#pragma once

#include <safetyhook.hpp>

#include <cstdint>

namespace mc2vr::hooks {

bool create_mid(SafetyHookMid &out, uintptr_t site, safetyhook::MidHookFn fn, const char *what);
bool create_inline(SafetyHookInline &out, uintptr_t target, void *fn, const char *what);

} // namespace mc2vr::hooks

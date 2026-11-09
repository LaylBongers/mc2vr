// See site.hpp.

#include "hooks/site.hpp"

#include <utility>

#include "hooks/reimpl.hpp"
#include "log.hpp"

namespace mc2vr::hooks {

namespace {

bool refuse_if_replaced(uintptr_t site, const char *what)
{
    const Replacement *r = replaced_at(site);
    if (r == nullptr) {
        return false;
    }
    MC2VR_LOG("hooks: REFUSED %s @ %08x: inside %s, which is reimplemented — move it into the "
              "reimplementation (src/carrier/reimpl/)",
              what, (unsigned)site, r->name);
    return true;
}

} // namespace

bool create_mid(SafetyHookMid &out, uintptr_t site, safetyhook::MidHookFn fn, const char *what)
{
    if (refuse_if_replaced(site, what)) {
        return false;
    }
    auto hook = SafetyHookMid::create(reinterpret_cast<uint8_t *>(site), fn);
    if (!hook) {
        MC2VR_LOG("hooks: %s @ %08x failed (error %u)", what, (unsigned)site, (unsigned)hook.error().type);
        return false;
    }
    out = std::move(*hook);
    return true;
}

bool create_inline(SafetyHookInline &out, uintptr_t target, void *fn, const char *what)
{
    if (refuse_if_replaced(target, what)) {
        return false;
    }
    auto hook = SafetyHookInline::create(reinterpret_cast<uint8_t *>(target), reinterpret_cast<uint8_t *>(fn));
    if (!hook) {
        MC2VR_LOG("hooks: %s @ %08x failed (error %u)", what, (unsigned)target, (unsigned)hook.error().type);
        return false;
    }
    out = std::move(*hook);
    return true;
}

} // namespace mc2vr::hooks

#include "hooks.hpp"

#include "log.hpp"

namespace mc2vr::hooks {

bool install()
{
    // M0: no hooks yet. The toolchain building SafetyHook into this DLL is
    // itself an M0 milestone ("toolchain builds"); first real hook lands in
    // M1 (GameShell_FrameTick).
    MC2VR_LOG("hooks: M0 — nothing to install yet (first hook lands in M1)");
    return true;
}

} // namespace mc2vr::hooks

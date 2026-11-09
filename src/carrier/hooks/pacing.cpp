// See pacing.hpp.

#include "hooks/pacing.hpp"

#include "game/addresses.h"
#include "hooks/site.hpp"
#include "log.hpp"
#include "vr/pacing.hpp"

namespace mc2vr::hooks {

namespace {

SafetyHookMid g_drain_mid;

// At BeginSubmit's drain check: EAX = first GetData result (1 = GPU busy).
// Writing EAX = 0 reports "done": skips the spin, takes the normal exit.
void drain_check_midhook(safetyhook::Context &ctx)
{
    if (ctx.eax != 1) {
        return;  // the GPU is already idle
    }
    if (pacing::on_drain_check_busy()) {
        ctx.eax = 0;
    }
}

} // namespace

void install_pacing()
{
    if (!create_mid(g_drain_mid, MC2_LTI_BEGINSUBMIT_DRAIN_CHECK, drain_check_midhook,
                    "pace BeginSubmit drain MidHook")) {
        MC2VR_LOG("pace: BeginSubmit drain MidHook missing — replay pass keeps the engine's "
                  "GPU drain");
        return;
    }
    MC2VR_LOG("pace: BeginSubmit drain check hooked @ %p (replay pass %s the GPU drain)",
              (void *)MC2_LTI_BEGINSUBMIT_DRAIN_CHECK,
              pacing::keep_replay_drain() ? "keeps" : "skips");
}

} // namespace mc2vr::hooks

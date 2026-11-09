// See submit_to_gpu.hpp.

#include "hooks/submit_to_gpu.hpp"

#include "game/addresses.h"
#include "hooks/site.hpp"
#include "log.hpp"
#include "vr/eye_replay.hpp"

namespace mc2vr::hooks {

namespace {

SafetyHookInline g_submit_hook;

// void(void): cdecl and stdcall are identical.
void call_original() { g_submit_hook.call<void>(); }

void submit_to_gpu_hook() { eye_replay::run_frame(&call_original); }

} // namespace

void install_submit_to_gpu()
{
    if (!create_inline(g_submit_hook, MC2_PGPRIMITIVE_SUBMITTOGPU, reinterpret_cast<void *>(&submit_to_gpu_hook),
                       "SubmitToGPU stereo replay")) {
        MC2VR_LOG("stream: warning — NO second (RIGHT eye) pass this run");
        return;
    }
    MC2VR_LOG("stream: installed SubmitToGPU InlineHook @ %p (two-pass stereo replay)",
              (void *)MC2_PGPRIMITIVE_SUBMITTOGPU);
}

} // namespace mc2vr::hooks

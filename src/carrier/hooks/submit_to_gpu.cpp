// See submit_to_gpu.hpp.

#include "hooks/submit_to_gpu.hpp"

#include <safetyhook.hpp>

#include <utility>

#include "game/addresses.h"
#include "hooks/reimpl.hpp"
#include "log.hpp"
#include "reimpl/render/submit_to_gpu.hpp"
#include "vr/eye_replay.hpp"

namespace mc2vr::hooks {

namespace {

SafetyHookInline g_submit_hook;
void (*g_pass1)() = &call_submit_to_gpu_original;
void (*g_pass2)() = &call_submit_to_gpu_original;

void submit_to_gpu_hook() { eye_replay::run_frame(g_pass1, g_pass2); }

} // namespace

// void(void): cdecl and stdcall are identical.
void call_submit_to_gpu_original() { g_submit_hook.call<void>(); }

void set_submit_passes(void (*pass1)(), void (*pass2)())
{
    g_pass1 = pass1;
    g_pass2 = pass2;
}

void install_submit_to_gpu()
{
    if (reimpl_mode() == ReimplMode::Replace) {
        set_submit_passes(&reimpl::PgPrimitive_SubmitToGPU, &reimpl::PgPrimitive_SubmitToGPU);
    }
    // Created directly: the function is in the replaced-range table
    // (hooks/reimpl.cpp), and this hook is the replacement.
    auto hook = SafetyHookInline::create(reinterpret_cast<uint8_t *>(MC2_PGPRIMITIVE_SUBMITTOGPU),
                                         reinterpret_cast<uint8_t *>(&submit_to_gpu_hook));
    if (!hook) {
        MC2VR_LOG("stream: warning — SubmitToGPU InlineHook failed (error %u): NO second (RIGHT eye) pass this run",
                  (unsigned)hook.error().type);
        return;
    }
    g_submit_hook = std::move(*hook);
    MC2VR_LOG("stream: installed SubmitToGPU InlineHook @ %p (two-pass stereo replay, %s)",
              (void *)MC2_PGPRIMITIVE_SUBMITTOGPU,
              reimpl_mode() == ReimplMode::Replace ? "reimplementation" : "original");
}

} // namespace mc2vr::hooks

// See render_outputs.hpp.

#include "hooks/render_outputs.hpp"

#include <cstdint>

#include "debug/rt_probe.hpp"
#include "game/addresses.h"
#include "hooks/site.hpp"
#include "log.hpp"
#include "vr/render_outputs.hpp"

namespace mc2vr::hooks {

namespace {

SafetyHookInline g_setup_hook;
SafetyHookMid g_restore_mid;

// ---- SetupRenderTargets wrap (thiscall, 1 stack arg; caller 0x004c0050:
// `push flag; call [vtbl+0x18]; ret`). fastcall = ECX + EDX + stack arg with
// callee cleanup — EDX is passed through untouched.

void __fastcall setup_hook(void *self, void *edx, uint32_t flag)
{
    rt_probe::on_setup_enter(self);
    g_setup_hook.fastcall<void>(self, edx, flag);
    render_outputs::on_setup_done();
}

// RenderSystem_CheckDevice, right after the engine's device-restore calls:
// the table now names the NEW swapchain surfaces — retake before the next
// frame renders (otherwise the next Present's invariant check would, one
// frame late).
void restore_midhook(safetyhook::Context &)
{
    render_outputs::on_device_restored();
}

} // namespace

void install_render_outputs()
{
    if (!create_inline(g_setup_hook, MC2_SETUPRENDERTARGETS, (void *)&setup_hook,
                       "SetupRenderTargets wrap")) {
        MC2VR_LOG("outputs: FATAL — SetupRenderTargets wrap missing — takeover falls back to "
                  "the per-Present check");
    } else {
        MC2VR_LOG("outputs: wrapped SetupRenderTargets @ %p", (void *)MC2_SETUPRENDERTARGETS);
    }

    if (!create_mid(g_restore_mid, MC2_CHECKDEVICE_RESTORED, restore_midhook,
                    "device-restore MidHook")) {
        MC2VR_LOG("outputs: device-restore MidHook missing — retake falls back to the next "
                  "Present");
    } else {
        MC2VR_LOG("outputs: device-restore MidHook @ %p", (void *)MC2_CHECKDEVICE_RESTORED);
    }
}

} // namespace mc2vr::hooks

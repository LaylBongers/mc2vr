// See cull_frustum.hpp.

#include "hooks/cull_frustum.hpp"

#include "game/addresses.h"
#include "hooks/site.hpp"
#include "log.hpp"
#include "vr/cull_frustum.hpp"

namespace mc2vr::hooks {

namespace {

SafetyHookMid g_mid, g_clear_mid;

// ViewContext_BuildCameraConstants, right after the tans were stored to
// ctx+0x30/+0x34: EBX = the view render-ctx, XMM2 = tanH, XMM0 = tanV —
// every later frustum product of the build derives from these two registers.
void tans_midhook(safetyhook::Context &ctx)
{
    float th = ctx.xmm2.f32[0];
    float tv = ctx.xmm0.f32[0];
    if (cull_frustum::on_tans_ready(ctx.ebx, th, tv)) {
        ctx.xmm2.f32[0] = th;
        ctx.xmm0.f32[0] = tv;
    }
}

// Third-person camera clearance (CamCtrl_NearPlaneClearance, camera thread): runs right after
// the camera copied the main view's frustum struct onto its stack, at
// ESP + MC2_CAMCLEAR_COPY_ESP_OFF.
void camclear_midhook(safetyhook::Context &ctx)
{
    cull_frustum::on_camera_frustum_copied((float *)(ctx.esp + MC2_CAMCLEAR_COPY_ESP_OFF));
}

} // namespace

void install_cull_frustum()
{
    if (!create_mid(g_mid, MC2_VCCAM_TANS_READY, tans_midhook, "cullfov builder MidHook")) {
        MC2VR_LOG("cullfov: builder MidHook missing — culling keeps the game frustum");
        return;
    }
    MC2VR_LOG("cullfov: installed builder MidHook @ %p", (void *)MC2_VCCAM_TANS_READY);

    if (!create_mid(g_clear_mid, MC2_CAMCLEAR_FRUSTUM_COPIED, camclear_midhook,
                    "cullfov camera-clearance MidHook")) {
        MC2VR_LOG("cullfov: camera-clearance MidHook missing — the camera sees the HMD near "
                  "plane (pull-in)");
        return;
    }
    MC2VR_LOG("cullfov: installed camera-clearance MidHook @ %p",
              (void *)MC2_CAMCLEAR_FRUSTUM_COPIED);
}

} // namespace mc2vr::hooks

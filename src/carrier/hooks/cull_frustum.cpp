// See cull_frustum.hpp.

#include "hooks/cull_frustum.hpp"

#include "game/addresses.h"
#include "hooks/site.hpp"
#include "log.hpp"
#include "vr/cull_frustum.hpp"

namespace mc2vr::hooks {

namespace {

SafetyHookMid g_clear_mid, g_copy_mid;

// FUN_00480640, right after it copied its source view's camera into a stack
// camera object: EDI = the stack camera, ESI = the source view ctx (whose
// camera object is at +0x28).
void view_copy_midhook(safetyhook::Context &ctx)
{
    const uint32_t *const src = *reinterpret_cast<const uint32_t *const *>(ctx.esi + 0x28);
    cull_frustum::on_view_camera_copied(reinterpret_cast<const uint32_t *>(ctx.edi), src);
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
    // The frustum widening itself is a direct call in the reimplemented
    // ViewContext_BuildCameraConstants (reimpl/render/build_camera_constants.hpp).
    if (!create_mid(g_copy_mid, MC2_VIEW_CAMERA_COPIED, view_copy_midhook, "cullfov view-camera-copy MidHook")) {
        MC2VR_LOG("cullfov: view-camera-copy MidHook missing — FUN_00480640's view keeps the game frustum");
    } else {
        MC2VR_LOG("cullfov: installed view-camera-copy MidHook @ %p", (void *)MC2_VIEW_CAMERA_COPIED);
    }
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

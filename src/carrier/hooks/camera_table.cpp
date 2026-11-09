// See camera_table.hpp.

#include "hooks/camera_table.hpp"

#include "game/addresses.h"
#include "hooks/site.hpp"
#include "log.hpp"
#include "vr/camera_table.hpp"

namespace mc2vr::hooks {

namespace {

SafetyHookMid g_mid;

// Right after the fill's Matrix_Copy3x4 call (CameraTable_FillFromPose).
// EAX = Matrix_Copy3x4 destination = the just-filled entry+0x10 (the fill loop
// is the function's only caller, so EAX is always inside g_CameraTable).
// ESI = the 0x620 slot base (live across the hooked call — the fill itself
// uses it after us).
void fill_midhook(safetyhook::Context &ctx) { camera_table::on_entry_filled(ctx.eax, ctx.esi); }

} // namespace

void install_camera_table()
{
    if (!create_mid(g_mid, MC2_CAMTABLE_FILL_COPY_END, fill_midhook,
                    "camtable fill-site MidHook")) {
        MC2VR_LOG("camtable: fill-site MidHook missing — union injection stays idle");
        return;
    }
    MC2VR_LOG("camtable: installed fill-site MidHook @ %p (g_CameraTable union injection)",
              (void *)MC2_CAMTABLE_FILL_COPY_END);
}

} // namespace mc2vr::hooks

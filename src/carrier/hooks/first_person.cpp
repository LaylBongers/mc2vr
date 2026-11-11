// See first_person.hpp.

#include "hooks/first_person.hpp"

#include "game/addresses.h"
#include "hooks/site.hpp"
#include "log.hpp"
#include "vr/first_person.hpp"

namespace mc2vr::hooks {

namespace {

SafetyHookMid g_mid;

// CamHuman_Update, the instruction that reads the final matrix's translation
// for the working pose (MC2_CAMHUMAN_FINAL_MATRIX). EBX = the camera; the
// matrix is the stack local at ESP + MC2_CAMHUMAN_FINAL_MATRIX_ESP_OFF.
void final_matrix_midhook(safetyhook::Context &ctx)
{
    first_person::on_human_camera_matrix(reinterpret_cast<uint8_t *>(ctx.ebx),
                                         reinterpret_cast<float *>(ctx.esp + MC2_CAMHUMAN_FINAL_MATRIX_ESP_OFF));
}

} // namespace

void install_first_person()
{
    if (!first_person::enabled()) {
        return;  // off: CamHuman_Update stays untouched
    }
    if (!create_mid(g_mid, MC2_CAMHUMAN_FINAL_MATRIX, final_matrix_midhook, "fp: CamHuman final-matrix MidHook")) {
        MC2VR_LOG("fp: final-matrix MidHook missing — first person stays off");
        return;
    }
    MC2VR_LOG("fp: installed CamHuman final-matrix MidHook @ %p", (void *)MC2_CAMHUMAN_FINAL_MATRIX);
}

} // namespace mc2vr::hooks

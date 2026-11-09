// See view_rewrite.hpp.

#include "hooks/view_rewrite.hpp"

#include <cstdint>

#include "game/addresses.h"
#include "hooks/reimpl.hpp"
#include "hooks/site.hpp"
#include "log.hpp"
#include "vr/view_rewrite.hpp"

namespace mc2vr::hooks {

namespace {

SafetyHookMid g_vcd_mid;

// MidHook @ MC2_VCD_UPLOAD_CMP (`cmp [edi+0xd8],0` at the upload gate):
// EDI = the CURRENT technique object; [esp+0x18] (MC2_VCD_GATE_REC_SLOT) =
// this pass's viewContext record pointer.
void vcd_midhook(safetyhook::Context &ctx)
{
    const uintptr_t tech = (uintptr_t)ctx.edi;
    if (tech == 0) {
        return;
    }
    view_rewrite::on_upload_gate(
        tech, *(const uintptr_t *)(uintptr_t)(ctx.esp + MC2_VCD_GATE_REC_SLOT));
}

} // namespace

void install_view_rewrite()
{
    if (replaced_at(MC2_VCD_UPLOAD_CMP) != nullptr) {
        // Our PgPrimitive_SubmitToGPU calls view_rewrite::on_upload_gate itself.
        MC2VR_LOG("view: upload gate folded into the PgPrimitive_SubmitToGPU reimplementation");
        return;
    }
    // Plaintext .text MidHook (same pattern as the debug MidHooks). Failure
    // is non-fatal: the map stays INVALID and the rewrite idles.
    if (!create_mid(g_vcd_mid, MC2_VCD_UPLOAD_CMP, vcd_midhook, "view upload-gate MidHook")) {
        MC2VR_LOG("view: upload-gate MidHook missing — rewrite stays idle");
        return;
    }
    MC2VR_LOG("view: installed upload-gate MidHook @ %p", (void *)MC2_VCD_UPLOAD_CMP);
}

} // namespace mc2vr::hooks

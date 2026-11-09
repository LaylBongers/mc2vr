// See hud.hpp.

#include "hooks/hud.hpp"

#include "debug/build_census.hpp"
#include "game/addresses.h"
#include "game/pg_primitive.hpp"
#include "hooks/site.hpp"
#include "log.hpp"
#include "vr/hud.hpp"
#include "vr/world_markers.hpp"

namespace mc2vr::hooks {

namespace {

SafetyHookMid g_publish_mid;
SafetyHookMid g_quad_mid;

// Hud_DrawQuad about to emit its quad (vr/world_markers): the record it
// writes is the current write count.
void quad_emit(safetyhook::Context &ctx)
{
    const float cy = ctx.xmm6.f32[0];
    const float y0 = *reinterpret_cast<const float *>(ctx.esp + 0x10 + 0x4);
    const float depth = *reinterpret_cast<const float *>(ctx.ebp + 8);
    world_markers::on_quad(game::g_PrimWriteCount::get(), ctx.xmm5.f32[0], cy, cy - y0, depth);
}

void publish_return(safetyhook::Context &)
{
    hud::on_publish();
    world_markers::on_publish();
    build_census::on_publish_return();  // debug tap
}

} // namespace

void install_hud()
{
    if (!create_mid(g_publish_mid, MC2_PUBLISH_RETURN, publish_return, "PublishFrame return (HUD ranges)")) {
        MC2VR_LOG("hud: PublishFrame-return hook missing — the HUD stays in the eye images");
    }
    if (!create_mid(g_quad_mid, MC2_HUD_DRAWQUAD_EMIT, quad_emit, "Hud_DrawQuad emit (world markers)")) {
        MC2VR_LOG("markers: Hud_DrawQuad hook missing — world markers stay mono");
    }
}

} // namespace mc2vr::hooks

// See hud.hpp.

#include "hooks/hud.hpp"

#include "debug/build_census.hpp"
#include "game/addresses.h"
#include "game/pg_primitive.hpp"
#include "game/render_shell.hpp"
#include "hooks/site.hpp"
#include "log.hpp"
#include "vr/hud.hpp"
#include "vr/world_markers.hpp"

namespace mc2vr::hooks {

namespace {

SafetyHookMid g_publish_mid;
SafetyHookMid g_quad_mid;
SafetyHookMid g_layout_mid;
SafetyHookMid g_gui_w_mid, g_gui_h_mid;
SafetyHookMid g_world_scale_mid, g_pinned_scale_mid;
SafetyHookMid g_offscreen_wh_mid, g_ind_wh_a_mid, g_ind_wh_b_mid;
SafetyHookMid g_display_rect_mid;

// The HUD layer's screen policy (vr/hud.hpp, docs/reverse_engineering/view_blocks.md).
// Handlers write integers / float bit patterns only: several sites have live
// x87 state, and float math in a handler may use the x87 stack.

// The two GUI readers that draw into the eye image: the stock UI scale back.
void eye_scale(safetyhook::Context &ctx, uintptr_t block_offset)
{
    if (!hud::layout_16x9()) return;
    const uint32_t bits = hud::eye_scale_bits((uint32_t)(block_offset / game::kRenderShellViewStride));
    if (bits != 0) ctx.xmm0.u32[0] = bits;
}
void world_markers_scale(safetyhook::Context &ctx)
{
    eye_scale(ctx, ctx.ecx);  // ECX = view index * 0xe80
}
void pinned_markers_scale(safetyhook::Context &ctx)
{
    eye_scale(ctx, ctx.edi);  // EDI = view index * 0xe80
}

// HUD step 4 (off-screen indicators, build thread): the layer's W/H.
void offscreen_wh(safetyhook::Context &ctx)
{
    if (!hud::layout_16x9()) return;
    ctx.eax = hud::kLayoutW;
    ctx.ecx = hud::kLayoutH;
}
void ind_wh_a(safetyhook::Context &ctx)
{
    if (!hud::layout_16x9()) return;
    ctx.xmm5.u32[0] = hud::kLayoutHBits;
    ctx.xmm2.u32[0] = hud::kLayoutWBits;
}
void ind_wh_b(safetyhook::Context &ctx)
{
    if (!hud::layout_16x9()) return;
    ctx.edi = hud::kLayoutW;
    ctx.eax = hud::kLayoutH;
}

// GuiRenderer_BeginDisplay: the view rect it copied into the renderer
// (EDI + 0x6c048..+0x6c054) is the layer's screen.
void display_rect(safetyhook::Context &ctx)
{
    if (!hud::layout_16x9()) return;
    uint32_t *const rect = reinterpret_cast<uint32_t *>(ctx.edi + 0x6c048);
    rect[0] = 0;
    rect[1] = 0;
    rect[2] = hud::kLayoutW;
    rect[3] = hud::kLayoutH;
}

// GuiMgr_OnScreenResize, the view's W (ECX) / H (EDI) just loaded: the HUD
// layer's screen, for the GUI screen info and the scripts' resize event.
void gui_resize_w(safetyhook::Context &ctx)
{
    if (hud::layout_16x9()) ctx.ecx = hud::kLayoutW;
}
void gui_resize_h(safetyhook::Context &ctx)
{
    if (hud::layout_16x9()) ctx.edi = hud::kLayoutH;
}

// Widget_CorrectForResolution, the view's W/H just loaded (update thread):
// the HUD layer's 16:9 screen instead (vr/hud.hpp).
void layout_size(safetyhook::Context &ctx)
{
    if (hud::layout_16x9()) {
        ctx.xmm0.u32[0] = hud::kLayoutWBits;
        ctx.xmm5.u32[0] = hud::kLayoutHBits;
    }
}

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
    if (!create_mid(g_layout_mid, MC2_WIDGET_LAYOUT_SIZE, layout_size, "Widget_CorrectForResolution (HUD layout)")) {
        MC2VR_LOG("hud: widget-layout hook missing — the HUD layer keeps the eye-image layout");
    }
    if (!create_mid(g_gui_w_mid, MC2_GUI_RESIZE_W, gui_resize_w, "GuiMgr_OnScreenResize W (HUD layout)") ||
        !create_mid(g_gui_h_mid, MC2_GUI_RESIZE_H, gui_resize_h, "GuiMgr_OnScreenResize H (HUD layout)")) {
        MC2VR_LOG("hud: GUI screen-info hooks missing — the GUI screen info and the scripts see the eye-image size");
    }
    // The UI scale is set by our RenderShell_SetupViewports (reimpl); these
    // give the two eye-image readers the stock scale back.
    if (!create_mid(g_world_scale_mid, MC2_WORLDMARKERS_SCALE, world_markers_scale, "Hud_DrawWorldMarkers scale (HUD layout)") ||
        !create_mid(g_pinned_scale_mid, MC2_PINNEDMARKERS_SCALE, pinned_markers_scale, "Hud_DrawPinnedMarkers scale (HUD layout)")) {
        MC2VR_LOG("hud: marker UI-scale hooks missing — world / pinned markers draw at the HUD layer's scale");
    }
    if (!create_mid(g_offscreen_wh_mid, MC2_OFFSCREEN_IND_WH, offscreen_wh, "Hud_DrawOffscreenIndicators W/H (HUD layout)") ||
        !create_mid(g_ind_wh_a_mid, MC2_IND_NDC_WH_A, ind_wh_a, "Hud_IndicatorNdcToPixels W/H A (HUD layout)") ||
        !create_mid(g_ind_wh_b_mid, MC2_IND_NDC_WH_B, ind_wh_b, "Hud_IndicatorNdcToPixels W/H B (HUD layout)")) {
        MC2VR_LOG("hud: off-screen indicator hooks missing — they keep the eye-image screen");
    }
    if (!create_mid(g_display_rect_mid, MC2_BEGINDISPLAY_VIEWRECT, display_rect, "GuiRenderer_BeginDisplay view rect (HUD layout)")) {
        MC2VR_LOG("hud: BeginDisplay view-rect hook missing — Scaleform keeps the eye-image view rect");
    }
}

} // namespace mc2vr::hooks

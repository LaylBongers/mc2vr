// RenderShell_EndSubView — see end_sub_view.hpp. Follows the original
// (0x0062ee20-0x0062eebe) step by step; deviations: none intended.

#include "reimpl/render/end_sub_view.hpp"

#include "game/hud.hpp"
#include "game/render_shell.hpp"
#include "vr/hud.hpp"

namespace mc2vr::reimpl {

using namespace game;

void RenderShell_EndSubView(uint32_t phase, uint32_t view)
{
    if (phase != 2) {
        return;
    }

    // ---- 1. post chain + composite 0x62ee2d-0x62ee5e ----
    uint8_t *const shell = g_RenderShellPtrB::get();
    if (shell[view * kRenderShellViewStride + kRenderShellViewPostFlag] != 0) {
        uint8_t *const cam = g_GlobalCamCtx::get();
        if (cam[0x108] != 0) {
            uint8_t *const input = *reinterpret_cast<uint8_t **>(cam + 0x104);
            if (input != nullptr) {
                PostFx_ChainAndComposite(shell, input);
            }
        }
    }

    // ---- 2. world markers 0x62ee63-0x62ee7e ----
    const bool hud_on = DAT_01174cdc::get()[kHudEnabledFlag] != 0;
    auto context = []() { return *reinterpret_cast<void **>(g_GuiMgr::get() + kHudContext); };
    hud::begin_markers();
    if (hud_on) {
        if (void *const ctx = context()) {
            Hud_DrawWorldMarkers(ctx, view);
        }
    }
    hud::end_markers();

    // ---- 3-6. the HUD and the GUI 0x62ee83-0x62eeb9 ----
    hud::begin_hud();
    LtiRenderer_Slot14(g_RenderShellPtrB::get());
    if (hud_on) {
        if (void *const ctx = context()) {
            Hud_DrawOffscreenIndicators(ctx, view);
        }
    }
    Bink_DrawOverlays(view);
    GuiMgr_DrawView(g_GuiMgr::get(), view);
    hud::end_hud();
}

} // namespace mc2vr::reimpl

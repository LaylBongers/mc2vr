// See install.hpp. One line per hook group; site details in each file.

#include "hooks/install.hpp"

#include "hooks/adaptation.hpp"
#include "hooks/camera_table.hpp"
#include "hooks/cull_frustum.hpp"
#include "hooks/device.hpp"
#include "hooks/frame_tick.hpp"
#include "hooks/hud.hpp"
#include "hooks/reimpl.hpp"
#include "hooks/render_outputs.hpp"
#include "hooks/render_size.hpp"

namespace mc2vr::hooks {

void install_attach()
{
    install_reimpl();         // first: the guard refuses hooks inside replaced originals.
                              // SubmitToGPU's replacement renders the stereo frame and
                              // calls view_rewrite's upload gate itself; BeginSubmit's
                              // calls the replay-pass drain skip.
    install();                // GameShell_FrameTick: frame counter, timing
    install_post_update();    // FrameTick slot-5 site: host-event drain
    install_camera_table();   // HMD pose injection at g_CameraTable
    install_cull_frustum();   // HMD cull frustum
    install_render_outputs(); // carrier-owned outputs: setup wrap + restore
    install_render_size();    // render_size=WxH policy
    install_adaptation();     // exposure continuity across render-size switches
    install_hud();            // HUD layer: record ranges published with the list
    // Device-hook chain root (vsync patch + device VmtHook at creation):
    // RenderSystem_Init calls Direct3DCreate9 during boot, so this must be in
    // place before the game resumes.
    device::install_d3d9_gate();
}

} // namespace mc2vr::hooks

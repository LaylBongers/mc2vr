// See install.hpp. One line per hook group; site details in each file.

#include "hooks/install.hpp"

#include "hooks/adaptation.hpp"
#include "hooks/camera_table.hpp"
#include "hooks/cull_frustum.hpp"
#include "hooks/device.hpp"
#include "hooks/frame_tick.hpp"
#include "hooks/occluder_boxes.hpp"
#include "hooks/pacing.hpp"
#include "hooks/reimpl.hpp"
#include "hooks/render_outputs.hpp"
#include "hooks/render_size.hpp"
#include "hooks/submit_to_gpu.hpp"
#include "hooks/view_rewrite.hpp"

namespace mc2vr::hooks {

void install_attach()
{
    install_reimpl();         // first: the guard refuses hooks inside replaced originals
    install();                // GameShell_FrameTick: frame counter, timing
    install_post_update();    // FrameTick slot-5 site: host-event drain
    install_pacing();         // replay-pass GPU drain skip (LtiRenderer_BeginSubmit)
    install_view_rewrite();   // per-eye camera upload gate
    install_camera_table();   // HMD pose injection at g_CameraTable
    install_cull_frustum();   // HMD cull frustum
    install_occluder_boxes(); // occlusion-query boxes off (VR smearing)
    install_submit_to_gpu();  // two-pass stereo replay
    install_render_outputs(); // carrier-owned outputs: setup wrap + restore
    install_render_size();    // render_size=WxH policy
    install_adaptation();     // exposure continuity across render-size switches
    // Device-hook chain root (vsync patch + device VmtHook at creation):
    // RenderSystem_Init calls Direct3DCreate9 during boot, so this must be in
    // place before the game resumes.
    device::install_d3d9_gate();
}

} // namespace mc2vr::hooks

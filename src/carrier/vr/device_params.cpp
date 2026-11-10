#include "vr/device_params.hpp"

#include <cstdint>
#include <cstring>

#include "log.hpp"

namespace mc2vr::device_params {

namespace {

// ---- Pacing: vsync=off (CreateDevice/Reset interval patch) ----------
// The frame's two Presents (present-prev at each BeginSubmit) each block on
// a 60 Hz vsync slot -> ~30 Hz game (live-log-proven). vsync=off forces
// D3DPRESENT_INTERVAL_IMMEDIATE so the game renders as fast as the passes
// actually cost; the OpenXR host/runtime paces the HMD.
bool g_present_immediate = false;

} // namespace

bool set_vsync(const char *value)
{
    if (strcmp(value, "on") == 0) {
        g_present_immediate = false;
    } else if (strcmp(value, "off") == 0) {
        g_present_immediate = true;
    } else {
        return false;
    }
    MC2VR_LOG("vsync: vsync=%s (%s)", value,
              g_present_immediate
                  ? "CreateDevice/Reset force D3DPRESENT_INTERVAL_IMMEDIATE"
                  : "game's own present params untouched");
    return true;
}

// ---- Device-parameter policy ----------------------------------------------------
// Applied to a COPY of the engine's D3DPRESENT_PARAMETERS: the engine keeps
// its stored copy exactly as it asked (RenderSystem_ApplySettings compares
// that stored copy field by field — MultiSampleType @+0x10, interval @+0x34 —
// against its settings, so in-place patches read as "settings changed").
//   - vsync=off: PresentationInterval = IMMEDIATE.
//   - The swapchain is only the carrier's monitor mirror, so it runs without
//     MSAA; the engine-requested MSAA moves to the carrier's front/depth
//     surfaces (render_outputs).
D3DPRESENT_PARAMETERS apply_param_policy(const D3DPRESENT_PARAMETERS &engine, const char *what)
{
    D3DPRESENT_PARAMETERS p = engine;
    if (g_present_immediate && p.PresentationInterval != D3DPRESENT_INTERVAL_IMMEDIATE) {
        MC2VR_LOG("vsync: %s params: %ux%u windowed=%u swapeffect=%u refresh=%u "
                  "interval=0x%08x -> forcing IMMEDIATE",
                  what, p.BackBufferWidth, p.BackBufferHeight, p.Windowed, p.SwapEffect,
                  p.FullScreen_RefreshRateInHz, p.PresentationInterval);
        p.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    }
    // The game's AA option only ever multisamples the swapchain, i.e. the final
    // composite + HUD (the 3D scene renders into a non-MSAA texture — a port
    // bug), and with it on, depth-tested composited icons fail to draw (also
    // without the mod). The request is dropped: the swapchain only carries the
    // monitor mirror. Scene MSAA is the carrier's own (vr/msaa.hpp,
    // docs/msaa.md).
    if (p.MultiSampleType != D3DMULTISAMPLE_NONE) {
        MC2VR_LOG("outputs: %s swapchain MSAA %u/%lu requested -> none (ignored: composite/HUD "
                  "only, hides composited icons)",
                  what, (unsigned)p.MultiSampleType, (unsigned long)p.MultiSampleQuality);
        p.MultiSampleType = D3DMULTISAMPLE_NONE;
        p.MultiSampleQuality = 0;
    }
    return p;
}

// D3D may update the params it was given (BackBufferCount/Format, …): mirror
// those back, keeping the policy fields exactly as the engine requested.
void copy_back_params(D3DPRESENT_PARAMETERS *engine, const D3DPRESENT_PARAMETERS &used)
{
    const D3DMULTISAMPLE_TYPE ms = engine->MultiSampleType;
    const DWORD msq = engine->MultiSampleQuality;
    const UINT interval = engine->PresentationInterval;
    *engine = used;
    engine->MultiSampleType = ms;
    engine->MultiSampleQuality = msq;
    engine->PresentationInterval = interval;
}

DWORD apply_behavior_policy(DWORD behavior_flags)
{
    MC2VR_LOG("D3D: CreateDevice behavior flags 0x%08lx (MULTITHREADED %s by the game)",
              (unsigned long)behavior_flags,
              (behavior_flags & D3DCREATE_MULTITHREADED) ? "already set" : "not set");
    if (!(behavior_flags & D3DCREATE_MULTITHREADED)) {
        // The OpenXR session thread polls the eye pairs' event queries
        // (eye_share.cpp).
        behavior_flags |= D3DCREATE_MULTITHREADED;
        MC2VR_LOG("D3D: CreateDevice + D3DCREATE_MULTITHREADED (the session thread polls eye-pair fences)");
    }
    return behavior_flags;
}

// ---- Present-parameter logging (main thread, first Present) ---------------

void log_present_params(IDirect3DDevice9 *device)
{
    IDirect3DSwapChain9 *swapchain = nullptr;
    HRESULT hr = device->GetSwapChain(0, &swapchain);
    if (FAILED(hr) || !swapchain) {
        MC2VR_LOG("D3D: GetSwapChain(0) failed hr=%08lx", (unsigned long)hr);
        return;
    }

    D3DPRESENT_PARAMETERS pp = {};
    hr = swapchain->GetPresentParameters(&pp);

    // Release the AddRef from GetSwapChain.
    swapchain->Release();

    if (FAILED(hr)) {
        MC2VR_LOG("D3D: GetPresentParameters failed hr=%08lx", (unsigned long)hr);
        return;
    }

    MC2VR_LOG("D3D: present params: %ux%u fmt=%u count=%u windowed=%u swapeffect=%u "
              "refresh=%u interval=0x%08x hdeviceWindow=%p",
              pp.BackBufferWidth, pp.BackBufferHeight, pp.BackBufferFormat,
              pp.BackBufferCount, pp.Windowed, pp.SwapEffect,
              pp.FullScreen_RefreshRateInHz, pp.PresentationInterval,
              pp.hDeviceWindow);
}

} // namespace mc2vr::device_params

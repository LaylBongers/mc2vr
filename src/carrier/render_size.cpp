// Render-size policy. See render_size.hpp.

#include "render_size.hpp"

#include <safetyhook.hpp>

#include <windows.h>
#include <d3d9.h> // types only

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "game_addresses.h"
#include "hooks.hpp"
#include "ipc.hpp"
#include "log.hpp"
#include "mc2vr_ipc.h"
#include "render_outputs.hpp"
#include "debug/rt_probe.hpp"

namespace mc2vr::render_size {

namespace {

// Configured size (render_size=WxH; 0 = off) and the CURRENT target the
// hooks apply (0 = stock: render size = window size). With
// render_size_follow_hmd=on (default) the current target is the configured
// size only while the HMD is in use, else stock — so with the HMD on standby
// the game renders exactly as unmodded (mouse mapping included).
uint32_t g_cfg_w = 0, g_cfg_h = 0;
uint32_t g_w = 0, g_h = 0;
bool g_true_aspect = true;
bool g_follow_hmd = true;

// Switch state (main thread): a pending target, applied when the engine's
// CheckDevice consumes the forced device-lost signal.
bool g_switch_armed = false;
uint32_t g_pending_w = 0, g_pending_h = 0;
bool g_hmd_active_seen = false;      // debounced state
bool g_hmd_candidate = false;        // raw state being debounced
LARGE_INTEGER g_candidate_since = {};
LARGE_INTEGER g_qpc_freq = {};
uint64_t g_switches = 0;
constexpr double HMD_DEBOUNCE_SEC = 1.0;

// Targets established by the last OnScreenResize epilogue / setup.
float g_target_aspect = 0.0f;
float g_target_mouse_x = 0.0f, g_target_mouse_y = 0.0f;
bool g_mouse_target_valid = false;

// Window counters / one-shot logs (main thread).
uint64_t g_osr_rewrites = 0, g_setup_rewrites = 0, g_aspect_reapplies = 0;
uint64_t g_mouse_reapplies = 0;
uint32_t g_drift_logged_w = 0, g_drift_logged_h = 0;

SafetyHookMid g_osr_entry, g_osr_exit, g_setup_store, g_chain_desc;

template <typename T> T &field(uintptr_t off)
{
    return *(T *)(MC2_G_RENDERSHELL + off);
}
float &mouse_x()
{
    return *(float *)MC2_G_MOUSESCALE_X;
}
float &mouse_y()
{
    return *(float *)MC2_G_MOUSESCALE_Y;
}

bool enabled()
{
    return g_w != 0 && g_h != 0;
}

void apply_aspect(const char *reason, bool quiet = false)
{
    if (!enabled() || !g_true_aspect) {
        return;
    }
    const float a = (float)g_w / (float)g_h;
    g_target_aspect = a;
    if (field<float>(MC2_RS_ASPECT_X_OFF) != a || field<float>(MC2_RS_ASPECT_Y_OFF) != a) {
        const float old = field<float>(MC2_RS_ASPECT_X_OFF);
        field<float>(MC2_RS_ASPECT_X_OFF) = a;
        field<float>(MC2_RS_ASPECT_Y_OFF) = a;
        if (!quiet) {
            MC2VR_LOG("rsize: aspect %.4f -> %.4f (%s)", (double)old, (double)a, reason);
        }
    }
}

// ---- OnScreenResize entry: W = [esp+4], H = EAX, this = ESI -----------------------

void osr_entry_midhook(safetyhook::Context &ctx)
{
    uint32_t *w = (uint32_t *)(ctx.esp + 4);
    rt_probe::on_osr_enter(*w, (uint32_t)ctx.eax, ctx.esi, ctx.esp);
    if (!enabled() || ctx.esi != MC2_G_RENDERSHELL) {
        return;
    }
    if (*w != g_w || ctx.eax != g_h) {
        MC2VR_LOG("rsize: OnScreenResize(%ux%u) -> %ux%u (frame %llu)", *w, (unsigned)ctx.eax,
                  g_w, g_h, (unsigned long long)hooks::frame_count());
    }
    *w = g_w;
    ctx.eax = g_h;
    g_osr_rewrites++;
}

// ---- OnScreenResize epilogue: fields, floats, viewports, half-dims and the
// mouse scale (cfg / client) are written — post-fix aspect + mouse.

void osr_exit_midhook(safetyhook::Context &)
{
    if (!enabled()) {
        return;
    }
    const uint32_t cfg_w = *(const uint32_t *)MC2_G_RENDERCFG_SCRW;
    const uint32_t cfg_h = *(const uint32_t *)MC2_G_RENDERCFG_SCRH;
    if (cfg_w && cfg_h) {
        // mouse = cfg / client  ->  render / client
        mouse_x() *= (float)g_w / (float)cfg_w;
        mouse_y() *= (float)g_h / (float)cfg_h;
        g_target_mouse_x = mouse_x();
        g_target_mouse_y = mouse_y();
        g_mouse_target_valid = true;
    }
    apply_aspect("OnScreenResize");
    MC2VR_LOG("rsize: render size %ux%u (cfg %ux%u), aspect %.4f, flag169=%u, mouse scale "
              "%.4f/%.4f",
              field<uint32_t>(MC2_RS_SCREEN_W_OFF), field<uint32_t>(MC2_RS_SCREEN_H_OFF), cfg_w,
              cfg_h, (double)field<float>(MC2_RS_ASPECT_X_OFF),
              field<uint32_t>(MC2_RS_FLAG169_OFF), (double)mouse_x(), (double)mouse_y());
}

// ---- SetupRenderTargets field store: ECX = cfg W, EAX = cfg H -------------------

void setup_store_midhook(safetyhook::Context &ctx)
{
    if (!enabled()) {
        return;
    }
    ctx.ecx = g_w;
    ctx.eax = g_h;
    g_setup_rewrites++;
}

// ---- SetupRenderTargets chain desc: the engine sizes its main chain from the
// front's GetDesc (still the swapchain backbuffer at this point) — make that
// desc say the render size. EAX = GetDesc's HRESULT.

void chain_desc_midhook(safetyhook::Context &ctx)
{
    if (!enabled() || (int32_t)ctx.eax < 0) {
        return;
    }
    D3DSURFACE_DESC *d = (D3DSURFACE_DESC *)(ctx.esp + MC2_SETUP_CHAIN_DESC_ESP_OFF);
    if (d->Width != g_w || d->Height != g_h) {
        MC2VR_LOG("rsize: setup chain desc %ux%u -> %ux%u (main/depth copy/scene depth/"
                  "downsample are sized from it)",
                  d->Width, d->Height, g_w, g_h);
        d->Width = g_w;
        d->Height = g_h;
    }
}

bool install_mid(SafetyHookMid &storage, uintptr_t target, safetyhook::MidHookFn fn,
                 const char *name)
{
    auto mid = SafetyHookMid::create(reinterpret_cast<uint8_t *>(target), fn);
    if (!mid) {
        MC2VR_LOG("rsize: FATAL — %s MidHook install failed @ %p (error %u)", name,
                  (void *)target, (unsigned)mid.error().type);
        return false;
    }
    storage = std::move(*mid);
    MC2VR_LOG("rsize: installed %s MidHook @ %p", name, (void *)target);
    return true;
}

} // namespace

bool set_size(const char *value)
{
    if (strcmp(value, "off") == 0) {
        g_cfg_w = g_cfg_h = g_w = g_h = 0;
        MC2VR_LOG("rsize: render_size=off (render size = window size)");
        return true;
    }
    unsigned w = 0, h = 0;
    char tail = 0;
    if (sscanf(value, "%ux%u%c", &w, &h, &tail) != 2 || w < 64 || h < 64 || w > 8192 ||
        h > 8192) {
        return false;
    }
    g_cfg_w = w;
    g_cfg_h = h;
    g_w = g_follow_hmd ? 0 : w; // follow mode starts stock; the HMD switches it
    g_h = g_follow_hmd ? 0 : h;
    MC2VR_LOG("rsize: render_size=%ux%u (aspect %.4f)", w, h, (double)w / (double)h);
    return true;
}

bool set_follow_hmd(const char *value)
{
    if (strcmp(value, "on") == 0) {
        g_follow_hmd = true;
    } else if (strcmp(value, "off") == 0) {
        g_follow_hmd = false;
    } else {
        return false;
    }
    g_w = g_follow_hmd ? 0 : g_cfg_w;
    g_h = g_follow_hmd ? 0 : g_cfg_h;
    MC2VR_LOG("rsize: render_size_follow_hmd=%s (%s)", value,
              g_follow_hmd ? "render_size only while the HMD is in use, window size otherwise"
                           : "render_size always");
    return true;
}

bool consume_forced_reset(void *ret)
{
    const uintptr_t r = (uintptr_t)ret;
    if (!g_switch_armed || r < MC2_CHECKDEVICE_BEGIN || r >= MC2_CHECKDEVICE_END) {
        return false;
    }
    g_switch_armed = false;
    g_w = g_pending_w;
    g_h = g_pending_h;
    g_target_aspect = 0.0f;
    g_mouse_target_valid = false;
    g_drift_logged_w = g_drift_logged_h = 0;
    g_switches++;
    MC2VR_LOG("rsize: switch -> %s (CheckDevice device-lost recovery, frame %llu)",
              g_w ? "render size" : "window size", (unsigned long long)hooks::frame_count());
    return true;
}

bool set_aspect(const char *value)
{
    if (strcmp(value, "true") == 0) {
        g_true_aspect = true;
    } else if (strcmp(value, "snap") == 0) {
        g_true_aspect = false;
    } else {
        return false;
    }
    MC2VR_LOG("rsize: render_aspect=%s", value);
    return true;
}

void install()
{
    const bool configured = g_cfg_w != 0 && g_cfg_h != 0;
    if (!configured && !rt_probe::enabled()) {
        return;
    }
    QueryPerformanceFrequency(&g_qpc_freq);
    install_mid(g_osr_entry, MC2_ONSCREENRESIZE, osr_entry_midhook, "OnScreenResize entry");
    if (configured) {
        install_mid(g_osr_exit, MC2_ONSCREENRESIZE_EPILOGUE, osr_exit_midhook,
                    "OnScreenResize epilogue");
        install_mid(g_setup_store, MC2_SETUP_FIELD_STORE, setup_store_midhook,
                    "SetupRenderTargets field store");
        install_mid(g_chain_desc, MC2_SETUP_CHAIN_DESC, chain_desc_midhook,
                    "SetupRenderTargets chain desc");
    }
}

void after_setup()
{
    apply_aspect("SetupRenderTargets");
}

namespace {

// HMD in use = host connected, session VISIBLE/FOCUSED and the pose tracked —
// the same condition under which the camera rewrite is live (else the game
// runs with camera pass-through).
bool hmd_in_use()
{
    Mc2IpcState st;
    if (!ipc::connected() || !ipc::read_state(&st)) {
        return false;
    }
    const bool visible = st.sessionState == MC2VR_XR_SESSION_VISIBLE ||
                         st.sessionState == MC2VR_XR_SESSION_FOCUSED;
    return visible && (st.flags & MC2VR_IPC_STF_TRACKED);
}

void evaluate_follow()
{
    if (!g_follow_hmd || g_cfg_w == 0 || g_switch_armed || !render_outputs::active()) {
        return;
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const bool raw = hmd_in_use();
    if (raw != g_hmd_candidate) {
        g_hmd_candidate = raw;
        g_candidate_since = now;
        return;
    }
    const double held = (double)(now.QuadPart - g_candidate_since.QuadPart) /
                        (double)g_qpc_freq.QuadPart;
    if (held < HMD_DEBOUNCE_SEC || raw == g_hmd_active_seen) {
        return;
    }
    g_hmd_active_seen = raw;
    const uint32_t want_w = raw ? g_cfg_w : 0, want_h = raw ? g_cfg_h : 0;
    if (want_w == g_w && want_h == g_h) {
        return;
    }
    g_pending_w = want_w;
    g_pending_h = want_h;
    g_switch_armed = true; // the next CheckDevice TestCooperativeLevel answers NOTRESET
    MC2VR_LOG("rsize: HMD %s -> switching to %s (engine device reset)",
              raw ? "in use" : "inactive", raw ? "render size" : "window size");
}

} // namespace

void on_present()
{
    evaluate_follow();
    if (!enabled()) {
        return;
    }
    const uint32_t w = field<uint32_t>(MC2_RS_SCREEN_W_OFF);
    const uint32_t h = field<uint32_t>(MC2_RS_SCREEN_H_OFF);
    if ((w != g_w || h != g_h) && (w != g_drift_logged_w || h != g_drift_logged_h)) {
        // Canary: something other than the two hooked writers set the render
        // fields; surfaces were already sized, so this is logged, not patched.
        g_drift_logged_w = w;
        g_drift_logged_h = h;
        MC2VR_LOG("rsize: WARNING render fields %ux%u != render_size %ux%u (frame %llu)", w, h,
                  g_w, g_h, (unsigned long long)hooks::frame_count());
    }
    if (g_true_aspect && g_target_aspect != 0.0f &&
        (field<float>(MC2_RS_ASPECT_X_OFF) != g_target_aspect ||
         field<float>(MC2_RS_ASPECT_Y_OFF) != g_target_aspect)) {
        g_aspect_reapplies++;
        // Log the first few; the window report counts the rest.
        apply_aspect("drift (script aspect setter?)", g_aspect_reapplies > 3);
    }
    if (g_mouse_target_valid &&
        (mouse_x() != g_target_mouse_x || mouse_y() != g_target_mouse_y)) {
        g_mouse_reapplies++;
        mouse_x() = g_target_mouse_x;
        mouse_y() = g_target_mouse_y;
    }
}

void report_window()
{
    if (g_cfg_w == 0) {
        return;
    }
    MC2VR_LOG("rsize: window target=%s fields=%ux%u aspect=%.4f switches=%llu osrRewrites=%llu "
              "setupRewrites=%llu aspectReapplies=%llu mouseReapplies=%llu",
              g_w ? "render" : "window", field<uint32_t>(MC2_RS_SCREEN_W_OFF),
              field<uint32_t>(MC2_RS_SCREEN_H_OFF), (double)field<float>(MC2_RS_ASPECT_X_OFF),
              (unsigned long long)g_switches, (unsigned long long)g_osr_rewrites,
              (unsigned long long)g_setup_rewrites, (unsigned long long)g_aspect_reapplies,
              (unsigned long long)g_mouse_reapplies);
    g_mouse_reapplies = 0; // aspect reapplies stay cumulative (gate the drift log)
}

} // namespace mc2vr::render_size

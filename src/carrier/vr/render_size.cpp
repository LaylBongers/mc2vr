// Render-size policy. See render_size.hpp.

#include "vr/render_size.hpp"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>

#include "game/addresses.h"
#include "hooks/frame_tick.hpp"
#include "log.hpp"
#include "vr/render_outputs.hpp"
#include "vr/adaptation.hpp"
#include "vr/xr_session.hpp"

namespace mc2vr::render_size {

namespace {

// Configured size (render_size=WxH; 0 = off) and the CURRENT target the
// hooks apply (0 = stock: render size = window size). With
// render_size_follow_hmd=on (default) the current target is the configured
// size only while the HMD is in use, else stock — so with the HMD on standby
// the game renders exactly as unmodded (mouse mapping included).
uint32_t g_cfg_w = 0, g_cfg_h = 0;
bool g_cfg_hmd = false; // render_size=hmd: g_cfg_* come from the OpenXR session
uint32_t g_w = 0, g_h = 0;
bool g_true_aspect = true;
bool g_follow_hmd = true;

// Switch state (main thread): a pending target, applied when the engine's
// CheckDevice consumes the forced device-lost signal.
bool g_switch_armed = false;
uint32_t g_pending_w = 0, g_pending_h = 0;
bool g_hmd_active_seen = false;      // debounced state

uint64_t g_switches = 0;


// Targets established by the last OnScreenResize epilogue / setup.
float g_target_aspect = 0.0f;
float g_target_mouse_x = 0.0f, g_target_mouse_y = 0.0f;
bool g_mouse_target_valid = false;

// Window counters / one-shot logs (main thread).
uint64_t g_osr_rewrites = 0, g_setup_rewrites = 0, g_aspect_reapplies = 0;
uint64_t g_mouse_reapplies = 0;
uint32_t g_drift_logged_w = 0, g_drift_logged_h = 0;

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

} // namespace

bool set_size(const char *value)
{
    if (strcmp(value, "off") == 0) {
        g_cfg_w = g_cfg_h = g_w = g_h = 0;
        g_cfg_hmd = false;
        MC2VR_LOG("rsize: render_size=off (render size = window size)");
        return true;
    }
    if (strcmp(value, "hmd") == 0) {
        // Size arrives from the runtime (xr_session::on_update_tick). Implies follow:
        // without an HMD size there is nothing else to render at.
        g_cfg_hmd = true;
        g_cfg_w = g_cfg_h = g_w = g_h = 0;
        MC2VR_LOG("rsize: render_size=hmd (the runtime's recommended eye size, while in use)");
        return true;
    }
    unsigned w = 0, h = 0;
    char tail = 0;
    if (sscanf(value, "%ux%u%c", &w, &h, &tail) != 2 || w < 64 || h < 64 || w > 8192 ||
        h > 8192) {
        return false;
    }
    g_cfg_hmd = false;
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

void on_hmd_view_size(uint32_t w, uint32_t h)
{
    if (!g_cfg_hmd || w < 64 || h < 64 || w > 8192 || h > 8192) {
        return;
    }
    if (w == g_cfg_w && h == g_cfg_h) {
        return;
    }
    g_cfg_w = w;
    g_cfg_h = h;
    MC2VR_LOG("rsize: render size from the runtime: %ux%u (aspect %.4f)", w, h,
              (double)w / (double)h);
    // If the HMD is already in use at the old size, the follow logic sees the
    // mismatch and switches (g_hmd_active_seen stays; target compare below).
    if (g_hmd_active_seen && (g_w != 0) && (g_w != w || g_h != h)) {
        g_hmd_active_seen = false; // re-evaluate -> switch to the new size
    }
}

bool consume_forced_reset(void *ret)
{
    const uintptr_t r = (uintptr_t)ret;
    if (!g_switch_armed || r < MC2_CHECKDEVICE_BEGIN || r >= MC2_CHECKDEVICE_END) {
        return false;
    }
    g_switch_armed = false;
    adaptation::save(); // auto-exposure state survives the reset (no bright flash)
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

bool configured()
{
    return (g_cfg_w != 0 && g_cfg_h != 0) || g_cfg_hmd;
}

std::optional<Size> on_osr_enter(Size requested, uintptr_t self)
{
    if (!enabled() || self != MC2_G_RENDERSHELL) {
        return std::nullopt;
    }
    if (requested.w != g_w || requested.h != g_h) {
        MC2VR_LOG("rsize: OnScreenResize(%ux%u) -> %ux%u (frame %llu)", requested.w,
                  requested.h, g_w, g_h, (unsigned long long)hooks::frame_count());
    }
    g_osr_rewrites++;
    return Size{g_w, g_h};
}

void on_osr_done()
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

std::optional<Size> on_setup_store()
{
    if (!enabled()) {
        return std::nullopt;
    }
    g_setup_rewrites++;
    return Size{g_w, g_h};
}

std::optional<Size> on_setup_chain_desc(Size desc)
{
    if (!enabled() || (desc.w == g_w && desc.h == g_h)) {
        return std::nullopt;
    }
    MC2VR_LOG("rsize: setup chain desc %ux%u -> %ux%u (main/depth copy/scene depth/"
              "downsample are sized from it)",
              desc.w, desc.h, g_w, g_h);
    return Size{g_w, g_h};
}

void after_setup()
{
    apply_aspect("SetupRenderTargets");
}

// HMD in use = the session VISIBLE/FOCUSED — the OpenXR
// lifecycle signal (the runtime already debounces it: proximity sensor /
// standby). Deliberately NOT the per-frame tracked flag: a brief tracking
// loss must not trigger a device reset (the camera falls back to
// pass-through meanwhile).
bool hmd_in_use()
{
    xr_session::ViewState st;
    return xr_session::read_state(&st) && xr_session::lockstep(st.session_state);
}

namespace {

void evaluate_follow()
{
    // render_size=hmd always follows (it has no size without the HMD).
    if ((!g_follow_hmd && !g_cfg_hmd) || g_cfg_w == 0 || g_switch_armed ||
        !render_outputs::active()) {
        return;
    }
    const bool raw = hmd_in_use();
    if (raw == g_hmd_active_seen) {
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
    if (g_cfg_w == 0 && !g_cfg_hmd) {
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

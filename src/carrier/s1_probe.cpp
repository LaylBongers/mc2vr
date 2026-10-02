// S2 channel module — see s1_probe.hpp for scope. S1 hunt instrumentation
// removed (S1 complete, 2026-10-02 — docs/s1_camera_hunt.md); this file
// keeps only the SetVertexShaderConstantF channel + ambient rewrite.

#include "s1_probe.hpp"

#include <safetyhook.hpp>

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "game_addresses.h"
#include "hooks.hpp"
#include "log.hpp"

namespace mc2vr::s1 {

namespace {

constexpr uint32_t VS_ROWS = 256;  // vs_2_0 max registers
constexpr float PATCH_DELTA = 4.0f;

// ---- ambient rewrite mode (mc2vr.conf gpu_boundary_rewrite) ----------------
enum class RewriteMode { Off, On, Pulse };
RewriteMode g_ambient_rewrite = RewriteMode::Off;

// ---- VS register cache (the S2a identification input) ------------------------
// Last uploaded 4-float row per register + validity. The S1 classification
// machinery is gone; S2a (camera-row identification) builds on this cache.
float g_vs_rows[VS_ROWS][4];
uint8_t g_vs_valid[VS_ROWS];
uint64_t g_vs_frame_key = UINT64_MAX;

// ---- window report aggregates -------------------------------------------------
uint64_t g_vs_calls = 0, g_vs_vec4s = 0;
uint64_t g_owin_rewrites = 0;
uint32_t g_owin_log_regs[24]; // one-shot per distinct rewritten register
uint32_t g_owin_log_n = 0;

} // namespace

bool set_ambient_rewrite(const char *value)
{
    RewriteMode mode;
    if (strcmp(value, "off") == 0) {
        mode = RewriteMode::Off;
    } else if (strcmp(value, "on") == 0) {
        mode = RewriteMode::On;
    } else if (strcmp(value, "pulse") == 0) {
        mode = RewriteMode::Pulse;
    } else {
        return false;
    }
    g_ambient_rewrite = mode;
    MC2VR_LOG("S1 owin: ambient GPU-boundary rewrite mode = %s%s", value,
              mode != RewriteMode::Off
                  ? " (every world-position VS constant row w==1.0, |x|>5 "
                    "gets shifted IN THE UPLOAD — camera-row narrowing is S2a)"
                  : " (pass-through)");
    return true;
}

void on_set_vs_constant(uint32_t start_register, const float *data,
                        uint32_t vec4_count)
{
    g_vs_calls++;
    if (!data || vec4_count == 0) {
        return;
    }
    g_vs_vec4s += vec4_count;
    const uint64_t frame = hooks::frame_count();
    if (frame != g_vs_frame_key) {
        g_vs_frame_key = frame;
    }

    // Register cache: rows exactly as uploaded (pre-rewrite).
    if (start_register < VS_ROWS) {
        const uint32_t n = vec4_count < VS_ROWS - start_register
                               ? vec4_count
                               : VS_ROWS - start_register;
        for (uint32_t i = 0; i < n; i++) {
            const uint32_t r = start_register + i;
            memcpy(g_vs_rows[r], data + i * 4, 16);
            g_vs_valid[r] = 1;
        }
    }

    // Ambient GPU-boundary rewrite (the verified S2 mechanism — S1 runs
    // 16–17): every world-position constant row (w == 1.0, |x| > 5) in THIS
    // upload gets x shifted IN THE UPLOAD BUFFER before the driver call —
    // the draw consumes the modification directly. The game's own data is
    // never touched. NOTE (S2a): the VISIBLE view is driven by the
    // view-matrix rows (w != 1.0 — not matched by this filter); narrowing
    // to those rows is the next step (docs/s1_camera_hunt.md §handoff).
    if (g_ambient_rewrite == RewriteMode::Off) {
        return;
    }
    float delta = PATCH_DELTA;
    if (g_ambient_rewrite == RewriteMode::Pulse) {
        // ~2s sine drift at 60fps — unmistakable, self-reversing.
        delta = PATCH_DELTA * sinf(6.2831853f * (float)(frame % 120u) / 120.0f);
    }
    float *rows = (float *)data;
    for (uint32_t i = 0; i < vec4_count; i++) {
        float *row = rows + i * 4;
        if (row[3] != 1.0f || (row[0] <= 5.0f && row[0] >= -5.0f)) {
            continue;
        }
        row[0] += delta;
        g_owin_rewrites++;
        const uint32_t reg = start_register + i;
        bool logged = false;
        for (uint32_t k = 0; k < g_owin_log_n; k++) {
            if (g_owin_log_regs[k] == reg) {
                logged = true;
                break;
            }
        }
        if (!logged && g_owin_log_n < 24) {
            g_owin_log_regs[g_owin_log_n++] = reg;
            MC2VR_LOG("S1 owin: rewriting c%u (was (%g,%g,%g,%g)) — "
                      "world-position row; S2a: narrow to the view-matrix "
                      "rows to move the camera itself",
                      reg, (double)(row[0] - delta), (double)row[1],
                      (double)row[2], (double)row[3]);
        }
    }
}

void report_window()
{
    MC2VR_LOG("S1 vs: calls=%llu vec4s=%llu | owin rewrites=%llu (mode=%s)",
              (unsigned long long)g_vs_calls, (unsigned long long)g_vs_vec4s,
              (unsigned long long)g_owin_rewrites,
              g_ambient_rewrite == RewriteMode::Pulse ? "pulse"
              : g_ambient_rewrite == RewriteMode::On ? "on" : "off");
    g_vs_calls = 0;
    g_vs_vec4s = 0;
    g_owin_rewrites = 0;
}

void install()
{
    MC2VR_LOG("S1: hunt instrumentation removed (S1 complete, 2026-10-02 — "
              "evidence in docs/s1_camera_hunt.md). Active channel: "
              "SetVertexShaderConstantF (device VmtHook slot 94) + ambient "
              "rewrite via mc2vr.conf (gpu_boundary_rewrite=off|on|pulse)");
}

} // namespace mc2vr::s1

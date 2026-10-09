#include "init.hpp"

#include <windows.h>

#include "build_lock.h"
#include "cull_frustum.hpp"
#include "occluder_boxes.hpp"
#include "device.hpp"
#include "eye_replay.hpp"
#include "eye_share.hpp"
#include "game_addresses.h"
#include "hooks.hpp"
#include "ipc.hpp"
#include "log.hpp"

#include "debug/probes.hpp"
#include "debug/render_dump.hpp"
#include "debug/stream_capture.hpp"
#include "debug/stub_trace.hpp"
#include "debug/watch.hpp"
#include "view_rewrite.hpp"
#include "camera_table.hpp"
#include "debug/vm_dump.hpp"
#include "sha256.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mc2vr {

// Numeric conf values: true and *out set only on a clean, fully-consumed
// parse; otherwise logs and leaves the setting alone.
static bool parse_double(const char *key, const char *value, double *out)
{
    char *end = nullptr;
    const double v = strtod(value, &end);
    if (end == value || end[0] != 0) {
        MC2VR_LOG("conf: %s=%s not a number, ignored", key, value);
        return false;
    }
    *out = v;
    return true;
}

static bool parse_count(const char *key, const char *value, uint32_t *out)
{
    char *end = nullptr;
    const long n = strtol(value, &end, 10);
    if (end == value || end[0] != 0 || n < 0) {
        MC2VR_LOG("conf: %s=%s not a count, ignored", key, value);
        return false;
    }
    *out = (uint32_t)n;
    return true;
}

// Read <deploy dir>/mc2vr.conf (next to this DLL). Simple
// key=value lines, '#' comments, whitespace-tolerant. Unknown keys are
// logged and ignored; unknown values leave the default (off).
static void load_conf()
{
    HMODULE self = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          (LPCWSTR)&load_conf, &self) == 0 ||
        self == nullptr) {
        MC2VR_LOG("conf: cannot locate own module — defaults apply");
        return;
    }
    wchar_t wpath[MAX_PATH];
    if (GetModuleFileNameW(self, wpath, MAX_PATH) == 0) {
        MC2VR_LOG("conf: GetModuleFileNameW failed — defaults apply");
        return;
    }
    wchar_t *slash = wcsrchr(wpath, L'\\');
    if (slash == nullptr) {
        return;
    }
    wcscpy(slash + 1, L"mc2vr.conf");

    FILE *f = _wfopen(wpath, L"rb");
    if (f == nullptr) {
        MC2VR_LOG("conf: no mc2vr.conf next to the DLL — defaults apply "
                  "(view_row_rewrite=off)");
        return;
    }
    char line[512];
    while (fgets(line, sizeof(line), f) != nullptr) {
        char *hash = strchr(line, '#');
        if (hash != nullptr) {
            *hash = '\0';
        }
        char *eq = strchr(line, '=');
        if (eq == nullptr) {
            continue;
        }
        *eq = '\0';
        char *key = line;
        char *value = eq + 1;
        // trim
        while (*key == ' ' || *key == '\t') key++;
        while (*value == ' ' || *value == '\t') value++;
        char *end = key + strlen(key);
        while (end > key && (end[-1] == ' ' || end[-1] == '\t' ||
                             end[-1] == '\r' || end[-1] == '\n')) {
            *--end = '\0';
        }
        end = value + strlen(value);
        while (end > value && (end[-1] == ' ' || end[-1] == '\t' ||
                               end[-1] == '\r' || end[-1] == '\n')) {
            *--end = '\0';
        }
        if (strcmp(key, "view_row_rewrite") == 0) {
            // The viewContextData camera channel (view_rewrite.cpp).
            if (!view_rewrite::set_view_row_rewrite(value)) {
                MC2VR_LOG("conf: view_row_rewrite=%s not recognized "
                          "(use off|on|pulse|stereo|hmd_delta|hmd_identity) — defaulting to off",
                          value);
                view_rewrite::set_view_row_rewrite("off");
            }
        } else if (strcmp(key, "view_table_inject") == 0) {
            // Union HMD injection at g_CameraTable (docs/plans/stereo_improvements.md
            // "Decided architecture"): rewrites each just-filled table entry's
            // camera entry with the game pose composed with the mid-eye HMD
            // pose — the single upstream point feeding BOTH the draw-camera
            // builder and the culling/fov readers. Pair with
            // view_row_rewrite=hmd_delta for the per-eye FOV/position delta.
            if (!camera_table::set_inject_enabled(value)) {
                MC2VR_LOG("conf: view_table_inject=%s not recognized (use on|off)", value);
            }
        } else if (strcmp(key, "cull_hmd_fov") == 0) {
            // HMD cull frustum (docs/plans/frustum_cull.md): the engine's
            // culling/LOD frustum covers the HMD FOV union instead of the
            // game's widescreen fov. Needs view_table_inject=on.
            if (!cull_frustum::set_enabled(value)) {
                MC2VR_LOG("conf: cull_hmd_fov=%s not recognized (use on|off)", value);
            }
        } else if (strcmp(key, "skip_occluder_boxes") == 0) {
            if (!occluder_boxes::set_skip(value)) {
                MC2VR_LOG("conf: skip_occluder_boxes=%s not recognized (use on|off)", value);
            }
        } else if (strcmp(key, "cull_fov_margin") == 0) {
            // Degrees added to each HMD half-angle (default 5).
            double v;
            if (parse_double(key, value, &v)) {
                cull_frustum::set_margin(v);
            }
        } else if (strcmp(key, "debug_camtable_probe") == 0) {
            // Transfer-function probe: injects fixed local-axis test
            // rotations at the fill site and logs entry + rendered response
            // — settles the rotation convention numerically (run standing
            // still in gameplay; no HMD needed).
            if (!camera_table::set_probe_enabled(value)) {
                MC2VR_LOG("conf: debug_camtable_probe=%s not recognized (use on|off)", value);
            }
        } else if (strcmp(key, "debug_stub_trace") == 0) {
            if (!trace::set_enabled(value)) {
                MC2VR_LOG("conf: debug_stub_trace=%s not recognized (use on|off)", value);
            }
        } else if (strcmp(key, "debug_vm_dump") == 0) {
            if (!vmdump::set_enabled(value)) {
                MC2VR_LOG("conf: debug_vm_dump=%s not recognized (use on|off)", value);
            }
        } else if (strcmp(key, "debug_stream_capture") == 0) {
            // S2c-0: render-command-stream capture + opcode census (read-only;
            // docs/plans/stereo_design.md §S2). Requires the opcode MidHook (M3).
            if (!s2c::set_enabled(value)) {
                MC2VR_LOG("conf: debug_stream_capture=%s not recognized (use on|off)", value);
            }
        } else if (strcmp(key, "eye_pass") == 0) {
            // S2c-2: deterministic per-pass eye (pass1=LEFT pass2=RIGHT).
            if (!eye_replay::set_pass_enabled(value)) {
                MC2VR_LOG("conf: eye_pass=%s not recognized (use on|off)", value);
            }
        } else if (strcmp(key, "eye_rt") == 0) {
            // S2c-2: pass-2 SetRenderTarget(0)/StretchRect redirect to an eye RT.
            if (!eye_replay::set_rt_enabled(value)) {
                MC2VR_LOG("conf: eye_rt=%s not recognized (use on|off)", value);
            }
        } else if (strcmp(key, "eye_monitor_pin") == 0) {
            // S2c-2: skip the pass-2 EndSubmit RT->backbuffer copy so the
            // monitor holds pass 1's LEFT image (S4 steady state).
            if (!eye_replay::set_pin_enabled(value)) {
                MC2VR_LOG("conf: eye_monitor_pin=%s not recognized (use on|off)", value);
            }
        } else if (strcmp(key, "eye_share") == 0) {
            // S4-2: pass-boundary backbuffer capture into shared-handle RTs +
            // FRAME_READY publish to the OpenXR host.
            if (!eye_share::set_enabled(value)) {
                MC2VR_LOG("conf: eye_share=%s not recognized (use on|off)", value);
            }
        } else if (strcmp(key, "vsync") == 0) {
            // S4-5 pacing: off = force D3DPRESENT_INTERVAL_IMMEDIATE at
            // CreateDevice/Reset (the frame's two Presents are vsync-locked
            // and cap the game at ~30 Hz).
            if (!device::set_vsync(value)) {
                MC2VR_LOG("conf: vsync=%s not recognized (use on|off)", value);
            }
        } else if (strcmp(key, "debug_eye_dump_frames") == 0) {
            uint32_t n;
            if (parse_count(key, value, &n)) {
                eye_replay::set_dump_frames(n);
            }
        } else if (strcmp(key, "frame_replay") == 0) {
            // S2c-1: second draw pass — re-invoke PgPrimitive_SubmitToGPU after
            // the original (same eye/RTs; state-safety test, docs/plans/stereo_design.md §S2).
            if (!s2c::set_replay_enabled(value)) {
                MC2VR_LOG("conf: frame_replay=%s not recognized (use on|off)", value);
            }
        } else if (strcmp(key, "debug_stream_dump_frames") == 0) {
            uint32_t n;
            if (parse_count(key, value, &n)) {
                s2c::set_dump_frames(n);
            }
        } else if (strcmp(key, "debug_dump_delay") == 0) {
            double d;
            if (parse_double(key, value, &d) && d >= 0.0) {
                s2c::set_dump_delay((float)d);
                eye_replay::set_dump_delay((float)d);
            }
        } else if (strcmp(key, "view_world_scale") == 0) {
            // Game world units per metre for the HMD camera (default 1.0).
            double v;
            if (parse_double(key, value, &v)) {
                view_rewrite::set_view_world_scale((float)v);
            }
        } else if (strcmp(key, "debug_watch") == 0) {
            // S5: hardware watchpoints on ViewEntry camera fields (culling-RE
            // evidence; docs/reverse_engineering/view_and_camera.md).
            if (!watch::set_targets(value)) {
                MC2VR_LOG("conf: debug_watch=%s not recognized (named ViewEntry "
                          "fields or addr:<hex>, '+'-separated, max 4) — disabled",
                          value);
            }
        } else if (strcmp(key, "debug_watch_mode") == 0) {
            if (!watch::set_mode(value)) {
                MC2VR_LOG("conf: debug_watch_mode=%s not recognized (use full|write)",
                          value);
            }
        } else if (strcmp(key, "debug_watch_view") == 0) {
            uint32_t n;
            if (parse_count(key, value, &n)) {
                watch::set_view_index(n);
            }
        } else if (strcmp(key, "debug_watch_hits") == 0) {
            uint32_t n;
            if (parse_count(key, value, &n)) {
                watch::set_detail_hits(n);
            }
        } else {
            MC2VR_LOG("conf: unknown key '%s' ignored", key);
        }
    }
    fclose(f);
}

static bool verify_image_base()
{
    uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
    if (base != MC2_GAME_BASE_EXPECTED) {
        MC2VR_LOG("FATAL: host exe base is %p, expected %p — all recorded "
                  "VAs are invalid; refusing to hook",
                  (void *)base, (void *)MC2_GAME_BASE_EXPECTED);
        return false;
    }
    MC2VR_LOG("image base verified: %p", (void *)base);
    return true;
}

// SHA-256 the host exe on disk. The on-disk image is the plaintext build
// (verified in docs/reverse_engineering/target_binary.md); the in-memory image differs once
// the SecuROM stub decrypts its sections, so the file is the stable identity.
static bool verify_build_lock()
{
    wchar_t exe_path[MAX_PATH];
    if (GetModuleFileNameW(nullptr, exe_path, MAX_PATH) == 0) {
        MC2VR_LOG("FATAL: GetModuleFileNameW failed (%lu)", GetLastError());
        return false;
    }
    MC2VR_LOG("host exe: %ls", exe_path);

    HANDLE file = CreateFileW(exe_path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        MC2VR_LOG("FATAL: cannot open host exe for hashing (%lu)", GetLastError());
        return false;
    }

    // Size check first — cheap and catches the obvious mismatch.
    LARGE_INTEGER size;
    if (!GetFileSizeEx(file, &size) ||
        size.QuadPart != (LONGLONG)MC2_EXPECTED_EXE_SIZE_BYTES) {
        MC2VR_LOG("FATAL: host exe size is %lld bytes, expected %lld — "
                  "build lock mismatch",
                  size.QuadPart, (LONGLONG)MC2_EXPECTED_EXE_SIZE_BYTES);
        CloseHandle(file);
        return false;
    }

    sha256_ctx ctx;
    sha256_init(&ctx);

    uint8_t buffer[64 * 1024];
    for (;;) {
        DWORD read_bytes = 0;
        if (!ReadFile(file, buffer, sizeof(buffer), &read_bytes, nullptr)) {
            MC2VR_LOG("FATAL: ReadFile failed during hashing (%lu)", GetLastError());
            CloseHandle(file);
            return false;
        }
        if (read_bytes == 0) {
            break;
        }
        sha256_update(&ctx, buffer, read_bytes);
    }
    CloseHandle(file);

    uint8_t hash[32];
    sha256_final(&ctx, hash);
    char hex[65];
    sha256_hex(hash, hex);

    if (strcmp(hex, MC2_EXPECTED_EXE_SHA256_HEX) != 0) {
        MC2VR_LOG("FATAL: host exe sha256 %s != expected %s — build lock "
                  "mismatch; refusing to hook", hex, MC2_EXPECTED_EXE_SHA256_HEX);
        return false;
    }

    MC2VR_LOG("build lock verified: sha256 %s", hex);
    return true;
}

// Block until the engine has built its D3D device. Reads the same two
// globals GetD3DDevice_Impl does (`g_LtiRenderer ? g_LtiRenderer->dx9State :
// NULL`) instead of CALLING the GetD3DDevice thunk: before the loader patches
// that VM-slot thunk, a call would enter the unpatched SecuROM stub, and the
// frame counter is no help (it spins ~1400 Hz pre-D3D). A non-NULL device
// means the engine is well past startup, so the thunk is safe afterwards.
static bool wait_for_device()
{
    for (DWORD waited = 0; waited < 120 * 1000; waited += 50) {
        const uintptr_t renderer = *(volatile uintptr_t *)MC2_G_LTIRENDERER;
        if (renderer != 0 &&
            *(volatile uintptr_t *)(renderer + MC2_LTIRENDERER_DX9STATE_OFF) != 0) {
            return true;
        }
        Sleep(50);
    }
    return false;
}

void init()
{
    log_init();

    MC2VR_LOG("=== mc2vr carrier attached (pid=%lu) ===", GetCurrentProcessId());

    // Config (mc2vr.conf next to the DLL) — before any hook so the mode is
    // settled first.
    load_conf();

    // Gate: no hooks unless this is exactly the RE'd binary at the expected
    // base. Log everything either way — the log is the M0 deliverable.
    const bool base_ok = verify_image_base();
    const bool lock_ok = base_ok && verify_build_lock();

    if (!lock_ok) {
        MC2VR_LOG("init complete — build lock FAILED, staying resident but idle");
        MC2VR_LOG("early init done (idle)"); // launcher's resume marker
        return;
    }

    // ---- Stage 1: early. Runs possibly before the game's first instruction
    // (launcher starts it suspended and resumes on the marker below). Plaintext
    // .text hooks only — nothing that needs engine-constructed state.

    // S4-1: attach to the OpenXR host's shared section, if one exists (the
    // launcher spawns the host before the game). Non-fatal: no host means
    // the game runs the monitor-stereo path exactly as today. Only a
    // lock-passing carrier registers — an idle one stays fully idle.
    ipc::connect();

    hooks::install();
    render::install_early();
    // S4-5 pacing (vsync=off): must run while the game is still suspended —
    // RenderSystem_Init calls Direct3DCreate9 during boot, before stage 2.
    device::install_d3d9_gate();
    MC2VR_LOG("early init done"); // launcher's resume marker

    // ---- Stage 2: late. Needs the engine up (device, render shell).
    MC2VR_LOG("waiting for D3D device");
    if (!wait_for_device()) {
        MC2VR_LOG("FATAL: D3D device never appeared — late init skipped");
        return;
    }
    MC2VR_LOG("D3D device live — late init");

    // M1 SecuROM pre-probes: each step logs, so a crash is attributable to
    // exactly one action.
    probes::data_write_restore();
    probes::call_vm_thunk();

    // M2: device capture + VmtHook (Present/BeginScene/EndScene/Reset).
    // Best-effort: a failure here keeps the game and FrameTick hook alive.
    device::capture_and_hook();

    // M3: g_RenderShell slot claim + queue poll.
    render::install_late();

    // Read-only live dump of the VM chain behind RenderTask_RenderFrame.
    vmdump::install();

    MC2VR_LOG("init complete");
}

} // namespace mc2vr

#include "init.hpp"

#include <windows.h>

#include "build_lock.h"
#include "vr/hud.hpp"
#include "vr/msaa.hpp"
#include "vr/world_markers.hpp"
#include "vr/cull_frustum.hpp"
#include "hooks/frame_tick.hpp"
#include "hooks/install.hpp"
#include "vr/device_params.hpp"
#include "vr/eye_replay.hpp"
#include "vr/eye_share.hpp"
#include "vr/render_outputs.hpp"
#include "vr/render_size.hpp"
#include "vr/adaptation.hpp"
#include "game/addresses.h"
#include "ipc.hpp"
#include "log.hpp"

#include "debug/render_dump.hpp"
#include "debug/stream_capture.hpp"
#include "debug/build_census.hpp"
#include "debug/occlusion_off.hpp"
#include "debug/xr_probe.hpp"
#include "debug/camera_census.hpp"
#include "debug/watch.hpp"
#include "vr/view_rewrite.hpp"
#include "vr/camera_table.hpp"
#include "debug/vm_dump.hpp"
#include "sha256.h"
#include "debug/gpu_census.hpp"
#include "debug/gui_census.hpp"
#include "debug/ps_census.hpp"
#include "debug/wait_census.hpp"
#include "vr/pacing.hpp"

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
        MC2VR_LOG("conf: no mc2vr.conf next to the DLL — defaults apply");
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
        if (strcmp(key, "debug_view_identity") == 0) {
            // Decomposition oracle (view_rewrite.cpp): rebuild with the game's
            // own camera instead of the per-eye HMD rewrite.
            if (!view_rewrite::set_identity_oracle(value)) {
                MC2VR_LOG("conf: debug_view_identity=%s not recognized (use on|off)", value);
            }
        } else if (strcmp(key, "debug_camtable_probe") == 0) {
            // Transfer-function probe: injects fixed local-axis test
            // rotations at the fill site and logs entry + rendered response
            // — settles the rotation convention numerically (run standing
            // still in gameplay; no HMD needed).
            if (!camera_table::set_probe_enabled(value)) {
                MC2VR_LOG("conf: debug_camtable_probe=%s not recognized (use on|off)", value);
            }
        } else if (strcmp(key, "debug_sync_frame_ready") == 0) {
            // A/B: synchronous end-of-frame GPU wait instead of the completion
            // thread (also keeps the device non-multithreaded).
            if (!eye_share::set_sync_frame_ready(value)) {
                MC2VR_LOG("conf: debug_sync_frame_ready=%s not recognized (use on|off)", value);
            }
        } else if (strcmp(key, "debug_keep_replay_drain") == 0) {
            // A/B: keep LtiRenderer_BeginSubmit's GPU drain in the replay pass.
            if (!pacing::set_keep_replay_drain(value)) {
                MC2VR_LOG("conf: debug_keep_replay_drain=%s not recognized (use on|off)", value);
            }
        } else if (strcmp(key, "debug_view_dumps") == 0) {
            // View-table entry hex dumps (log-only; written from the game
            // thread, each one a visible hitch).
            if (!render::set_view_dumps(value)) {
                MC2VR_LOG("conf: debug_view_dumps=%s not recognized (use on|off)", value);
            }
        } else if (strcmp(key, "debug_ps_census") == 0) {
            // Per-pixel-shader GPU time + pixel counts (log-only;
            // debug/ps_census.hpp). Must precede CreateDevice.
            if (!ps_census::set_enabled(value)) {
                MC2VR_LOG("conf: debug_ps_census=%s not recognized (use on|off)", value);
            }
        } else if (strcmp(key, "hud_distance") == 0) {
            // HUD layer distance in metres (vr/hud.hpp, docs/hud.md).
            if (!hud::set_distance(value)) {
                MC2VR_LOG("conf: hud_distance=%s not recognized (metres, 0.3-20)", value);
            }
        } else if (strcmp(key, "marker_size") == 0) {
            // World-marker height in degrees (vr/world_markers.hpp, docs/hud.md).
            if (!world_markers::set_size(value)) {
                MC2VR_LOG("conf: marker_size=%s not recognized (degrees, 0.5-30)", value);
            }
        } else if (strcmp(key, "hud_size") == 0) {
            // HUD layer width in degrees (vr/hud.hpp, docs/hud.md).
            if (!hud::set_size(value)) {
                MC2VR_LOG("conf: hud_size=%s not recognized (degrees, 10-120)", value);
            }
        } else if (strcmp(key, "msaa") == 0) {
            // Scene MSAA sample count (vr/msaa.hpp, docs/msaa.md). Must
            // precede the first render-target setup.
            if (!msaa::set_samples(value)) {
                MC2VR_LOG("conf: msaa=%s not recognized (use off|2|4|8)", value);
            }
        } else if (strcmp(key, "debug_gui_census") == 0) {
            // Draw-time view of the GUI records every 30 s (log-only;
            // debug/gui_census.hpp). Must precede the hook installs.
            if (!gui_census::set_enabled(value)) {
                MC2VR_LOG("conf: debug_gui_census=%s not recognized (use on|off)", value);
            }
        } else if (strcmp(key, "debug_gpu_census") == 0) {
            // Per-pass / per-render-target GPU + CPU + draw-call census
            // (log-only; debug/gpu_census.hpp). Must precede CreateDevice.
            if (!gpu_census::set_enabled(value)) {
                MC2VR_LOG("conf: debug_gpu_census=%s not recognized (use on|off)", value);
            }
        } else if (strcmp(key, "debug_wait_census") == 0) {
            // Main-thread wait census + per-frame phase split (log-only;
            // debug/wait_census.hpp).
            if (!wait_census::set_enabled(value)) {
                MC2VR_LOG("conf: debug_wait_census=%s not recognized (use on|off)", value);
            }
        } else if (strcmp(key, "debug_camera_census") == 0) {
            // Which shaders take the camera from outside viewContextData (debug/camera_census.hpp).
            if (!camera_census::set_enabled(value)) {
                MC2VR_LOG("conf: debug_camera_census=%s not recognized (use on|off)", value);
            }
        } else if (strcmp(key, "debug_xr_probe") == 0) {
            // In-process OpenXR probe on DXVK's device (debug/xr_probe.hpp).
            if (!xr_probe::set_enabled(value)) {
                MC2VR_LOG("conf: debug_xr_probe=%s not recognized (use on|off)", value);
            }
        } else if (strcmp(key, "debug_occlusion_off") == 0) {
            // Occlusion culling off, for A/B (debug/occlusion_off.hpp).
            if (!occlusion_off::set_enabled(value)) {
                MC2VR_LOG("conf: debug_occlusion_off=%s not recognized (use on|off)", value);
            }
        } else if (strcmp(key, "debug_camera_census_skip") == 0) {
            camera_census::set_skip(value);
        } else if (strcmp(key, "debug_build_census") == 0) {
            // Scene -> draw-record build census (log-only; debug/build_census.hpp).
            if (!build_census::set_enabled(value)) {
                MC2VR_LOG("conf: debug_build_census=%s not recognized (use on|off)", value);
            }
        } else if (strcmp(key, "debug_vm_dump") == 0) {
            if (!vmdump::set_enabled(value)) {
                MC2VR_LOG("conf: debug_vm_dump=%s not recognized (use on|off)", value);
            }
        } else if (strcmp(key, "debug_stream_capture") == 0) {
            // Render-command-stream capture + opcode census (read-only;
            // docs/stereo.md). Requires the opcode MidHook (render_dump.cpp).
            if (!s2c::set_enabled(value)) {
                MC2VR_LOG("conf: debug_stream_capture=%s not recognized (use on|off)", value);
            }
        } else if (strcmp(key, "render_size") == 0) {
            // Render size W×H (docs/render_resolution.md): the engine's
            // render chain + the carrier outputs run at W×H; the window and
            // swapchain stay stock. off = render size = window size.
            if (!render_size::set_size(value)) {
                MC2VR_LOG("conf: render_size=%s not recognized (use WxH, e.g. 2016x2240, or off)",
                          value);
            }
        } else if (strcmp(key, "render_size_follow_hmd") == 0) {
            if (!render_size::set_follow_hmd(value)) {
                MC2VR_LOG("conf: render_size_follow_hmd=%s not recognized (use on|off)", value);
            }
        } else if (strcmp(key, "render_aspect") == 0) {
            if (!render_size::set_aspect(value)) {
                MC2VR_LOG("conf: render_aspect=%s not recognized (use true|snap)", value);
            }
        } else if (strcmp(key, "vsync") == 0) {
            // Vsync pacing: off = force D3DPRESENT_INTERVAL_IMMEDIATE at
            // CreateDevice/Reset (the frame's two Presents are vsync-locked
            // and cap the game at ~30 Hz).
            if (!device_params::set_vsync(value)) {
                MC2VR_LOG("conf: vsync=%s not recognized (use on|off)", value);
            }
        } else if (strcmp(key, "debug_eye_dump_frames") == 0) {
            uint32_t n;
            if (parse_count(key, value, &n)) {
                eye_replay::set_dump_frames(n);
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
            // Hardware watchpoints on ViewEntry camera fields (culling-RE
            // evidence; docs/reverse_engineering/camera_data_flow.md).
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

void init()
{
    log_init();

    MC2VR_LOG("=== mc2vr carrier attached (pid=%lu) ===", GetCurrentProcessId());

    // Config (mc2vr.conf next to the DLL) — before any hook so the mode is
    // settled first.
    load_conf();

    // Gate: no hooks unless this is exactly the RE'd binary at the expected
    // base. Log everything either way — this log is the deliverable.
    const bool base_ok = verify_image_base();
    const bool lock_ok = base_ok && verify_build_lock();

    if (!lock_ok) {
        MC2VR_LOG("init complete — build lock FAILED, staying resident but idle");
        MC2VR_LOG("early init done (idle)"); // launcher's resume marker
        return;
    }

    // ---- Attach: runs before the game's first instruction (the launcher
    // starts it suspended and resumes on the marker below). Everything is
    // installed here; hooks on objects that do not exist yet are chained off
    // their creation (Direct3DCreate9 -> CreateDevice -> device VmtHook,
    // device.cpp) or the first Present (on_engine_running below).

    // Attach to the OpenXR host's shared section, if one exists (the
    // launcher spawns the host before the game). Non-fatal: no host means
    // the game runs the monitor-stereo path exactly as today. Only a
    // lock-passing carrier registers — an idle one stays fully idle.
    ipc::connect();
    // debug_xr_probe: its fault log, from attach on.
    xr_probe::install_early();

    adaptation::init();         // VR state that needs no hook (before the hooks fire)
    hooks::install_attach();    // the VR implementation's hooks (hooks/install.cpp)
    // Debug tools (each a no-op unless enabled in the conf).
    render::install_early();
    MC2VR_LOG("early init done"); // launcher's resume marker (src/launcher/main.c)
}

void on_engine_running()
{
    MC2VR_LOG("engine running (first Present, frame %llu) — post-boot installs",
              (unsigned long long)hooks::frame_count());

    // Queue poller (reads g_RenderQueue fields set up during boot).
    render::install_late();

    // Read-only live dump of the VM chain behind RenderTask_RenderFrame.
    vmdump::install();

    // debug_wait_census: game IAT wrappers (the loader has filled the IAT).
    wait_census::install();

    // debug_xr_probe: the in-process OpenXR probe, on its own thread.
    xr_probe::start();

    MC2VR_LOG("init complete"); // launcher waits for this before dumping the log
}

} // namespace mc2vr

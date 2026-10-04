#include "init.hpp"

#include <windows.h>

#include "build_lock.h"
#include "device.hpp"
#include "eye_replay.hpp"
#include "game_addresses.h"
#include "hooks.hpp"
#include "log.hpp"
#include "openvr_bridge.hpp"
#include "probes.hpp"
#include "render_dump.hpp"
#include "stream_capture.hpp"
#include "stub_trace.hpp"
#include "view_rewrite.hpp"
#include "vm_dump.hpp"
#include "sha256.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mc2vr {

// Sticky state for the view_asym_x/y conf pair (each key updates one
// component; both must survive the other's arrival).
static float g_conf_asym_x = 0.0f, g_conf_asym_y = 0.0f;

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
            if (!view::set_view_row_rewrite(value)) {
                MC2VR_LOG("conf: view_row_rewrite=%s not recognized "
                          "(use off|on|pulse|stereo) — defaulting to off", value);
                view::set_view_row_rewrite("off");
            }
        } else if (strcmp(key, "openvr") == 0) {
            // S4-1: OpenVR/SteamVR bridge bootstrap (openvr_bridge.cpp).
            if (!ovr::set_enabled(value)) {
                MC2VR_LOG("conf: openvr=%s not recognized (use on|off)", value);
            }
        } else if (strcmp(key, "openvr_init_registry") == 0) {
            // S4-1 run 5: in-game vrclient_init_registry DEADLOCKED the game
            // (Background-session teardown race) — off by default, experiments
            // only (docs/s4_handover.md).
            if (!ovr::set_init_registry(value)) {
                MC2VR_LOG("conf: openvr_init_registry=%s not recognized "
                          "(use on|off)", value);
            }
        } else if (strcmp(key, "stub_trace") == 0) {
            if (!trace::set_enabled(value)) {
                MC2VR_LOG("conf: stub_trace=%s not recognized (use on|off)", value);
            }
        } else if (strcmp(key, "vm_dump") == 0) {
            if (!vmdump::set_enabled(value)) {
                MC2VR_LOG("conf: vm_dump=%s not recognized (use on|off)", value);
            }
        } else if (strcmp(key, "stream_capture") == 0) {
            // S2c-0: render-command-stream capture + opcode census (read-only;
            // docs/stereo_design.md §S2). Requires the opcode MidHook (M3).
            if (!s2c::set_enabled(value)) {
                MC2VR_LOG("conf: stream_capture=%s not recognized (use on|off)", value);
            }
        } else if (strcmp(key, "eye_pass") == 0) {
            // S2c-2: deterministic per-pass eye (pass1=LEFT pass2=RIGHT).
            if (!eye::set_pass_enabled(value)) {
                MC2VR_LOG("conf: eye_pass=%s not recognized (use on|off)", value);
            }
        } else if (strcmp(key, "eye_rt") == 0) {
            // S2c-2: pass-2 SetRenderTarget(0)/StretchRect redirect to an eye RT.
            if (!eye::set_rt_enabled(value)) {
                MC2VR_LOG("conf: eye_rt=%s not recognized (use on|off)", value);
            }
        } else if (strcmp(key, "eye_monitor_pin") == 0) {
            // S2c-2: skip the pass-2 EndSubmit RT->backbuffer copy so the
            // monitor holds pass 1's LEFT image (S4 steady state).
            if (!eye::set_pin_enabled(value)) {
                MC2VR_LOG("conf: eye_monitor_pin=%s not recognized (use on|off)", value);
            }
        } else if (strcmp(key, "eye_dump_frames") == 0) {
            char *end = nullptr;
            const long n = strtol(value, &end, 10);
            if (end != value && end[0] == 0 && n >= 0) {
                eye::set_dump_frames((uint32_t)n);
            } else {
                MC2VR_LOG("conf: eye_dump_frames=%s not a count, ignored", value);
            }
        } else if (strcmp(key, "frame_replay") == 0) {
            // S2c-1: second draw pass — re-invoke PgPrimitive_SubmitToGPU after
            // the original (same eye/RTs; state-safety test, docs/stereo_design.md §S2).
            if (!s2c::set_replay_enabled(value)) {
                MC2VR_LOG("conf: frame_replay=%s not recognized (use on|off)", value);
            }
        } else if (strcmp(key, "stream_dump_frames") == 0) {
            char *end = nullptr;
            const long n = strtol(value, &end, 10);
            if (end != value && end[0] == 0 && n >= 0) {
                s2c::set_dump_frames((uint32_t)n);
            } else {
                MC2VR_LOG("conf: stream_dump_frames=%s not a count, ignored", value);
            }
        } else if (strcmp(key, "stream_dump_delay") == 0) {
            char *end = nullptr;
            const double d = strtod(value, &end);
            if (end != value && end[0] == 0 && d >= 0.0) {
                s2c::set_dump_delay((float)d);
                eye::set_dump_delay((float)d);
            } else {
                MC2VR_LOG("conf: stream_dump_delay=%s not a number, ignored", value);
            }
        } else if (strcmp(key, "view_row_amp") == 0) {
            // Pan amplitude in world units (default 4.0). ~0.05 for
            // game-scale checks; 0.032 = IPD scale.
            char *end = nullptr;
            const double amp = strtod(value, &end);
            if (end != value && end[0] == 0) {
                view::set_view_row_amp((float)amp);
            } else {
                MC2VR_LOG("conf: view_row_amp=%s not a number, ignored", value);
            }
        } else if (strcmp(key, "view_ipd") == 0) {
            // Full IPD in world units for view_row_rewrite=stereo (default
            // 0.065; per-eye offset is half of this).
            char *end = nullptr;
            const double ipd = strtod(value, &end);
            if (end != value && end[0] == 0) {
                view::set_view_ipd((float)ipd);
            } else {
                MC2VR_LOG("conf: view_ipd=%s not a number, ignored", value);
            }
        } else if (strcmp(key, "view_stereo_hold") == 0) {
            // Seconds each eye is held in stereo A/B mode (default 2.0).
            char *end = nullptr;
            const double hold = strtod(value, &end);
            if (end != value && end[0] == 0) {
                view::set_view_stereo_hold((float)hold);
            } else {
                MC2VR_LOG("conf: view_stereo_hold=%s not a number, ignored", value);
            }
        } else if (strcmp(key, "view_asym_x") == 0 ||
                   strcmp(key, "view_asym_y") == 0) {
            // Per-eye asymmetric-projection centre shift, NDC units. Both
            // keys land in one setter; values are sticky (default 0).
            char *end = nullptr;
            const double v = strtod(value, &end);
            if (end != value && end[0] == 0) {
                if (strcmp(key, "view_asym_x") == 0) {
                    view::set_view_asym((float)v, g_conf_asym_y);
                    g_conf_asym_x = (float)v;
                } else {
                    view::set_view_asym(g_conf_asym_x, (float)v);
                    g_conf_asym_y = (float)v;
                }
            } else {
                MC2VR_LOG("conf: %s=%s not a number, ignored", key, value);
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
// (verified in docs/initial_analysis.md); the in-memory image differs once
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
    // base. Log everything either way — the log is the M0 deliverable.
    const bool base_ok = verify_image_base();
    const bool lock_ok = base_ok && verify_build_lock();

    if (!lock_ok) {
        MC2VR_LOG("init complete — build lock FAILED, staying resident but idle");
        return;
    }

    // M1 SecuROM pre-probes, before any hook touches .text: each step logs,
    // so a crash is attributable to exactly one action (probes first — the
    // FrameTick patch below is the higher-risk live-.text test).
    probes::data_write_restore();
    probes::call_vm_thunk();

    hooks::install();

    // M2: device capture + VmtHook (Present/BeginScene/EndScene/Reset).
    // Best-effort: a failure here keeps the game and FrameTick hook alive.
    device::capture_and_hook();

    // M3: view-table dump + command histogram + slot claim test + queue poll.
    // Best-effort per component.
    render::install();

    // Read-only live dump of the VM chain behind RenderTask_RenderFrame.
    vmdump::install();

    // S4-1: OpenVR/SteamVR bootstrap (conf openvr=on). After everything else —
    // a SteamVR failure must not interfere with any installed hook, and the
    // bridge needs no game state for its connectivity smoke test.
    ovr::init();

    MC2VR_LOG("init complete");
}

} // namespace mc2vr

#include "init.hpp"

#include <windows.h>

#include "build_lock.h"
#include "device.hpp"
#include "game_addresses.h"
#include "hooks.hpp"
#include "log.hpp"
#include "probes.hpp"
#include "render_dump.hpp"
#include "stub_trace.hpp"
#include "view_rewrite.hpp"
#include "vm_dump.hpp"
#include "sha256.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mc2vr {

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
            // The viewContextData camera pan (view_rewrite.cpp).
            if (!view::set_view_row_rewrite(value)) {
                MC2VR_LOG("conf: view_row_rewrite=%s not recognized "
                          "(use off|on|pulse) — defaulting to off", value);
                view::set_view_row_rewrite("off");
            }
        } else if (strcmp(key, "stub_trace") == 0) {
            if (!trace::set_enabled(value)) {
                MC2VR_LOG("conf: stub_trace=%s not recognized (use on|off)", value);
            }
        } else if (strcmp(key, "vm_dump") == 0) {
            if (!vmdump::set_enabled(value)) {
                MC2VR_LOG("conf: vm_dump=%s not recognized (use on|off)", value);
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

    MC2VR_LOG("init complete");
}

} // namespace mc2vr

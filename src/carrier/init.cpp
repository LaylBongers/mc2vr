#include "init.hpp"

#include <windows.h>

#include "build_lock.h"
#include "game_addresses.h"
#include "hooks.hpp"
#include "log.hpp"
#include "sha256.h"

namespace mc2vr {

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

    // Gate: no hooks unless this is exactly the RE'd binary at the expected
    // base. Log everything either way — the log is the M0 deliverable.
    const bool base_ok = verify_image_base();
    const bool lock_ok = base_ok && verify_build_lock();

    if (!lock_ok) {
        MC2VR_LOG("init complete — build lock FAILED, staying resident but idle");
        return;
    }

    hooks::install();

    MC2VR_LOG("init complete");
}

} // namespace mc2vr

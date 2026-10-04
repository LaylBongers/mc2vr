// mc2vr launcher (M0): start the game, wait for boot-complete (frame counter
// moving), then inject the carrier DLL. Orchestration only — all game
// memory *writes* happen in-process in the carrier.
//
// Layout: launcher + carrier are deployed into <game_dir>\mc2vr\. Paths are
// resolved relative to the launcher's own directory, so the working
// directory doesn't matter:
//   game exe   = <launcher_dir>\..\Mercenaries2.exe
//   carrier    = <launcher_dir>\mc2vr_carrier.dll
//   log file   = <launcher_dir>\mc2vr_launcher.log

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>

#include "game_addresses.h"
#include "boot_wait.h"
#include "inject.h"

#define GAME_EXE_DEFAULT     L"MERCENARIES2.EXE"
#define BOOT_POLL_MS        100
#define BOOT_TIMEOUT_MS     (180 * 1000)
#define INJECT_TIMEOUT_MS   (30 * 1000)
#define ACK_TIMEOUT_MS      (15 * 1000)

static wchar_t g_launcher_dir[MAX_PATH];
static wchar_t g_log_path[MAX_PATH];
static FILE *g_log_file;

static void mc2_log(const char *fmt, ...)
{
    va_list ap;

    SYSTEMTIME st;
    GetLocalTime(&st);

    char line[1024];
    int n = snprintf(line, sizeof(line), "[%02u:%02u:%02u.%03u] ",
                     st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_start(ap, fmt);
    vsnprintf(line + n, sizeof(line) - (size_t)n, fmt, ap);
    va_end(ap);

    fputs(line, stdout);
    fputc('\n', stdout);
    if (g_log_file) {
        fputs(line, g_log_file);
        fputc('\n', g_log_file);
        fflush(g_log_file);
    }
}

// Strip the final path component. Overwrites in place.
static void path_dirname(wchar_t *path)
{
    wchar_t *slash = wcsrchr(path, L'\\');
    if (slash) {
        *slash = L'\0';
    }
}

static void path_join(wchar_t *out, size_t out_elems, const wchar_t *dir, const wchar_t *name)
{
    _snwprintf(out, out_elems, L"%ls\\%ls", dir, name);
    out[out_elems - 1] = L'\0';
}

// Resolve `rel` (possibly relative) against `base` into a full path.
static BOOL resolve_path(const wchar_t *rel, const wchar_t *base, wchar_t *out, size_t out_elems)
{
    wchar_t tmp[MAX_PATH];
    if (GetFullPathNameW(rel, MAX_PATH, tmp, NULL) == 0) {
        return FALSE;
    }
    (void)base;
    wcscpy_s(out, out_elems, tmp);
    return TRUE;
}

static BOOL file_exists(const wchar_t *path)
{
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

// ---- S4-2 option (c): arm DXVK's boot-time OpenVR interop --------------------
//
// Runs 7/8 (docs/s4_handover.md §S4 item 2) live-proved that
// IVRCompositor::Submit with a D3D9 texture CRASHES the game process when
// DXVK's boot-time OpenVR interop is disarmed — the conversion path is only
// initialized at DEVICE CREATION, and device creation happens at game boot,
// BEFORE the carrier exists. So the registry values must be in place before
// the game process starts; this is the ONLY place that can do it.
//
// The carrier's attach-time ensure_vr_registry (openvr_bridge.cpp) is NOT
// enough for this: it runs after the device was created. The two writers
// coexist: this one arms the boot interop (conf openvr_boot_interop=on), the
// carrier one still serves vrclient for openvr=on boots without it.
//
// RUN-6A DISCIPLINE (live-proven): with the values present at boot, DXVK's
// d3d9 connects to SteamVR at device creation — if SteamVR is mid-transition
// (standby cycling / vrserver restarting) the game can hang before the main
// loop. SteamVR must be FULLY UP and stable BEFORE launching with
// openvr_boot_interop=on. It must be up anyway for openvr=on.

// Minimal conf parse: key=value lines, '#' comments. Returns TRUE when the
// key exists with value "on".
static BOOL conf_key_on(const wchar_t *conf_path, const char *key)
{
    FILE *f = _wfopen(conf_path, L"r");
    if (!f) {
        return FALSE;
    }
    char line[512];
    BOOL on = FALSE;
    size_t keylen = strlen(key);
    while (fgets(line, sizeof(line), f)) {
        char *hash = strchr(line, '#');
        if (hash) {
            *hash = '\0';
        }
        char *eq = strchr(line, '=');
        if (!eq || (size_t)(eq - line) != keylen || strncmp(line, key, keylen) != 0) {
            continue;
        }
        char *value = eq + 1;
        while (*value == ' ' || *value == '\t') value++;
        char *end = value + strlen(value);
        while (end > value && (end[-1] == ' ' || end[-1] == '\t' ||
                               end[-1] == '\r' || end[-1] == '\n')) {
            *--end = '\0';
        }
        if (strcmp(value, "on") == 0) {
            on = TRUE;
        }
        break;
    }
    fclose(f);
    return on;
}

static void arm_boot_interop(void)
{
    // Same three values as the carrier's ensure_vr_registry (openvr_bridge.cpp)
    // — proven values; PROTON_VR_RUNTIME is set by `proton run`, so the
    // launcher sees exactly what the game process would.
    char rt[MAX_PATH];
    DWORD n = GetEnvironmentVariableA("PROTON_VR_RUNTIME", rt, sizeof(rt));
    if (n == 0 || n >= sizeof(rt)) {
        mc2_log("warning: openvr_boot_interop=on but PROTON_VR_RUNTIME is "
                "missing — boot interop NOT armed (is SteamVR-for-Proton set up?)");
        return;
    }

    HKEY key = NULL;
    DWORD disp = 0;
    LSTATUS rc = RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\Wine\\VR", 0,
                                 NULL, 0, KEY_SET_VALUE, NULL, &key, &disp);
    if (rc != ERROR_SUCCESS) {
        mc2_log("warning: RegCreateKeyExA(Software\\Wine\\VR) failed %lu — "
                "boot interop NOT armed", rc);
        return;
    }

    BOOL ok = TRUE;
    if (RegSetValueExA(key, "PROTON_VR_RUNTIME", 0, REG_SZ,
                       (const BYTE *)rt, n) != ERROR_SUCCESS) {
        ok = FALSE;
    }
    const DWORD state = 1; // ready (probe-proven value; see openvr_bridge.cpp)
    if (RegSetValueExA(key, "state", 0, REG_DWORD,
                       (const BYTE *)&state, sizeof(state)) != ERROR_SUCCESS) {
        ok = FALSE;
    }
    // The compositor reports EMPTY required instance extensions — "" is the
    // semantically correct value (probe-verified).
    if (RegSetValueExA(key, "openvr_vulkan_instance_extensions", 0, REG_SZ,
                       (const BYTE *)"", 1) != ERROR_SUCCESS) {
        ok = FALSE;
    }
    RegCloseKey(key);
    mc2_log("openvr: boot-time DXVK interop ARMED (SteamVR must be up and "
            "stable — run-6a discipline; disp=%lu, %s)",
            disp, ok ? "ok" : "PARTIAL");
}

int main(void) // no arguments: everything is resolved from the install layout
{
    // ---- Locate ourselves ------------------------------------------------
    if (GetModuleFileNameW(NULL, g_launcher_dir, MAX_PATH) == 0) {
        fprintf(stderr, "fatal: cannot determine launcher path\n");
        return 1;
    }
    path_dirname(g_launcher_dir);
    path_join(g_log_path, MAX_PATH, g_launcher_dir, L"mc2vr_launcher.log");

    g_log_file = _wfopen(g_log_path, L"w");
    // Log file is best-effort; console output is the primary channel.

    mc2_log("mc2vr launcher starting (pid=%lu)", GetCurrentProcessId());

    // ---- Resolve paths ----------------------------------------------------
    wchar_t game_dir[MAX_PATH];
    wcscpy_s(game_dir, MAX_PATH, g_launcher_dir);
    path_dirname(game_dir); // launcher lives in <game_dir>\mc2vr

    wchar_t game_exe[MAX_PATH];
    wchar_t carrier_dll[MAX_PATH];
    path_join(game_exe, MAX_PATH, game_dir, GAME_EXE_DEFAULT);
    path_join(carrier_dll, MAX_PATH, g_launcher_dir, L"mc2vr_carrier.dll");

    if (!file_exists(game_exe)) {
        mc2_log("fatal: game exe not found: %ls", game_exe);
        return 1;
    }
    if (!file_exists(carrier_dll)) {
        mc2_log("fatal: carrier dll not found: %ls", carrier_dll);
        return 1;
    }
    mc2_log("game exe : %ls", game_exe);
    mc2_log("carrier  : %ls", carrier_dll);

    // ---- S4-2 option (c): arm the boot-time interop BEFORE the game starts --
    // DXVK's d3d9 reads these values at device creation (early game boot);
    // after CreateProcessW is too late. Conf-gated (default off).
    wchar_t conf_path[MAX_PATH];
    path_join(conf_path, MAX_PATH, g_launcher_dir, L"mc2vr.conf");
    if (conf_key_on(conf_path, "openvr_boot_interop")) {
        arm_boot_interop();
    }

    // ---- Start the game ----------------------------------------------------
    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    // Normal start — no suspension needed (docs/launcher_plan.md step 2).
    if (!CreateProcessW(game_exe, NULL, NULL, NULL, FALSE, 0, NULL, game_dir, &si, &pi)) {
        mc2_log("fatal: CreateProcessW failed (%lu)", GetLastError());
        return 1;
    }
    CloseHandle(pi.hThread);
    mc2_log("game started (pid=%lu)", pi.dwProcessId);

    // ---- Verify load base --------------------------------------------------
    // All hook VAs are absolute; the image can't relocate, so anything other
    // than the fixed base means our addresses are wrong — refuse loudly.
    uintptr_t base = 0;
    DWORD rc = mc2_remote_module_base(pi.hProcess, GAME_EXE_DEFAULT, &base, 50);
    if (rc != 0) {
        mc2_log("fatal: cannot find game module in remote process (%lu)", rc);
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess);
        return 1;
    }
    if (base != MC2_GAME_BASE_EXPECTED) {
        mc2_log("fatal: game loaded at unexpected base %p (expected %p); "
                "all recorded VAs are invalid",
                (void *)base, (void *)MC2_GAME_BASE_EXPECTED);
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess);
        return 1;
    }
    mc2_log("game module base verified: %p", (void *)base);

    // ---- Wait for boot completion ------------------------------------------
    mc2_log("waiting for main-loop frame counter at %p ...",
            (void *)MC2_FRAME_COUNTER_2);
    uint64_t counter_initial = 0;
    uint64_t counter_final = 0;
    rc = mc2_wait_boot_counter(pi.hProcess, MC2_FRAME_COUNTER_2,
                               BOOT_POLL_MS, BOOT_TIMEOUT_MS,
                               &counter_initial, &counter_final);
    if (rc != 0) {
        mc2_log("fatal: boot marker never ticked twice (rc=%lu) — game may be "
                "stuck or this build mismatches the RE'd binary", rc);
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess);
        return 1;
    }
    // The values are logged so "counter moving" can be audited against the
    // plan's expectation (a frame counter shows small, steady increments).
    mc2_log("boot complete: main loop ticking (counter %016llx -> %016llx)",
            (unsigned long long)counter_initial, (unsigned long long)counter_final);

    // ---- Inject the carrier --------------------------------------------------
    rc = mc2_inject_dll(pi.hProcess, carrier_dll, INJECT_TIMEOUT_MS);
    if (rc != 0) {
        mc2_log("fatal: carrier injection failed (rc=%lu)", rc);
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess);
        return 1;
    }
    mc2_log("carrier injected");

    // Game keeps running on its own from here.
    CloseHandle(pi.hProcess);

    // ---- Wait for the carrier's "attached" ack ------------------------------
    // M0 success criterion: carrier logs "attached". Poll for the actual log
    // line rather than bare file existence — the file is created before the
    // first line is written (a plain existence check raced and dumped an
    // empty file in the first real run).
    wchar_t carrier_log[MAX_PATH];
    path_join(carrier_log, MAX_PATH, g_launcher_dir, L"mc2vr_carrier.log");

    FILE *f = NULL;
    for (DWORD waited = 0; waited < ACK_TIMEOUT_MS; waited += 250) {
        if ((f = _wfopen(carrier_log, L"r")) != NULL) {
            char line[512];
            BOOL attached = FALSE;
            while (fgets(line, sizeof(line), f)) {
                if (strstr(line, "carrier attached") != NULL) {
                    attached = TRUE;
                    break;
                }
            }
            if (attached) {
                break; // ack received — reopen below for the full dump
            }
            fclose(f);
            f = NULL;
        }
        Sleep(250);
    }

    if (f) {
        // Rewind and dump everything written so far.
        fseek(f, 0, SEEK_SET);
        mc2_log("---- carrier log ----");
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            fputs(line, stdout);
            if (g_log_file) {
                fputs(line, g_log_file);
            }
        }
        fclose(f);
        mc2_log("---- end carrier log ----");
    } else {
        mc2_log("warning: carrier ack not found after %us — check mc2vr_carrier.log",
                ACK_TIMEOUT_MS / 1000);
    }

    mc2_log("done — game is running with the carrier attached");
    if (g_log_file) {
        fclose(g_log_file);
    }
    return 0;
}

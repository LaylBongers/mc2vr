// 32-bit OpenXR runtime selection — see xr_runtime.h.

#include "xr_runtime.h"

#include <stdio.h>
#include <string.h>

// ---- Unix files from the Wine side ----------------------------------------

typedef WCHAR *(CDECL *WineGetDosFileName)(const char *unix_path);

BOOL xr_under_wine(void)
{
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    return ntdll != NULL && GetProcAddress(ntdll, "wine_get_version") != NULL;
}

// Opens a Unix path for reading: Wine's own path mapping, or Z: (Wine's root
// drive) if that export is missing.
static HANDLE open_unix(const char *path)
{
    static WineGetDosFileName to_dos = NULL;
    static BOOL looked = FALSE;
    if (!looked) {
        looked = TRUE;
        to_dos = (WineGetDosFileName)GetProcAddress(GetModuleHandleW(L"kernel32.dll"),
                                                    "wine_get_dos_file_name");
    }
    HANDLE h = INVALID_HANDLE_VALUE;
    WCHAR *dos = to_dos != NULL ? to_dos(path) : NULL;
    if (dos != NULL) {
        h = CreateFileW(dos, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        HeapFree(GetProcessHeap(), 0, dos);
        return h;
    }
    char z[1024];
    if (snprintf(z, sizeof(z), "Z:%s", path) >= (int)sizeof(z)) {
        return INVALID_HANDLE_VALUE;
    }
    for (char *c = z; *c != '\0'; c++) {
        if (*c == '/') *c = '\\';
    }
    return CreateFileA(z, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
}

// Reads up to cap-1 bytes, NUL-terminated. FALSE if the file can't be opened.
static BOOL read_unix(const char *path, char *buf, size_t cap)
{
    HANDLE h = open_unix(path);
    if (h == INVALID_HANDLE_VALUE) {
        return FALSE;
    }
    DWORD got = 0;
    const BOOL ok = ReadFile(h, buf, (DWORD)(cap - 1), &got, NULL);
    CloseHandle(h);
    buf[ok ? got : 0] = '\0';
    return ok;
}

static BOOL exists_unix(const char *path)
{
    HANDLE h = open_unix(path);
    if (h == INVALID_HANDLE_VALUE) {
        return FALSE;
    }
    CloseHandle(h);
    return TRUE;
}

// ELF class of a shared object: 1 = 32-bit, 2 = 64-bit, 0 = not an ELF file.
static int elf_class(const char *path)
{
    char head[8];
    if (!read_unix(path, head, sizeof(head)) || memcmp(head, "\x7f" "ELF", 4) != 0) {
        return 0;
    }
    return head[4];
}

// ---- runtime manifests -----------------------------------------------------

// The string value of "key" in a flat JSON manifest (no escapes, as runtime
// manifests write them). FALSE if absent.
static BOOL json_string(const char *json, const char *key, char *out, size_t cap)
{
    char quoted[64];
    snprintf(quoted, sizeof(quoted), "\"%s\"", key);
    const char *p = strstr(json, quoted);
    if (p == NULL || (p = strchr(p + strlen(quoted), ':')) == NULL || (p = strchr(p, '"')) == NULL) {
        return FALSE;
    }
    const char *end = strchr(++p, '"');
    if (end == NULL || (size_t)(end - p) >= cap) {
        return FALSE;
    }
    memcpy(out, p, (size_t)(end - p));
    out[end - p] = '\0';
    return TRUE;
}

static BOOL json_true(const char *json, const char *key)
{
    char quoted[64];
    snprintf(quoted, sizeof(quoted), "\"%s\"", key);
    const char *p = strstr(json, quoted);
    if (p == NULL || (p = strchr(p + strlen(quoted), ':')) == NULL) {
        return FALSE;
    }
    while (*++p == ' ' || *p == '\t') {
    }
    return strncmp(p, "true", 4) == 0;
}

typedef char *(CDECL *WineGetUnixFileName)(const WCHAR *dos_path);

// The Unix $HOME. Wine keeps a few Unix variables that clash with Windows ones
// (HOME, PATH, TEMP, ...) under a WINE_HOST_ prefix in the Windows
// environment, and sets WINEHOMEDIR to the home directory as an NT path
// (\??\Z:\...). FALSE when none of the three is there.
static BOOL unix_home(char *out, DWORD cap)
{
    if (GetEnvironmentVariableA("HOME", out, cap) > 0 && out[0] == '/') {
        return TRUE;
    }
    if (GetEnvironmentVariableA("WINE_HOST_HOME", out, cap) > 0 && out[0] == '/') {
        return TRUE;
    }
    WCHAR nt[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"WINEHOMEDIR", nt, MAX_PATH);
    WineGetUnixFileName to_unix = (WineGetUnixFileName)GetProcAddress(GetModuleHandleW(L"kernel32.dll"),
                                                                      "wine_get_unix_file_name");
    if (n == 0 || n >= MAX_PATH || to_unix == NULL) {
        return FALSE;
    }
    const WCHAR *dos = wcsncmp(nt, L"\\??\\", 4) == 0 ? nt + 4 : nt;
    char *unix_path = to_unix(dos);
    if (unix_path == NULL) {
        return FALSE;
    }
    const BOOL ok = strlen(unix_path) < cap;
    if (ok) {
        strcpy(out, unix_path);
    }
    HeapFree(GetProcessHeap(), 0, unix_path);
    return ok;
}

// The loader's search for a 32-bit process. Returns 1 = a 32-bit
// registration exists, 2 = `active` = the undecorated file found, 0 = none,
// -1 = no home directory to search (and no XDG_CONFIG_HOME) and none found.
static int find_active_runtime(char *active, size_t cap, char *config_home, size_t home_cap)
{
    char dirs[16][512];
    int n = 0;
    char v[1024];
    BOOL have_home = TRUE;
    if (GetEnvironmentVariableA("XDG_CONFIG_HOME", v, sizeof(v)) > 0) {
        snprintf(dirs[n++], sizeof(dirs[0]), "%s", v);
    } else if (unix_home(v, sizeof(v))) {
        snprintf(dirs[n++], sizeof(dirs[0]), "%s/.config", v);
    } else {
        have_home = FALSE;
    }
    snprintf(config_home, home_cap, "%s", have_home ? dirs[0] : "");
    if (GetEnvironmentVariableA("XDG_CONFIG_DIRS", v, sizeof(v)) == 0) {
        strcpy(v, "/etc/xdg");
    }
    for (char *tok = strtok(v, ":"); tok != NULL && n < 15; tok = strtok(NULL, ":")) {
        snprintf(dirs[n++], sizeof(dirs[0]), "%s", tok);
    }
    snprintf(dirs[n++], sizeof(dirs[0]), "/etc");
    char path[600];
    for (int i = 0; i < n; i++) {
        snprintf(path, sizeof(path), "%s/openxr/1/active_runtime.i686.json", dirs[i]);
        if (exists_unix(path)) {
            return 1;
        }
        snprintf(path, sizeof(path), "%s/openxr/1/active_runtime.json", dirs[i]);
        if (exists_unix(path)) {
            snprintf(active, cap, "%s", path);
            return 2;
        }
    }
    return have_home ? 0 : -1;
}

BOOL xr_runtime_32_check(char *fix, size_t cap, const char **why)
{
    static const char kSteamVr64Lib[] = "/bin/linux64/vrclient.so";
    char v[16];
    fix[0] = '\0';
    if (!xr_under_wine()) {
        *why = "not under Wine";
        return TRUE;
    }
    if (GetEnvironmentVariableA("XR_RUNTIME_JSON", v, sizeof(v)) > 0) {
        *why = "XR_RUNTIME_JSON set";
        return TRUE;
    }
    char active[600], config_home[512];
    switch (find_active_runtime(active, sizeof(active), config_home, sizeof(config_home))) {
    case 1:
        *why = "a 32-bit runtime is registered (active_runtime.i686.json)";
        return TRUE;
    case 0:
        *why = "no OpenXR runtime registered";
        return TRUE;
    case -1:
        *why = "no OpenXR runtime found, and no home directory in the environment "
               "(HOME / WINE_HOST_HOME / WINEHOMEDIR) to look in";
        return TRUE;
    default:
        break;
    }
    char json[4096], lib[600];
    if (!read_unix(active, json, sizeof(json)) || !json_string(json, "library_path", lib, sizeof(lib))) {
        *why = "active_runtime.json unreadable or without library_path";
        return TRUE;
    }
    if (lib[0] == '/' && elf_class(lib) == 1) {
        *why = "the registered runtime is already 32-bit";
        return TRUE;
    }
    // SteamVR: <root>/bin/linux64/vrclient.so is the 64-bit library; the
    // 32-bit runtime is <root>/steamxr_linux32.json (library bin/vrclient.so).
    const size_t len = strlen(lib), suffix = sizeof(kSteamVr64Lib) - 1;
    if (!json_true(json, "VALVE_runtime_is_steamvr") || lib[0] != '/' || len <= suffix ||
        strcmp(lib + len - suffix, kSteamVr64Lib) != 0) {
        *why = "the registered runtime is 64-bit and not SteamVR (no known 32-bit counterpart)";
        return FALSE;  // broken, no fix to suggest
    }
    lib[len - suffix] = '\0';  // the SteamVR root
    char manifest[700], lib32[600], lib32_path[1300];
    snprintf(manifest, sizeof(manifest), "%s/steamxr_linux32.json", lib);
    if (!read_unix(manifest, json, sizeof(json)) || !json_string(json, "library_path", lib32, sizeof(lib32))) {
        *why = "the registered SteamVR runtime is 64-bit and SteamVR has no readable steamxr_linux32.json";
        return FALSE;
    }
    if (lib32[0] == '/') {
        snprintf(lib32_path, sizeof(lib32_path), "%s", lib32);
    } else {
        snprintf(lib32_path, sizeof(lib32_path), "%s/%s", lib, lib32);
    }
    if (elf_class(lib32_path) != 1) {
        *why = "the registered SteamVR runtime is 64-bit and its steamxr_linux32.json library is not 32-bit";
        return FALSE;
    }
    *why = "SteamVR registers only its 64-bit runtime";
    if (config_home[0] != '\0') {
        snprintf(fix, cap, "ln -s '%s' '%s/openxr/1/active_runtime.i686.json'", manifest, config_home);
    }
    return FALSE;
}

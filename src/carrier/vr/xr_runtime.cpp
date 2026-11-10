// The OpenXR runtime without a loader — see xr_runtime.hpp.

#include "vr/xr_runtime.hpp"

#include <cstdio>
#include <cstring>

#include "log.hpp"

namespace mc2vr::xr_runtime {

namespace {

bool under_wine()
{
    const HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    return ntdll != nullptr && GetProcAddress(ntdll, "wine_get_version") != nullptr;
}

// The "library_path" string of a runtime manifest (minimal JSON reading: the
// key, a colon, a string with \\ and \/ escapes). False when absent.
bool manifest_library(const char *json_path, char *out, size_t n)
{
    FILE *f = fopen(json_path, "rb");
    if (f == nullptr) {
        MC2VR_LOG("xr: runtime manifest %s not readable", json_path);
        return false;
    }
    static char buf[16384];
    const size_t len = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[len] = '\0';
    const char *p = strstr(buf, "\"library_path\"");
    if (p == nullptr || (p = strchr(p + 14, ':')) == nullptr || (p = strchr(p, '"')) == nullptr) {
        MC2VR_LOG("xr: runtime manifest %s has no library_path", json_path);
        return false;
    }
    char lib[MAX_PATH];
    size_t k = 0;
    for (p++; *p != '\0' && *p != '"' && k + 1 < sizeof(lib); p++) {
        if (*p == '\\' && (p[1] == '\\' || p[1] == '/' || p[1] == '"')) {
            p++;
        }
        lib[k++] = *p;
    }
    lib[k] = '\0';
    // Relative paths are relative to the manifest's directory.
    const bool absolute = (lib[0] != '\0' && lib[1] == ':') || lib[0] == '\\' || lib[0] == '/';
    if (absolute) {
        snprintf(out, n, "%s", lib);
    } else {
        char dir[MAX_PATH];
        snprintf(dir, sizeof(dir), "%s", json_path);
        char *slash = strrchr(dir, '\\');
        char *fwd = strrchr(dir, '/');
        if (fwd > slash) slash = fwd;
        if (slash != nullptr) *slash = '\0'; else dir[0] = '\0';
        snprintf(out, n, "%s%s%s", dir, dir[0] ? "\\" : "", lib);
    }
    return true;
}

HMODULE load_windows_runtime()
{
    char json[MAX_PATH] = "";
    const DWORD env = GetEnvironmentVariableA("XR_RUNTIME_JSON", json, sizeof(json));
    if (env == 0 || env >= sizeof(json)) {
        DWORD size = sizeof(json);
        if (RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Khronos\\OpenXR\\1", "ActiveRuntime",
                         RRF_RT_REG_SZ, nullptr, json, &size) != ERROR_SUCCESS) {
            MC2VR_LOG("xr: no XR_RUNTIME_JSON and no 32-bit ActiveRuntime registered — no runtime");
            return nullptr;
        }
    }
    char lib[MAX_PATH];
    if (!manifest_library(json, lib, sizeof(lib))) {
        return nullptr;
    }
    MC2VR_LOG("xr: runtime manifest %s -> %s", json, lib);
    HMODULE m = LoadLibraryA(lib);
    if (m == nullptr) {
        MC2VR_LOG("xr: LoadLibrary(%s) failed (%lu) — a 64-bit-only runtime?", lib, GetLastError());
    }
    return m;
}

} // namespace

PFN_xrGetInstanceProcAddr load()
{
    static bool tried = false;
    static PFN_xrGetInstanceProcAddr gipa = nullptr;
    if (tried) {
        return gipa;
    }
    tried = true;

    HMODULE rt = nullptr;
    if (under_wine()) {
        rt = LoadLibraryW(L"wineopenxr.dll");
        if (rt == nullptr) {
            MC2VR_LOG("xr: no 32-bit wineopenxr.dll (%lu) — Proton without OpenXR?", GetLastError());
        }
    } else {
        rt = load_windows_runtime();
    }
    if (rt == nullptr) {
        return nullptr;
    }
    auto negotiate = reinterpret_cast<PFN_xrNegotiateLoaderRuntimeInterface>(
        GetProcAddress(rt, "xrNegotiateLoaderRuntimeInterface"));
    if (negotiate == nullptr) {
        MC2VR_LOG("xr: the runtime DLL has no xrNegotiateLoaderRuntimeInterface");
        return nullptr;
    }
    XrNegotiateLoaderInfo li{};
    li.structType = XR_LOADER_INTERFACE_STRUCT_LOADER_INFO;
    li.structVersion = XR_LOADER_INFO_STRUCT_VERSION;
    li.structSize = sizeof(li);
    li.minInterfaceVersion = 1;
    li.maxInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
    li.minApiVersion = XR_MAKE_VERSION(1, 0, 0);
    li.maxApiVersion = XR_MAKE_VERSION(1, 0x3ff, 0xfff);
    XrNegotiateRuntimeRequest rr{};
    rr.structType = XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST;
    rr.structVersion = XR_RUNTIME_INFO_STRUCT_VERSION;
    rr.structSize = sizeof(rr);
    const XrResult r = negotiate(&li, &rr);
    if (XR_FAILED(r) || rr.getInstanceProcAddr == nullptr) {
        // -6 under Wine: the Wine VR registry is missing (docs/openxr_proton.md item 2).
        MC2VR_LOG("xr: runtime negotiation failed (%d)", (int)r);
        return nullptr;
    }
    MC2VR_LOG("xr: runtime negotiated (interface %u, api %u.%u.%u)", rr.runtimeInterfaceVersion,
              (unsigned)XR_VERSION_MAJOR(rr.runtimeApiVersion),
              (unsigned)XR_VERSION_MINOR(rr.runtimeApiVersion),
              (unsigned)XR_VERSION_PATCH(rr.runtimeApiVersion));
    gipa = rr.getInstanceProcAddr;
    return gipa;
}

} // namespace mc2vr::xr_runtime

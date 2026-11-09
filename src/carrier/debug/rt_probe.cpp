// Render-target / render-size probe (log-only). See rt_probe.hpp.
//
// What it logs:
//   - who binds the backbuffer/front (bind/stretch signatures per pass, the
//     FUN_0074fae0 front-ref read, device getter callers);
//   - the depth mode (g_LtiRenderer+0x594) and the live table contents;
//   - the surface registries' vtables;
//   - the order of OnScreenResize vs SetupRenderTargets with the size and
//     aspect state around each;
//   - the DEFAULT-pool creation census (sizes + callers).
// Every line is prefixed "rtprobe:". Game code addresses print as raw VAs
// (paste straight into Ghidra); stacks are heuristic return-address scans.

#include "debug/rt_probe.hpp"

#include <safetyhook.hpp>

#include <windows.h>
#include <d3d9.h> // types only

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "vr/eye_replay.hpp"
#include "hooks/device.hpp"
#include "hooks/frame_tick.hpp"
#include "log.hpp"
#include "game/addresses.h"

namespace mc2vr::rt_probe {

namespace {

bool g_enabled = false;

// Census kinds (device creation / getter VmtHook slots, installed at device
// creation — see install_device_hooks).
enum class Create : uint8_t { Texture, RenderTarget, DepthStencil, OffscreenPlain };
enum class Getter : uint8_t { BackBuffer, RenderTarget, DepthStencil, SwapChain };


constexpr double REPORT_INTERVAL_SEC = 10.0;
constexpr int SIG_DEPTH = 6;      // return addresses kept per signature
constexpr int STACK_SCAN_DWORDS = 512;

// ---- helpers ------------------------------------------------------------------

template <typename T> T rd(uintptr_t addr) { return *(volatile const T *)addr; }

uintptr_t rs(uintptr_t off) { return MC2_G_RENDERSHELL + off; }

void *table_slot(uint32_t i)
{
    return rd<void *>(rs(MC2_RS_RT_TABLE_OFF) + i * 4u);
}

const char *slot_name(uint32_t i)
{
    static const char *names[MC2_RS_RT_TABLE_SLOTS] = {
        "+3e90 main", "+3e94 autoDepth", "+3e98 frontRef", "+3e9c ?",   "+3ea0 sceneDepth",
        "+3ea4 front", "+3ea8 ?",        "+3eac ?",        "+3eb0 ?"};
    return i < MC2_RS_RT_TABLE_SLOTS ? names[i] : "?";
}

// Return address heuristic: the bytes before `v` decode as a CALL.
bool looks_like_ret(uintptr_t v)
{
    if (v < MC2_TEXT_BEGIN + 8 || v >= MC2_TEXT_END) {
        return false;
    }
    const uint8_t *p = (const uint8_t *)v;
    if (p[-5] == 0xE8) {
        return true; // call rel32
    }
    // call r/m32 = FF /2
    const uint8_t m1 = p[-1], m2 = p[-2], m5 = p[-5];
    if (p[-2] == 0xFF && (m1 & 0x38) == 0x10 && ((m1 & 0xC0) == 0xC0 || (m1 & 0xC0) == 0x00)) {
        return true; // call reg / call [reg]
    }
    if (p[-3] == 0xFF && (m2 & 0x38) == 0x10 && (m2 & 0xC0) == 0x40) {
        return true; // call [reg+disp8]
    }
    if (p[-6] == 0xFF && (m5 & 0x38) == 0x10 && ((m5 & 0xC0) == 0x80 || (m5 & 0xC7) == 0x05)) {
        return true; // call [reg+disp32] / call [abs32]
    }
    if (p[-4] == 0xFF && (p[-3] & 0x38) == 0x10 && (p[-3] & 0x07) == 0x04 && (p[-3] & 0xC0) == 0x40) {
        return true; // call [sib+disp8]
    }
    return false;
}

struct Sig {
    uintptr_t a[SIG_DEPTH];
};

void stack_sig(uintptr_t sp, Sig *out)
{
    memset(out, 0, sizeof(*out));
    const uintptr_t base = (uintptr_t)((NT_TIB *)NtCurrentTeb())->StackBase;
    int n = 0;
    for (int i = 0; i < STACK_SCAN_DWORDS && n < SIG_DEPTH; i++) {
        const uintptr_t at = sp + (uintptr_t)i * 4u;
        if (at + 4 > base) {
            break;
        }
        const uintptr_t v = *(const uintptr_t *)at;
        if (looks_like_ret(v)) {
            out->a[n++] = v;
        }
    }
}

void stack_sig_here(Sig *out)
{
    stack_sig((uintptr_t)__builtin_frame_address(0), out);
}

void fmt_sig(const Sig &s, char *out, size_t size)
{
    int n = 0;
    out[0] = '\0';
    for (int i = 0; i < SIG_DEPTH && s.a[i]; i++) {
        n += _snprintf(out + n, size - n, "%s0x%08x", i ? "<" : "", (unsigned)s.a[i]);
        if (n < 0 || (size_t)n >= size) {
            out[size - 1] = '\0';
            return;
        }
    }
    if (n == 0) {
        _snprintf(out, size, "(none)");
        out[size - 1] = '\0';
    }
}

void fmt_code(void *addr, char *out, size_t size)
{
    const uintptr_t v = (uintptr_t)addr;
    if (v >= MC2_TEXT_BEGIN && v < MC2_TEXT_END) {
        _snprintf(out, size, "0x%08x", (unsigned)v);
        out[size - 1] = '\0';
    } else {
        describe_code_address(addr, out, size);
    }
}

bool in_d3d9_module(void *obj)
{
    if (!obj) {
        return false;
    }
    HMODULE module = nullptr;
    void *vt = *(void **)obj;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)vt, &module)) {
        return false;
    }
    return module == GetModuleHandleW(L"d3d9.dll");
}

// Resource description via the object's own vtable (IDirect3DResource9
// GetType = slot 10; surface GetDesc = 12; texture GetLevelCount = 13,
// GetLevelDesc = 17). Only for objects whose vtable lives in d3d9.dll.
void describe_resource(void *obj, char *out, size_t size)
{
    if (!obj) {
        _snprintf(out, size, "null");
        out[size - 1] = '\0';
        return;
    }
    if (!in_d3d9_module(obj)) {
        char vt[96];
        describe_code_address(*(void **)obj, vt, sizeof(vt));
        _snprintf(out, size, "%p (vtbl %s — not a d3d9 object)", obj, vt);
        out[size - 1] = '\0';
        return;
    }
    void **vt = *(void ***)obj;
    const DWORD type = ((DWORD(__stdcall *)(void *))vt[10])(obj);
    D3DSURFACE_DESC d = {};
    HRESULT hr = E_FAIL;
    DWORD levels = 0;
    if (type == D3DRTYPE_SURFACE) {
        hr = ((HRESULT(__stdcall *)(void *, D3DSURFACE_DESC *))vt[12])(obj, &d);
    } else if (type == D3DRTYPE_TEXTURE) {
        levels = ((DWORD(__stdcall *)(void *))vt[13])(obj);
        hr = ((HRESULT(__stdcall *)(void *, UINT, D3DSURFACE_DESC *))vt[17])(obj, 0, &d);
    }
    if (FAILED(hr)) {
        _snprintf(out, size, "%p type=%lu (no desc)", obj, (unsigned long)type);
    } else {
        _snprintf(out, size, "%p %s %ux%u fmt=%u usage=0x%lx pool=%u ms=%u%s", obj,
                  type == D3DRTYPE_SURFACE ? "surf" : "tex", d.Width, d.Height,
                  (unsigned)d.Format, (unsigned long)d.Usage, (unsigned)d.Pool,
                  (unsigned)d.MultiSampleType, levels > 1 ? " (mips)" : "");
    }
    out[size - 1] = '\0';
}

// ---- state (render thread) --------------------------------------------------------

LARGE_INTEGER g_qpc_freq = {};
LARGE_INTEGER g_window_start = {};
void *g_device = nullptr;
void *g_backbuffer = nullptr; // cached at snapshot time (ref released immediately)
bool g_internal = false;      // our own device calls — getter taps ignore them
const char *g_pending = "first-present";
void *g_last_table[MC2_RS_RT_TABLE_SLOTS] = {};
bool g_registry_logged = false;

// Target classes for binds/copies.
enum Cls : uint8_t { C_MAIN, C_FRONT, C_FRONTREF, C_BACKBUFFER, C_OTHER, C_NULL, C_COUNT };
const char *cls_name(uint8_t c)
{
    static const char *n[C_COUNT] = {"main", "front", "frontRef", "backbuffer", "other", "null"};
    return c < C_COUNT ? n[c] : "?";
}
uint8_t classify(void *s)
{
    if (!s) {
        return C_NULL;
    }
    if (s == table_slot(0)) {
        return C_MAIN;
    }
    if (s == table_slot(5)) {
        return C_FRONT;
    }
    if (s == table_slot(2)) {
        return C_FRONTREF;
    }
    if (s == g_backbuffer) {
        return C_BACKBUFFER;
    }
    return C_OTHER;
}

// Window counters [pass 0..2][class].
uint64_t g_binds[3][C_COUNT] = {};
uint64_t g_stretch_dst[3][C_COUNT] = {};
uint64_t g_fae0_hits[3] = {};
uint64_t g_fae0_front[3] = {}; // read value == live front
uint64_t g_creates_default = 0, g_creates_other = 0;
uint64_t g_getters = 0;

// Unique signatures (bind/stretch of front/backbuffer, getters, fae0 reads).
struct SigEntry {
    char what[24];
    uint8_t pass;
    uint8_t cls;
    Sig sig;
    uint64_t count;
};
constexpr int MAX_SIGS = 48;
SigEntry g_sigs[MAX_SIGS];
int g_sig_count = 0;
bool g_sigs_full_logged = false;

// Counts the signature; logs it the first time it is seen.
void note_sig(const char *what, uint8_t pass, uint8_t cls, const Sig &sig, const char *extra)
{
    for (int i = 0; i < g_sig_count; i++) {
        SigEntry &e = g_sigs[i];
        if (e.pass == pass && e.cls == cls && strcmp(e.what, what) == 0 &&
            memcmp(&e.sig, &sig, sizeof(sig)) == 0) {
            e.count++;
            return;
        }
    }
    char s[160];
    fmt_sig(sig, s, sizeof(s));
    if (g_sig_count >= MAX_SIGS) {
        if (!g_sigs_full_logged) {
            g_sigs_full_logged = true;
            MC2VR_LOG("rtprobe: signature table full (%d) — further new signatures dropped",
                      MAX_SIGS);
        }
        return;
    }
    SigEntry &e = g_sigs[g_sig_count++];
    _snprintf(e.what, sizeof(e.what), "%s", what);
    e.what[sizeof(e.what) - 1] = '\0';
    e.pass = pass;
    e.cls = cls;
    e.sig = sig;
    e.count = 1;
    MC2VR_LOG("rtprobe: sig#%d NEW %s pass=%u target=%s frame=%llu %s| stack %s", g_sig_count,
              what, (unsigned)pass, cls_name(cls), (unsigned long long)hooks::frame_count(),
              extra ? extra : "", s);
}

// Creation census (DEFAULT pool, unique by kind/size/format/usage/caller).
struct CreateEntry {
    uint8_t kind;
    uint32_t w, h, fmt, usage;
    uintptr_t caller;
    uintptr_t sig0;
    uint64_t count;
};
constexpr int MAX_CREATES = 192;
CreateEntry g_creates[MAX_CREATES];
int g_create_count = 0;

const char *create_name(Create k)
{
    switch (k) {
    case Create::Texture: return "CreateTexture";
    case Create::RenderTarget: return "CreateRenderTarget";
    case Create::DepthStencil: return "CreateDepthStencilSurface";
    case Create::OffscreenPlain: return "CreateOffscreenPlainSurface";
    }
    return "?";
}

const char *getter_name(Getter g)
{
    switch (g) {
    case Getter::BackBuffer: return "GetBackBuffer";
    case Getter::RenderTarget: return "GetRenderTarget";
    case Getter::DepthStencil: return "GetDepthStencilSurface";
    case Getter::SwapChain: return "GetSwapChain";
    }
    return "?";
}

// ---- snapshots ----------------------------------------------------------------------

void log_dims(const char *tag)
{
    const uintptr_t lti = rd<uintptr_t>(MC2_G_LTIRENDERER);
    const int sep = lti ? (int)rd<uint32_t>(lti + MC2_LTIRENDERER_SEPDEPTH_OFF) : -1;
    MC2VR_LOG("rtprobe: dims[%s] frame=%llu cfg=%ux%u fields=%ux%u float=%.0fx%.0f "
              "flag169=%u aspect=%.4f/%.4f ratio=%.4f renderScale=%.3f mouseScale=%.4f/%.4f "
              "sepDepth(+0x594)=%d",
              tag, (unsigned long long)hooks::frame_count(),
              rd<uint32_t>(MC2_G_RENDERCFG_SCRW), rd<uint32_t>(MC2_G_RENDERCFG_SCRH),
              rd<uint32_t>(rs(MC2_RS_SCREEN_W_OFF)), rd<uint32_t>(rs(MC2_RS_SCREEN_H_OFF)),
              rd<float>(rs(MC2_RS_FLOAT_W_OFF)), rd<float>(rs(MC2_RS_FLOAT_H_OFF)),
              rd<uint32_t>(rs(MC2_RS_FLAG169_OFF)), rd<float>(rs(MC2_RS_ASPECT_X_OFF)),
              rd<float>(rs(MC2_RS_ASPECT_Y_OFF)), rd<float>(rs(MC2_RS_ASPECT_R_OFF)),
              rd<float>(rs(MC2_RS_RENDERSCALE_OFF)), rd<float>(MC2_G_MOUSESCALE_X),
              rd<float>(MC2_G_MOUSESCALE_Y), sep);
}

void refresh_backbuffer()
{
    if (!g_device) {
        return;
    }
    void *bb = nullptr;
    g_internal = true;
    const HRESULT hr = ((HRESULT(__stdcall *)(void *, UINT, UINT, D3DBACKBUFFER_TYPE, void **))(
        *(void ***)g_device)[18])(g_device, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
    g_internal = false;
    if (SUCCEEDED(hr) && bb) {
        g_backbuffer = bb;
        ((ULONG(__stdcall *)(void *))(*(void ***)bb)[2])(bb); // drop our ref; keep identity
    }
}

void log_registry(const char *name, uintptr_t global)
{
    const uintptr_t obj = rd<uintptr_t>(global);
    if (!obj) {
        MC2VR_LOG("rtprobe: registry %s: *0x%08x = null", name, (unsigned)global);
        return;
    }
    const uintptr_t vt = rd<uintptr_t>(obj);
    char line[512];
    int n = _snprintf(line, sizeof(line), "rtprobe: registry %s: *0x%08x = 0x%08x vtbl=0x%08x",
                      name, (unsigned)global, (unsigned)obj, (unsigned)vt);
    // Only dereference a vtable that lies in the game image (.rdata/.data).
    if (vt >= MC2_TEXT_END && vt < 0x01a48000u) {
        n += _snprintf(line + n, sizeof(line) - n, " slots:");
        for (int i = 0; i < 12 && n > 0 && n < (int)sizeof(line) - 16; i++) {
            n += _snprintf(line + n, sizeof(line) - n, " [%d]0x%08x", i,
                           (unsigned)rd<uintptr_t>(vt + i * 4u));
        }
    } else {
        char where[96];
        describe_code_address((void *)vt, where, sizeof(where));
        n += _snprintf(line + n, sizeof(line) - n, " (vtbl in %s, not dumped)", where);
    }
    line[sizeof(line) - 1] = '\0';
    MC2VR_LOG("%s", line);
}

void snapshot(const char *reason)
{
    refresh_backbuffer();
    log_dims(reason);
    char bbdesc[160];
    describe_resource(g_backbuffer, bbdesc, sizeof(bbdesc));
    MC2VR_LOG("rtprobe: table[%s] swapchain backbuffer = %s", reason, bbdesc);
    for (uint32_t i = 0; i < MC2_RS_RT_TABLE_SLOTS; i++) {
        void *p = table_slot(i);
        g_last_table[i] = p;
        char desc[192];
        describe_resource(p, desc, sizeof(desc));
        const char *alias = "";
        if (p && p == g_backbuffer) {
            alias = " == swapchain backbuffer";
        } else if (p && i == 4 && p == table_slot(1)) {
            alias = " == autoDepth (+0x3e94)";
        } else if (p && i == 2 && p == table_slot(5)) {
            alias = " == front (+0x3ea4)";
        }
        MC2VR_LOG("rtprobe: table[%s] %-16s %s%s", reason, slot_name(i), desc, alias);
    }
    if (!g_registry_logged) {
        g_registry_logged = true;
        log_registry("A", MC2_G_SURFACE_REGISTRY_A);
        log_registry("B", MC2_G_SURFACE_REGISTRY_B);
    }
}

bool table_changed()
{
    for (uint32_t i = 0; i < MC2_RS_RT_TABLE_SLOTS; i++) {
        if (table_slot(i) != g_last_table[i]) {
            return true;
        }
    }
    return false;
}

void report_window(double sec)
{
    for (uint32_t p = 0; p < 3; p++) {
        MC2VR_LOG("rtprobe: window %.1fs pass%u: SetRT0 main=%llu front=%llu frontRef=%llu "
                  "backbuffer=%llu other=%llu null=%llu | StretchRect dst main=%llu front=%llu "
                  "backbuffer=%llu other=%llu | fae0 reads=%llu (==front %llu)",
                  sec, p, (unsigned long long)g_binds[p][C_MAIN],
                  (unsigned long long)g_binds[p][C_FRONT],
                  (unsigned long long)g_binds[p][C_FRONTREF],
                  (unsigned long long)g_binds[p][C_BACKBUFFER],
                  (unsigned long long)g_binds[p][C_OTHER], (unsigned long long)g_binds[p][C_NULL],
                  (unsigned long long)g_stretch_dst[p][C_MAIN],
                  (unsigned long long)g_stretch_dst[p][C_FRONT],
                  (unsigned long long)g_stretch_dst[p][C_BACKBUFFER],
                  (unsigned long long)g_stretch_dst[p][C_OTHER],
                  (unsigned long long)g_fae0_hits[p], (unsigned long long)g_fae0_front[p]);
    }
    MC2VR_LOG("rtprobe: window creates default=%llu other-pool=%llu (unique default=%d) "
              "getters=%llu sigs=%d",
              (unsigned long long)g_creates_default, (unsigned long long)g_creates_other,
              g_create_count, (unsigned long long)g_getters, g_sig_count);
    // Signature counts (cumulative) — which binds are per-frame vs one-off.
    for (int i = 0; i < g_sig_count; i++) {
        const SigEntry &e = g_sigs[i];
        MC2VR_LOG("rtprobe:   sig#%d %s pass=%u target=%s count=%llu", i + 1, e.what,
                  (unsigned)e.pass, cls_name(e.cls), (unsigned long long)e.count);
    }
    memset(g_binds, 0, sizeof(g_binds));
    memset(g_stretch_dst, 0, sizeof(g_stretch_dst));
    memset(g_fae0_hits, 0, sizeof(g_fae0_hits));
    memset(g_fae0_front, 0, sizeof(g_fae0_front));
    g_creates_default = g_creates_other = g_getters = 0;
}

uint32_t pass_idx()
{
    const uint32_t p = eye_replay::current_pass();
    return p < 3 ? p : 0;
}

// ---- attach-time MidHooks ---------------------------------------------------------------

SafetyHookMid g_fae0_mid;
uint64_t g_osr_calls = 0, g_setup_calls = 0;

// FUN_0074fae0 `mov edx,[edx+0x3e98]`: EDX = g_RenderShell (expected).
void fae0_midhook(safetyhook::Context &ctx)
{
    const uint32_t p = pass_idx();
    g_fae0_hits[p]++;
    if (ctx.edx != MC2_G_RENDERSHELL) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            MC2VR_LOG("rtprobe: fae0 read with EDX=0x%08x (NOT g_RenderShell)",
                      (unsigned)ctx.edx);
        }
        return;
    }
    void *v = rd<void *>(ctx.edx + 0x3e98);
    if (v && v == table_slot(5)) {
        g_fae0_front[p]++;
    }
    Sig sig;
    stack_sig(ctx.esp, &sig);
    char extra[64];
    _snprintf(extra, sizeof(extra), "value=%p ", v);
    extra[sizeof(extra) - 1] = '\0';
    note_sig("fae0-frontRef-read", (uint8_t)p, classify(v), sig, extra);
}

bool install_mid(SafetyHookMid &storage, uintptr_t target, safetyhook::MidHookFn fn,
                 const char *name)
{
    auto mid = SafetyHookMid::create(reinterpret_cast<uint8_t *>(target), fn);
    if (!mid) {
        MC2VR_LOG("rtprobe: %s MidHook install FAILED @ %p (error %u)", name, (void *)target,
                  (unsigned)mid.error().type);
        return false;
    }
    storage = std::move(*mid);
    MC2VR_LOG("rtprobe: installed %s MidHook @ %p", name, (void *)target);
    return true;
}

} // namespace

bool set_enabled(const char *value)
{
    if (strcmp(value, "on") == 0) {
        g_enabled = true;
    } else if (strcmp(value, "off") == 0) {
        g_enabled = false;
    } else {
        return false;
    }
    MC2VR_LOG("rtprobe: debug_rt_probe=%s", value);
    return true;
}

bool enabled()
{
    return g_enabled;
}

void install_early()
{
    if (!g_enabled) {
        return;
    }
    QueryPerformanceFrequency(&g_qpc_freq);
    install_mid(g_fae0_mid, MC2_FAE0_FRONTREF_READ, fae0_midhook, "FUN_0074fae0 front-ref read");
}

void on_osr_enter(uint32_t w, uint32_t h, uintptr_t self, uintptr_t esp)
{
    if (!g_enabled) {
        return;
    }
    g_osr_calls++;
    Sig sig;
    stack_sig(esp, &sig);
    char s[160];
    fmt_sig(sig, s, sizeof(s));
    MC2VR_LOG("rtprobe: OnScreenResize #%llu args=%ux%u this=0x%08x%s frame=%llu | stack %s",
              (unsigned long long)g_osr_calls, w, h, (unsigned)self,
              self == MC2_G_RENDERSHELL ? " (g_RenderShell)" : " (NOT g_RenderShell)",
              (unsigned long long)hooks::frame_count(), s);
    log_dims("OnScreenResize-entry");
    g_pending = "after-OnScreenResize";
}

void on_setup_enter(void *self)
{
    if (!g_enabled) {
        return;
    }
    g_setup_calls++;
    Sig sig;
    stack_sig_here(&sig);
    char s[160];
    fmt_sig(sig, s, sizeof(s));
    MC2VR_LOG("rtprobe: SetupRenderTargets #%llu this=%p%s frame=%llu | stack %s",
              (unsigned long long)g_setup_calls, self,
              (uintptr_t)self == MC2_G_RENDERSHELL ? " (g_RenderShell)" : " (NOT g_RenderShell)",
              (unsigned long long)hooks::frame_count(), s);
    log_dims("Setup-entry");
    g_pending = "after-SetupRenderTargets";
}

void on_present(void *device)
{
    if (!g_enabled) {
        return;
    }
    g_device = device;
    if (!g_pending && table_changed()) {
        g_pending = "table-changed";
    }
    if (g_pending) {
        const char *reason = g_pending;
        g_pending = nullptr;
        snapshot(reason);
    }

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (g_window_start.QuadPart == 0) {
        g_window_start = now;
        return;
    }
    const double sec =
        (double)(now.QuadPart - g_window_start.QuadPart) / (double)g_qpc_freq.QuadPart;
    if (sec >= REPORT_INTERVAL_SEC) {
        report_window(sec);
        g_window_start = now;
    }
}

void on_set_render_target(uint32_t index, void *surface, void *ret)
{
    if (!g_enabled || index != 0) {
        return;
    }
    const uint32_t p = pass_idx();
    const uint8_t c = classify(surface);
    g_binds[p][c]++;
    if (c == C_FRONT || c == C_FRONTREF || c == C_BACKBUFFER) {
        Sig sig;
        stack_sig_here(&sig);
        char r[96], extra[128];
        fmt_code(ret, r, sizeof(r));
        _snprintf(extra, sizeof(extra), "ret=%s ", r);
        extra[sizeof(extra) - 1] = '\0';
        note_sig("SetRenderTarget0", (uint8_t)p, c, sig, extra);
    }
}

void on_stretch_rect(void *src, void *dst, void *ret)
{
    if (!g_enabled) {
        return;
    }
    const uint32_t p = pass_idx();
    const uint8_t c = classify(dst);
    g_stretch_dst[p][c]++;
    if (c == C_FRONT || c == C_FRONTREF || c == C_BACKBUFFER) {
        Sig sig;
        stack_sig_here(&sig);
        char r[96], extra[160];
        fmt_code(ret, r, sizeof(r));
        _snprintf(extra, sizeof(extra), "src=%s ret=%s ", cls_name(classify(src)), r);
        extra[sizeof(extra) - 1] = '\0';
        note_sig("StretchRect", (uint8_t)p, c, sig, extra);
    }
}

void on_reset(bool before, long hr)
{
    if (!g_enabled) {
        return;
    }
    if (before) {
        MC2VR_LOG("rtprobe: Reset BEGIN frame=%llu", (unsigned long long)hooks::frame_count());
        log_dims("pre-Reset");
    } else {
        MC2VR_LOG("rtprobe: Reset END hr=%08lx frame=%llu", (unsigned long)hr,
                  (unsigned long long)hooks::frame_count());
        g_backbuffer = nullptr;
        g_pending = "post-Reset";
    }
}

namespace {

void on_create(Create kind, uint32_t w, uint32_t h, uint32_t fmt, uint32_t usage, uint32_t pool,
               long hr, void *object, void *ret)
{
    if (!g_enabled) {
        return;
    }
    if (pool != D3DPOOL_DEFAULT) {
        g_creates_other++;
        return;
    }
    g_creates_default++;
    Sig sig;
    stack_sig_here(&sig);
    for (int i = 0; i < g_create_count; i++) {
        CreateEntry &e = g_creates[i];
        if (e.kind == (uint8_t)kind && e.w == w && e.h == h && e.fmt == fmt && e.usage == usage &&
            e.caller == (uintptr_t)ret && e.sig0 == sig.a[0]) {
            e.count++;
            return;
        }
    }
    if (g_create_count < MAX_CREATES) {
        g_creates[g_create_count++] = {(uint8_t)kind, w, h, fmt, usage, (uintptr_t)ret, sig.a[0], 1};
    }
    char r[96], s[160];
    fmt_code(ret, r, sizeof(r));
    fmt_sig(sig, s, sizeof(s));
    MC2VR_LOG("rtprobe: create#%d %s %ux%u fmt=%u usage=0x%x hr=%08lx obj=%p pass=%u frame=%llu "
              "ret=%s | stack %s",
              g_create_count, create_name(kind), w, h, fmt, usage, (unsigned long)hr, object,
              pass_idx(), (unsigned long long)hooks::frame_count(), r, s);
}

void on_getter(Getter kind, uint32_t index, void *result, void *ret)
{
    if (!g_enabled || g_internal) {
        return;
    }
    g_getters++;
    Sig sig;
    stack_sig_here(&sig);
    char r[96], extra[160];
    fmt_code(ret, r, sizeof(r));
    _snprintf(extra, sizeof(extra), "index=%u result=%p ret=%s ", index, result, r);
    extra[sizeof(extra) - 1] = '\0';
    note_sig(getter_name(kind), (uint8_t)pass_idx(),
             kind == Getter::SwapChain ? (uint8_t)C_OTHER : classify(result), sig, extra);
}

// ---- Census VmtHook slots (observe + forward only) ---------------------------
// Installed on the carrier's device VmtHook from hook_device (rt_probe::install_device_hooks) (d3d9.h
// method order).
constexpr size_t SLOT_GetSwapChain = 14;
constexpr size_t SLOT_GetBackBuffer = 18;
constexpr size_t SLOT_CreateTexture = 23;
constexpr size_t SLOT_CreateRenderTarget = 28;
constexpr size_t SLOT_CreateDepthStencilSurface = 29;
constexpr size_t SLOT_CreateOffscreenPlainSurface = 36;
constexpr size_t SLOT_GetRenderTarget = 38;
constexpr size_t SLOT_GetDepthStencilSurface = 40;

// Leaked by design (hooks/device.hpp).
safetyhook::VmHook *g_getswapchain_hook = nullptr;
safetyhook::VmHook *g_getbackbuffer_hook = nullptr;
safetyhook::VmHook *g_createtexture_hook = nullptr;
safetyhook::VmHook *g_creatert_hook = nullptr;
safetyhook::VmHook *g_createds_hook = nullptr;
safetyhook::VmHook *g_createplain_hook = nullptr;
safetyhook::VmHook *g_getrt_hook = nullptr;
safetyhook::VmHook *g_getds_hook = nullptr;

HRESULT __stdcall createtexture_hook(void *self, UINT w, UINT h, UINT levels, DWORD usage,
                                     D3DFORMAT fmt, D3DPOOL pool, void **out, HANDLE *shared)
{
    const HRESULT hr = g_createtexture_hook->stdcall<HRESULT>(self, w, h, levels, usage, fmt,
                                                              pool, out, shared);
    on_create(Create::Texture, w, h, (uint32_t)fmt, usage, (uint32_t)pool, (long)hr,
              out ? *out : nullptr, __builtin_return_address(0));
    return hr;
}

HRESULT __stdcall creatert_hook(void *self, UINT w, UINT h, D3DFORMAT fmt,
                                D3DMULTISAMPLE_TYPE ms, DWORD msq, BOOL lockable, void **out,
                                HANDLE *shared)
{
    const HRESULT hr = g_creatert_hook->stdcall<HRESULT>(self, w, h, fmt, ms, msq, lockable, out,
                                                         shared);
    on_create(Create::RenderTarget, w, h, (uint32_t)fmt, D3DUSAGE_RENDERTARGET, D3DPOOL_DEFAULT,
              (long)hr, out ? *out : nullptr, __builtin_return_address(0));
    return hr;
}

HRESULT __stdcall createds_hook(void *self, UINT w, UINT h, D3DFORMAT fmt,
                                D3DMULTISAMPLE_TYPE ms, DWORD msq, BOOL discard, void **out,
                                HANDLE *shared)
{
    const HRESULT hr = g_createds_hook->stdcall<HRESULT>(self, w, h, fmt, ms, msq, discard, out,
                                                         shared);
    on_create(Create::DepthStencil, w, h, (uint32_t)fmt, D3DUSAGE_DEPTHSTENCIL, D3DPOOL_DEFAULT,
              (long)hr, out ? *out : nullptr, __builtin_return_address(0));
    return hr;
}

HRESULT __stdcall createplain_hook(void *self, UINT w, UINT h, D3DFORMAT fmt, D3DPOOL pool,
                                   void **out, HANDLE *shared)
{
    const HRESULT hr = g_createplain_hook->stdcall<HRESULT>(self, w, h, fmt, pool, out, shared);
    on_create(Create::OffscreenPlain, w, h, (uint32_t)fmt, 0, (uint32_t)pool, (long)hr,
              out ? *out : nullptr, __builtin_return_address(0));
    return hr;
}

HRESULT __stdcall getswapchain_hook(void *self, UINT index, void **out)
{
    const HRESULT hr = g_getswapchain_hook->stdcall<HRESULT>(self, index, out);
    on_getter(Getter::SwapChain, index, SUCCEEDED(hr) && out ? *out : nullptr,
              __builtin_return_address(0));
    return hr;
}

HRESULT __stdcall getbackbuffer_hook(void *self, UINT swapchain, UINT index,
                                     D3DBACKBUFFER_TYPE type, void **out)
{
    const HRESULT hr = g_getbackbuffer_hook->stdcall<HRESULT>(self, swapchain, index, type, out);
    on_getter(Getter::BackBuffer, index, SUCCEEDED(hr) && out ? *out : nullptr,
              __builtin_return_address(0));
    return hr;
}

HRESULT __stdcall getrt_hook(void *self, DWORD index, void **out)
{
    const HRESULT hr = g_getrt_hook->stdcall<HRESULT>(self, index, out);
    on_getter(Getter::RenderTarget, index, SUCCEEDED(hr) && out ? *out : nullptr,
              __builtin_return_address(0));
    return hr;
}

HRESULT __stdcall getds_hook(void *self, void **out)
{
    const HRESULT hr = g_getds_hook->stdcall<HRESULT>(self, out);
    on_getter(Getter::DepthStencil, 0, SUCCEEDED(hr) && out ? *out : nullptr,
              __builtin_return_address(0));
    return hr;
}

void on_device(void * /*device*/)
{
    if (!g_enabled) {
        return;
    }
    struct SlotSpec {
        size_t slot;
        void *destination;
        safetyhook::VmHook **storage;
        const char *name;
    };
    const SlotSpec probe_slots[] = {
        {SLOT_GetSwapChain, (void *)&getswapchain_hook, &g_getswapchain_hook, "GetSwapChain"},
        {SLOT_GetBackBuffer, (void *)&getbackbuffer_hook, &g_getbackbuffer_hook,
         "GetBackBuffer"},
        {SLOT_CreateTexture, (void *)&createtexture_hook, &g_createtexture_hook,
         "CreateTexture"},
        {SLOT_CreateRenderTarget, (void *)&creatert_hook, &g_creatert_hook,
         "CreateRenderTarget"},
        {SLOT_CreateDepthStencilSurface, (void *)&createds_hook, &g_createds_hook,
         "CreateDepthStencilSurface"},
        {SLOT_CreateOffscreenPlainSurface, (void *)&createplain_hook, &g_createplain_hook,
         "CreateOffscreenPlainSurface"},
        {SLOT_GetRenderTarget, (void *)&getrt_hook, &g_getrt_hook, "GetRenderTarget"},
        {SLOT_GetDepthStencilSurface, (void *)&getds_hook, &g_getds_hook,
         "GetDepthStencilSurface"},
    };
    for (const SlotSpec &spec : probe_slots) {
        unsigned error = 0;
        safetyhook::VmHook *hook = device::hook_method(spec.slot, spec.destination, &error);
        if (hook == nullptr) {
            // Non-fatal: the probe only loses this tap.
            MC2VR_LOG("rtprobe: hook_method(%s, slot %u) failed (error %u)", spec.name,
                      (unsigned)spec.slot, error);
            continue;
        }
        *spec.storage = hook;
        MC2VR_LOG("rtprobe: hooked %s (slot %u)", spec.name, (unsigned)spec.slot);
    }
}

} // namespace

void install_device_hooks(void *device)
{
    on_device(device);
}

} // namespace mc2vr::rt_probe

#include "eye_replay.hpp"

#include <windows.h>
#include <d3d9.h> // type/layout constants only

#include <cmath>
#include <cstdio>
#include <cstring>

#include <new>

#include "hooks.hpp"
#include "log.hpp"
#include "view_rewrite.hpp"

namespace mc2vr::eye {

namespace {

// Device + surface vtable slots (indices pinned by M2 runtime evidence;
// see device.cpp — Surface GetDesc = 12).
constexpr size_t DSLOT_CreateRenderTarget = 28;
constexpr size_t DSLOT_GetRenderTargetData = 32;
constexpr size_t DSLOT_CreateOffscreenPlainSurface = 36;
constexpr size_t SSLOT_LockRect = 13; // IDirect3DSurface9 vtable (d3d9.h): GetContainer=11, GetDesc=12, LockRect=13, UnlockRect=14. The 2026-10-04 crash: slot 10 = GetType ignores its args and returns a small positive D3DRESOURCETYPE, which passed SUCCEEDED() and left pBits uninitialized -> read through a garbage pointer.
constexpr size_t SSLOT_UnlockRect = 14;
constexpr size_t SSLOT_GetDesc = 12;
constexpr size_t SSLOT_Release = 2;
// IDirect3DDevice9 (d3d9.h): GetBackBuffer = 18 (re-verified against the
// header alongside CreateRenderTarget=28 / StretchRect=34).
constexpr size_t DSLOT_GetBackBuffer = 18;

using DevCall_t = HRESULT(__stdcall *)(void *);
using SurfaceDesc_t = HRESULT(__stdcall *)(void *, D3DSURFACE_DESC *);
using LockRect_t = HRESULT(__stdcall *)(void *, D3DLOCKED_RECT *, const RECT *, DWORD);
using Release_t = HRESULT(__stdcall *)(void *);

bool g_pass_enabled = false;
bool g_rt_enabled = false;
bool g_pin_enabled = false;
uint32_t g_dump_frames = 0;
float g_dump_delay_s = 15.0f;

uint32_t g_pass = 0; // 0 = not inside a submit; 1/2 while passes run
void *g_device = nullptr;

// The game's main scene target (first slot-0 RT observed; re-recorded after
// Reset) and the carrier-created same-size eye RT for pass 2.
void *g_main_rt = nullptr;
void *g_eye_rt = nullptr;
D3DSURFACE_DESC g_main_desc = {};

// The swapchain's backbuffer = the EndSubmit copy's destination, recorded on
// first use for the monitor pin. GetBackBuffer AddRefs; our ref is released
// in on_reset (which runs BEFORE the original Reset, while the surface
// still exists). Re-fetched lazily after Reset.
void *g_backbuffer = nullptr;
bool g_backbuffer_failed = false;

// ---- window counters (main thread writes; poller reads+resets) -------------

uint64_t g_redirects = 0;
uint64_t g_blit_redirects = 0;
uint64_t g_pin_skips = 0;
uint64_t g_dumps_written = 0;
uint64_t g_dump_failures = 0;

// ---- dump window --------------------------------------------------------------

bool dump_armed()
{
    if (g_dump_frames == 0 || !g_rt_enabled) {
        return false;
    }
    static LARGE_INTEGER freq = {};
    static LARGE_INTEGER first = {};
    if (freq.QuadPart == 0) {
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&first);
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (double)(now.QuadPart - first.QuadPart) / (double)freq.QuadPart >=
           (double)g_dump_delay_s;
}

// 24-bit bottom-up BMP writer (no external libs; viewable everywhere).
bool write_bmp(const wchar_t *path, const uint8_t *bgr, uint32_t width, uint32_t height)
{
    const uint32_t row = ((width * 3 + 3) / 4) * 4;
    const uint32_t pixels = row * height;
    const uint32_t size = 54 + pixels;

    FILE *f = _wfopen(path, L"wb");
    if (!f) {
        return false;
    }
    uint8_t hdr[54] = {};
    hdr[0] = 'B';
    hdr[1] = 'M';
    memcpy(hdr + 2, &size, 4);
    hdr[10] = 54;
    hdr[14] = 40;
    memcpy(hdr + 18, &width, 4);
    memcpy(hdr + 22, &height, 4);
    hdr[26] = 1;
    hdr[28] = 24;
    memcpy(hdr + 34, &pixels, 4);
    fwrite(hdr, 1, 54, f);
    fwrite(bgr, 1, pixels, f);
    fclose(f);
    return true;
}

// ---- fp16 (D3DFMT_A16B16G16R16F) decode + simple tonemap -----------------------
//
// The main scene RT is fp16 HDR (live fact 2026-10-04, plated on
// LtiRenderer_EndSubmit): 4 half floats per pixel, memory order R,G,B,A
// (channel names run most->least significant). BMP output needs a float
// decode + tonemap so both eyes' dumps are viewable and comparable.

// IEEE 754 half -> float32 (zero, subnormal, normal, inf/NaN).
float half_to_float(uint16_t h)
{
    const uint32_t sign = (uint32_t)(h & 0x8000u) << 16;
    const uint32_t exp = (h >> 10) & 0x1fu;
    uint32_t frac = h & 0x3ffu;
    uint32_t bits;
    if (exp == 0) {
        if (frac == 0) {
            bits = sign; // +-0
        } else {
            // Subnormal half = frac * 2^-24: normalize the leading 1 into
            // bit 10, each shift = one exponent step.
            uint32_t e = 113; // 127 - 15 + 1
            while ((frac & 0x400u) == 0) {
                frac <<= 1;
                e--;
            }
            bits = sign | (e << 23) | ((frac & 0x3ffu) << 13);
        }
    } else if (exp == 31) {
        bits = sign | 0x7f800000u | (frac << 13); // +-inf / NaN
    } else {
        bits = sign | ((exp - 15 + 127) << 23) | (frac << 13);
    }
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

// Display path: linear HDR -> Reinhard per channel ([0,inf) -> [0,1)) ->
// ~sRGB gamma -> 8 bit. LUT over [0, TM_MAX); brighter pixels clamp near
// white (the parallax check wants structure, not a pretty image). Built
// lazily on first use (render thread, once per process).
constexpr uint32_t TM_LEVELS = 8192;
constexpr float TM_MAX = 16.0f;
uint8_t g_tonemap_lut[TM_LEVELS];
bool g_tonemap_ready = false;

uint8_t tonemap(float v)
{
    if (!g_tonemap_ready) {
        for (uint32_t i = 0; i < TM_LEVELS; i++) {
            const double x = (double)TM_MAX * (double)i / (double)(TM_LEVELS - 1);
            const double m = x / (1.0 + x);
            g_tonemap_lut[i] = (uint8_t)(255.0 * pow(m, 1.0 / 2.2) + 0.5);
        }
        g_tonemap_ready = true;
    }
    if (!(v > 0.0f)) {
        return 0; // negative / NaN -> black (filter overshoot clamps)
    }
    const float scaled = v * ((float)(TM_LEVELS - 1) / TM_MAX);
    if (scaled >= (float)(TM_LEVELS - 1)) {
        return g_tonemap_lut[TM_LEVELS - 1];
    }
    return g_tonemap_lut[(uint32_t)scaled];
}

// Read a render target into system memory and write it as a BMP.
// Handles 32-bit RGB (A8R8G8B8=21, X8R8G8B8=22) and fp16 HDR
// (A16B16G16R16F=113); anything else is logged and skipped rather than
// misread.
void dump_surface(const char *tag, void *surface, uint64_t frame)
{
    uint32_t bpp = 0;
    if (g_main_desc.Format == 21 || g_main_desc.Format == 22) {
        bpp = 4;
    } else if (g_main_desc.Format == 113) {
        bpp = 8;
    } else {
        static bool warned = false;
        if (!warned) {
            warned = true;
            g_dump_failures++;
            MC2VR_LOG("eye: dump skipped — main RT format %u not 32-bit RGB or fp16",
                      (unsigned)g_main_desc.Format);
        }
        return;
    }
    void **vt = *(void ***)g_device;

    D3DLOCKED_RECT locked = {};
    void *sysmem = nullptr;
    // CreateOffscreenPlainSurface(w, h, fmt, SYSTEMMEM=1, &sysmem, NULL)
    auto create = (HRESULT(__stdcall *)(void *, UINT, UINT, D3DFORMAT, D3DPOOL,
                                         void **, void *))vt[DSLOT_CreateOffscreenPlainSurface];
    HRESULT hr = create(g_device, g_main_desc.Width, g_main_desc.Height,
                        g_main_desc.Format, D3DPOOL_SYSTEMMEM, &sysmem, nullptr);
    if (FAILED(hr) || !sysmem) {
        g_dump_failures++;
        MC2VR_LOG("eye: dump %s FAILED at CreateOffscreenPlainSurface hr=%08lx", tag, (unsigned long)hr);
        return;
    }
    auto grab = (HRESULT(__stdcall *)(void *, void *, void *))vt[DSLOT_GetRenderTargetData];
    hr = grab(g_device, surface, sysmem);
    void **sys_vt = *(void ***)sysmem; // sysmem's OWN vtable (not the RT's)
    if (SUCCEEDED(hr)) {
        hr = ((LockRect_t)sys_vt[SSLOT_LockRect])(sysmem, &locked, nullptr, 0);
    }
    if (SUCCEEDED(hr)) {
        const uint32_t w = g_main_desc.Width;
        const uint32_t h = g_main_desc.Height;
        if (!locked.pBits || (uint32_t)locked.Pitch < w * bpp) {
            g_dump_failures++;
            MC2VR_LOG("eye: dump %s FAILED — bad lock (pBits=%p pitch=%d)",
                      tag, locked.pBits, (int)locked.Pitch);
            ((DevCall_t)sys_vt[SSLOT_UnlockRect])(sysmem);
            ((Release_t)sys_vt[SSLOT_Release])(sysmem);
            return;
        }
        const uint32_t row = ((w * 3 + 3) / 4) * 4;
        uint8_t *bgr = new (std::nothrow) uint8_t[row * h];
        if (bgr) {
            const uint8_t *src = (const uint8_t *)locked.pBits;
            for (uint32_t y = 0; y < h; y++) {
                const uint8_t *srow = src + (h - 1 - y) * locked.Pitch;
                uint8_t *d = bgr + y * row;
                if (bpp == 4) {
                    const uint32_t *s = (const uint32_t *)srow;
                    for (uint32_t x = 0; x < w; x++) {
                        const uint32_t px = s[x]; // X8R8G8B8 / A8R8G8B8
                        *d++ = (uint8_t)(px >> 16); // B
                        *d++ = (uint8_t)(px >> 8);  // G
                        *d++ = (uint8_t)(px);       // R
                    }
                } else {
                    // A16B16G16R16F: R,G,B,A halves per pixel; alpha ignored.
                    const uint16_t *s = (const uint16_t *)srow;
                    for (uint32_t x = 0; x < w; x++) {
                        *d++ = tonemap(half_to_float(s[4 * x + 2])); // B
                        *d++ = tonemap(half_to_float(s[4 * x + 1])); // G
                        *d++ = tonemap(half_to_float(s[4 * x + 0])); // R
                    }
                }
            }
            wchar_t path[MAX_PATH];
            // Deploy dir = next to this DLL (same derivation as log.cpp).
            HMODULE self = nullptr;
            wchar_t dir[MAX_PATH] = L".";
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                      GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                  (LPCWSTR)&write_bmp, &self) &&
                GetModuleFileNameW(self, dir, MAX_PATH) != 0) {
                wchar_t *slash = wcsrchr(dir, L'\\');
                if (slash) {
                    *slash = L'\0';
                }
            }
            _snwprintf(path, MAX_PATH, L"%s\\mc2vr_eye_%s_frame%llu.bmp", dir, tag,
                       (unsigned long long)frame);
            if (write_bmp(path, bgr, w, h)) {
                g_dumps_written++;
                MC2VR_LOG("eye: dumped %s eye -> %ls (%ux%u)", tag, path, w, h);
            } else {
                g_dump_failures++;
                MC2VR_LOG("eye: dump %s FAILED to write BMP", tag);
            }
            delete[] bgr;
        }
        ((DevCall_t)sys_vt[SSLOT_UnlockRect])(sysmem);
    } else {
        g_dump_failures++;
        MC2VR_LOG("eye: dump %s FAILED hr=%08lx", tag, (unsigned long)hr);
    }
    ((Release_t)sys_vt[SSLOT_Release])(sysmem);
}

// ---- eye RT --------------------------------------------------------------------

bool ensure_eye_rt()
{
    if (g_eye_rt || !g_main_rt || !g_device) {
        return g_eye_rt != nullptr;
    }
    void **vt = *(void ***)g_device;
    auto create = (HRESULT(__stdcall *)(void *, UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE,
                                        DWORD, BOOL, void **, void **))vt[DSLOT_CreateRenderTarget];
    void *rt = nullptr;
    HRESULT hr = create(g_device, g_main_desc.Width, g_main_desc.Height,
                        g_main_desc.Format, g_main_desc.MultiSampleType,
                        g_main_desc.MultiSampleQuality, FALSE, &rt, nullptr);
    if (FAILED(hr) || !rt) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            MC2VR_LOG("eye: CreateRenderTarget FAILED hr=%08lx (%ux%u fmt=%u) — "
                      "pass 2 draws into the game's RT (no eye isolation)",
                      (unsigned long)hr, g_main_desc.Width, g_main_desc.Height,
                      (unsigned)g_main_desc.Format);
        }
        return false;
    }
    g_eye_rt = rt;
    MC2VR_LOG("eye: created eye RT %ux%u fmt=%u (pass-2 redirect target)", g_main_desc.Width,
              g_main_desc.Height, (unsigned)g_main_desc.Format);
    return true;
}

// The swapchain's backbuffer (dst of the EndSubmit RT0->backbuffer copy),
// for the monitor pin. Cached; re-fetched after Reset.
void *backbuffer()
{
    if (g_backbuffer || g_backbuffer_failed || !g_device) {
        return g_backbuffer;
    }
    void **vt = *(void ***)g_device;
    void *bb = nullptr;
    // GetBackBuffer(0, 0 /* D3DBACKBUFFER_TYPE_MONO */, &bb) — AddRefs bb.
    const HRESULT hr = ((HRESULT(__stdcall *)(void *, UINT, UINT, D3DBACKBUFFER_TYPE,
                                              void **))vt[DSLOT_GetBackBuffer])(
        g_device, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
    if (FAILED(hr) || !bb) {
        g_backbuffer_failed = true;
        MC2VR_LOG("eye: GetBackBuffer FAILED hr=%08lx — monitor pin inactive",
                  (unsigned long)hr);
        return nullptr;
    }
    g_backbuffer = bb;
    MC2VR_LOG("eye: backbuffer recorded: %p (monitor-pin dst)", bb);
    return g_backbuffer;
}

} // namespace

// ---- public entry points ---------------------------------------------------------

void *on_set_render_target(void *device, uint32_t index, void *game_surface)
{
    g_device = device; // render thread; the object is stable across the run

    if (index != 0) {
        return game_surface;
    }

    // Record the game's main scene target: the first slot-0 surface observed
    // after boot/Reset. (BeginSubmit sets it every frame; menus included.)
    if (!g_main_rt && game_surface && g_pass != 2) {
        D3DSURFACE_DESC desc = {};
        if (SUCCEEDED(((SurfaceDesc_t)(*(void ***)game_surface)[SSLOT_GetDesc])(
                game_surface, &desc))) {
            g_main_rt = game_surface;
            g_main_desc = desc;
            MC2VR_LOG("eye: main scene RT recorded: %p %ux%u fmt=%u", game_surface,
                      desc.Width, desc.Height, (unsigned)desc.Format);
        }
    }

    if (!g_rt_enabled || g_pass != 2 || game_surface != g_main_rt || !ensure_eye_rt()) {
        return game_surface;
    }

    g_redirects++;
    return g_eye_rt;
}

void *on_stretch_src(void *game_src, void *dst, bool *skip)
{
    *skip = false;
    if (!g_rt_enabled || g_pass != 2 || game_src != g_main_rt || !g_eye_rt) {
        return game_src;
    }
    g_blit_redirects++;
    // Monitor pin: pass 2's EndSubmit RT0->backbuffer copy is skipped so the
    // backbuffer keeps pass 1's LEFT image (stable monitor; the pass-2 RIGHT
    // image stays in the eye RT — this is also the S4 steady state, where
    // the compositor consumes the eye RT and Present keeps showing pass 1).
    // Mid-frame reads (dst != backbuffer) still run: they feed pass-2 post
    // effects and must see the redirected source.
    if (g_pin_enabled && dst == backbuffer()) {
        g_pin_skips++;
        *skip = true;
    }
    return g_eye_rt;
}

void set_pass(uint32_t pass)
{
    static uint32_t dump_remaining = 0;
    static bool window_done = false;
    const uint64_t frame = hooks::frame_count();

    if (pass == g_pass) {
        return;
    }

    // Dump window opens on a frame boundary (0 -> 1) so left/right dumps of
    // one frame land as a pair (left at 1->2, right at 2->0).
    if (!window_done && dump_remaining == 0 && pass == 1 && dump_armed()) {
        window_done = true;
        dump_remaining = g_dump_frames;
        MC2VR_LOG("eye: dump window open — dumping the next %u frame pairs", g_dump_frames);
    }

    // Pass boundaries: pass 1 -> 2 = pass 1 finished (dump left = main RT);
    // pass 2 -> 0 = pass 2 finished (dump right = eye RT).
    if (dump_remaining > 0 && g_rt_enabled && g_pass == 1 && g_main_rt) {
        dump_surface("left", g_main_rt, frame);
    }
    if (dump_remaining > 0 && g_rt_enabled && g_pass == 2 && g_eye_rt) {
        dump_surface("right", g_eye_rt, frame);
        dump_remaining--;
        if (dump_remaining == 0) {
            MC2VR_LOG("eye: dump window closed");
        }
    }

    g_pass = pass;
    // Per-pass eye override only when eye_pass=on (hold timer stays in
    // charge otherwise); pass 1 = LEFT (-1), pass 2 = RIGHT (+1).
    view::set_pass_eye(g_pass_enabled && pass != 0 ? (pass == 1 ? -1 : 1) : 0);
}

void on_reset()
{
    // All surfaces are gone. Forget the recording; the first post-Reset
    // slot-0 set re-records, and the eye RT is re-created lazily.
    if (g_backbuffer) {
        // Runs BEFORE the original Reset, while the surface still exists:
        // drop the ref our GetBackBuffer took (no dangling ref across Reset).
        ((Release_t)(*(void ***)g_backbuffer)[SSLOT_Release])(g_backbuffer);
        g_backbuffer = nullptr;
    }
    g_backbuffer_failed = false;
    if (g_eye_rt) {
        ((Release_t)(*(void ***)g_eye_rt)[SSLOT_Release])(g_eye_rt);
        g_eye_rt = nullptr;
    }
    g_main_rt = nullptr;
    g_main_desc = {};
    MC2VR_LOG("eye: Reset — eye RT dropped, main RT recording cleared");
}

void report_window()
{
    if (g_redirects > 0 || g_blit_redirects > 0 || g_pin_skips > 0 ||
        g_dump_failures > 0 || g_dumps_written > 0) {
        MC2VR_LOG("eye window: rtRedirects=%llu blitRedirects=%llu pinSkips=%llu "
                  "dumps=%llu dumpFails=%llu",
                  (unsigned long long)g_redirects, (unsigned long long)g_blit_redirects,
                  (unsigned long long)g_pin_skips,
                  (unsigned long long)g_dumps_written,
                  (unsigned long long)g_dump_failures);
    }
    g_redirects = 0;
    g_blit_redirects = 0;
    g_pin_skips = 0;
    g_dump_failures = 0;
}

bool set_pass_enabled(const char *value)
{
    if (strcmp(value, "on") == 0) {
        g_pass_enabled = true;
    } else if (strcmp(value, "off") == 0) {
        g_pass_enabled = false;
    } else {
        return false;
    }
    MC2VR_LOG("eye: eye_pass=%s (per-pass deterministic eye: pass1=LEFT pass2=RIGHT)",
              g_pass_enabled ? "on" : "off");
    return true;
}

bool set_rt_enabled(const char *value)
{
    if (strcmp(value, "on") == 0) {
        g_rt_enabled = true;
    } else if (strcmp(value, "off") == 0) {
        g_rt_enabled = false;
    } else {
        return false;
    }
    MC2VR_LOG("eye: eye_rt=%s (pass-2 SetRenderTarget(0)/StretchRect redirect)",
              g_rt_enabled ? "on" : "off");
    return true;
}

bool set_pin_enabled(const char *value)
{
    if (strcmp(value, "on") == 0) {
        g_pin_enabled = true;
    } else if (strcmp(value, "off") == 0) {
        g_pin_enabled = false;
    } else {
        return false;
    }
    MC2VR_LOG("eye: eye_monitor_pin=%s (pass-2 EndSubmit RT->backbuffer copy %s)",
              g_pin_enabled ? "on" : "off",
              g_pin_enabled ? "SKIPPED — monitor holds pass 1 LEFT" :
                              "runs — monitor alternates L/R per frame");
    return true;
}

void set_dump_frames(uint32_t n)
{
    g_dump_frames = n;
    MC2VR_LOG("eye: eye_dump_frames=%u", n);
}

void set_dump_delay(float seconds)
{
    g_dump_delay_s = seconds;
    MC2VR_LOG("eye: stream_dump_delay=%.1fs (shared with stream dumps)", (double)seconds);
}

} // namespace mc2vr::eye

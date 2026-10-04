#include "eye_replay.hpp"

#include <windows.h>
#include <d3d9.h> // type/layout constants only

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

using DevCall_t = HRESULT(__stdcall *)(void *);
using SurfaceDesc_t = HRESULT(__stdcall *)(void *, D3DSURFACE_DESC *);
using LockRect_t = HRESULT(__stdcall *)(void *, D3DLOCKED_RECT *, const RECT *, DWORD);
using Release_t = HRESULT(__stdcall *)(void *);

bool g_pass_enabled = false;
bool g_rt_enabled = false;
uint32_t g_dump_frames = 0;
float g_dump_delay_s = 15.0f;

uint32_t g_pass = 0; // 0 = not inside a submit; 1/2 while passes run
void *g_device = nullptr;

// The game's main scene target (first slot-0 RT observed; re-recorded after
// Reset) and the carrier-created same-size eye RT for pass 2.
void *g_main_rt = nullptr;
void *g_eye_rt = nullptr;
D3DSURFACE_DESC g_main_desc = {};

// ---- window counters (main thread writes; poller reads+resets) -------------

uint64_t g_redirects = 0;
uint64_t g_blit_redirects = 0;
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

// Read a render target into system memory and write it as a BMP.
// 32-bit formats only (A8R8G8B8=21, X8R8G8B8=22); anything else is logged
// and skipped rather than misread.
void dump_surface(const char *tag, void *surface, uint64_t frame)
{
    if (g_main_desc.Format != 21 && g_main_desc.Format != 22) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            g_dump_failures++;
            MC2VR_LOG("eye: dump skipped — main RT format %u not 32-bit RGB",
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
        if (!locked.pBits || (uint32_t)locked.Pitch < w * 4) {
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
                const uint32_t *s = (const uint32_t *)(src + (h - 1 - y) * locked.Pitch);
                uint8_t *d = bgr + y * row;
                for (uint32_t x = 0; x < w; x++) {
                    const uint32_t px = s[x]; // X8R8G8B8 / A8R8G8B8
                    *d++ = (uint8_t)(px >> 16); // B
                    *d++ = (uint8_t)(px >> 8);  // G
                    *d++ = (uint8_t)(px);       // R
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

void *on_stretch_src(void *game_src)
{
    if (!g_rt_enabled || g_pass != 2 || game_src != g_main_rt || !g_eye_rt) {
        return game_src;
    }
    g_blit_redirects++;
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
    if (g_redirects > 0 || g_blit_redirects > 0 || g_dump_failures > 0) {
        MC2VR_LOG("eye window: rtRedirects=%llu blitRedirects=%llu dumps=%llu "
                  "dumpFails=%llu",
                  (unsigned long long)g_redirects, (unsigned long long)g_blit_redirects,
                  (unsigned long long)g_dumps_written,
                  (unsigned long long)g_dump_failures);
    }
    g_redirects = 0;
    g_blit_redirects = 0;
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

#include "vr/eye_replay.hpp"

#include <windows.h>
#include <d3d9.h> // type/layout constants only

#include <cmath>
#include <cstdio>
#include <cstring>

#include <new>

#include "vr/adaptation.hpp"
#include "vr/eye_share.hpp"
#include "hooks/frame_tick.hpp"
#include "vr/hud_timing.hpp"
#include "log.hpp"
#include "vr/render_outputs.hpp"
#include "vr/view_rewrite.hpp"
#include "debug/wait_census.hpp"
#include "debug/gpu_census.hpp"
#include "debug/ps_census.hpp"

// Stereo pass boundaries (docs/stereo.md). Pass 1 renders LEFT, pass 2
// RIGHT; the engine's final frame of each pass lands in the carrier-owned
// front surface (render_outputs), which is captured at the boundaries:
// 1->2 = LEFT, 2->0 = RIGHT. Retired 2026-11-08 with carrier-owned outputs:
// the pass-2 main-RT -> eye-RT redirect (eye_rt), the monitor pin
// (eye_monitor_pin) and backbuffer capture.

namespace mc2vr::eye_replay {

namespace {

// Device + surface vtable slots (d3d9.h order, runtime-pinned).
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

uint32_t g_dump_frames = 0;
float g_dump_delay_s = 15.0f;

uint32_t g_pass = 0; // 0 = not inside a submit; 1/2 while passes run

// ---- window counters (main thread writes; poller reads+resets) -------------

uint64_t g_dumps_written = 0;
uint64_t g_dump_failures = 0;
uint64_t g_dump_empty = 0; // empty-surface dump attempts (black loading frames)

// ---- dump window --------------------------------------------------------------

bool dump_armed()
{
    if (g_dump_frames == 0) {
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

// Rate limit for empty-surface dump attempts: loading screens / videos hold
// fully-black RTs, and without this the dump window would re-copy the RT on
// every frame boundary until real content shows up.
bool dump_attempt_due()
{
    static LARGE_INTEGER freq = {};
    static LARGE_INTEGER until = {};
    if (freq.QuadPart == 0) {
        QueryPerformanceFrequency(&freq);
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (until.QuadPart != 0 && now.QuadPart < until.QuadPart) {
        return false;
    }
    until.QuadPart = now.QuadPart + freq.QuadPart / 2; // retry every 0.5s
    return true;
}

// Read a resolved render target into system memory and write it as a BMP.
// Returns true only when a non-empty BMP was written (empty/black surfaces do
// not count against the dump window). Handles 32-bit RGB (A8R8G8B8=21,
// X8R8G8B8=22) and fp16 HDR (A16B16G16R16F=113); anything else is logged and
// skipped rather than misread. The eye dumps are the per-eye LDR FINALS
// (composite + HUD): the HUD sits at the same screen position in both eyes.
bool dump_surface(void *device, const char *tag, void *surface, uint64_t frame)
{
    D3DSURFACE_DESC desc = {};
    if (!device || !surface ||
        FAILED(((SurfaceDesc_t)(*(void ***)surface)[SSLOT_GetDesc])(surface, &desc)) ||
        desc.MultiSampleType != D3DMULTISAMPLE_NONE) {
        g_dump_failures++;
        return false; // GetRenderTargetData needs a resolved (non-MSAA) RT
    }
    uint32_t bpp = 0;
    if (desc.Format == 21 || desc.Format == 22) {
        bpp = 4;
    } else if (desc.Format == 113) {
        bpp = 8;
    } else {
        static bool warned = false;
        if (!warned) {
            warned = true;
            g_dump_failures++;
            MC2VR_LOG("eye: dump skipped — surface format %u not 32-bit RGB or fp16",
                      (unsigned)desc.Format);
        }
        return false;
    }
    void **vt = *(void ***)device;

    D3DLOCKED_RECT locked = {};
    void *sysmem = nullptr;
    // CreateOffscreenPlainSurface(w, h, fmt, SYSTEMMEM=1, &sysmem, NULL)
    auto create = (HRESULT(__stdcall *)(void *, UINT, UINT, D3DFORMAT, D3DPOOL,
                                         void **, void *))vt[DSLOT_CreateOffscreenPlainSurface];
    HRESULT hr = create(device, desc.Width, desc.Height,
                        desc.Format, D3DPOOL_SYSTEMMEM, &sysmem, nullptr);
    if (FAILED(hr) || !sysmem) {
        g_dump_failures++;
        MC2VR_LOG("eye: dump %s FAILED at CreateOffscreenPlainSurface hr=%08lx", tag, (unsigned long)hr);
        return false;
    }
    auto grab = (HRESULT(__stdcall *)(void *, void *, void *))vt[DSLOT_GetRenderTargetData];
    hr = grab(device, surface, sysmem);
    void **sys_vt = *(void ***)sysmem; // sysmem's OWN vtable (not the RT's)
    if (SUCCEEDED(hr)) {
        hr = ((LockRect_t)sys_vt[SSLOT_LockRect])(sysmem, &locked, nullptr, 0);
    }
    if (SUCCEEDED(hr)) {
        const uint32_t w = desc.Width;
        const uint32_t h = desc.Height;
        if (!locked.pBits || (uint32_t)locked.Pitch < w * bpp) {
            g_dump_failures++;
            MC2VR_LOG("eye: dump %s FAILED — bad lock (pBits=%p pitch=%d)",
                      tag, locked.pBits, (int)locked.Pitch);
            ((DevCall_t)sys_vt[SSLOT_UnlockRect])(sysmem);
            ((Release_t)sys_vt[SSLOT_Release])(sysmem);
            return false;
        }
        // Empty probe: subsample every 64 bytes (8 px at fp16). A fully
        // black RT = loading/video frame — skip it; the window retries
        // until real (gameplay) content appears.
        {
            const uint8_t *p = (const uint8_t *)locked.pBits;
            const uint32_t bytes = (uint32_t)locked.Pitch * h;
            bool nonempty = false;
            for (uint32_t i = 0; i < bytes; i += 64) {
                if (p[i] != 0) {
                    nonempty = true;
                    break;
                }
            }
            if (!nonempty) {
                g_dump_empty++;
                if (g_dump_empty == 1 || g_dump_empty % 100 == 0) {
                    MC2VR_LOG("eye: dump %s skipped — surface empty "
                              "(loading/video; %llu attempts so far)",
                              tag, (unsigned long long)g_dump_empty);
                }
                ((DevCall_t)sys_vt[SSLOT_UnlockRect])(sysmem);
                ((Release_t)sys_vt[SSLOT_Release])(sysmem);
                return false;
            }
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
            // Widen the tag by hand: in a WIDE printf plain %s means a WIDE
            // string (MSVCRT + C99 agree), so passing the narrow "left"/
            // "right" literal reinterprets its bytes (plus adjacent merged
            // literals) as UTF-16 — the 2026-10-04 run produced mojibake
            // filenames that broke the analysis regex (bit us once).
            wchar_t wtag[8] = {};
            size_t tlen = strlen(tag);
            if (tlen > 7) {
                tlen = 7;
            }
            for (size_t i = 0; i < tlen; i++) {
                wtag[i] = (wchar_t)(unsigned char)tag[i];
            }
            _snwprintf(path, MAX_PATH, L"%s\\mc2vr_eye_%s_frame%llu.bmp", dir, wtag,
                       (unsigned long long)frame);
            if (write_bmp(path, bgr, w, h)) {
                g_dumps_written++;
                MC2VR_LOG("eye: dumped %s eye -> %ls (%ux%u)", tag, path, w, h);
                delete[] bgr;
                ((DevCall_t)sys_vt[SSLOT_UnlockRect])(sysmem);
                ((Release_t)sys_vt[SSLOT_Release])(sysmem);
                return true;
            }
            g_dump_failures++;
            MC2VR_LOG("eye: dump %s FAILED to write BMP", tag);
            delete[] bgr;
        }
        ((DevCall_t)sys_vt[SSLOT_UnlockRect])(sysmem);
    } else {
        g_dump_failures++;
        MC2VR_LOG("eye: dump %s FAILED hr=%08lx", tag, (unsigned long)hr);
    }
    ((Release_t)sys_vt[SSLOT_Release])(sysmem);
    return false;
}

} // namespace

// ---- public entry points ---------------------------------------------------------

uint32_t current_pass()
{
    return g_pass;
}

void set_pass(uint32_t pass)
{
    static uint32_t dump_remaining = 0;
    static bool window_done = false;
    static bool left_pending = false; // left BMP written this frame, awaiting right
    static uint64_t rewrites_at_pass_start = 0;
    static bool pass1_rewritten = false;
    const uint64_t frame = hooks::frame_count();

    if (pass == g_pass) {
        return;
    }
    gpu_census::on_pass(pass);
    ps_census::on_pass(pass);
    wait_census::mark(pass == 1 ? wait_census::Mark::Pass1
                      : pass == 2 ? wait_census::Mark::Pass2
                                  : wait_census::Mark::PassEnd);

    // HUD timing: boundary sample point (pass = the NEW pass; 1 =
    // pass-1 start, 2 = pass 1 done, 0 = pass 2 done). Main thread.
    hud_timing::on_boundary(pass);

    // Capture: the carrier front holds pass 1's LEFT final at 1->2 and pass
    // 2's RIGHT final at 2->0 (the 0->1 boundary carries no fresh eye — the
    // callee ignores it). Inactive (boot / mid-Reset) or the post-Reset
    // transition frame -> no capture. Must run BEFORE render_outputs sees the
    // boundary: its 2->0 handling ends the transition frame, and capturing
    // after that published the white RIGHT eye (live 2026-11-08).
    mc2vr::eye_share::on_pass_boundary(pass, render_outputs::device(),
                                       render_outputs::capture_source());

    // Carrier-owned outputs: refresh the LEFT monitor mirror at 1->2 (before
    // the dumps below read it); end the transition frame at 2->0.
    render_outputs::on_pass_boundary(pass);

    // Dump window opens on a frame boundary (0 -> 1) so left/right dumps of
    // one frame land as a pair (left at 1->2, right at 2->0).
    if (!window_done && dump_remaining == 0 && pass == 1 && dump_armed()) {
        window_done = true;
        dump_remaining = g_dump_frames;
        MC2VR_LOG("eye: dump window open — dumping the next %u NON-EMPTY frame pairs",
                  g_dump_frames);
    }
    // A pair counts only when BOTH sides are non-empty (empty = black
    // loading/video frame) AND the per-eye rewrite ran in BOTH passes (else
    // both eyes used the game's own camera — menus, or before the HMD union
    // locks — and the pair is identical by construction: the 2026-11-08 run
    // dumped three such pairs). Skipped pairs keep the window open (retry at
    // the 0.5s cadence). LEFT = the mirror (resolved at 1->2 just above),
    // RIGHT = a resolve of the front at 2->0.
    const uint64_t rewrites = view_rewrite::rewrite_count();
    const bool pass_rewritten = rewrites != rewrites_at_pass_start;
    rewrites_at_pass_start = rewrites;
    if (g_pass == 1 && pass == 2) {
        pass1_rewritten = pass_rewritten;
    }
    if (dump_remaining > 0 && g_pass == 1 && pass == 2 && pass1_rewritten &&
        dump_attempt_due()) {
        left_pending =
            dump_surface(render_outputs::device(), "left", render_outputs::mirror(), frame);
    }
    if (dump_remaining > 0 && g_pass == 2 && pass == 0 && left_pending) {
        if (!pass_rewritten) {
            g_dump_empty++; // pass 2 drew without the rewrite: discard the pair
        } else if (dump_surface(render_outputs::device(), "right",
                                render_outputs::resolve_front(), frame)) {
            dump_remaining--;
            if (dump_remaining == 0) {
                MC2VR_LOG("eye: dump window closed");
            }
        }
        left_pending = false;
    }

    g_pass = pass;
    // Per-pass eye: pass 1 = LEFT (-1), pass 2 = RIGHT (+1), outside = none.
    view_rewrite::set_pass_eye(pass != 0 ? (pass == 1 ? -1 : 1) : 0);
}

void on_reset()
{
    mc2vr::eye_share::on_reset();
}

namespace {
uint64_t g_replays = 0;  // extra passes this window
double g_replay_ms_sum = 0.0;
double g_replay_ms_max = 0.0;
} // namespace

void run_frame(void (*submit)())
{
    adaptation::on_frame_begin();  // exposure restore after a render-size switch
    set_pass(1);                   // pass 1 = LEFT eye
    // Two passes -> two present-prevs per frame; skip pass 1's (duplicate).
    render_outputs::begin_present_skip();
    submit();

    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);

    render_outputs::end_present_skip();
    set_pass(2);  // pass 2 = RIGHT eye
    submit();

    QueryPerformanceCounter(&t1);
    const double ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart;
    g_replays++;
    g_replay_ms_sum += ms;
    if (ms > g_replay_ms_max) {
        g_replay_ms_max = ms;
    }
    set_pass(0);  // back to "outside a submit"
}

void report_window()
{
    if (g_replays > 0) {
        MC2VR_LOG("stream replay: extraPasses=%llu avgMs=%.2f maxMs=%.2f", (unsigned long long)g_replays,
                  g_replay_ms_sum / (double)g_replays, g_replay_ms_max);
    }
    g_replays = 0;
    g_replay_ms_sum = 0.0;
    g_replay_ms_max = 0.0;
    mc2vr::eye_share::report_window();
    if (g_dump_failures > 0 || g_dumps_written > 0 || g_dump_empty > 0) {
        MC2VR_LOG("eye window: dumps=%llu dumpFails=%llu dumpEmpty=%llu",
                  (unsigned long long)g_dumps_written, (unsigned long long)g_dump_failures,
                  (unsigned long long)g_dump_empty);
    }
    g_dump_failures = 0;
    g_dump_empty = 0;
}

void set_dump_frames(uint32_t n)
{
    g_dump_frames = n;
    MC2VR_LOG("eye: debug_eye_dump_frames=%u", n);
}

void set_dump_delay(float seconds)
{
    g_dump_delay_s = seconds;
    MC2VR_LOG("eye: debug_dump_delay=%.1fs (shared with stream dumps)", (double)seconds);
}

} // namespace mc2vr::eye_replay

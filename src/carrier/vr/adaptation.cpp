// HDR eye-adaptation continuity. See adaptation.hpp.

#include "vr/adaptation.hpp"

#include <windows.h>
#include <d3d9.h>

#include <cstdint>
#include <cstring>
#include <cstdio>

#include "game/addresses.h"
#include "hooks/frame_tick.hpp"
#include "log.hpp"
#include "vr/render_outputs.hpp"

namespace mc2vr::adaptation {

namespace {

constexpr int SLOTS = 3; // adaptive, prev adaptive, g_pAdaptiveDest

struct Slot {
    IDirect3DSurface9 **holder = nullptr;  // where the engine keeps the surface pointer
    IDirect3DSurface9 *sysmem = nullptr; // SYSTEMMEM copy (survives Reset)
    UINT w = 0, h = 0;
    D3DFORMAT fmt = D3DFMT_UNKNOWN;
    bool pending = false;
};
Slot g_slots[SLOTS];
const char *const kNames[SLOTS] = {"adaptive", "prevAdaptive", "adaptiveDest"};

uint64_t g_saves = 0, g_restores = 0;
// A device Reset completed since the save: the engine's restore path has
// re-created the surfaces (synchronously, inside the same CheckDevice call).
// Keyed on the Reset, not on pointer change — the allocator reuses addresses.
bool g_reset_done = false;

// Frames (pass-1 starts) left in the post-reset hold. The first frame after
// the reset measures ~zero scene luminance (freshly re-created chain) with a
// reset-sized timestep, so its adaptation step drags the restored value to ~0
// (live 2026-11-08: 0.257 -> 0.000, then a slow climb = the bright flash).
// Re-applying the saved values for the first frames removes the persistence
// of that step; at most the hitch frame itself is bright.
constexpr uint32_t HOLD_FRAMES = 3;
uint32_t g_hold_frames = 0;

// IEEE half -> float (normals/zero only; enough for a luminance readout).
float half_to_float(uint16_t h)
{
    const uint32_t sign = (uint32_t)(h & 0x8000u) << 16;
    const uint32_t exp = (h >> 10) & 0x1fu;
    const uint32_t frac = h & 0x3ffu;
    uint32_t bits = sign;
    if (exp != 0 && exp != 31) {
        bits |= ((exp - 15 + 127) << 23) | (frac << 13);
    } else if (exp == 31) {
        bits |= 0x7f800000u | (frac << 13);
    }
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

// Format a SYSTEMMEM 1x1 surface's first pixel (fp16 RGBA / fp32 R).
void fmt_pixel(IDirect3DSurface9 *sysmem, D3DFORMAT fmt, char *out, size_t size)
{
    D3DLOCKED_RECT lr = {};
    if (FAILED(sysmem->LockRect(&lr, nullptr, D3DLOCK_READONLY)) || !lr.pBits) {
        _snprintf(out, size, "(lock failed)");
        out[size - 1] = '\0';
        return;
    }
    if (fmt == D3DFMT_A16B16G16R16F) {
        const uint16_t *p = (const uint16_t *)lr.pBits;
        _snprintf(out, size, "%.5f %.5f %.5f %.5f", (double)half_to_float(p[0]),
                  (double)half_to_float(p[1]), (double)half_to_float(p[2]),
                  (double)half_to_float(p[3]));
    } else if (fmt == D3DFMT_R32F || fmt == D3DFMT_A32B32G32R32F) {
        _snprintf(out, size, "%.5f", (double)*(const float *)lr.pBits);
    } else {
        _snprintf(out, size, "fmt=%u raw=%08x", (unsigned)fmt, *(const uint32_t *)lr.pBits);
    }
    out[size - 1] = '\0';
    sysmem->UnlockRect();
}

bool desc_of(IDirect3DSurface9 *s, D3DSURFACE_DESC *d)
{
    return s && SUCCEEDED(s->GetDesc(d));
}

} // namespace

void init()
{
    g_slots[2].holder = (IDirect3DSurface9 **)MC2_G_ADAPTIVE_DEST;
}

void on_adaptive_slot(IDirect3DSurface9 **holder)
{
    g_slots[0].holder = holder;
}

void on_prev_adaptive_slot(IDirect3DSurface9 **holder)
{
    g_slots[1].holder = holder;
}

void save()
{
    IDirect3DDevice9 *device = render_outputs::device();
    if (!device) {
        return;
    }
    int saved = 0;
    for (int i = 0; i < SLOTS; i++) {
        Slot &s = g_slots[i];
        s.pending = false;
        IDirect3DSurface9 *surf = s.holder ? *s.holder : nullptr;
        D3DSURFACE_DESC d = {};
        if (!desc_of(surf, &d) || d.MultiSampleType != D3DMULTISAMPLE_NONE) {
            continue;
        }
        if (s.sysmem && (s.w != d.Width || s.h != d.Height || s.fmt != d.Format)) {
            s.sysmem->Release();
            s.sysmem = nullptr;
        }
        if (!s.sysmem) {
            if (FAILED(device->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format,
                                                           D3DPOOL_SYSTEMMEM, &s.sysmem, nullptr)) ||
                !s.sysmem) {
                s.sysmem = nullptr;
                continue;
            }
            s.w = d.Width;
            s.h = d.Height;
            s.fmt = d.Format;
        }
        if (SUCCEEDED(device->GetRenderTargetData(surf, s.sysmem))) {
            s.pending = true;
            saved++;
            if (i == 1) { // prev = the persistent adapted value (op 0x16 copies it)
                char px[96];
                fmt_pixel(s.sysmem, s.fmt, px, sizeof(px));
                MC2VR_LOG("adapt: saved exposure %s", px);
            }
        }
    }
    g_reset_done = false;
    if (saved > 0) {
        g_saves++;
        MC2VR_LOG("adapt: saved %d exposure surface(s) before the reset", saved);
    }
}

void on_reset_done()
{
    g_reset_done = true;
}

// Write every saved slot back into the engine's current surfaces.
int apply_saved(IDirect3DDevice9 *device)
{
    int restored = 0;
    for (int i = 0; i < SLOTS; i++) {
        Slot &s = g_slots[i];
        IDirect3DSurface9 *surf = s.holder ? *s.holder : nullptr;
        D3DSURFACE_DESC d = {};
        if (!s.sysmem || !desc_of(surf, &d) || d.Width != s.w || d.Height != s.h ||
            d.Format != s.fmt) {
            continue;
        }
        if (SUCCEEDED(device->UpdateSurface(s.sysmem, nullptr, surf, nullptr))) {
            restored++;
        }
    }
    return restored;
}

void on_frame_begin()
{
    if (g_hold_frames > 0) {
        g_hold_frames--;
        if (IDirect3DDevice9 *device = render_outputs::device()) {
            apply_saved(device);
        }
        return;
    }
    if (!g_reset_done) {
        return;
    }
    IDirect3DDevice9 *device = nullptr;
    int restored = 0;
    for (int i = 0; i < SLOTS; i++) {
        Slot &s = g_slots[i];
        if (!s.pending) {
            continue;
        }
        IDirect3DSurface9 *surf = s.holder ? *s.holder : nullptr;
        if (!surf) {
            continue; // not re-created (yet) — keep it pending
        }
        D3DSURFACE_DESC d = {};
        if (!desc_of(surf, &d) || d.Width != s.w || d.Height != s.h || d.Format != s.fmt) {
            s.pending = false; // different surface shape: leave the engine's value
            continue;
        }
        if (!device) {
            device = render_outputs::device();
            if (!device) {
                return;
            }
        }
        const HRESULT hr = device->UpdateSurface(s.sysmem, nullptr, surf, nullptr);
        s.pending = false;
        if (SUCCEEDED(hr)) {
            restored++;

        } else {
            MC2VR_LOG("adapt: restore %s FAILED hr=%08lx", kNames[i], (unsigned long)hr);
        }
    }
    if (restored > 0) {
        g_restores++;
        MC2VR_LOG("adapt: restored %d exposure surface(s) after the reset (held %u frames)",
                  restored, HOLD_FRAMES);
        g_hold_frames = HOLD_FRAMES;
    }
    bool any_pending = false;
    for (const Slot &s : g_slots) {
        any_pending |= s.pending;
    }
    if (!any_pending) {
        g_reset_done = false;
    }
}

} // namespace mc2vr::adaptation

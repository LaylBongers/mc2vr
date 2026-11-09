// Scene MSAA — see msaa.hpp.

#include "vr/msaa.hpp"

#include <windows.h>
#include <d3d9.h>

#include <cstring>

#include "game/addresses.h"
#include "game/dx9.hpp"
#include "game/render_shell.hpp"
#include "hooks/device.hpp"
#include "log.hpp"
#include "vr/render_outputs.hpp"

namespace mc2vr::msaa {

namespace {

uint32_t g_samples = 4;  // 0 = off
bool g_checked = false;
bool g_supported = false;

// Sampler stages as the device numbers them: 0..15, then the vertex samplers
// D3DVERTEXTEXTURESAMPLER0..3 (257..260) as bits 16..19.
int stage_bit(DWORD stage)
{
    if (stage < 16) return (int)stage;
    if (stage >= D3DVERTEXTEXTURESAMPLER0 && stage <= D3DVERTEXTEXTURESAMPLER3) {
        return 16 + (int)(stage - D3DVERTEXTEXTURESAMPLER0);
    }
    return -1;
}

enum class State : uint8_t {
    InSync,     // both hold the same image
    MsaaNewer,  // drawn into since the last resolve
    RealNewer,  // the real surface was written (StretchRect destination): the stand-in is stale
};

struct StandIn {
    const char *name;
    uintptr_t surface_off;  // RenderShell slot of the real surface (texture level 0)
    uintptr_t texture_off;  // RenderShell slot of its texture
    IDirect3DSurface9 *real = nullptr;     // identity only (the engine owns it)
    IDirect3DBaseTexture9 *texture = nullptr;
    IDirect3DSurface9 *msaa = nullptr;     // ours
    State state = State::InSync;
    uint32_t stages = 0;  // stage bits where its texture is bound
};

StandIn g_standins[2] = {
    {"main", MC2_RS_RT_TABLE_OFF, game::kRenderShellMainTexture},
    {"depthCopy", MC2_RS_RT_TABLE_OFF + 0xcu, game::kRenderShellDepthCopyTexture},
};

// Device bindings (render thread): the physical RT0, and the logical and
// physical depth.
IDirect3DSurface9 *g_phys_rt0 = nullptr;
IDirect3DSurface9 *g_logical_ds = nullptr;
IDirect3DSurface9 *g_phys_ds = nullptr;

// Window counters.
uint64_t g_resolves = 0, g_stale_binds = 0, g_failures = 0;

bool active() { return g_samples != 0 && g_supported; }

uint8_t *shell() { return reinterpret_cast<uint8_t *>(game::render_shell()); }
template <class T>
T shell_field(uintptr_t offset)
{
    return *reinterpret_cast<T *>(shell() + offset);
}

D3DMULTISAMPLE_TYPE configured_type() { return (D3DMULTISAMPLE_TYPE)g_samples; }

bool format_supported(IDirect3D9 *d3d, const D3DDEVICE_CREATION_PARAMETERS &cp, D3DFORMAT format)
{
    DWORD quality = 0;
    const HRESULT hr =
        d3d->CheckDeviceMultiSampleType(cp.AdapterOrdinal, cp.DeviceType, format, TRUE, configured_type(), &quality);
    if (FAILED(hr)) {
        MC2VR_LOG("msaa: %ux not supported for format %u (hr %08lx)", g_samples, (unsigned)format,
                  (unsigned long)hr);
        return false;
    }
    return true;
}

StandIn *standin_for_real(IDirect3DSurface9 *s)
{
    if (s == nullptr) return nullptr;
    for (StandIn &si : g_standins) {
        if (si.real == s && si.msaa != nullptr) return &si;
    }
    return nullptr;
}

StandIn *standin_for_msaa(IDirect3DSurface9 *s)
{
    if (s == nullptr) return nullptr;
    for (StandIn &si : g_standins) {
        if (si.msaa == s) return &si;
    }
    return nullptr;
}

bool is_msaa(IDirect3DSurface9 *s)
{
    return s != nullptr && (s == render_outputs::front() || s == render_outputs::depth() || standin_for_msaa(s));
}

void resolve(StandIn &si)
{
    if (device::blit_surfaces(si.msaa, si.real)) {
        si.state = State::InSync;
        g_resolves++;
    } else {
        g_failures++;
    }
}

void release(StandIn &si)
{
    if (si.msaa != nullptr) {
        si.msaa->Release();
    }
    si.msaa = nullptr;
    si.real = nullptr;
    si.texture = nullptr;
    si.state = State::InSync;
    si.stages = 0;
}

// The depth the device gets for a logical depth, given the physical RT0.
IDirect3DSurface9 *physical_depth(IDirect3DSurface9 *logical)
{
    if (!active() || logical == nullptr) {
        return logical;
    }
    // An own engine scene depth (in-game AA on) is replaced by the MSAA
    // carrier depth: nothing samples it.
    IDirect3DSurface9 *const scene = shell_field<IDirect3DSurface9 *>(MC2_RS_RT_SCENEDEPTH_OFF);
    IDirect3DSurface9 *ds = logical;
    if (ds == scene && render_outputs::depth() != nullptr) {
        ds = render_outputs::depth();
    }
    return is_msaa(ds) == is_msaa(g_phys_rt0) ? ds : nullptr;
}

} // namespace

bool set_samples(const char *value)
{
    if (std::strcmp(value, "off") == 0 || std::strcmp(value, "0") == 0) g_samples = 0;
    else if (std::strcmp(value, "2") == 0) g_samples = 2;
    else if (std::strcmp(value, "4") == 0) g_samples = 4;
    else if (std::strcmp(value, "8") == 0) g_samples = 8;
    else return false;
    MC2VR_LOG("msaa: msaa=%s", value);
    return true;
}

D3DMULTISAMPLE_TYPE outputs_type(IDirect3DDevice9 *device, D3DFORMAT front_format, D3DFORMAT depth_format)
{
    if (g_samples == 0 || device == nullptr) {
        return D3DMULTISAMPLE_NONE;
    }
    if (!g_checked) {
        g_checked = true;
        IDirect3D9 *d3d = nullptr;
        D3DDEVICE_CREATION_PARAMETERS cp = {};
        if (SUCCEEDED(device->GetDirect3D(&d3d)) && d3d != nullptr && SUCCEEDED(device->GetCreationParameters(&cp))) {
            const D3DFORMAT main_format = D3DFMT_A16B16G16R16F, depth_copy_format = D3DFMT_G32R32F;
            g_supported = format_supported(d3d, cp, front_format) && format_supported(d3d, cp, depth_format) &&
                          format_supported(d3d, cp, main_format) && format_supported(d3d, cp, depth_copy_format);
        }
        if (d3d != nullptr) {
            d3d->Release();
        }
        MC2VR_LOG("msaa: scene MSAA %ux %s", g_samples, g_supported ? "on" : "OFF (not supported, see above)");
    }
    return g_supported ? configured_type() : D3DMULTISAMPLE_NONE;
}

void ensure(IDirect3DDevice9 *device)
{
    if (!active() || device == nullptr || !render_outputs::active()) {
        return;
    }
    for (StandIn &si : g_standins) {
        IDirect3DSurface9 *const real = shell_field<IDirect3DSurface9 *>(si.surface_off);
        if (real == si.real && si.msaa != nullptr) {
            continue;
        }
        if (si.msaa != nullptr) {
            MC2VR_LOG("msaa: %s changed %p -> %p, re-creating its stand-in", si.name, si.real, real);
            if (g_phys_rt0 == si.msaa) {
                g_phys_rt0 = nullptr;  // still bound; the engine's next bind replaces it
            }
            release(si);
        }
        D3DSURFACE_DESC d = {};
        if (real == nullptr || FAILED(real->GetDesc(&d))) {
            continue;
        }
        IDirect3DSurface9 *s = nullptr;
        const HRESULT hr =
            device->CreateRenderTarget(d.Width, d.Height, d.Format, configured_type(), 0, FALSE, &s, nullptr);
        if (FAILED(hr) || s == nullptr) {
            g_failures++;
            MC2VR_LOG("msaa: create %s stand-in %ux%u fmt=%u FAILED hr=%08lx", si.name, d.Width, d.Height,
                      (unsigned)d.Format, (unsigned long)hr);
            continue;
        }
        si.real = real;
        si.texture = shell_field<IDirect3DBaseTexture9 *>(si.texture_off);
        si.msaa = s;
        si.state = State::InSync;  // the engine redraws both every pass
        si.stages = 0;
        MC2VR_LOG("msaa: created %s stand-in %p for %p (%ux%u fmt=%u, %ux)", si.name, s, real, d.Width, d.Height,
                  (unsigned)d.Format, g_samples);
    }
}

void on_reset_before()
{
    for (StandIn &si : g_standins) {
        release(si);
    }
    g_phys_rt0 = g_logical_ds = g_phys_ds = nullptr;
}

void on_reset_done()
{
    g_phys_rt0 = g_logical_ds = g_phys_ds = nullptr;
}

IDirect3DSurface9 *map_render_target(DWORD index, IDirect3DSurface9 *logical)
{
    if (!active() || index != 0) {
        if (index == 0) g_phys_rt0 = logical;
        return logical;
    }
    // Unbinding a stand-in whose texture sits on a sampler: resolve now, a
    // later draw may sample it without a new SetTexture.
    if (StandIn *prev = standin_for_msaa(g_phys_rt0)) {
        if (prev->real != logical && prev->state == State::MsaaNewer && prev->stages != 0) {
            resolve(*prev);
        }
    }
    IDirect3DSurface9 *physical = logical;
    if (StandIn *si = standin_for_real(logical)) {
        // A real surface written behind the stand-in (StretchRect
        // destination) is not copied back: MSAA cannot be a StretchRect
        // destination. Not seen in the engine; counted.
        if (si->state == State::RealNewer) {
            g_stale_binds++;
        }
        si->state = State::MsaaNewer;
        physical = si->msaa;
    }
    g_phys_rt0 = physical;
    return physical;
}

IDirect3DSurface9 *map_depth(IDirect3DSurface9 *logical)
{
    g_logical_ds = logical;
    g_phys_ds = physical_depth(logical);
    return g_phys_ds;
}

bool depth_after_rt0(IDirect3DSurface9 **physical)
{
    if (!active()) {
        return false;
    }
    IDirect3DSurface9 *const ds = physical_depth(g_logical_ds);
    if (ds == g_phys_ds) {
        return false;
    }
    g_phys_ds = ds;
    *physical = ds;
    return true;
}

void before_stretch_rect(IDirect3DSurface9 *src, IDirect3DSurface9 *dst)
{
    if (!active()) {
        return;
    }
    if (StandIn *si = standin_for_real(src)) {
        if (si->state == State::MsaaNewer) {
            resolve(*si);
        }
    }
    if (StandIn *si = standin_for_real(dst)) {
        si->state = State::RealNewer;
    }
}

void before_set_texture(DWORD stage, IDirect3DBaseTexture9 *texture)
{
    if (!active()) {
        return;
    }
    const int bit = stage_bit(stage);
    for (StandIn &si : g_standins) {
        if (si.msaa == nullptr) continue;
        if (texture != nullptr && texture == si.texture) {
            if (bit >= 0) si.stages |= 1u << bit;
            if (si.state == State::MsaaNewer) {
                resolve(si);
            }
        } else if (bit >= 0) {
            si.stages &= ~(1u << bit);
        }
    }
}

void on_begin_submit()
{
    if (active()) {
        game::Dx9_SetRenderState(D3DRS_MULTISAMPLEANTIALIAS, TRUE);
    }
}

void report_window()
{
    if (!active()) {
        return;
    }
    MC2VR_LOG("msaa: %ux resolves=%llu staleBinds=%llu failures=%llu", g_samples, (unsigned long long)g_resolves,
              (unsigned long long)g_stale_binds, (unsigned long long)g_failures);
    g_resolves = g_stale_binds = g_failures = 0;
}

} // namespace mc2vr::msaa

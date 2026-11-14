// The shadow atlas's surfaces and pass setup — see shadow_surfaces.hpp.
// PgLtiShadow_CreateSurfaces follows 0x00755f80-0x00756387,
// RenderCmd_BeginSurfacePass 0x00756420-0x00756582.

#include "reimpl/render/shadow_surfaces.hpp"

#include <windows.h>
#include <d3d9.h>

#include <cstring>

#include "game/dx9.hpp"
#include "game/lti.hpp"
#include "game/render_cmd.hpp"
#include "game/render_shell.hpp"
#include "log.hpp"

namespace mc2vr::reimpl {

namespace {

using namespace game;

constexpr uint32_t kStockCascade = 0x400;  // the stock cascade edge (immediates throughout)
uint32_t g_cascade = kStockCascade;

// The original's .data tables, by cascade: D3DRS_DEPTHBIAS and
// D3DRS_SLOPESCALEDEPTHBIAS (float bits; 2^-20 and 2.5 for every cascade).
using g_ShadowDepthBias = GlobalArray<uint32_t, 0x00dfcdf0u, 4>;
using g_ShadowSlopeBias = GlobalArray<uint32_t, 0x00dfce00u, 4>;

template <class T>
T &field(uint8_t *base, uintptr_t off)
{
    return *reinterpret_cast<T *>(base + off);
}

void log_failure(const char *what, HRESULT hr, uint32_t w, uint32_t h)
{
    MC2VR_LOG("shadow: ERROR — %s %ux%u failed (hr=0x%08lx)%s", what, w, h, (unsigned long)hr,
              g_cascade != kStockCascade ? " — lower shadow_resolution" : "");
}

void register_surface(void *registry, const char *name, void *object)
{
    Registry_Register(registry, Lti_LazyNameHash(name), object, 3);
}

} // namespace

bool set_shadow_resolution(const char *value)
{
    if (std::strcmp(value, "1024") == 0) g_cascade = 1024;
    else if (std::strcmp(value, "2048") == 0) g_cascade = 2048;
    else if (std::strcmp(value, "4096") == 0) g_cascade = 4096;
    else return false;
    MC2VR_LOG("shadow: shadow_resolution=%u (atlas %ux%u)", g_cascade, g_cascade, g_cascade * 4);
    return true;
}

uint32_t shadow_cascade_size()
{
    return g_cascade;
}

void PgLtiShadow_CreateSurfaces(uint8_t *shell)
{
    const uint32_t w = g_cascade, h = g_cascade * 4;

    // ---- the colour format: R5G6B5 if the format table has it as a render target ----
    D3DFORMAT colour = D3DFMT_R5G6B5;
    {
        uint8_t *const table = field<uint8_t *>(g_LtiRenderer::get(), kLtiFormatTable);
        const uint8_t *e = field<uint8_t *>(table, 0x28);
        for (; e != nullptr; e = *reinterpret_cast<uint8_t *const *>(e + 4)) {
            if (*reinterpret_cast<const uint32_t *>(e + 0x10) == D3DFMT_R5G6B5 &&
                (*reinterpret_cast<const uint32_t *>(e + 0x18) & 0x800) != 0) {
                break;
            }
        }
        if (e == nullptr) {
            MC2VR_LOG("shadow: shadow colour format changed to X8R8G8B8 (R5G6B5 not supported)");
            colour = D3DFMT_X8R8G8B8;
        }
    }
    IDirect3DDevice9 *const dev = field<IDirect3DDevice9 *>(shell, kRenderShellDevice);
    void *const surfaces = g_SurfaceRegistryPtrB::get();

    // ---- ShadowMapCombined: unused after creation, so at the stock size ----
    auto &combined = field<IDirect3DSurface9 *>(shell, kRenderShellShadowCombined);
    HRESULT hr = dev->CreateRenderTarget(kStockCascade, kStockCascade * 4, colour, D3DMULTISAMPLE_NONE, 0, FALSE,
                                         &combined, nullptr);
    if (FAILED(hr)) log_failure("ShadowMapCombined", hr, kStockCascade, kStockCascade * 4);
    register_surface(surfaces, "ShadowMapCombined", combined);

    // ---- the dummy colour RT0 (must cover the cascade viewports) ----
    auto &colourTex = field<IDirect3DTexture9 *>(shell, kRenderShellShadowColorTexture);
    auto &colourSurf = field<IDirect3DSurface9 *>(shell, kRenderShellShadowColorSurface);
    hr = dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, colour, D3DPOOL_DEFAULT, &colourTex, nullptr);
    if (FAILED(hr)) log_failure("ShadowMapColorTex", hr, w, h);
    register_surface(g_SurfaceRegistryPtrA::get(), "ShadowMapColorTex", colourTex);
    // (The original calls GetSurfaceLevel on a failed texture too and crashes.)
    hr = colourTex != nullptr ? colourTex->GetSurfaceLevel(0, &colourSurf) : E_FAIL;
    if (FAILED(hr)) log_failure("ShadowMapColorSurf", hr, w, h);
    register_surface(surfaces, "ShadowMapColorSurf", colourSurf);

    // ---- the depth atlas ----
    auto &depthTex = field<IDirect3DTexture9 *>(shell, kRenderShellShadowTexture);
    auto &depthSurf = field<IDirect3DSurface9 *>(shell, kRenderShellShadowSurface);
    if (field<uint8_t>(g_LtiRenderer::get(), kLtiDepthTextureD24) != 0) {
        hr = dev->CreateTexture(w, h, 1, D3DUSAGE_DEPTHSTENCIL, D3DFMT_D24S8, D3DPOOL_DEFAULT, &depthTex, nullptr);
        if (FAILED(hr)) log_failure("ShadowMapTex (D24S8)", hr, w, h);
        register_surface(g_SurfaceRegistryPtrA::get(), "ShadowMapTex", depthTex);
        hr = depthTex != nullptr ? depthTex->GetSurfaceLevel(0, &depthSurf) : E_FAIL;
        if (FAILED(hr)) log_failure("ShadowMapSurf", hr, w, h);
    } else {
        hr = dev->CreateTexture(w, h, 1, D3DUSAGE_DEPTHSTENCIL, static_cast<D3DFORMAT>(MAKEFOURCC('D', 'F', '2', '4')),
                                D3DPOOL_DEFAULT, &depthTex, nullptr);
        if (FAILED(hr)) {
            depthTex = nullptr;  // no depth texture: a plain depth surface
            hr = dev->CreateDepthStencilSurface(w, h, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, FALSE, &depthSurf, nullptr);
        } else {
            hr = depthTex->GetSurfaceLevel(0, &depthSurf);
        }
        if (FAILED(hr)) log_failure("ShadowMapSurf", hr, w, h);
    }
    register_surface(surfaces, "ShadowMapSurf", depthSurf);
    MC2VR_LOG("shadow: atlas %ux%u: depth %s %p (surface %p), colour %s %p", w, h,
              depthTex == nullptr ? "surface" : (field<uint8_t>(g_LtiRenderer::get(), kLtiDepthTextureD24) ? "D24S8"
                                                                                                          : "DF24"),
              (void *)depthTex, (void *)depthSurf, colour == D3DFMT_R5G6B5 ? "R5G6B5" : "X8R8G8B8", (void *)colourTex);

    // ---- the cascades' RECTs: square quarters stacked vertically ----
    RECT *const rects = reinterpret_cast<RECT *>(shell + kRenderShellShadowRects);
    for (uint32_t i = 0; i < 4; i++) {
        rects[i] = RECT{0, (LONG)(i * w), (LONG)w, (LONG)((i + 1) * w)};
    }
}

void RenderCmd_BeginSurfacePass(uint32_t flag, uint32_t cascade, uint32_t gate)
{
    if ((int32_t)(gate - 1) < 0) return;
    uint8_t *const shell = reinterpret_cast<uint8_t *>(kRenderShellObject);
    if (flag == 0 && cascade == 0) {
        IDirect3DDevice9 *const dev = field<IDirect3DDevice9 *>(shell, kRenderShellDevice);
        dev->GetViewport(&field<D3DVIEWPORT9>(shell, kRenderShellSavedViewport));
    }
    Dx9_SetTexture(15, 0, nullptr);
    Dx9_SetRenderTarget(0, field<IDirect3DSurface9 *>(shell, kRenderShellShadowColorSurface));
    Dx9_SetRenderTarget(1, nullptr);
    Dx9_SetDepthStencilSurface(field<IDirect3DSurface9 *>(shell, kRenderShellShadowSurface), 0);

    RECT *const rect = reinterpret_cast<RECT *>(shell + kRenderShellShadowRects) + cascade;
    D3DVIEWPORT9 vp;
    vp.X = (DWORD)rect->left;
    vp.Y = (DWORD)rect->top;
    vp.Width = g_cascade;   // the stock hard-codes 0x400
    vp.Height = g_cascade;
    vp.MinZ = 0.0f;
    vp.MaxZ = 1.0f;
    IDirect3DDevice9 *const dev = lti_device();
    dev->SetViewport(&vp);
    Dx9_SetRenderState(D3DRS_ZWRITEENABLE, 1);
    Dx9_SetRenderState(D3DRS_ZENABLE, 1);
    Dx9_SetTexture(0, 0, nullptr);
    Dx9_SetRenderState(D3DRS_COLORWRITEENABLE, 0);
    lti_device()->Clear(1, reinterpret_cast<const D3DRECT *>(rect), D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0, 1.0f, 0x80);
    Dx9_SetRenderState(D3DRS_DEPTHBIAS, g_ShadowDepthBias::data()[cascade]);
    Dx9_SetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, g_ShadowSlopeBias::data()[cascade]);
    g_PassCullMode::set(D3DCULL_NONE);
}

} // namespace mc2vr::reimpl

// See render_size.hpp.

#include "hooks/render_size.hpp"

#include <windows.h>
#include <d3d9.h> // types only

#include <cstdint>

#include "debug/rt_probe.hpp"
#include "game/addresses.h"
#include "hooks/site.hpp"
#include "log.hpp"
#include "vr/render_size.hpp"

namespace mc2vr::hooks {

namespace {

SafetyHookMid g_osr_entry, g_osr_exit, g_setup_store, g_chain_desc;

// ---- OnScreenResize entry: W = [esp+4], H = EAX, this = ESI -----------------------

void osr_entry_midhook(safetyhook::Context &ctx)
{
    uint32_t *w = (uint32_t *)(ctx.esp + 4);
    rt_probe::on_osr_enter(*w, (uint32_t)ctx.eax, ctx.esi, ctx.esp);
    if (auto s = render_size::on_osr_enter({*w, (uint32_t)ctx.eax}, ctx.esi)) {
        *w = s->w;
        ctx.eax = s->h;
    }
}

// ---- OnScreenResize epilogue: fields, floats, viewports, half-dims and the
// mouse scale (cfg / client) are written — post-fix aspect + mouse.

void osr_exit_midhook(safetyhook::Context &)
{
    render_size::on_osr_done();
}

// ---- SetupRenderTargets field store: ECX = cfg W, EAX = cfg H -------------------

void setup_store_midhook(safetyhook::Context &ctx)
{
    if (auto s = render_size::on_setup_store()) {
        ctx.ecx = s->w;
        ctx.eax = s->h;
    }
}

// ---- SetupRenderTargets chain desc: the engine sizes its main chain from the
// front's GetDesc (still the swapchain backbuffer at this point) — make that
// desc say the render size. EAX = GetDesc's HRESULT; the desc is on the stack
// at ESP + MC2_SETUP_CHAIN_DESC_ESP_OFF.

void chain_desc_midhook(safetyhook::Context &ctx)
{
    if ((int32_t)ctx.eax < 0) {
        return;
    }
    D3DSURFACE_DESC *d = (D3DSURFACE_DESC *)(ctx.esp + MC2_SETUP_CHAIN_DESC_ESP_OFF);
    if (auto s = render_size::on_setup_chain_desc({d->Width, d->Height})) {
        d->Width = s->w;
        d->Height = s->h;
    }
}

void install_one(SafetyHookMid &storage, uintptr_t target, safetyhook::MidHookFn fn,
                 const char *name)
{
    if (!create_mid(storage, target, fn, name)) {
        MC2VR_LOG("rsize: FATAL — %s MidHook missing", name);
        return;
    }
    MC2VR_LOG("rsize: installed %s MidHook @ %p", name, (void *)target);
}

} // namespace

void install_render_size()
{
    const bool configured = render_size::configured();
    if (!configured && !rt_probe::enabled()) {
        return;
    }
    install_one(g_osr_entry, MC2_ONSCREENRESIZE, osr_entry_midhook, "OnScreenResize entry");
    if (configured) {
        install_one(g_osr_exit, MC2_ONSCREENRESIZE_EPILOGUE, osr_exit_midhook,
                    "OnScreenResize epilogue");
        install_one(g_setup_store, MC2_SETUP_FIELD_STORE, setup_store_midhook,
                    "SetupRenderTargets field store");
        install_one(g_chain_desc, MC2_SETUP_CHAIN_DESC, chain_desc_midhook,
                    "SetupRenderTargets chain desc");
    }
}

} // namespace mc2vr::hooks

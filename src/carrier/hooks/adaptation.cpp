// See adaptation.hpp.

#include "hooks/adaptation.hpp"

#include "game/addresses.h"
#include "hooks/site.hpp"
#include "log.hpp"
#include "vr/adaptation.hpp"

namespace mc2vr::hooks {

namespace {

SafetyHookMid g_adapt_mid, g_prev_mid;

// ESI = &bloom->_AdaptiveLuminanceSurface at the creation log site.
void adapt_midhook(safetyhook::Context &ctx) { adaptation::on_adaptive_slot((IDirect3DSurface9 **)ctx.esi); }
// EDI = &bloom->_pPrevAdaptiveLuminanceSurface.
void prev_midhook(safetyhook::Context &ctx) { adaptation::on_prev_adaptive_slot((IDirect3DSurface9 **)ctx.edi); }

} // namespace

void install_adaptation()
{
    struct Site {
        SafetyHookMid *storage;
        uintptr_t va;
        safetyhook::MidHookFn fn;
        const char *name;
    } sites[] = {
        {&g_adapt_mid, MC2_BLOOM_ADAPTIVE_LOG, adapt_midhook, "bloom adaptive-luminance slot"},
        {&g_prev_mid, MC2_BLOOM_PREV_ADAPTIVE_LOG, prev_midhook, "bloom prev-adaptive slot"},
    };
    for (const Site &s : sites) {
        if (!create_mid(*s.storage, s.va, s.fn, s.name)) {
            MC2VR_LOG("adapt: %s missing — exposure resets on render-size switches", s.name);
            continue;
        }
        MC2VR_LOG("adapt: installed %s MidHook @ %p", s.name, (void *)s.va);
    }
}

} // namespace mc2vr::hooks

#include "debug/census_hooks.hpp"

#include <safetyhook.hpp>

#include <windows.h>
#include <d3d9.h> // types only

#include <cstdint>

#include "debug/gpu_census.hpp"
#include "debug/ps_census.hpp"
#include "hooks/device.hpp"
#include "log.hpp"

namespace mc2vr::census_hooks {

namespace {

// d3d9.h method order.
constexpr size_t SLOT_SetPixelShader = 107;
constexpr size_t SLOT_DrawPrimitive = 81, SLOT_DrawIndexedPrimitive = 82,
                 SLOT_DrawPrimitiveUP = 83, SLOT_DrawIndexedPrimitiveUP = 84;

// Leaked by design (hooks/device.hpp).
safetyhook::VmHook *g_setps_hook = nullptr;
safetyhook::VmHook *g_draw_hooks[4] = {};

// debug_ps_census: pixel-shader binds.
HRESULT __stdcall setps_hook(void *self, void *ps)
{
    ps_census::on_set_ps(ps);
    return g_setps_hook->stdcall<HRESULT>(self, ps);
}

// debug_gpu_census / debug_ps_census: draw-call counting.
HRESULT __stdcall draw_hook(void *self, DWORD a, DWORD b, DWORD c)
{
    gpu_census::on_draw();
    ps_census::on_draw();
    return g_draw_hooks[0]->stdcall<HRESULT>(self, a, b, c);
}
HRESULT __stdcall drawindexed_hook(void *self, DWORD a, INT b, UINT c, UINT d, UINT e, UINT f)
{
    gpu_census::on_draw();
    ps_census::on_draw();
    return g_draw_hooks[1]->stdcall<HRESULT>(self, a, b, c, d, e, f);
}
HRESULT __stdcall drawup_hook(void *self, DWORD a, UINT b, const void *c, UINT d)
{
    gpu_census::on_draw();
    ps_census::on_draw();
    return g_draw_hooks[2]->stdcall<HRESULT>(self, a, b, c, d);
}
HRESULT __stdcall drawindexedup_hook(void *self, DWORD a, UINT b, UINT c, UINT d, const void *e,
                                     DWORD f, const void *g, UINT h)
{
    gpu_census::on_draw();
    ps_census::on_draw();
    return g_draw_hooks[3]->stdcall<HRESULT>(self, a, b, c, d, e, f, g, h);
}

void on_device(void * /*device*/)
{
    if (ps_census::enabled()) {
        if (safetyhook::VmHook *hook = device::hook_method(SLOT_SetPixelShader,
                                                           (void *)&setps_hook)) {
            g_setps_hook = hook;
            MC2VR_LOG("ps: SetPixelShader hook installed (debug_ps_census)");
        } else {
            MC2VR_LOG("ps: hook_method(SetPixelShader) failed — no per-shader census");
        }
    }
    if (gpu_census::enabled() || ps_census::enabled()) {
        struct SlotSpec {
            size_t slot;
            void *destination;
            safetyhook::VmHook **storage;
            const char *name;
        };
        const SlotSpec draw_slots[] = {
            {SLOT_DrawPrimitive, (void *)&draw_hook, &g_draw_hooks[0], "DrawPrimitive"},
            {SLOT_DrawIndexedPrimitive, (void *)&drawindexed_hook, &g_draw_hooks[1],
             "DrawIndexedPrimitive"},
            {SLOT_DrawPrimitiveUP, (void *)&drawup_hook, &g_draw_hooks[2], "DrawPrimitiveUP"},
            {SLOT_DrawIndexedPrimitiveUP, (void *)&drawindexedup_hook, &g_draw_hooks[3],
             "DrawIndexedPrimitiveUP"},
        };
        for (const SlotSpec &spec : draw_slots) {
            safetyhook::VmHook *hook = device::hook_method(spec.slot, spec.destination);
            if (hook == nullptr) {
                MC2VR_LOG("gpu: hook_method(%s) failed — draw counts incomplete", spec.name);
                continue;
            }
            *spec.storage = hook;
        }
        MC2VR_LOG("gpu: draw-call hooks installed (debug_gpu_census / debug_ps_census)");
    }
}

} // namespace

void install(void *device)
{
    on_device(device);
}

} // namespace mc2vr::census_hooks

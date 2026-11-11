// See occlusion_queries.hpp.

#include "hooks/occlusion_queries.hpp"

#include <windows.h>

#include <cstdint>
#include <cstring>

#include "game/addresses.h"
#include "game/pg_primitive.hpp"
#include "hooks/site.hpp"
#include "log.hpp"
#include "vr/occlusion_queries.hpp"

namespace mc2vr::hooks {

namespace {

SafetyHookMid g_ring_mid, g_release_mid, g_id_mid;

// RenderShell_InitDrawRecordTables, right after `mov [g_OcclusionQueryArray],
// edx` (or the branch that skips it while occlusion queries are off).
void ring_pointer_midhook(safetyhook::Context &)
{
    if (game::g_OcclusionQueryArray::get() != nullptr) {
        game::g_OcclusionQueryArray::set(occlusion_queries::ring());
    }
}

// LtiRenderer_ReleaseRenderTargets' entry: the engine releases its queries.
void release_midhook(safetyhook::Context &)
{
    occlusion_queries::on_release_render_targets();
}

// OcclusionBox_EmitPrimRecord's new ring id in EAX: an id still referenced
// becomes 0xffff (no query).
void id_midhook(safetyhook::Context &ctx)
{
    ctx.eax = occlusion_queries::on_id_allocated(ctx.eax);
}

// `and eax,0x800003ff` -> `and eax,0x80000fff` (kRingSize - 1). The sign
// fix-up after it (`jns`, then `or eax,0xfffffc00`) is never taken: the
// counter is masked non-negative.
bool widen_id_mask()
{
    static_assert(occlusion_queries::kRingSize == 0x1000, "the patched mask is 0xfff");
    uint8_t *const p = reinterpret_cast<uint8_t *>(MC2_RING_ID_MASK_AND);
    static const uint8_t expect[5] = {0x25, 0xff, 0x03, 0x00, 0x80};
    if (std::memcmp(p, expect, sizeof expect) != 0) {
        return false;
    }
    DWORD old;
    if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old)) {
        return false;
    }
    p[2] = 0x0f;
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    return true;
}

} // namespace

void install_occlusion_queries()
{
    if (!create_mid(g_ring_mid, MC2_RING_POINTER_SET, ring_pointer_midhook, "occlusion ring-pointer MidHook") ||
        !create_mid(g_release_mid, MC2_RELEASE_RENDER_TARGETS, release_midhook, "occlusion release MidHook")) {
        MC2VR_LOG("occlusion: ring hooks missing — the engine's 1024-id ring stays (no per-eye queries)");
        return;
    }
    if (!widen_id_mask()) {
        MC2VR_LOG("occlusion: id mask @ %p not `and eax,0x800003ff` — ids stay 10-bit",
                  (void *)MC2_RING_ID_MASK_AND);
    }
    if (!create_mid(g_id_mid, MC2_RING_ID_READY, id_midhook, "occlusion id-guard MidHook")) {
        MC2VR_LOG("occlusion: id-guard MidHook missing — a full ring would alias silently");
    }
    MC2VR_LOG("occlusion: carrier ring installed (%u slots, per-eye queries)", occlusion_queries::kRingSize);
}

} // namespace mc2vr::hooks

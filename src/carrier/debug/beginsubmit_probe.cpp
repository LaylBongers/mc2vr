#include "debug/beginsubmit_probe.hpp"

#include <safetyhook.hpp>

#include <cstdint>

#include "game/addresses.h"
#include "hooks/frame_tick.hpp"
#include "log.hpp"

namespace mc2vr::beginsubmit_probe {

namespace {

// BeginSubmit is thiscall via virtual dispatch — a plain inline hook can't
// portably preserve ECX, but a MidHook at the entry instruction hands us the
// full register context: ECX = this (its vtable slot reveals base-vs-derived),
// [ESP] = return address (reveals the frame driver — the encrypted
// thunk_FUN_0256b6f0 is the suspect per docs/reverse_engineering/frame_chain.md).
// One-shot burst; zero behavior change afterwards.
SafetyHookMid g_beginsubmit_mid;
uint64_t g_beginsubmit_hits = 0;

void beginsubmit_midhook(safetyhook::Context &ctx)
{
    if (++g_beginsubmit_hits > 3) {
        return;
    }

    const void *ths = (const void *)ctx.ecx;
    const uintptr_t return_address = *(const uintptr_t *)ctx.esp;

    char caller[96];
    char vtable_desc[96];
    describe_code_address((void *)return_address, caller, sizeof(caller));
    describe_code_address(ths ? *(void **)ths : nullptr, vtable_desc, sizeof(vtable_desc));

    MC2VR_LOG("BeginSubmit probe #%llu: frame=%llu | this=%p (vtable %s) | caller=%s",
              (unsigned long long)g_beginsubmit_hits, (unsigned long long)hooks::frame_count(),
              ths, vtable_desc, caller);
}

} // namespace

void install()
{
    // Entry instruction — register-context access, zero calling-convention
    // risk; instrument-only, no trampoline dispatch. Leaked by design like
    // the device hooks.
    auto mid = SafetyHookMid::create(reinterpret_cast<uint8_t *>(MC2_LTI_BEGINSUBMIT),
                                     beginsubmit_midhook);
    if (!mid) {
        MC2VR_LOG("warning: BeginSubmit MidHook install failed (error %u) — "
                  "driver/vtable probe skipped, frame hooks unaffected",
                  (unsigned)mid.error().type);
    } else {
        g_beginsubmit_mid = std::move(*mid);
        MC2VR_LOG("hooks: installed BeginSubmit probe (MidHook) @ %p", (void *)MC2_LTI_BEGINSUBMIT);
    }
}

} // namespace mc2vr::beginsubmit_probe

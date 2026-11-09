// Capturing the original's calls to a game::Function / game::Method.
//
// A capture is a MidHook that decodes the call's arguments back from
// registers and stack using the function's own declaration — the same one
// the call shim uses to make the call, so the two encodings cannot drift.
//
//   at_entry(fn)        hook the callee's entry. Fires for every caller; the
//                       harness keeps only calls returning into the original.
//   at_call(site, fn)   hook the `call` instruction inside the original — for
//                       callees that must not be hooked (VM thunks) or have no
//                       fixed entry (vtable methods).
//   at_site(site, fn, decode)  a few instructions earlier, with a hand-written
//                       decoder — when the call's 5-byte patch window would
//                       cover a branch target (check with a listing first).
#pragma once

#include <safetyhook.hpp>

#include <cstdint>

#include "game/function.hpp"

namespace mc2vr::shadow {

struct Capture {
    enum class Where : uint8_t { Entry, CallInstruction, Custom };
    const game::FnInfo *fn;
    uintptr_t site;
    Where where;
    // Custom: decodes the arguments at `site` (a few instructions before a
    // call whose own site cannot be hooked).
    void (*decode)(const safetyhook::Context &ctx, uint32_t *args) = nullptr;
};

template <class F>
constexpr Capture at_entry(const F &fn)
{
    return {&fn.info, fn.info.va, Capture::Where::Entry};
}
template <class F>
constexpr Capture at_call(uintptr_t site, const F &fn)
{
    return {&fn.info, site, Capture::Where::CallInstruction};
}
template <class F>
constexpr Capture at_site(uintptr_t site, const F &fn, void (*decode)(const safetyhook::Context &, uint32_t *))
{
    return {&fn.info, site, Capture::Where::Custom, decode};
}

// Arguments of the call in C++ parameter order.
inline void decode(const safetyhook::Context &ctx, const Capture &cap, uint32_t *args)
{
    if (cap.where == Capture::Where::Custom) {
        cap.decode(ctx, args);
        return;
    }
    // At an entry [esp] is the return address; at the call instruction the
    // first stack argument is [esp].
    const uint32_t *sp = reinterpret_cast<const uint32_t *>(ctx.esp) + (cap.where == Capture::Where::Entry ? 1 : 0);
    unsigned next_stack = 0;
    const game::FnInfo &fn = *cap.fn;
    for (uint8_t i = 0; i < fn.argc; i++) {
        switch (fn.params[i].loc) {
        case game::Loc::Stack: args[i] = sp[next_stack++]; break;
        case game::Loc::EAX: args[i] = ctx.eax; break;
        case game::Loc::ECX: args[i] = ctx.ecx; break;
        case game::Loc::EDX: args[i] = ctx.edx; break;
        case game::Loc::EBX: args[i] = ctx.ebx; break;
        case game::Loc::ESI: args[i] = ctx.esi; break;
        case game::Loc::EDI: args[i] = ctx.edi; break;
        case game::Loc::XMM0: args[i] = ctx.xmm0.u32[0]; break;
        case game::Loc::AL: args[i] = ctx.eax & 0xff; break;
        }
    }
}

} // namespace mc2vr::shadow

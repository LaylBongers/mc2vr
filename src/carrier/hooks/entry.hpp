// Entries for reimplemented originals: the inverse of the game call shim
// (game/function.hpp). Mod mechanism, so it lives in hooks/; the convention
// facts it reads (parameter locations, Pops) are the game/ declaration's.
//
// A reimplementation is plain C++ with the original's signature, e.g.
// `bool LtiRenderer_BeginSubmit(RenderShell *self)`; reimplemented code calls
// it directly. Game code still calls the original's address with the
// original's convention, so the inline hook there jumps to an entry generated
// from the original's Function declaration:
//
//   inline constexpr Function<bool(RenderShell *self)> LtiRenderer_BeginSubmit{
//       "LtiRenderer_BeginSubmit", 0x0074aaa0u, {ECX}};
//   make_entry(&kEntry<game::LtiRenderer_BeginSubmit, &reimpl::LtiRenderer_BeginSubmit>)
//
// (hooks/reimpl.cpp's replace<> does this for each replacement.)
//
// The entry saves the caller's registers, reads each parameter from where
// the declaration puts it, calls ours, and returns like the original: result
// in EAX, the declared Pops{N} popped. It restores every general register but
// EAX, and XMM1..XMM7 in full: the engine's custom conventions pass floats in
// XMM registers, and a caller may keep values in the ones its callee doesn't
// touch (Hud_DrawQuad's caller keeps x in XMM5 across calls).
#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <tuple>
#include <type_traits>
#include <utility>

#include "game/function.hpp"

namespace mc2vr::hooks {

namespace entry_abi {

// What the game called the original with (written by the asm entry in
// entry.cpp, which hard-codes these offsets).
struct EntryFrame {
    uint32_t eax, ecx, edx, ebx, esi, edi;  // +0 .. +20
    const uint32_t *stack;                  // +24 (first = [esp+4] at the original's entry)
    uint32_t pad;                           // +28
    uint32_t xmm[8][4];                     // +32: XMM0..XMM7, all four lanes
};
static_assert(sizeof(EntryFrame) == 160);

// One per replacement; the stub hands it to the asm entry.
struct EntryInfo {
    uint32_t (*adapter)(const EntryFrame *frame);  // +0
    uint16_t pops;                                 // +4
};

extern "C" void mc2vr_game_entry();  // the shared asm entry, reached through a stub

// The raw value of parameter i, from where the declaration puts it.
inline uint32_t entry_arg(const game::FnInfo &fn, const EntryFrame *f, size_t i)
{
    size_t slot = 0;
    for (size_t j = 0; j < i; j++) {
        if (fn.params[j].loc == game::Loc::Stack) slot++;
    }
    switch (fn.params[i].loc) {
    case game::Loc::Stack: return f->stack[slot];
    case game::Loc::EAX: return f->eax;
    case game::Loc::ECX: return f->ecx;
    case game::Loc::EDX: return f->edx;
    case game::Loc::EBX: return f->ebx;
    case game::Loc::ESI: return f->esi;
    case game::Loc::EDI: return f->edi;
    case game::Loc::XMM0: return f->xmm[0][0];
    case game::Loc::AL: return f->eax & 0xff;
    case game::Loc::XMM1: case game::Loc::XMM2: case game::Loc::XMM3: case game::Loc::XMM4:
    case game::Loc::XMM5: case game::Loc::XMM6: case game::Loc::XMM7:
        return f->xmm[1 + (int)fn.params[i].loc - (int)game::Loc::XMM1][0];
    }
    return 0;
}

template <class T>
T from_u32(uint32_t v)
{
    if constexpr (std::is_pointer_v<T>) {
        return reinterpret_cast<T>((uintptr_t)v);
    } else if constexpr (std::is_same_v<T, bool>) {
        return (v & 0xff) != 0;  // the engine's bools are a byte
    } else if constexpr (std::is_same_v<T, float>) {
        return std::bit_cast<float>(v);
    } else {
        static_assert(std::is_integral_v<T> || std::is_enum_v<T>, "unsupported parameter type");
        return (T)v;
    }
}

template <class F>
struct FnTraits;
template <class R, class... A>
struct FnTraits<R (*)(A...)> {
    using Sig = R(A...);
    using Ret = R;
    using Args = std::tuple<A...>;
    static constexpr size_t argc = sizeof...(A);
};

template <const auto &Decl, auto Fn, size_t... I>
uint32_t call_from_frame(const EntryFrame *f, std::index_sequence<I...>)
{
    using T = FnTraits<decltype(Fn)>;
    if constexpr (std::is_void_v<typename T::Ret>) {
        Fn(from_u32<std::tuple_element_t<I, typename T::Args>>(entry_arg(Decl.info, f, I))...);
        return 0;
    } else {
        return game::abi::to_u32(Fn(from_u32<std::tuple_element_t<I, typename T::Args>>(entry_arg(Decl.info, f, I))...));
    }
}

template <const auto &Decl, auto Fn>
uint32_t adapt(const EntryFrame *f)
{
    return call_from_frame<Decl, Fn>(f, std::make_index_sequence<FnTraits<decltype(Fn)>::argc>{});
}

} // namespace entry_abi

// The entry for Fn replacing the original declared by Decl. Fn has the
// declaration's signature.
template <const auto &Decl, auto Fn>
inline constexpr entry_abi::EntryInfo kEntry = [] {
    using T = entry_abi::FnTraits<decltype(Fn)>;
    static_assert(std::is_same_v<std::remove_cvref_t<decltype(Decl)>, game::Function<typename T::Sig>>,
                  "the reimplementation must have the declaration's signature");
    static_assert(Decl.info.kind == game::FnInfo::Kind::Direct, "entries replace a function at its address");
    using R = typename T::Ret;
    if constexpr (!std::is_void_v<R>) {
        static_assert(sizeof(R) <= 4 && !std::is_floating_point_v<R>,
                      "returns in EAX only (64-bit EDX:EAX and x87 float returns not supported)");
    }
    return entry_abi::EntryInfo{&entry_abi::adapt<Decl, Fn>, Decl.info.pops};
}();

// Builds the stub the inline hook jumps to (`push info; jmp mc2vr_game_entry`)
// in executable memory. Not thread-safe; call at install time. NULL on
// failure.
void *make_entry(const entry_abi::EntryInfo *info);

} // namespace mc2vr::hooks

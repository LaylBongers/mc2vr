// Original game functions as typed C++ callables.
//
// The engine passes arguments in registers more often than not (this in ESI,
// counts in EDX, floats in XMM0, ...). A Function declares one original
// function ONCE: its Ghidra name, its entry VA and where each parameter
// lives. Code then calls it like any C++ function:
//
//   inline constexpr Function<void(uint32_t, uint32_t)> Dx9_SetRenderState{
//       "Dx9_SetRenderState", 0x004843e0u, {STK, STK}};
//   Dx9_SetRenderState(D3DRS_CLIPPLANEENABLE, mask);
//
// The same declaration drives every use of the boundary: the call shim that
// loads the registers (function.cpp), and the shadow-mode capture that
// decodes a real call back into arguments (reimpl/glue/shadow). Parameter
// order is the C++ order; stack parameters are pushed in that order's
// sequence, first STK parameter at [esp+4] on entry.
//
// Only the inputs a callee actually reads are declared. Registers an original
// caller happens to leave set (leftovers from earlier code) are not modeled.
#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace mc2vr::game {

constexpr size_t kMaxParams = 8;

enum class Loc : uint8_t { Stack, EAX, ECX, EDX, EBX, ESI, EDI, XMM0 };

struct Param {
    Loc loc = Loc::Stack;
    // The value depends on the outcome of an earlier call (an HRESULT, the
    // device pointer). A shadow dry run cannot know it, so it is not compared.
    bool opaque = false;
    constexpr Param() = default;
    constexpr Param(Loc l, bool o = false) : loc(l), opaque(o) {}
};

inline constexpr Param STK{Loc::Stack};
inline constexpr Param EAX{Loc::EAX};
inline constexpr Param ECX{Loc::ECX};
inline constexpr Param EDX{Loc::EDX};
inline constexpr Param EBX{Loc::EBX};
inline constexpr Param ESI{Loc::ESI};
inline constexpr Param EDI{Loc::EDI};
inline constexpr Param XMM0{Loc::XMM0};
constexpr Param opaque(Param p) { return Param{p.loc, true}; }

// Type-erased description of a Function (what the shim and the shadow
// capture need).
struct FnInfo {
    enum class Kind : uint8_t {
        Direct,  // call `va`
        Method,  // COM-style: call [[arg0] + vtbl_offset], arg0 = this on the stack
    };
    const char *name;
    uintptr_t va;           // Direct: entry VA (0 for Method)
    uint16_t vtbl_offset;   // Method only
    Kind kind;
    uint8_t argc;
    Param params[kMaxParams];
};

namespace abi {

// Executes a call: through the current thread's CallInterceptor if one is
// set, otherwise for real (function.cpp). Returns EAX after the call.
uint32_t invoke(const FnInfo &fn, const uint32_t *args);

template <class T>
constexpr uint32_t to_u32(T v)
{
    if constexpr (std::is_pointer_v<T> || std::is_null_pointer_v<T>) {
        return (uint32_t)(uintptr_t)v;
    } else if constexpr (std::is_same_v<T, float>) {
        return std::bit_cast<uint32_t>(v);
    } else {
        static_assert(std::is_integral_v<T> || std::is_enum_v<T>, "unsupported parameter type");
        return (uint32_t)v;
    }
}

template <class R>
R from_eax(uint32_t eax)
{
    if constexpr (std::is_void_v<R>) {
        (void)eax;
    } else if constexpr (std::is_same_v<R, bool>) {
        return (eax & 0xff) != 0;  // the engine's bool returns are AL
    } else if constexpr (std::is_pointer_v<R>) {
        return reinterpret_cast<R>((uintptr_t)eax);
    } else {
        return (R)eax;
    }
}

} // namespace abi

template <class Sig>
struct Function;

template <class R, class... Args>
struct Function<R(Args...)> {
    static_assert(sizeof...(Args) <= kMaxParams);
    FnInfo info;

    template <size_t N>
    constexpr Function(const char *name, uintptr_t va, const Param (&params)[N])
        : info{name, va, 0, FnInfo::Kind::Direct, (uint8_t)N, {}}
    {
        static_assert(N == sizeof...(Args), "one Param per parameter");
        for (size_t i = 0; i < N; i++) info.params[i] = params[i];
    }
    constexpr Function(const char *name, uintptr_t va)
        : info{name, va, 0, FnInfo::Kind::Direct, 0, {}}
    {
        static_assert(sizeof...(Args) == 0, "parameters need their locations");
    }

    R operator()(Args... args) const
    {
        const uint32_t a[sizeof...(Args) + 1] = {abi::to_u32(args)..., 0};
        return abi::from_eax<R>(abi::invoke(info, a));
    }
};

// A virtual method called through the object's vtable (stdcall, all on the
// stack, `this` first). `this` is opaque for comparison.
template <class Sig>
struct Method;

template <class R, class This, class... Args>
struct Method<R(This, Args...)> {
    static_assert(sizeof...(Args) + 1 <= kMaxParams);
    FnInfo info;

    constexpr Method(const char *name, uint16_t vtbl_offset)
        : info{name, 0, vtbl_offset, FnInfo::Kind::Method, (uint8_t)(sizeof...(Args) + 1), {}}
    {
        info.params[0] = opaque(STK);
        for (size_t i = 1; i <= sizeof...(Args); i++) info.params[i] = STK;
    }

    R operator()(This self, Args... args) const
    {
        const uint32_t a[] = {abi::to_u32(self), abi::to_u32(args)...};
        return abi::from_eax<R>(abi::invoke(info, a));
    }
};

// Receives every Function/Method call made on its thread while installed
// (shadow dry runs and tests record instead of calling). Not nestable.
struct CallInterceptor {
    virtual uint32_t on_call(const FnInfo &fn, const uint32_t *args) = 0;

protected:
    ~CallInterceptor() = default;
};

void set_thread_interceptor(CallInterceptor *ic);  // nullptr = real calls
CallInterceptor *thread_interceptor();

class ScopedInterceptor {
public:
    explicit ScopedInterceptor(CallInterceptor &ic) : prev_(thread_interceptor()) { set_thread_interceptor(&ic); }
    ~ScopedInterceptor() { set_thread_interceptor(prev_); }
    ScopedInterceptor(const ScopedInterceptor &) = delete;
    ScopedInterceptor &operator=(const ScopedInterceptor &) = delete;

private:
    CallInterceptor *prev_;
};

} // namespace mc2vr::game

// Typed access to game globals at their fixed addresses.
//
// The image never relocates (docs/reverse_engineering/target_binary.md), so a
// game global is a fixed VA. Global<T, VA> gives it a type at compile time and
// keeps the address next to the Ghidra name, e.g.
//
//   using g_PrimitiveHead = Global<uint16_t, 0x01169780u>;
//   uint16_t head = g_PrimitiveHead::get();
//
// Every access is volatile: the game writes these from its own threads, and a
// reimplemented function must observe them exactly as the original did.
#pragma once

#include <cstdint>

namespace mc2vr::game {

template <typename T, uintptr_t VA>
struct Global {
    using type = T;
    static constexpr uintptr_t address = VA;

    static volatile T &ref() { return *reinterpret_cast<volatile T *>(VA); }
    static T get() { return ref(); }
    static void set(T value) { ref() = value; }
    static T *ptr() { return reinterpret_cast<T *>(VA); }
};

// A fixed-address array of N elements of T.
template <typename T, uintptr_t VA, uint32_t N>
struct GlobalArray {
    static constexpr uintptr_t address = VA;
    static constexpr uint32_t count = N;

    static T *data() { return reinterpret_cast<T *>(VA); }
    static T &at(uint32_t i) { return data()[i]; }
};

} // namespace mc2vr::game

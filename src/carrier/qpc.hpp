// QPC timing that stays exact on threads whose x87 FPU runs at single
// precision, like the game's render thread (docs/x87_precision.md): stamps are
// integer ticks, and only differences become milliseconds. Converting an
// absolute QPC value to double first (~3.7e7 ms after ~10 h of uptime) rounds
// it to 4 ms steps there.
#pragma once

#include <windows.h>

#include <cstdint>

namespace mc2vr::qpc {

// Now, in QPC ticks (the carrier's own import, never a game IAT slot).
inline int64_t now()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

inline int64_t freq()
{
    static const int64_t f = [] {
        LARGE_INTEGER v;
        QueryPerformanceFrequency(&v);
        return v.QuadPart;
    }();
    return f;
}

// A tick DIFFERENCE in ms (exact for any realistic interval).
inline double to_ms(int64_t ticks)
{
    return (double)ticks * 1000.0 / (double)freq();
}

inline double ms_since(int64_t t0)
{
    return to_ms(now() - t0);
}

} // namespace mc2vr::qpc

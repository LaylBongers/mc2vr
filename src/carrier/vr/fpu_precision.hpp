// Full x87 precision for a scope, restored afterwards.
//
// The game's render thread runs the x87 FPU at 24-bit (single) precision:
// D3D9 sets that on device creation without D3DCREATE_FPU_PRESERVE, and DXVK
// does the same (D3D9DeviceEx::SetupFPU). Double math there silently rounds to
// a 24-bit mantissa: QPC in ms (~3.7e7) to 4 ms steps, XrTime in ns (~3.7e13)
// to ~4 ms steps. That broke the OpenXR runtime's own frame timing once its
// calls moved onto the render thread (predictedDisplayPeriod read 11.01/11.53
// instead of 11.11 ms, 2026-11-10; docs/xr_session.md).
//
// Our OpenXR calls and timing run inside this guard; the game's own math keeps
// the precision it was built for. Precision control = x87 control word bits
// 8-9: 00 = 24-bit, 10 = 53-bit, 11 = 64-bit (the Linux default the runtime's
// native code expects). SSE math (reimpl/, -mfpmath=sse) is unaffected either
// way.
#pragma once

#include <cstdint>

namespace mc2vr {

class FullFpuPrecision {
public:
    FullFpuPrecision()
    {
        asm volatile("fnstcw %0" : "=m"(m_saved));
        const uint16_t full = (uint16_t)(m_saved | 0x0300);
        asm volatile("fldcw %0" : : "m"(full));
    }
    ~FullFpuPrecision() { asm volatile("fldcw %0" : : "m"(m_saved)); }
    FullFpuPrecision(const FullFpuPrecision &) = delete;
    FullFpuPrecision &operator=(const FullFpuPrecision &) = delete;

    // The precision-control bits of the control word on entry (0 = 24-bit).
    unsigned saved_precision() const { return (m_saved >> 8) & 3u; }

private:
    uint16_t m_saved = 0;
};

} // namespace mc2vr

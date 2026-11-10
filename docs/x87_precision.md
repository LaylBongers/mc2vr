# x87 precision on the render thread

The game's render thread runs the x87 FPU at 24-bit (single) precision, at least from the first device Reset on (the first OpenXR frame, before it, saw 53-bit; 2026-11-10). D3D9 sets single precision on device creation and Reset unless `D3DCREATE_FPU_PRESERVE` is passed; the game doesn't pass it, and DXVK reproduces the behavior (`D3D9DeviceEx::SetupFPU`). The carrier is compiled for i686 with x87 math (except `reimpl/`, which uses SSE: [reimpl.md](reimpl.md)), so carrier `double` math on that thread rounds to a 24-bit mantissa.

What it breaks: anything large in magnitude. QPC in ms (~3.7×10⁷ after ~10 h of uptime) has 4 ms steps; XrTime in ns (~3.7×10¹³) has ~4 ms steps. Seen 2026-11-10, when the OpenXR frame calls moved onto the render thread ([xr_session.md](xr_session.md)): every timing max read exactly 4/8/12/16 ms, and the runtime's own `predictedDisplayPeriod` read 11.01/11.53 ms instead of 11.11 (SteamVR's 32-bit runtime runs its timing math on the calling thread), which also skewed its predicted display times by up to ~±2 ms and the simulation step fed from them ([pacing.md](pacing.md)).

Rules:
- Timing: keep stamps as integer QPC ticks and convert only differences (`src/carrier/qpc.hpp`: `qpc::now`, `qpc::to_ms`, `qpc::ms_since`). Exact at any precision; used by the session's call timers, the eye flush, `debug_gpu_census` and `debug_wait_census` (`build_census`, `FrameTick`, `stream replay` already subtracted ticks first).
- Carrier code on the render thread that calls the OpenXR runtime runs under `FullFpuPrecision` (`src/carrier/vr/fpu_precision.hpp`: full precision for the scope, the game's control word restored afterwards): `xr_session::begin_frame` / `end_frame`.
- The game's own math keeps the precision it was built with; forcing `D3DCREATE_FPU_PRESERVE` instead would change it.

Not affected: integer math, SSE code, other threads (the update thread's precision is unchecked). Census CPU times from before 2026-11-10 (render thread) were quantized to 4 ms steps; their averages over many frames are roughly right, single values and maxima are not.

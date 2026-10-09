// Hook installation — one entry point so the init thread stays simple and
// the mechanism discipline (docs/plans/launcher.md) has a single choke point:
//   - inline/Mid hooks via SafetyHook, plaintext .text only
//   - never patch inside 0x01a48000+ (SecuROM) or VM-stub thunks
//   - the one exception: VmtHook on the captured IDirect3DDevice9 (M2+)
//
// Installed sites:
//   GameShell_FrameTick  0x00630e10  InlineHook  M1 — frame counter, timing
//   sanity, install ack; also observes the D3D device pointer once it exists
//   (M2 pre-work).
//
// Planned (M2/M3): GetD3DDevice thunk 0x0047f2f0 (InlineHook, capture
// device), device VmtHook (Present/EndScene/Reset pinning),
// RenderCmd_ExecuteStream 0x008569d0 (cmd histogram),
// RenderQueue_SubmitWorldPackets 0x0048e620 (view-table dump),
// RenderShell vtable +0x10/+0x14 @ 0x00be84c0 (direct pointer store).
#pragma once

#include <cstdint>

namespace mc2vr::hooks {

// Installs all hooks for the current milestone. Returns true on success.
// Caller must have verified the build lock (init.cpp gates on it).
bool install();

// Total GameShell_FrameTick invocations since install. Written on the main
// thread; readers on other threads get approximate values (fine for
// diagnostics — the count is only correlated by same-thread callers anyway).
uint64_t frame_count();

} // namespace mc2vr::hooks

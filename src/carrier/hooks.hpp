// Hook installation — one entry point so the init thread stays simple and
// the mechanism discipline (docs/launcher_plan.md) has a single choke point:
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

namespace mc2vr::hooks {

// Installs all hooks for the current milestone. Returns true on success.
// Caller must have verified the build lock (init.cpp gates on it).
bool install();

} // namespace mc2vr::hooks

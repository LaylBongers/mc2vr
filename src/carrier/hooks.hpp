// Hook installation — one entry point so the init thread stays simple and
// the mechanism discipline (docs/launcher_plan.md) has a single choke point:
//   - inline/Mid hooks via SafetyHook, plaintext .text only
//   - never patch inside 0x01a48000+ (SecuROM) or VM-stub thunks
//   - the one exception: VmtHook on the captured IDirect3DDevice9
//
// Planned sites (instrumentation first, redirect later):
//   GameShell_FrameTick          0x00630e10  InlineHook   frame counter/ack (M1)
//   GetD3DDevice thunk          0x0047f2f0  InlineHook   capture device     (M2)
//   RenderCmd_ExecuteStream     0x008569d0  Inline+Mid   cmd histogram      (M3)
//   RenderQueue_SubmitWorldPkt  0x0048e620  MidHook      view-table dump    (M3)
//   RenderShell vtable +0x10/+0x14  0x00be84c0  ptr store  NoOp confirm      (M3)
#pragma once

namespace mc2vr::hooks {

// Installs all hooks for the current milestone. Returns true on success.
// Caller must have verified the build lock (init.cpp gates on it).
bool install();

} // namespace mc2vr::hooks

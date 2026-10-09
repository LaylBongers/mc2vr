// Hook installation — one entry point so the init thread stays simple and
// the mechanism discipline (docs/hooks.md) has a single choke point:
//   - inline/Mid hooks via SafetyHook, plaintext .text only
//   - never patch inside 0x01a48000+ (SecuROM) or VM-stub thunks
//   - the one exception: VmtHook on the captured IDirect3DDevice9
//
// Installs the early (stage-1) hook: GameShell_FrameTick 0x00630e10 — frame
// counter, timing sanity, install ack; it also observes the D3D device
// pointer once it exists. The full installed-site inventory: docs/hooks.md.
#pragma once

#include <cstdint>

namespace mc2vr::hooks {

// Installs the stage-1 hooks. Returns true on success.
// Caller must have verified the build lock (init.cpp gates on it).
bool install();

// Total GameShell_FrameTick invocations since install. Written on the main
// thread; readers on other threads get approximate values (fine for
// diagnostics — the count is only correlated by same-thread callers anyway).
uint64_t frame_count();

} // namespace mc2vr::hooks

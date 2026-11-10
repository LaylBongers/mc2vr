// Hook installation — one entry point so the init thread stays simple and
// the mechanism discipline (docs/hooks.md) has a single choke point:
//   - inline/Mid hooks via SafetyHook, plaintext .text only
//   - never patch inside 0x01a48000+ (SecuROM) or VM-stub thunks
//   - the one exception: VmtHook on the captured IDirect3DDevice9
//
// Installs the attach-time hook: GameShell_FrameTick 0x00630e10 — frame
// counter, timing sanity, install ack, and the pacing gate
// (vr/pacing.hpp frame_start) before the game's own FrameTick. The full
// installed-site inventory: docs/hooks.md.
#pragma once

#include <cstdint>

namespace mc2vr::hooks {

// MidHook at FrameTick's g_RenderShell slot-5 (PostUpdateHook) call site:
// the once-per-frame session tick (xr_session::on_update_tick), main thread.
void install_post_update();
uint64_t post_update_calls();  // since install (diagnostics)

// Installs the attach-time hooks. Returns true on success.
// Caller must have verified the build lock (init.cpp gates on it).
bool install();

// Total GameShell_FrameTick invocations since install. Written on the main
// thread; readers on other threads get approximate values (fine for
// diagnostics — the count is only correlated by same-thread callers anyway).
uint64_t frame_count();

// Thread ID of the last GameShell_FrameTick entry (the update thread); 0
// before the first frame. For diagnostics that label threads.
uint32_t update_thread_id();

} // namespace mc2vr::hooks

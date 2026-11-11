// The first-person head-hiding bone controller (docs/first_person.md,
// increment 2): our own engine bone controller, attached to the player's
// character through BoneControllerRuntime_AddController — the mechanism the
// game uses for look-at, jostle and the like (game/animation.hpp). In its
// OnPose, vr/first_person collapses the head's subtree in the pose before
// syncModelSpace and publication, so the character renders headless from that
// frame's pose on (its shadow too).
//
// The object is the engine's controller layout: a hand-built vtable (the slot
// functions are thiscall, the engine's convention; unused slots point at the
// game's own no-op bodies) and the runtime's list node. All of it runs on the
// update thread (AnimSystem_PoseTask / BoneControllerRuntime_Update, live-
// verified 2026-11-11).
#pragma once

#include <cstdint>

namespace mc2vr::head_hide {

// Makes sure our controller is attached to the character behind handle
// (update thread, once per frame from vr/first_person). A controller left on
// a previous character reports IsDone and the engine deletes it.
void attach(uint32_t handle);

// Attached controllers / failed attach attempts since start (diagnostics).
uint32_t attached_count();
uint32_t failed_count();

} // namespace mc2vr::head_hide

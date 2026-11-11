// Gameplay census (mc2vr.conf debug_gameplay_census=on; docs/first_person.md):
// the runtime facts the first-person design still needs, in one run.
//
//   - Frame phases: MidHooks at the entries of Player_FrameUpdate,
//     HumanPhysics_FrameStep, AnimSystem_FrameUpdate, AnimSystem_PostUpdate
//     and CameraTable_FillLoop set the update thread's current phase.
//   - Bone controllers: BodyBoneTracker_OnPose (slot 5) and its slot 2 record
//     the caller (return address), the thread and the phase they ran in —
//     who runs the controllers, and whether the animation work is done before
//     the camera reads its target (OnPose in the camera phase or later = not).
//   - Camera classes: at CameraTable_FillLoop, each live g_CameraTable slot's
//     camera class (vtable -> type name), logged on change.
//   - Player modes: at Player_FrameUpdate, each player's overlay-mode stack
//     (null in gameplay, PDA modes on top; the gameplay cameras are chosen
//     elsewhere), logged on change. The game updates every player slot each
//     frame; only players with a character count, and the first one (the
//     primary) is handed to the watch tool (watch targets
//     player:<hex offset>, e.g. debug_watch=player:13c+player:124 for the aim
//     ray's readers = the fire site).
//   - Look input: while the human camera is live, its stick (+0x610/+0x614)
//     and second input pair (+0x10/+0x14) and yaw/pitch, per window.
//
// Observe-only: nothing is written to game memory.
#pragma once

namespace mc2vr::gameplay_census {

// debug_gameplay_census=on|off (default off). False on bad input.
bool set_enabled(const char *value);

// Installs the MidHooks when enabled (attach time).
void install();

// 10 s window report (render_dump's poller).
void report_window();

} // namespace mc2vr::gameplay_census

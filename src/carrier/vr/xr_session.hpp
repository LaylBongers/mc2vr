// The OpenXR session, in the game process (docs/xr_session.md).
//
// One carrier thread runs the OpenXR app loop with the game as the renderer,
// on DXVK's own Vulkan device and queue (dxvk_interop.hpp):
//   xrWaitFrame -> xrBeginFrame -> locate + publish this frame's views (the
//   render thread's pass-1 gate waits for them, pacing.hpp) -> wait for the
//   pair rendered from them (eye_share.hpp) -> copy it into the swapchains on
//   DXVK's queue (xr_present.hpp) -> xrEndFrame with the pair's own pose.
// Both waits are bounded, so neither side can hang the other.
//
// No runtime / no DXVK / any setup failure: logged once, the game runs on
// with camera pass-through (no published views).
#pragma once

#include <cstdint>

#include <d3d9.h>

#include "vec_math.hpp"

namespace mc2vr::xr_session {

// OpenXR XrFovf half-angles in radians (left/down negative) — not tangents.
struct Fov {
    float left = 0, right = 0, up = 0, down = 0;
};

// One eye of XrView, in the LOCAL space.
struct EyePose {
    math::Vec3 pos;
    math::Quat rot;
    Fov fov;
};

// XrSessionState values the carrier tests.
inline constexpr uint32_t kStateVisible = 4, kStateFocused = 5;

// The latest published frame state.
struct ViewState {
    uint32_t session_state = 0;  // XrSessionState (0 = no session)
    bool tracked = false;        // views valid (position + orientation)
    uint32_t pose_id = 0;        // id of the published views (0 = none yet)
    int64_t display_time = 0;    // XrFrameState.predictedDisplayTime, ns
    int64_t display_period = 0;  // XrFrameState.predictedDisplayPeriod, ns
    EyePose eye[2];              // [0] = LEFT, [1] = RIGHT
};

// Session VISIBLE/FOCUSED: frames are shown, the lockstep is on.
inline bool lockstep(uint32_t session_state)
{
    return session_state == kStateVisible || session_state == kStateFocused;
}

// The engine's first Present: starts the session thread on the game's device.
void start(IDirect3DDevice9 *device);

// Any thread. False when no session thread runs (VR off for the run).
bool read_state(ViewState *out);

// The render gate (render thread): waits at most timeout_ms for views newer
// than `last_pose_id`. False when VR is off; *timed_out says whether the wait
// ran out.
bool wait_views(uint32_t last_pose_id, uint32_t timeout_ms, ViewState *out, bool *timed_out);

// The session runs and shows frames (VISIBLE/FOCUSED): eye pairs are wanted.
bool presenting();

// Update thread, once per frame (slot-5 PostUpdateHook): hands the runtime's
// recommended per-eye size to render_size (render_size=hmd).
void on_update_tick();

// Waits until no copy of ours still reads the eye images on the GPU (before
// the images are released at a device Reset). Any thread.
void wait_gpu_copies();

} // namespace mc2vr::xr_session

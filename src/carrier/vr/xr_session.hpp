// The OpenXR session, woven into the game's stereo frame (docs/xr_session.md).
//
// The frame is the game's: our PgPrimitive_SubmitToGPU
// (reimpl/render/submit_to_gpu.cpp) brackets its two eye passes with
//   begin_frame()  takes the OpenXR frame the frame thread began and locates
//                  this frame's views (xrLocateViews) on the render thread;
//   ...            both eye passes render from those views;
//   end_frame()    copies the eye pair into the swapchains on DXVK's queue
//                  (xr_present.hpp) and hands the layer, with the views it was
//                  rendered from, to the frame thread.
// The frame thread runs the calls that block or change the session: events,
// xrBegin/EndSession, xrWaitFrame + xrBeginFrame, and xrEndFrame, where
// SteamVR throttles. The render thread meanwhile Presents and builds the next
// scene, as engines split a render thread from the thread that submits to the
// runtime. One frame in flight; a frame exists only when the game renders
// one, so the runtime sees the game's real cadence. Both run on DXVK's own
// Vulkan device and queue (dxvk_interop.hpp).
//
// No runtime / no DXVK / any setup failure: logged once, the game runs on
// with camera pass-through (no views).
#pragma once

#include <cstdint>

#include <d3d9.h>

#include "vr/vec_math.hpp"

namespace mc2vr::eye_share {
struct Pair;
}

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

// A frame's views and session state.
struct ViewState {
    uint32_t session_state = 0;  // XrSessionState (0 = no session)
    bool tracked = false;        // views valid (position + orientation)
    uint32_t pose_id = 0;        // frame counter of the views (0 = none yet)
    int64_t display_time = 0;    // XrFrameState.predictedDisplayTime, ns
    int64_t display_period = 0;  // XrFrameState.predictedDisplayPeriod, ns
    EyePose eye[2];              // [0] = LEFT, [1] = RIGHT
};

// Session VISIBLE/FOCUSED: frames are shown.
inline bool lockstep(uint32_t session_state)
{
    return session_state == kStateVisible || session_state == kStateFocused;
}

// One OpenXR frame, from begin_frame() to end_frame() (render thread).
struct Frame {
    bool begun = false;   // an OpenXR frame is begun for it: end_frame() must follow
    bool show = false;    // the runtime shows this frame: capture the eyes
    uint32_t serial = 0;  // which begun frame (the frame thread's count)
    ViewState views;      // this frame's views (tracked = false: none)
};

// The engine's first Present: the game's device. Setup runs at the next
// begin_frame().
void start(IDirect3DDevice9 *device);

// Render thread, the start of the stereo frame. Waits (bounded) for the
// frame thread's next begun frame — SteamVR's throttle, when the render thread
// is early. Not begun when VR is off or the session isn't running.
Frame begin_frame();

// Render thread, after the RIGHT pass. `rendered` = the views both passes
// rendered with (view_rewrite; nullptr = none), `pair` = the frame's eye
// images (nullptr = none). Without both, the frame ends with no layers.
void end_frame(const Frame &frame, const ViewState *rendered, const eye_share::Pair *pair);

// Any thread: the latest frame's views and the session state. False when VR
// is off.
bool read_state(ViewState *out);

// Update thread, once per frame (slot-5 PostUpdateHook): hands the runtime's
// recommended per-eye size to render_size (render_size=hmd).
void on_update_tick();

// Waits until no copy of ours still reads the eye images on the GPU (before
// the images are released at a device Reset). Any thread.
void wait_gpu_copies();

} // namespace mc2vr::xr_session

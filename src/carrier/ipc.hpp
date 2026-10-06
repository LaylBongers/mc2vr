// Carrier side of the mc2vr IPC (S4-1): connects to the host's shared section
// in stage 1 (non-fatal — no host means the game runs exactly as today),
// registers the game's pid, and runs a low-rate monitor thread that logs
// session-state transitions and host death. Later milestones (S4-4) read the
// pose from the render thread via read_state() — lock-free, never blocking.
#pragma once

#include "mc2vr_ipc.h"

namespace mc2vr {
namespace ipc {

// Stage 1. Opens the host section (env MC2VR_IPC_NAME overrides the name),
// validates magic/version and registers this pid. Logs once either way and
// starts the monitor thread on success. Safe to call once at init.
void connect();

// Connected to a live host? (False after the monitor observes host exit.)
bool connected();

// Lock-free seqlock read of the latest state. False when not connected or
// the writer never settled (payload untouched then).
bool read_state(Mc2IpcState *out);

// Pop one host event. False when none pending.
bool pop_event(Mc2IpcMsg *out);

// S4-5 session events: drain + act on the host's event ring. THE drain point —
// call once per frame from the main thread (slot-5 PostUpdateHook). The event
// ring is SPSC: this is the ONLY consumer (the 250ms monitor thread no longer
// pops events; it only reads the seqlock state + watches host death).
//   SESSION_STATE — log transitions.
//   RECENTER      — log (nothing to apply: the camera consumes live HMD poses,
//                   so a reference-space change propagates at the next pass-1
//                   pose sample by construction).
//   EXIT          — one-shot WM_CLOSE to the game's root window (runtime wants
//                   the app to quit); the engine's own pump then shuts down,
//                   and the host follows via its carrier death watch.
void drain_events();

// Queue the Shutdown command (idempotent, logged once).
void send_shutdown();

// S4-2: announce the shared-texture ring geometry once (a=width b=height
// c=format). False when not connected or the ring is full.
bool send_config(uint32_t width, uint32_t height, uint32_t format);

// S4-2: publish one eye's rendered frame in a shared-texture slot
// (x=frameId y=sharedHandle a=slot b=eye c=width d=height). False when not
// connected or the command ring is full.
// poseId (S4-4) = the pose id the frame was rendered with (0 = none).
bool send_frame_ready(uint64_t frameId, uint64_t handle, uint32_t slot,
                      uint32_t eye, uint32_t width, uint32_t height,
                      uint32_t poseId);

}  // namespace ipc
}  // namespace mc2vr

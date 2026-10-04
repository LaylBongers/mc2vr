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

// Queue the Shutdown command (idempotent, logged once).
void send_shutdown();

}  // namespace ipc
}  // namespace mc2vr

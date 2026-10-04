// Host side of the mc2vr IPC (S4-1): creates the shared section, publishes the
// seqlocked state, feeds the host->carrier event ring, drains the command
// ring. All calls are non-blocking; the host owns OpenXR pacing.
#pragma once

#include <cstdint>

#include "mc2vr_ipc.h"
#include "pose.hpp"

namespace ipc {

// Create + map the section (fails if a host already owns one). Sets the
// ready-for-carrier header fields. Returns false (and logs) on failure; all
// other calls are safe no-ops afterwards.
bool server_init(const char* name);

// Publish the latest HmdFrame + session state (seqlock write).
void publish(const HmdFrame& f, float ipd, uint32_t sessionState,
             uint32_t recenterCount, uint32_t hostFrame);

// Push a session event to the carrier ring (drops + logs on full).
void push_event(uint32_t type, uint32_t a, uint32_t b);

// Drain one command; returns false when the ring is empty.
bool pop_command(Mc2IpcMsg* out);

// True once a carrier registered (pid visible in the header).
bool carrier_connected();
uint32_t carrier_pid();

// True when the registered carrier process has exited (checked at most every
// checkIntervalMs; uses a cached process handle).
bool carrier_died(unsigned checkIntervalMs);

// Set the header's hostExiting flag right before host shutdown.
void mark_exiting();

}  // namespace ipc

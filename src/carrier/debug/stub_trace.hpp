// SecuROM-stub callback tracer (docs/render_path.md, § VM stub callbacks).
//
// Question: does the opaque VM stub (call site 0x004c99f9 -> 0x0050f660)
// call back into plaintext code? Method (sanctioned — plaintext neighbors
// only, the VM is never touched):
//   - bracket the stub call with two MidHooks (before / after the call) to
//     keep a main-thread phase: PRE (before the stub, this frame), STUB
//     (inside the stub call), POST (after it, until the next frame);
//   - MidHook the ENTRY of a list of plaintext helpers the stub plausibly
//     needs; each hit records the phase and the caller's return address,
//     classified by region (.text / SecuROM sections / other). A return
//     address outside .text means protected code called the helper
//     directly; STUB-phase hits with .text return addresses are nested
//     (plaintext called by plaintext called by the stub) or direct from
//     a trampoline — the per-address one-shot log tells which.
// Controlled by mc2vr.conf debug_stub_trace=on|off (default off).
#pragma once

namespace mc2vr::trace {

// Parse the conf value; returns false on unrecognized input.
bool set_enabled(const char *value);

// Install the brackets + entry hooks (no-op when disabled). Call from the
// main render-hook install path after the device is up.
void install();

// 10s window report (called from render_dump.cpp's poller); resets counters.
void report_window();

} // namespace mc2vr::trace

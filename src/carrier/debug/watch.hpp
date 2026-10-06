// Hardware-watchpoint tracer for the ViewEntry camera fields (the suspected
// culling inputs; docs/reverse_engineering/view_and_camera.md § open items).
//
// The doc records two unproven assumptions:
//   1. The culling/draw-record build happens inside the SecuROM-VM'd packet
//      interpreter (stub 0x0050f660) — proposed evidence: watch a live
//      camera field and log WHO touches it (the proposed next step, never run).
//   2. ViewEntry's derived copies "feed streaming/culling" — likewise
//      unproven; the reader set of these fields has never been observed.
//
// This module turns x86 hardware debug registers (DR0-3) into a per-field
// access logger. Because DRs are per-thread and user mode cannot execute
// `mov dr`, arming suspends every process thread except the arming one
// (the render_dump poller) and SetThreadContext()s the debug-register block.
// Any armed thread that then touches a watched address raises a
// single-step exception caught by our vectored handler, which logs the
// accessor's EIP (the instruction AFTER the access — x86 data breakpoints
// are traps) with region + known-function classification.
//
// Watch mode "full" (RW=3, default) catches readers AND writers in one run:
// writer EIPs answer "is the camera-field producer VM or plaintext?", reader
// EIPs answer "is the culling consumer VM or plaintext?" — e.g. repeated
// reader EIPs inside Stext/Sitext (the VM interpreter loop) are direct
// evidence the consumer reads camera data from inside the VM.
//
// Self-instrumentation note: render_dump's whole-ViewEntry dumps run on the
// main thread and read the watched bytes; those hits classify as
// "carrier-self" and are expected/ignorable.
//
// Conf keys (mc2vr.conf; all default off):
//   debug_watch=quat+pos+fov+slot0   '+'-separated, max 4. Named ViewEntry
//       fields of the chosen view (first type-2 active view unless
//       debug_watch_view pins an index), or raw addresses "addr:0x01ABC" to
//       watch any VA (e.g. a g_ViewContextTable record's VP rows once
//       identified — the open camera-matrix writer hunt).
//   debug_watch_mode=full|write      full = read+write (default), write = writes only.
//   debug_watch_view=N               view index to watch (default: first type-2 seen).
//   debug_watch_hits=N                per-unique-EIP detail lines (default 30);
//       the 10s window report aggregates every unique EIP regardless.
//
// Caveats (deliberate, logged): arming waits for the first type-2 view
// submission, so activation-time writes (ViewEntry_Activate) may happen
// before arming — steady-state per-frame accesses are what the culling
// question needs. Threads created after the one-time arm sweep are not
// watched. Each window verifies the DRs still hold (detects SecuROM DRx
// clobbering — documented inert for anti-debug, but DRx use was untested).
#pragma once

#include <cstdint>

namespace mc2vr::watch {

// debug_watch=<targets> ('+'-separated; max 4). Returns false on a parse
// error (all-or-nothing: a bad token disables the feature).
bool set_targets(const char *value);

// debug_watch_mode=full|write.
bool set_mode(const char *value);

// debug_watch_view=N (UINT32_MAX = first type-2 active view).
void set_view_index(uint32_t idx);

// debug_watch_hits=N (per-unique-EIP detail lines).
void set_detail_hits(uint32_t n);

// Per-view callback from render_dump's SubmitWorldPackets MidHook (main
// thread): identifies the view entry whose fields get watched.
void on_view(uint32_t idx, uint32_t type, const uint8_t *entry);

// 250ms poller tick (render_dump poller thread): performs the one-time
// suspend-sweep arm once a target view exists.
void poll();

// 10s window report (render_dump poller thread): logs target values,
// the per-EIP hit table, DR verification, then resets the window.
void report_window();

} // namespace mc2vr::watch

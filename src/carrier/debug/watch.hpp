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
//       fields of the chosen view, or STAGING targets (stagingpos
//       stagingquat stagingserial — the VM consumer's staged camera block,
//       the direct culling-consumer probe), or raw addresses "addr:0x01ABC"
//       to watch any VA. View selection: among type-2 views whose companion
//       liveness byte is 01 (the live-camera marker), the most-walked one
//       after a 10s settle — loading templates (t3=00) and rarely-walked
//       special views lose; debug_watch_view pins an index instead.
//   debug_watch_mode=full|write      full = read+write (default), write = writes only.
//   debug_watch_view=N               view index to watch (default: first live type-2).
//   debug_watch_hits=N                per-unique-EIP detail lines (default 30);
//       the 10s window report aggregates every unique EIP regardless.
//
// Live-run lessons (2026-10-06, load-into-gameplay crash, fixed):
//   - Arming works under Wine (DR7 applied + verified on all 27 threads), BUT
//     Wine delivers the resulting #DB WITHOUT Dr6 in the exception context
//     (debug regs live server-side). The handler therefore decides ownership
//     WITHOUT Dr6: EFlags.TF set = someone else's single-step (passed on);
//     TF clear while armed = our data breakpoint (handled). Requiring Dr6
//     passed the exception to the default handler = the crash.
//   - Consequence: per-field attribution is unavailable under Wine (hits log
//     field "?"); run a single target (debug_watch=quat) for exact attribution.
//   - The first type-2 view is a loading template with dead camera fields;
//     selection now waits for a live quaternion.
//
// Remaining caveats (deliberate, logged): arming waits for a view submission
// (activation-time writes can predate it); threads created after the one-time
// arm sweep are not watched; each window verifies the DRs still hold (detects
// SecuROM DRx clobbering — documented inert for anti-debug, but DRx use was
// untested).
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
// thread): identifies the view entry whose fields get watched. ebx_this =
// the frame-ctx object (kept in EBX through the walk) — needed for the
// staging-block targets (this+0xc2110+idx*0x30).
void on_view(uint32_t idx, uint32_t type, const uint8_t *entry, uintptr_t ebx_this);

// 250ms poller tick (render_dump poller thread): performs the one-time
// suspend-sweep arm once a target view exists.
void poll();

// 10s window report (render_dump poller thread): logs target values,
// the per-EIP hit table, DR verification, then resets the window.
void report_window();

} // namespace mc2vr::watch

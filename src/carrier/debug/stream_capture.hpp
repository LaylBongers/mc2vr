// S2c-0: render-command-stream capture + opcode census (docs/plans/stereo_design.md §S2,
// docs/plans/stereo_design.md §S2c). Read-only — no behavior change.
//
// The M3 opcode MidHook inside RenderCmd_ExecuteStream (0x008569f5, installed
// once by render_dump.cpp) calls on_opcode() for every command. At that
// instruction EBP = pointer to the current command dword and [ESP+0x14] = the
// stream base argument (verified against the 0x008569d0 disassembly: entry
// pushes ecx/ebp/esi/edi, stream is stack arg 1). When EBP == stream base the
// whole stream is walked with the RE'd opcode size table and copied into a
// per-frame arena; op 0x00 terminates. Aggregates per window, logs a payload
// census (first samples per opcode, decoded), and can dump whole frames of raw
// streams to <GAME_DIR>/mc2vr/ for offline analysis (tools/analyze_dumps.py).
//
// Per-address facts (opcode table, helper semantics) live on the
// RenderCmd_ExecuteStream plate in Ghidra — do not re-derive here.
#pragma once

#include <cstdint>

namespace mc2vr::s2c {

// mc2vr.conf debug_stream_capture=off|on (default off — zero overhead when disabled).
bool set_enabled(const char *value);

// mc2vr.conf debug_stream_dump_frames=N: after debug_dump_delay seconds of capture,
// dump N consecutive frames' raw streams to mc2vr_stream_frame<N>.txt (0 = never).
void set_dump_frames(uint32_t n);

// mc2vr.conf debug_dump_delay=S: seconds after the first captured stream
// before the dump window arms (default 15.0 — let the game reach gameplay).
void set_dump_delay(float seconds);

// mc2vr.conf frame_replay=off|on (default off): S2c-1 — invoke
// PgPrimitive_SubmitToGPU a second time after the original returns (same
// eye, same RTs — the "replay unchanged" state-safety test; docs/plans/stereo_design.md §S2).
bool set_replay_enabled(const char *value);

// Install the S2c-1 frame-replay InlineHook on PgPrimitive_SubmitToGPU
// (no-op unless frame_replay=on was set first; the hook itself is installed
// unconditionally and cheap). Call after conf load.
void install();

// Called from render_dump.cpp's RenderCmd_ExecuteStream opcode MidHook (main
// thread, once per command): esp/ebp/eax are the live register values.
void on_opcode(uint32_t esp, uint32_t ebp, uint32_t eax);

// 10s window report (called from render_dump.cpp's poller); resets counters.
void report_window();

} // namespace mc2vr::s2c

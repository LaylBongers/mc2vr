// Render-command-stream capture + opcode census (docs/stereo.md,
// docs/stereo.md). Read-only — no behavior change.
//
// The RenderCmd_ExecuteStream reimplementation calls on_command() for every
// executed command (through render_dump's command tap). On a stream's first
// command the whole stream is walked with the RE'd opcode size table and copied into a
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

// Per executed command, from render_dump's command tap (the
// RenderCmd_ExecuteStream reimplementation calls it): cmd = the command,
// stream / arg2 / arg3 = ExecuteStream's arguments.
void on_command(const uint32_t *cmd, const uint32_t *stream, uint32_t arg2, uint32_t arg3);

// 10s window report (called from render_dump.cpp's poller); resets counters.
void report_window();

} // namespace mc2vr::s2c

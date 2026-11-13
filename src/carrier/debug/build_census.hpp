// Scene -> draw-record build census (docs/reverse_engineering/scene_record_build.md).
//
// Read-only MidHooks on the render task graph. Questions:
//   1. Which thread runs RenderShell_BuildSceneRecords / PgPrimitive_PublishFrame
//      (vs the update thread and the RenderTask_RenderFrame thread), and how long do
//      they take (own QPC + the engine's g_SceneBuildMs)? Is this the ~2.6 ms prep?
//   2. Task order: node 0 / 3 / 4 / 2 and build/publish brackets — a one-shot
//      sequence dump (~3 cycles) after the first 10 s window.
//   3. Which emitters write which records: PgPrimitiveScratchRecord_Submit tallied by
//      (emitter return address, sub-view index, record type).
//   4. GUI (Scaleform GRenderer impl, vtable 0x00bad178): BeginDisplay calls per frame,
//      a one-shot caller chain + viewport per distinct caller, GUI records per frame, and
//      (once per window, at PublishFrame return) where the GUI records sit in the sorted
//      draw list and which view / RT / stencil fields and VS constants they carry.
// "build:" log lines. Controlled by mc2vr.conf debug_build_census=on|off (default off).
#pragma once

#include <cstdint>

namespace mc2vr::build_census {

// Parse the conf value; returns false on unrecognized input.
bool set_enabled(const char *value);

// Install the MidHooks (no-op unless enabled or the GUI marks are required).
void install();

// PublishFrame return (the production hook in hooks/hud.cpp taps it).
void on_publish_return();

// debug_gui_census: install the hooks for the GUI record marks even with the
// census itself off (its reports stay off). Before install().
void require_gui_marks();
// Whether record `index` of the published list at `base` is a GUI record:
// 1 / 0, or -1 when unknown (marks not wanted, or a different list).
int is_gui_record(const void *base, uint32_t index);
// The return address of the Submit call that emitted record `index` of the
// published list at `base` (game VA), or 0 when unknown. `above` (3 entries)
// gets heuristic return addresses further up the stack (0 when none found).
uint32_t record_emitter(const void *base, uint32_t index, uint32_t above[3] = nullptr);

// A reimplemented emitter (reimpl/) calls Submit from our code, so the return
// address is ours: while `ret` is nonzero, Submits are filed under it instead
// (the original's call-site return address). 0 = off. Build thread only.
void set_emitter_override(uint32_t ret);

// 10 s window report (render_dump.cpp's poller); resets the window counters.
void report_window();

} // namespace mc2vr::build_census

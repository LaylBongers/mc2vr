// Per-eye occlusion queries (docs/occluder_boxes.md).
//
// The engine culls objects with hardware occlusion queries: each flagged
// object draws a box (view type 4) inside the query of its ring id, and later
// frames poll that query; S_OK with 0 pixels keeps the object hidden. The
// stereo frame draws every box twice, LEFT then RIGHT, and re-issuing a query
// restarts it, so with one query per id the game would only ever see the
// right eye's count, and objects visible only to the left eye would be culled.
//
// Each query the engine created in its ring (g_OcclusionQueryArray) is
// replaced by a pair: the engine's own query (RIGHT, pass 2) plus a carrier
// query (LEFT, pass 1). Issue goes to the current pass's eye. GetData combines
// both: not ready if either eye isn't, otherwise the larger eye's pixel count,
// so an object is hidden only when BOTH eyes saw none of its box. All three
// engine readers (the two pollers and OcclusionQuery_GetPixelCount) go through
// GetData, so none of them needs changing. None of them waits: a result that
// isn't ready counts as visible (no GPU stall). The engine's own Release of a pair releases both eyes:
// the pairs live and die with the engine's ring (created at SetupRenderTargets,
// released before a device Reset). The same substitution pattern as scene
// MSAA's surface stand-ins (docs/hooks.md rules).
//
// Render thread throughout: on_submit_begin, on_box_draw, and the engine's
// GetData polls (the scene build, RenderShell_BuildSceneRecords, before
// pass 1).
#pragma once

#include <cstdint>

namespace mc2vr::occlusion_queries {

// Start of PgPrimitive_SubmitToGPU (before pass 1): wrap every ring entry the
// engine (re)created since the last frame. Cheap when nothing changed.
void on_submit_begin();

// An occlusion box is drawn (view type 4); vcd_regs = its technique's
// viewContextData register count. Diagnostic: a box without viewContextData
// would not get the per-eye camera (counted in the window report).
void on_box_draw(uint32_t vcd_regs);

// 10s window report; resets counters.
void report_window();

} // namespace mc2vr::occlusion_queries

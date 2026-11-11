// The occlusion-query ring, carrier-owned and per eye (docs/occluder_boxes.md).
//
// The engine culls objects with hardware occlusion queries: each flagged
// object draws a box (view type 4) inside the query of its ring id, and the
// object's poll two frames later reads that query; S_OK with 0 pixels keeps
// the object hidden. Two things break that in VR:
//   - The stereo frame draws every box twice, LEFT then RIGHT, and re-issuing
//     a query restarts it: one query per id would only see the right eye.
//   - The engine's ring has 1024 ids, handed out in order, one per box. The
//     HMD cull frustum is ~7x the stock solid angle, so in dense areas a frame
//     issues 500+ boxes; with the two-frame read latency an id comes round
//     again before its owner reads it, and the owner reads another object's
//     box (0 pixels = hidden: undrawn objects, stale-colour smears).
//
// So the ring the engine reads (g_OcclusionQueryArray) is ours: kRingSize
// slots, each a query object with one query per eye. The emitter's id mask is
// widened to match (hooks/occlusion_queries.cpp), and an id still referenced
// is never handed out again (on_id_allocated); a read of a slot reissued since
// its owner's issue is counted (aliasedReads), so the failure shows in the log
// if it ever happens another way. Issue goes to the current
// pass's eye. GetData combines both: not ready if either issued eye isn't,
// otherwise the larger eye's pixel count, so an object is hidden only when
// BOTH eyes saw none of its box. A slot whose queries don't exist (before the
// device is up, after a release) is never ready, which every reader treats as
// visible. All three engine readers (the two pollers and
// OcclusionQuery_GetPixelCount) go through GetData and stay original; none
// waits. The engine still creates and releases its own 1024 queries; they are
// unused. Ours are created at the first submit after the engine's and
// released with the engine's (LtiRenderer_ReleaseRenderTargets, before a
// device Reset).
//
// Render thread throughout: the ring setup and release, on_submit_begin,
// on_box_draw, and the engine's GetData polls (the scene build, before pass 1).
#pragma once

#include <cstdint>

struct IDirect3DQuery9;

namespace mc2vr::occlusion_queries {

// Slots in our ring; the emitter's id mask is kRingSize - 1.
constexpr uint32_t kRingSize = 0x1000;
// Frames an id stays referenced after it is handed out: issued in frame F,
// read in F+2, its history slot overwritten in F+3.
constexpr uint32_t kIdLifetime = 3;
constexpr uint32_t kNoQuery = 0xffff;

// Our ring, kRingSize entries (always valid objects). hooks/occlusion_queries
// points g_OcclusionQueryArray at it right after the engine points it at its
// own (RenderShell_InitDrawRecordTables).
IDirect3DQuery9 **ring();

// The engine releases its render targets and queries (before a device Reset):
// release ours too.
void on_release_render_targets();

// OcclusionBox_EmitPrimRecord took ring id `id` for a box: returns the id to
// use. An id handed out fewer than kIdLifetime frames ago may still be read by
// its owner (the readers' 3-slot histories hold an id for three frames), so
// then the ring is full and the box gets 0xffff, no query: its object counts
// as visible (ringFull in the report).
uint32_t on_id_allocated(uint32_t id);

// Start of PgPrimitive_SubmitToGPU (before pass 1): (re)create our queries
// when the engine's ring exists and ours don't. Cheap otherwise.
void on_submit_begin();

// An occlusion box is drawn (view type 4); vcd_regs = its technique's
// viewContextData register count. Diagnostic: a box without viewContextData
// would not get the per-eye camera (counted in the window report).
void on_box_draw(uint32_t vcd_regs);

// 10s window report; resets counters.
void report_window();

} // namespace mc2vr::occlusion_queries

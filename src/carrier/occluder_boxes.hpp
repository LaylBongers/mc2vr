// Occluder boxes — the engine's per-object 'OcclusionMaterial' unit boxes.
//
// FUN_0046edf0 (Ghidra: OcclusionBox_EmitPrimRecord) emits a PgOcclusionVP box
// record per flagged object, called from ViewObjects_DrawAndEmitOcclusionBoxes
// (0x00468ea0) at 0x00468eed and 0x00468f12 (`call rel32`, no stack args).
// LIVE-VERIFIED 2026-10-09: with the boxes emitted, the two-pass VR render
// leaves undrawn regions (stale colour — the game never clears it) at fixed
// world seams; NOPing both calls removes the smearing completely.
//
// The boxes are HARDWARE OCCLUSION QUERY bounding boxes (static RE 2026-10-09):
// each flagged object issues a box draw with a query ring id; next frames
// ObjectOcclusionQueries_PollAndMarkVisible (0x00468c10) polls the oldest
// query (IDirect3DQuery9::GetData) and the object is drawn only while it was
// seen within the last 3 frames. An object with no query id counts as visible,
// so skipping the boxes simply disables occlusion culling (costs draw calls
// only). See docs/frustum_cull_plan.md "VR smearing: occluder boxes".
#pragma once

namespace mc2vr::occluder_boxes {

// mc2vr.conf skip_occluder_boxes=on|off (default on). Returns false on
// unrecognized input. Must be set before install().
bool set_skip(const char *value);

// Patch both box-emitting calls to NOPs when skipping is enabled. Verifies
// each site still holds the expected `call 0x46edf0` first.
void install();

} // namespace mc2vr::occluder_boxes

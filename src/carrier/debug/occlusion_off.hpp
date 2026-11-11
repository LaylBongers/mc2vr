// debug_occlusion_off=on: the engine's occlusion culling off, for A/B
// measurements of what it saves (docs/occluder_boxes.md).
//
// NOPs both OcclusionBox_EmitPrimRecord calls in
// ViewObjects_DrawAndEmitOcclusionBoxes (MC2_OCCLUSION_BOX_CALL_A/B, `call
// rel32`, no stack args). Without boxes no object gets a query ring id, and an
// object without one counts as visible: every object passing the frustum cull
// is drawn. For telling whether a visual issue comes from occlusion culling
// (vr/occlusion_queries.hpp).
#pragma once

namespace mc2vr::occlusion_off {

// mc2vr.conf debug_occlusion_off=on|off (default off). False on bad input.
bool set_enabled(const char *value);

// Patch both box-emitting calls to NOPs when enabled. Verifies each site
// still holds `call OcclusionBox_EmitPrimRecord` first.
void install();

} // namespace mc2vr::occlusion_off

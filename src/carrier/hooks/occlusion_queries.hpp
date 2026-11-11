// Hooks for vr/occlusion_queries (the carrier-owned per-eye query ring).
#pragma once

namespace mc2vr::hooks {

// Install the ring-pointer MidHook (RenderShell_InitDrawRecordTables' tail:
// g_OcclusionQueryArray -> our ring), the release MidHook
// (LtiRenderer_ReleaseRenderTargets' entry: our queries go with the engine's),
// widen the emitter's ring id mask to our ring's size, and guard the id it
// takes (an id still referenced is not handed out again). The mask is only
// widened when both MidHooks are in place: wider ids would overrun the
// engine's own 1024-entry ring.
void install_occlusion_queries();

} // namespace mc2vr::hooks

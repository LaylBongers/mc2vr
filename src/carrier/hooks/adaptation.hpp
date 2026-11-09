// Hooks for vr/adaptation (HDR eye-adaptation continuity).
#pragma once

namespace mc2vr::hooks {

// MidHooks at the bloom surface-creation log sites (PgLtiBloom_CreateSurfaces)
// that hold the adaptive-luminance slot addresses. Without them exposure
// resets on render-size switches; nothing else breaks.
void install_adaptation();

} // namespace mc2vr::hooks

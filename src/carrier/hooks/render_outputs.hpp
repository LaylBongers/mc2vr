// Hooks for vr/render_outputs (carrier-owned frame outputs).
#pragma once

namespace mc2vr::hooks {

// InlineHook-wrap PgLtiRendererPc_SetupRenderTargets + the CheckDevice
// device-restore MidHook. Without them the takeover falls back to the
// per-Present invariant check (one frame late).
void install_render_outputs();

} // namespace mc2vr::hooks

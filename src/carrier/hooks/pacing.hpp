// Hooks for vr/pacing (replay-pass GPU drain skip).
#pragma once

namespace mc2vr::hooks {

// MidHook at LtiRenderer_BeginSubmit's drain check. Without it the replay
// pass keeps the engine's GPU drain; nothing else breaks.
void install_pacing();

} // namespace mc2vr::hooks

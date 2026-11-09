// Hooks for vr/hud (the HUD layer, docs/hud.md).
#pragma once

namespace mc2vr::hooks {

// MidHooks:
//   - LtiRenderer_PublishIfNewFrame's PgPrimitive_PublishFrame return: the
//     HUD/marker record ranges and the world-marker quads are published with
//     the record list. Without it the HUD stays in the eye images.
//   - Hud_DrawQuad before its renderer call: each world-marker quad (centre,
//     half height, depth) for vr/world_markers. Without it markers stay mono.
void install_hud();

} // namespace mc2vr::hooks

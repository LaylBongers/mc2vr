// World markers placed per eye (docs/hud.md).
//
// The game draws each world marker (vehicle entry, pickups, objectives) as
// screen-space quads at the screen position and depth its single camera gives,
// identical in both eyes, at a fixed pixel size: no stereo, and far too big on
// the headset. Per quad record:
//   - Build (hooks/hud.cpp at Hud_DrawQuad's renderer call, build thread):
//     its centre, half height and depth.
//     Consecutive quads at the same depth are one marker (background + icon):
//     they share the first quad's centre as anchor and its half height for
//     the scale. Depth-0 quads (pinned to the screen edge) are left alone.
//   - Draw (PgPrimitive_SubmitToGPU, headset frames): the anchor goes through
//     the pass's mono -> eye clip map (view_rewrite, the scene's own delta);
//     the quad is moved to the eye's anchor and scaled around it to
//     marker_size degrees tall, through the technique's pixel-to-clip screen
//     constant (VS c2 = (2/W, -2/H, -1, 1)), read back and restored after the
//     draw. (The material's c0..c1 is the texture-coordinate transform.)
#pragma once

#include <cstdint>

#include "game/pg_primitive.hpp"

namespace mc2vr::world_markers {

// conf marker_size=<degrees> (default 3): a marker's height on the headset.
bool set_size(const char *value);

// ---- build side ----
void on_quad(uint32_t record, float x, float y, float half_height, float depth);
// PgPrimitive_PublishFrame return (hooks/hud.cpp).
void on_publish();

// ---- draw side (render thread) ----
void begin_frame();
// Around one record's draw: true when its transform was replaced (restore
// with after_draw).
bool before_draw(uint32_t record, const game::PgPrimitive *rec, const game::Technique *tech);
void after_draw(const game::PgPrimitive *rec, const game::Technique *tech);

void report_window();

} // namespace mc2vr::world_markers

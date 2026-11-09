// Camera-data census (mc2vr.conf debug_camera_census=on; docs/stereo.md):
// which vertex shaders in the main scene pass take the camera from something
// other than viewContextData — the only channel the per-eye rewrite
// (vr/view_rewrite) corrects. Such draws render from the mono camera.
//
// Per drawn record (left-eye pass, main scene pass only), from the
// PgPrimitive_SubmitToGPU reimplementation: the shader's constant table (CTAB,
// parsed once per shader from GetFunction) gives each constant's name and
// registers; the census records which uploads fill them (technique params,
// the record's own constants, command-stream op-2 uploads). The 10 s report
// logs every shader with camera-named constants other than viewContextData,
// once, with draw counts and each constant's source.
#pragma once

#include "game/pg_primitive.hpp"

namespace mc2vr::camera_census {

bool set_enabled(const char *value);

// mc2vr.conf debug_camera_census_skip=<constant name>: main-pass draws of
// vertex shaders with that constant are not drawn (both eyes) — to identify
// which draws an artifact comes from. Implies the census.
bool set_skip(const char *value);

// A record about to be drawn (render thread). Returns false when the draw is
// to be skipped (debug_camera_census_skip); always true otherwise.
bool on_draw(const game::PgPrimitive *rec, const game::Technique *tech);

// 10 s window report (render_dump's poller).
void report_window();

} // namespace mc2vr::camera_census

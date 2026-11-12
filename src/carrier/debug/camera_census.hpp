// Camera-data census (mc2vr.conf debug_camera_census=on; docs/view_rewrite.md
// "OPEN: per-eye camera producers"): which shaders in the main scene pass take
// camera data from something other than the vertex shader's viewContextData —
// the only channel the per-eye rewrite (vr/view_rewrite) corrects wholesale.
// Such draws render (or shade) from the mono camera.
//
// Per drawn record (left-eye pass, main scene pass only), from the
// PgPrimitive_SubmitToGPU reimplementation:
//   - Vertex shader: its constant table (CTAB, parsed once per shader) gives
//     each constant's name and registers; the census records which uploads
//     fill them (technique params, the record's own constants, command-stream
//     op-2 uploads).
//   - Pixel shader (the one bound at the draw): likewise, from the effect
//     pass's params and command-stream op-3 uploads; plus a device-level
//     SetPixelShaderConstantF shadow (hooked at device creation while the
//     census is on) that keeps each register's last writer — the device
//     call's return address and the next game-code address up the stack —
//     and its age in frames, so constants set outside SubmitToGPU (pass
//     binds, post chains) are attributable. The PS's viewContextData counts
//     as camera data: only the VS copy is rewritten.
// The 10 s report logs every shader with camera data, once, with draw counts,
// bytecode hash (tools/shader_hash.py) and each constant's sources.
#pragma once

#include <windows.h>
#include <d3d9.h> // types only

#include "game/pg_primitive.hpp"

namespace mc2vr::camera_census {

bool set_enabled(const char *value);
bool enabled();

// mc2vr.conf debug_camera_census_skip=<constant name>: main-pass draws whose
// vertex or pixel shader has that constant are not drawn (both eyes) — to
// identify which draws an artifact comes from. Implies the census.
bool set_skip(const char *value);

// A record about to be drawn (render thread), after its command stream ran.
// Returns false when the draw is to be skipped (debug_camera_census_skip);
// always true otherwise.
bool on_draw(const game::PgPrimitive *rec, const game::Technique *tech, const game::EffectPass *pass);

// Device SetPixelShaderConstantF tap (census_hooks; render thread). `ret` is
// the device call's return address, `stack` the caller's stack at the call.
void on_ps_constants(uint32_t start, uint32_t count, uintptr_t ret, const uint32_t *stack);

// 10 s window report (render_dump's poller).
void report_window();

} // namespace mc2vr::camera_census

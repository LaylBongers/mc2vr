// Pure math for the per-eye camera replacement (docs/camera.md): the
// four VP rows of a D3D view-projection decompose exactly into a camera pose,
// projection terms and depth terms, and can be rebuilt from replacements.
// No game or D3D dependencies — unit-tested natively by tools/test/test_vp_camera.cpp.
#pragma once

#include "vr/vec_math.hpp"

namespace mc2vr::vp_camera {

using math::Quat;
using math::Vec3;

// D3D clip = [a·x_v + c·z_v, b·y_v + d·z_v, A·z_v + B, z_v],
// x_v/y_v/z_v = dot(R/U/F, p − C). (R,U,F) orthonormal; F is forward.
struct Camera {
    Vec3 R, U, F, C;
    float a = 1, b = 1, c = 0, d = 0;  // projection: x/y scale and centre terms
    float A = 1, B = 0;                // depth terms (kept from the game)
};

// An OpenXR eye in its LOCAL reference space; fov = XrFovF ANGLES in radians.
struct EyePose {
    Vec3 pos;
    Quat rot;
    float fov_left = 0, fov_right = 0, fov_up = 0, fov_down = 0;
};

// Decompose raw VP rows (`rows` = 4×float4, row-major). False if the block is
// not of the assumed form (|row3.xyz| ≠ 1, depth row not ∥ forward, degenerate).
bool decompose(const float *rows, Camera *out);

// Write the 4 VP rows for `cam` into out[16].
void rebuild(const Camera &cam, float *out);

// The game camera `game` displaced by an HMD eye: `game` is the body, `eye` an
// offset on it (XR x→R, y→U, −z→F; position scaled by `units_per_metre`).
// Projection comes from the eye FOV; depth terms are kept from `game`.
Camera apply_eye(const Camera &game, const EyePose &eye, float units_per_metre);

// The world-space offset apply_eye adds to game.C for `eye` (computed from the
// eye's own position, not as a difference of two large world positions).
Vec3 eye_offset(const Camera &game, const EyePose &eye, float units_per_metre);

// The camera's matrices in row form (double): view V (view = V * (p, 1),
// view = (x_v, y_v, z_v, 1)), projection P (clip = P * view; P * V = the VP
// rows rebuild writes) and P^-1 in closed form from the projection terms.
void view_matrix(const Camera &cam, double out[16]);
void projection(const Camera &cam, double out[16]);
void inverse_projection(const Camera &cam, double out[16]);

// The view-space delta T with view_eye = T * view_mono for every point (row
// form, view = (x_v, y_v, z_v, 1) as in Camera): the eye camera's view of the
// mono camera's view space, a rigid transform with rotation M_eye * M_mono^T
// (M = rows R, U, F) and translation -M_eye * offset, where offset =
// eye.C - mono.C = eye_offset(mono, ...). Neither camera's world position
// enters: an identity eye gives T = I to the axes' float rounding. Double
// precision. Corrects view-space matrices: LocalToView_eye = T * LocalToView_mono.
void view_delta(const Camera &mono, const Camera &eye, const Vec3 &offset, double out[16]);

// The clip-space delta D with clip_eye = D * clip_mono for every point (row
// form: D * VP_mono = VP_eye), analytically: D = P_eye * T * P_mono^-1, T =
// view_delta(mono, eye, offset), P^-1 = inverse_projection(mono). Same precision properties as view_delta: an identity eye gives D = I.
void clip_delta(const Camera &mono, const Camera &eye, const Vec3 &offset, double out[16]);

// Largest relative |rebuild(decompose(rows)) − rows| entry (identity self-check).
float rebuild_residual(const float *rows, const Camera &decomposed);

} // namespace mc2vr::vp_camera

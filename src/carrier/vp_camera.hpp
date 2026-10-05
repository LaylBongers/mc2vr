// Pure math for the S4-4 camera replacement (docs/stereo_design.md §S4-4): the
// four VP rows of a D3D view-projection decompose exactly into a camera pose,
// projection terms and depth terms, and can be rebuilt from replacements.
// No game or D3D dependencies — unit-tested natively by tools/test/test_vp_camera.cpp.
#pragma once

#include "vec_math.hpp"

namespace mc2vr::vpcam {

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

// Largest relative |rebuild(decompose(rows)) − rows| entry (identity self-check).
float rebuild_residual(const float *rows, const Camera &decomposed);

} // namespace mc2vr::vpcam

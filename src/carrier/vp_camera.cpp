#include "vp_camera.hpp"

#include <cmath>

namespace mc2vr::vpcam {

using namespace math;

namespace {
// Row i's xyz / w inside the 4×float4 block.
Vec3 xyz(const float *rows, int i) { return load3(rows + i * 4); }
float w(const float *rows, int i) { return rows[i * 4 + 3]; }
} // namespace

bool decompose(const float *rows, Camera *out)
{
    const Vec3 r0 = xyz(rows, 0), r1 = xyz(rows, 1), r2 = xyz(rows, 2), r3 = xyz(rows, 3);
    Camera cam;

    // row3 = F, unit length (clip.w = z_v); anything else is an unknown form.
    const float fl = length(r3);
    if (fl < 0.98f || fl > 1.02f) {
        return false;
    }
    cam.F = r3 * (1.0f / fl);

    // row0 = aR + cF, row1 = bU + dF.
    cam.c = dot(r0, cam.F);
    cam.d = dot(r1, cam.F);
    const Vec3 t0 = reject(r0, cam.F), t1 = reject(r1, cam.F);
    cam.a = length(t0);
    cam.b = length(t1);
    if (!normalize(t0, &cam.R, 1e-6f) || !normalize(t1, &cam.U, 1e-6f)) {
        return false;
    }

    // row2 = A·F: a depth row that mixes in x/y is not the assumed form.
    cam.A = dot(r2, cam.F);
    if (length(reject(r2, cam.F)) > 1e-3f * (std::fabs(cam.A) + 1.0f)) {
        return false;
    }

    // Camera position: the point where clip.x = clip.y = clip.w = 0, i.e.
    // [r0;r1;r3]·C = −[w0;w1;w3] (3×3 Cramer).
    const float det = dot(r0, cross(r1, r3));
    if (std::fabs(det) < 1e-9f) {
        return false;
    }
    cam.C = (cross(r1, r3) * -w(rows, 0) + cross(r3, r0) * -w(rows, 1) +
             cross(r0, r1) * -w(rows, 3)) *
            (1.0f / det);
    cam.B = w(rows, 2) + dot(r2, cam.C);
    *out = cam;
    return true;
}

void rebuild(const Camera &cam, float *out)
{
    const Vec3 rows[4] = {cam.R * cam.a + cam.F * cam.c, cam.U * cam.b + cam.F * cam.d,
                          cam.F * cam.A, cam.F};
    for (int i = 0; i < 4; i++) {
        store3(out + i * 4, rows[i]);
        out[i * 4 + 3] = -dot(rows[i], cam.C);
    }
    out[11] += cam.B;
}

Camera apply_eye(const Camera &game, const EyePose &eye, float units_per_metre)
{
    // XR LOCAL (x right, y up, −z forward) → the game camera's basis.
    auto to_game = [&](Vec3 v) { return game.R * v.x + game.U * v.y + game.F * -v.z; };

    Camera out;
    out.C = game.C + to_game(eye.pos) * units_per_metre;
    out.R = to_game(rotate(eye.rot, {1, 0, 0}));
    out.U = to_game(rotate(eye.rot, {0, 1, 0}));
    out.F = to_game(rotate(eye.rot, {0, 0, -1}));

    // Off-centre projection from the half-angle tangents.
    const float tl = std::tan(eye.fov_left), tr = std::tan(eye.fov_right);
    const float tu = std::tan(eye.fov_up), td = std::tan(eye.fov_down);
    out.a = 2.0f / (tr - tl);
    out.c = -(tr + tl) / (tr - tl);
    out.b = 2.0f / (tu - td);
    out.d = -(tu + td) / (tu - td);

    out.A = game.A;
    out.B = game.B;
    return out;
}

float rebuild_residual(const float *rows, const Camera &decomposed)
{
    float re[16], worst = 0.0f;
    rebuild(decomposed, re);
    for (int i = 0; i < 16; i++) {
        const float e = std::fabs(re[i] - rows[i]) / (1.0f + std::fabs(rows[i]));
        if (e > worst) worst = e;
    }
    return worst;
}

} // namespace mc2vr::vpcam

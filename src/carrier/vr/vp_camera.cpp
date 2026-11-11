#include "vr/vp_camera.hpp"

#include <cmath>

namespace mc2vr::vp_camera {

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

// XR LOCAL (x right, y up, −z forward) → the game camera's basis.
static Vec3 to_game(const Camera &game, Vec3 v)
{
    return game.R * v.x + game.U * v.y + game.F * -v.z;
}

Vec3 eye_offset(const Camera &game, const EyePose &eye, float units_per_metre)
{
    return to_game(game, eye.pos) * units_per_metre;
}

void clip_delta(const Camera &mono, const Camera &eye, const Vec3 &offset, double out[16])
{
    const Vec3 me[3] = {eye.R, eye.U, eye.F}, mm[3] = {mono.R, mono.U, mono.F};
    // T: mono view space -> eye view space.
    double t[16] = {};
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            t[i * 4 + j] = (double)me[i].x * mm[j].x + (double)me[i].y * mm[j].y + (double)me[i].z * mm[j].z;
        }
        t[i * 4 + 3] = -((double)me[i].x * offset.x + (double)me[i].y * offset.y + (double)me[i].z * offset.z);
    }
    t[15] = 1.0;
    // P_eye (row form: clip = P * view).
    double pe[16] = {};
    pe[0] = eye.a;
    pe[2] = eye.c;
    pe[5] = eye.b;
    pe[6] = eye.d;
    pe[10] = eye.A;
    pe[11] = eye.B;
    pe[14] = 1.0;
    // P_mono^-1: x_v = (x - c*w)/a, y_v = (y - d*w)/b, z_v = w, 1 = (z - A*w)/B.
    double pm[16] = {};
    pm[0] = 1.0 / mono.a;
    pm[3] = -(double)mono.c / mono.a;
    pm[5] = 1.0 / mono.b;
    pm[7] = -(double)mono.d / mono.b;
    pm[11] = 1.0;
    pm[14] = 1.0 / mono.B;
    pm[15] = -(double)mono.A / mono.B;
    double pt[16];
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            double s = 0.0;
            for (int k = 0; k < 4; k++) s += pe[r * 4 + k] * t[k * 4 + c];
            pt[r * 4 + c] = s;
        }
    }
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            double s = 0.0;
            for (int k = 0; k < 4; k++) s += pt[r * 4 + k] * pm[k * 4 + c];
            out[r * 4 + c] = s;
        }
    }
}

Camera apply_eye(const Camera &game, const EyePose &eye, float units_per_metre)
{
    auto to_game = [&](Vec3 v) { return vp_camera::to_game(game, v); };

    Camera out;
    out.C = game.C + eye_offset(game, eye, units_per_metre);
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

} // namespace mc2vr::vp_camera

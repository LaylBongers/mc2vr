// Native unit test for src/carrier/vr/vp_camera.cpp (pure math, no game deps).
// Build + run (one line):
//   g++ -std=c++17 -I src/carrier tools/test/test_vp_camera.cpp src/carrier/vr/vp_camera.cpp -o /tmp/test_vp_camera && /tmp/test_vp_camera
#include <cmath>
#include <cstdio>

#include "vr/vp_camera.hpp"

using namespace mc2vr::math;
using namespace mc2vr::vp_camera;

static int g_fail = 0;
#define CHECK(cond, ...)                                  \
    do {                                                  \
        if (!(cond)) {                                    \
            g_fail++;                                     \
            std::printf("FAIL %s: ", #cond);              \
            std::printf(__VA_ARGS__);                     \
            std::printf("\n");                            \
        }                                                 \
    } while (0)

// Project world point p through VP rows -> ndc (x, y) and clip w.
static void project(const float *vp, Vec3 p, float *ndc_x, float *ndc_y, float *w)
{
    double c[4];
    transform_point4(vp, p, c);
    *w = (float)c[3];
    *ndc_x = (float)(c[0] / c[3]);
    *ndc_y = (float)(c[1] / c[3]);
}

static Camera make_game_camera()
{
    Camera g;
    // Yawed LH-style basis: R, U=(0,1,0), F=R x U.
    const float yaw = 0.7f;
    g.R = {std::cos(yaw), 0, -std::sin(yaw)};
    g.U = {0, 1, 0};
    g.F = {std::sin(yaw), 0, std::cos(yaw)};
    g.C = {120.f, 3.f, -45.f};
    g.a = 1.7f; g.b = 3.0f; g.c = 0.02f; g.d = -0.01f;
    g.A = 1.0003f; g.B = -0.1f;
    return g;
}

int main()
{
    const Camera game = make_game_camera();
    float rows[16];
    rebuild(game, rows);

    // 1. decompose(rebuild(x)) == x, and rebuild(decompose) reproduces rows.
    Camera d;
    CHECK(decompose(rows, &d), "decompose failed");
    CHECK(distance(d.C, game.C) < 1e-3f, "C off by %g", distance(d.C, game.C));
    CHECK(std::fabs(d.a - game.a) < 1e-5f && std::fabs(d.b - game.b) < 1e-5f, "a/b");
    CHECK(std::fabs(d.A - game.A) < 1e-5f && std::fabs(d.B - game.B) < 1e-3f, "A/B");
    CHECK(rebuild_residual(rows, d) < 1e-5f, "residual %g", rebuild_residual(rows, d));

    // 2. Rejects non-D3D forms (non-unit row3).
    float bad[16];
    for (int i = 0; i < 16; i++) bad[i] = rows[i];
    bad[12] *= 2; bad[13] *= 2; bad[14] *= 2;
    Camera tmp;
    CHECK(!decompose(bad, &tmp), "non-unit row3 accepted");

    // 3. Identity eye with the game's own FOV is a no-op on the picture:
    //    pose = origin, rot = identity, FOV tangents chosen to reproduce a,b,c,d.
    const float tl = -(1.0f + game.c) / game.a, tr = (1.0f - game.c) / game.a;
    const float td = -(1.0f + game.d) / game.b, tu = (1.0f - game.d) / game.b;
    EyePose id{{0, 0, 0}, {0, 0, 0, 1}, std::atan(tl), std::atan(tr), std::atan(tu), std::atan(td)};
    const Camera same = apply_eye(game, id, 1.0f);
    CHECK(std::fabs(same.a - game.a) < 1e-4f && std::fabs(same.b - game.b) < 1e-4f, "fov a/b");
    CHECK(std::fabs(same.c - game.c) < 1e-4f && std::fabs(same.d - game.d) < 1e-4f, "fov c/d");
    CHECK(distance(same.C, game.C) < 1e-4f, "identity eye moved the camera");
    CHECK(distance(same.F, game.F) < 1e-5f && distance(same.R, game.R) < 1e-5f, "basis");

    // 4. A point straight ahead of the game camera lands at ndc ~ (c, d) for the
    //    game's own projection; a 0.5 m eye shift right moves it LEFT on screen,
    //    and a yaw to the right (head turn right, XR: rotate about -y) moves it left too.
    const Vec3 ahead = game.C + game.F * 10.0f;
    float nx, ny, w;
    float vp[16];
    rebuild(game, vp);
    project(vp, ahead, &nx, &ny, &w);
    CHECK(std::fabs(nx - game.c) < 1e-4f && std::fabs(ny - game.d) < 1e-4f && std::fabs(w - 10.f) < 1e-3f,
          "ahead projects to (%g,%g,w=%g)", nx, ny, w);

    EyePose shift = id;
    shift.pos = {0.5f, 0, 0};
    rebuild(apply_eye(game, shift, 1.0f), vp);
    float sx, sy, sw;
    project(vp, ahead, &sx, &sy, &sw);
    CHECK(sx < nx, "eye shifted right but point did not move left (%g vs %g)", sx, nx);

    EyePose yaw = id;
    const float half = -0.2f;  // rotation about +y by a NEGATIVE angle = turn right (RH)
    yaw.rot = {0, std::sin(half), 0, std::cos(half)};
    rebuild(apply_eye(game, yaw, 1.0f), vp);
    project(vp, ahead, &sx, &sy, &sw);
    CHECK(sx < nx, "head turned right but point did not move left (%g vs %g)", sx, nx);

    // 5. Scale applies to translation only.
    const Camera big = apply_eye(game, shift, 10.0f);
    CHECK(std::fabs(distance(big.C, game.C) - 5.0f) < 1e-3f, "scale: %g", distance(big.C, game.C));

    // 6. Quaternion rotate sanity: 90 deg about +y takes +x to -z (RH).
    const float s45 = std::sqrt(0.5f);
    const Vec3 r = rotate({0, s45, 0, s45}, {1, 0, 0});
    CHECK(distance(r, {0, 0, -1}) < 1e-5f, "rotate: %g %g %g", r.x, r.y, r.z);

    // 7. Clip delta (clip_delta): D * clip_mono(p) = clip_eye(p) for
    //    any point, without inverting a VP; an identity eye gives D = I exactly,
    //    even with the camera far from the origin.
    {
        EyePose eye = id;
        eye.pos = {0.032f, -0.01f, 0.004f};
        const float yh = -0.05f, ph = 0.03f;  // a small turn and nod
        const Quat qy{0, std::sin(yh), 0, std::cos(yh)}, qx{std::sin(ph), 0, 0, std::cos(ph)};
        eye.rot = qy * qx;
        eye.fov_left = -0.95f; eye.fov_right = 0.80f; eye.fov_up = 0.85f; eye.fov_down = -0.92f;
        const Camera cam = apply_eye(game, eye, 1.0f);
        double D[16];
        clip_delta(game, cam, eye_offset(game, eye, 1.0f), D);
        float vp_mono[16], vp_eye[16];
        rebuild(game, vp_mono);
        rebuild(cam, vp_eye);
        const Vec3 pts[3] = {game.C + game.F * 10.0f + game.R * 2.0f, game.C + game.F * 300.0f - game.U * 40.0f,
                             game.C + game.F * 1.5f + game.R * -0.4f + game.U * 0.3f};
        double worst = 0;
        for (const Vec3 &p : pts) {
            double cm[4], ce[4], d[4];
            transform_point4(vp_mono, p, cm);
            transform_point4(vp_eye, p, ce);
            transform4(D, cm, d);
            worst = std::fmax(worst, max_relative_diff(d, ce, 4));
        }
        CHECK(worst < 1e-4, "clip_delta: D * clip_mono differs from clip_eye by %g (relative)", worst);

        // Identity eye: D = I up to the float axes' own rounding (~1e-8), the
        // same near the origin and ~3 km away (the inverse-VP route degrades
        // with the camera's distance: its translations are that large).
        auto identity_error = [&](Vec3 c) {
            Camera g2 = game;
            g2.C = c;
            double I[16], ident[16];
            clip_delta(g2, apply_eye(g2, id, 1.0f), eye_offset(g2, id, 1.0f), I);
            identity4(ident);
            return max_abs_diff(I, ident, 16);
        };
        const double near_err = identity_error({1.f, 2.f, 3.f}), far_err = identity_error({1500.f, 80.f, -2650.f});
        CHECK(near_err < 1e-6 && far_err < 1e-6 && far_err <= near_err * 1.01 + 1e-12,
              "clip_delta identity error near %g far %g (must not grow with distance)", near_err, far_err);
    }

    // 8. View-space delta (view_delta): T * view_mono(p) = view_eye(p) for any
    //    point; the corrected LocalToView = T * (V_mono * W) maps a local
    //    point where V_eye maps W * q (the row-form order view_rewrite uses),
    //    also in a sign-flipped view frame (S * T * S, the game's view space is
    //    x-negated); clip_delta = P_eye * T * P_mono^-1; an identity eye gives T = I.
    {
        EyePose eye = id;
        eye.pos = {-0.032f, 0.012f, -0.006f};
        const float yh = 0.08f, ph = -0.04f, rh = 0.02f;  // turn, nod, roll
        eye.rot = Quat{0, std::sin(yh), 0, std::cos(yh)} * Quat{std::sin(ph), 0, 0, std::cos(ph)} *
                  Quat{0, 0, std::sin(rh), std::cos(rh)};
        eye.fov_left = -0.95f; eye.fov_right = 0.80f; eye.fov_up = 0.85f; eye.fov_down = -0.92f;
        Camera far_game = game;
        far_game.C = {1500.f, 80.f, -2650.f};  // world translations of the size the game has
        const Camera cam = apply_eye(far_game, eye, 1.0f);
        double T[16], v_mono[16], v_eye[16];
        view_delta(far_game, cam, eye_offset(far_game, eye, 1.0f), T);
        view_matrix(far_game, v_mono);
        view_matrix(cam, v_eye);
        const Vec3 pts[3] = {far_game.C + far_game.F * 10.0f + far_game.R * 2.0f,
                             far_game.C + far_game.F * 300.0f - far_game.U * 40.0f,
                             far_game.C + far_game.F * 0.8f + far_game.R * -0.4f + far_game.U * 0.3f};
        double worst = 0;
        for (const Vec3 &p : pts) {
            double vm[4], ve[4], t[4];
            transform_point4(v_mono, p, vm);
            transform_point4(v_eye, p, ve);
            transform4(T, vm, t);
            // Absolute in view units: the inputs are float world points ~3 km out.
            worst = std::fmax(worst, max_abs_diff(t, ve, 4));
        }
        CHECK(worst < 2e-3, "view_delta: T * view_mono differs from view_eye by %g", worst);

        // LocalToView: an object 6 units ahead (local origin), rotated + scaled,
        // handed over in float as the game does, in our view frame and in the
        // game's x-negated one.
        const Vec3 o = far_game.C + far_game.F * 6.0f + far_game.R * 1.5f;
        const double W[16] = {1.6, -1.2, 0, o.x, 1.2, 1.6, 0, o.y, 0, 0, 2.0, o.z, 0, 0, 0, 1};
        const double signs[2][4] = {{1, 1, 1, 1}, {-1, 1, 1, 1}};
        for (const auto &sg : signs) {
            double vw[16], ts[16], ve_w[16];
            float l2v_ours[16], l2v_mono[16], l2v_eye[16];
            mul4(v_mono, W, vw);
            to_float4(vw, l2v_ours);
            scale_rows4(sg, l2v_ours, l2v_mono);  // S * V_mono * W
            diag_conjugate4(T, sg, ts);           // S * T * S
            mul4(ts, l2v_mono, l2v_eye);
            mul4(v_eye, W, ve_w);
            double ref[16];
            scale_rows4(sg, ve_w, ref);  // S * V_eye * W
            const double worst_l = max_abs_diff(l2v_eye, ref, 16);
            CHECK(worst_l < 2e-3, "corrected LocalToView (x sign %g) off by %g view units", sg[0], worst_l);
        }

        // clip_delta is P_eye * T * P_mono^-1: D * P_mono = P_eye * T.
        double D[16], pm[16], pe[16], dp[16], pt[16];
        clip_delta(far_game, cam, eye_offset(far_game, eye, 1.0f), D);
        projection(far_game, pm);
        projection(cam, pe);
        mul4(D, pm, dp);
        mul4(pe, T, pt);
        const double off = max_abs_diff(dp, pt, 16);
        CHECK(off < 1e-9, "clip_delta != P_eye * view_delta * P_mono^-1 (%g)", off);

        // P * V reproduces the VP rows rebuild writes; P^-1 * P = I.
        double pv[16], pinv[16], pp[16], ident[16];
        float vp[16];
        mul4(pm, v_mono, pv);
        rebuild(far_game, vp);
        CHECK(max_relative_diff(vp, pv, 16) < 1e-6, "projection * view_matrix != rebuild");
        inverse_projection(far_game, pinv);
        mul4(pinv, pm, pp);
        identity4(ident);
        CHECK(max_abs_diff(pp, ident, 16) < 1e-9, "inverse_projection * projection != I");

        // rigid_inverse4(T) * T = I (T is rigid: the ViewToLocal correction).
        double t_inv[16], tt[16];
        rigid_inverse4(T, t_inv);
        mul4(t_inv, T, tt);
        CHECK(max_abs_diff(tt, ident, 16) < 1e-6, "rigid_inverse4(T) * T != I (%g)", max_abs_diff(tt, ident, 16));

        // ViewToLocal (the inverse of LocalToView, applied in the PS to view-space
        // points): ViewToLocal_eye = ViewToLocal_mono * (S T S)^-1 maps the eye-view
        // point of a local point q back to q, in the game's x-negated frame.
        {
            const double sg[4] = {-1, 1, 1, 1};
            double vw[16], l2v_mono[16], v2l_mono[16], ts[16], ts_inv[16], v2l_eye[16], l2v_eye[16], back[16];
            mul4(v_mono, W, vw);
            scale_rows4(sg, vw, l2v_mono);
            CHECK(invert4(l2v_mono, v2l_mono), "LocalToView not invertible");
            diag_conjugate4(T, sg, ts);
            rigid_inverse4(ts, ts_inv);
            mul4(v2l_mono, ts_inv, v2l_eye);
            mul4(ts, l2v_mono, l2v_eye);
            mul4(v2l_eye, l2v_eye, back);
            CHECK(max_abs_diff(back, ident, 16) < 1e-6, "ViewToLocal_eye * LocalToView_eye != I (%g)",
                  max_abs_diff(back, ident, 16));
        }

        // Identity eye: T = I to the axes' rounding, ~3 km from the origin.
        double I[16];
        view_delta(far_game, apply_eye(far_game, id, 1.0f), eye_offset(far_game, id, 1.0f), I);
        const double ioff = max_abs_diff(I, ident, 16);
        CHECK(ioff < 1e-6, "view_delta identity error %g", ioff);
    }

    std::printf(g_fail ? "FAILED (%d)\n" : "ALL PASS\n", g_fail);
    return g_fail ? 1 : 0;
}

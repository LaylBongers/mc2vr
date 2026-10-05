// Native unit test for src/carrier/vp_camera.cpp (pure math, no game deps).
// Build + run (one line):
//   g++ -std=c++17 -I src/common -I src/carrier tools/test/test_vp_camera.cpp src/carrier/vp_camera.cpp -o /tmp/test_vp_camera && /tmp/test_vp_camera
#include <cmath>
#include <cstdio>

#include "vp_camera.hpp"

using namespace mc2vr::math;
using namespace mc2vr::vpcam;

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
    float c[4];
    for (int i = 0; i < 4; i++) {
        c[i] = dot(load3(vp + i * 4), p) + vp[i * 4 + 3];
    }
    *w = c[3];
    *ndc_x = c[0] / c[3];
    *ndc_y = c[1] / c[3];
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

    std::printf(g_fail ? "FAILED (%d)\n" : "ALL PASS\n", g_fail);
    return g_fail ? 1 : 0;
}

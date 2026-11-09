// World markers placed per eye — see world_markers.hpp.

#include "vr/world_markers.hpp"

#include <windows.h>
#include <d3d9.h>

#include <cmath>
#include <cstdlib>
#include <cstring>

#include "game/pg_primitive.hpp"
#include "log.hpp"
#include "vr/eye_replay.hpp"
#include "vr/render_outputs.hpp"
#include "vr/view_rewrite.hpp"

namespace mc2vr::world_markers {

namespace {

float g_size_deg = 3.0f;

struct Quad {
    uint32_t record;
    float ax, ay;    // the marker's anchor (its first quad's centre), pixels at the render size
    float half_h;    // the marker's first quad's half height, pixels
    float depth;     // NDC z/w of the single-camera projection
};
constexpr uint32_t kMaxQuads = 1024;

// Build thread.
Quad g_building[kMaxQuads];
uint32_t g_building_n = 0;
bool g_group_open = false;
Quad g_group = {};

// Published with the record list.
SRWLOCK g_lock = SRWLOCK_INIT;
Quad g_published[kMaxQuads];
uint32_t g_published_n = 0;
const void *g_published_base = nullptr;

// Render thread, this frame.
Quad g_frame[kMaxQuads];
uint32_t g_frame_n = 0;
bool g_frame_valid = false;
float g_w = 0.0f, g_h = 0.0f;

// The screen constant replaced for the current draw (restored after it).
float g_saved[4];

// Window counters.
uint64_t g_placed = 0, g_no_delta = 0, g_dropped = 0, g_not_pixel_map = 0;

const Quad *find(uint32_t record)
{
    uint32_t lo = 0, hi = g_frame_n;  // g_frame is in record order
    while (lo < hi) {
        const uint32_t mid = (lo + hi) / 2;
        if (g_frame[mid].record < record) lo = mid + 1;
        else hi = mid;
    }
    return lo < g_frame_n && g_frame[lo].record == record ? &g_frame[lo] : nullptr;
}

} // namespace

bool set_size(const char *value)
{
    char *end = nullptr;
    const float v = std::strtof(value, &end);
    if (end == value || *end != '\0' || !(v >= 0.5f && v <= 30.0f)) {
        return false;
    }
    g_size_deg = v;
    MC2VR_LOG("markers: marker_size=%.1f deg", g_size_deg);
    return true;
}

void on_quad(uint32_t record, float x, float y, float half_height, float depth)
{
    if (depth == 0.0f) {  // pinned to the screen edge: not a world position
        g_group_open = false;
        return;
    }
    // A batch the renderer dropped (overflow) wrote no record; the next quad
    // then reports the same record and replaces this one.
    if (g_building_n != 0 && g_building[g_building_n - 1].record == record) {
        g_building_n--;
    }
    if (g_building_n >= kMaxQuads) {
        g_dropped++;
        return;
    }
    // The same marker's next quad: consecutive record, same depth.
    if (!(g_group_open && depth == g_group.depth && record == g_group.record + 1)) {
        g_group = {record, x, y, half_height, depth};
        g_group_open = true;
    }
    g_group.record = record;
    g_building[g_building_n++] = g_group;
}

void on_publish()
{
    AcquireSRWLockExclusive(&g_lock);
    std::memcpy(g_published, g_building, g_building_n * sizeof(Quad));
    g_published_n = g_building_n;
    g_published_base = game::g_PrimitiveBase::get();
    ReleaseSRWLockExclusive(&g_lock);
    g_building_n = 0;
    g_group_open = false;
}

void begin_frame()
{
    AcquireSRWLockShared(&g_lock);
    std::memcpy(g_frame, g_published, g_published_n * sizeof(Quad));
    g_frame_n = g_published_n;
    const void *const base = g_published_base;
    ReleaseSRWLockShared(&g_lock);
    D3DSURFACE_DESC d = {};
    IDirect3DSurface9 *const front = render_outputs::front();
    g_frame_valid = g_frame_n != 0 && base == game::g_PrimitiveBase::get() && view_rewrite::current_pose_id() != 0 &&
                    front != nullptr && SUCCEEDED(front->GetDesc(&d));
    g_w = (float)d.Width;
    g_h = (float)d.Height;
}

bool before_draw(uint32_t record, const game::PgPrimitive *rec, const game::Technique *tech)
{
    if (!g_frame_valid || tech == nullptr || tech->screenConsts.count == 0) {
        return false;
    }
    const Quad *const q = find(record);
    const uint32_t pass = eye_replay::current_pass();
    vp_camera::EyePose eyes[2];
    if (q == nullptr || (pass != 1 && pass != 2) || !view_rewrite::frame_eyes(eyes) || q->half_h <= 0.0f) {
        return false;
    }

    // The anchor through the scene's mono -> eye clip map.
    const double in[4] = {2.0 * q->ax / g_w - 1.0, 1.0 - 2.0 * q->ay / g_h, q->depth, 1.0};
    double out[4];
    if (!view_rewrite::mono_clip_to_eye(in, out) || out[3] <= 1e-9) {
        g_no_delta++;
        return false;
    }
    const float ex = (float)((out[0] / out[3] + 1.0) * 0.5 * g_w);
    const float ey = (float)((1.0 - out[1] / out[3]) * 0.5 * g_h);

    // Scale: marker_size degrees tall on this eye's image.
    const vp_camera::EyePose &eye = eyes[pass - 1];
    const float px_per_tan = g_h / (std::tan(eye.fov_up) - std::tan(eye.fov_down));
    const float want_half_h = std::tan(0.5f * g_size_deg * 3.14159265f / 180.0f) * px_per_tan;
    const float k = std::fmin(1.0f, want_half_h / q->half_h);

    // The screen constant maps pixels to clip space: clip = p * (sx, sy) +
    // (ox, oy) = (2/W, -2/H, -1, 1). Scaling around the anchor and moving it to
    // the eye's anchor folds into it: p' = k (p - a) + a + d.
    IDirect3DDevice9 *const dev = render_outputs::device();
    const UINT reg = tech->screenConsts.start_reg;
    if (FAILED(dev->GetVertexShaderConstantF(reg, g_saved, 1)) || std::fabs(g_saved[0] - 2.0f / g_w) > 1e-5f ||
        std::fabs(g_saved[1] + 2.0f / g_h) > 1e-5f) {
        g_not_pixel_map++;
        return false;
    }
    const float sx = g_saved[0], sy = g_saved[1];
    const float c[4] = {k * sx, k * sy, g_saved[2] + sx * (q->ax * (1.0f - k) + (ex - q->ax)),
                        g_saved[3] + sy * (q->ay * (1.0f - k) + (ey - q->ay))};
    dev->SetVertexShaderConstantF(reg, c, 1);
    g_placed++;
    return true;
}

void after_draw(const game::PgPrimitive *rec, const game::Technique *tech)
{
    (void)rec;
    render_outputs::device()->SetVertexShaderConstantF(tech->screenConsts.start_reg, g_saved, 1);
}

void report_window()
{
    MC2VR_LOG("markers: placed=%llu no-delta=%llu not-pixel-map=%llu dropped=%llu (size %.1f deg)",
              (unsigned long long)g_placed, (unsigned long long)g_no_delta, (unsigned long long)g_not_pixel_map,
              (unsigned long long)g_dropped, g_size_deg);
    g_placed = g_no_delta = g_dropped = g_not_pixel_map = 0;
}

} // namespace mc2vr::world_markers

// First-person camera — see first_person.hpp.

#include "vr/first_person.hpp"

#include <cmath>
#include <cstdlib>
#include <cstring>

#include "game/camera.hpp"
#include "log.hpp"

namespace mc2vr::first_person {

namespace {

bool g_enabled = false;
float g_eye_height = 1.65f;
float g_eye_forward = 0.15f;

// Window stats (update thread writes, poller reads: approximate is fine).
uint64_t g_frames = 0, g_no_pose = 0;
float g_last_char[3] = {}, g_last_stock[3] = {}, g_last_eye[3] = {};
bool g_logged_first = false;

bool parse_metres(const char *value, float lo, float hi, float *out)
{
    char *end = nullptr;
    const float v = std::strtof(value, &end);
    if (end == value || *end != '\0' || !(v >= lo && v <= hi)) {
        return false;
    }
    *out = v;
    return true;
}

} // namespace

bool set_enabled(const char *value)
{
    if (strcmp(value, "on") == 0) g_enabled = true;
    else if (strcmp(value, "off") == 0) g_enabled = false;
    else return false;
    MC2VR_LOG("fp: first_person=%s", value);
    return true;
}

bool set_eye_height(const char *value)
{
    if (!parse_metres(value, 0.5f, 2.5f, &g_eye_height)) return false;
    MC2VR_LOG("fp: first_person_eye_height=%.2f m", g_eye_height);
    return true;
}

bool set_eye_forward(const char *value)
{
    if (!parse_metres(value, -0.5f, 1.0f, &g_eye_forward)) return false;
    MC2VR_LOG("fp: first_person_eye_forward=%.2f m", g_eye_forward);
    return true;
}

bool enabled() { return g_enabled; }

void on_human_camera_matrix(uint8_t *camera, float *matrix)
{
    if (!g_enabled) return;
    uint32_t handle;
    memcpy(&handle, camera + game::cam_human::kTargetHandle, 4);
    game::Pose pose = {};
    if (handle == 0 || !game::PoseStore_GetPoseByHandle(handle, &pose)) {
        g_no_pose++;
        return;  // no character: the stock camera stands
    }

    // Row 2 = the camera's forward axis (docs/camera.md). Horizontal part only:
    // looking down must not push the eye forward over the feet.
    float fx = matrix[8], fz = matrix[10];
    const float flen = std::sqrt(fx * fx + fz * fz);
    if (flen > 1e-4f) {
        fx /= flen;
        fz /= flen;
    } else {
        fx = fz = 0.0f;
    }
    const float eye[3] = {pose.pos[0] + fx * g_eye_forward, pose.pos[1] + g_eye_height,
                          pose.pos[2] + fz * g_eye_forward};

    memcpy(g_last_char, pose.pos, sizeof(g_last_char));
    memcpy(g_last_stock, matrix + 12, sizeof(g_last_stock));
    memcpy(g_last_eye, eye, sizeof(g_last_eye));
    matrix[12] = eye[0];
    matrix[13] = eye[1];
    matrix[14] = eye[2];

    // No obstruction pull-in (Cam_Update slot 15: it moves the working position
    // toward the target when the boom is blocked). The eye has no boom.
    camera[game::cam_human::kObstruction] = 0;


    if (!g_logged_first) {
        g_logged_first = true;
        MC2VR_LOG("fp: first frame camera=%p character (%.2f, %.2f, %.2f) stock camera (%.2f, %.2f, %.2f) -> eye "
                  "(%.2f, %.2f, %.2f)",
                  (void *)camera, pose.pos[0], pose.pos[1], pose.pos[2], g_last_stock[0], g_last_stock[1],
                  g_last_stock[2], eye[0], eye[1], eye[2]);
    }
    g_frames++;
}

void report_window()
{
    if (!g_enabled) return;
    const float dx = g_last_stock[0] - g_last_char[0], dy = g_last_stock[1] - g_last_char[1],
                dz = g_last_stock[2] - g_last_char[2];
    MC2VR_LOG("fp: window frames=%llu noPose=%llu | last character (%.2f, %.2f, %.2f) eye (%.2f, %.2f, %.2f) | stock "
              "camera was %.2f m from the character (%.2f up)",
              (unsigned long long)g_frames, (unsigned long long)g_no_pose, g_last_char[0], g_last_char[1],
              g_last_char[2], g_last_eye[0], g_last_eye[1], g_last_eye[2], std::sqrt(dx * dx + dy * dy + dz * dz),
              dy);
    g_frames = g_no_pose = 0;
}

} // namespace mc2vr::first_person

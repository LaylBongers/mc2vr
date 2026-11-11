// First-person camera — see first_person.hpp.

#include "vr/first_person.hpp"

#include <cmath>
#include <cstdlib>
#include <cstring>

#include "game/animation.hpp"
#include "game/camera.hpp"
#include "hooks/frame_tick.hpp"
#include "log.hpp"
#include "vr/head_hide.hpp"

namespace mc2vr::first_person {

namespace {

bool g_enabled = false;
float g_eye_height = 1.65f;
float g_eye_forward = 0.15f;
bool g_hide_head = false;  // the scale edit doesn't survive publication (docs/first_person.md)

// The character the first-person camera followed, and when (update thread).
uint32_t g_target = 0;
uint64_t g_target_frame = 0;
constexpr float kHiddenScale = 0.001f;  // not 0: keeps the skinning matrices invertible
uint64_t g_poses = 0, g_heads = 0, g_no_head = 0;
int32_t g_head_index = -1;

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

bool set_hide_head(const char *value)
{
    if (strcmp(value, "on") == 0) g_hide_head = true;
    else if (strcmp(value, "off") == 0) g_hide_head = false;
    else return false;
    MC2VR_LOG("fp: first_person_hide_head=%s", value);
    return true;
}

bool enabled() { return g_enabled; }

void on_head_pose(uint32_t character, const uint8_t *name_map, uint8_t *pose)
{
    g_poses++;
    // Only while first person is live on this character (the camera ran this
    // or last frame); in vehicles and cutscenes the head shows.
    if (!g_enabled || !g_hide_head || character != g_target || hooks::frame_count() - g_target_frame > 2 ||
        name_map == nullptr || pose == nullptr) {
        return;
    }
    using namespace game;
    const uint8_t *skel = *(uint8_t *const *)(pose + hka_pose::kSkeleton);
    if (skel == nullptr) return;
    const int32_t bones = *(const int32_t *)(skel + hka_skeleton::kBoneCount);
    const int16_t *parents = *(int16_t *const *)(skel + hka_skeleton::kParents);
    auto *local = *(QsTransform *const *)(pose + hka_pose::kLocal);
    auto *flags = *(uint32_t *const *)(pose + hka_pose::kFlags);
    if (bones <= 0 || bones > 512 || parents == nullptr || local == nullptr || flags == nullptr) return;

    const uint32_t *keys = *(uint32_t *const *)(name_map + bone_name_map::kKeys);
    const int32_t *values = *(int32_t *const *)(name_map + bone_name_map::kValues);
    const uint32_t cap = *(const uint32_t *)(name_map + bone_name_map::kCapacity);
    if (keys == nullptr || values == nullptr || cap == 0) return;
    const int32_t slot = HashTable_FindSlot(g_BoneHash_Head::get(), keys, cap);
    const int32_t head = slot >= 0 ? values[slot] : *(const int32_t *)(name_map + bone_name_map::kDefault);
    if (head < 0 || head >= bones) {
        g_no_head++;
        return;
    }
    g_head_index = head;

    // The head's subtree (parents precede children): shrink the head, mark
    // the subtree's model space dirty so syncModelSpace rebuilds it.
    local[head].s[0] = local[head].s[1] = local[head].s[2] = kHiddenScale;
    static bool in_subtree[512];
    memset(in_subtree, 0, (size_t)bones);
    in_subtree[head] = true;
    flags[head] |= hka_pose::kFlagModelDirty;
    for (int32_t i = head + 1; i < bones; i++) {
        const int16_t p = parents[i];
        if (p >= 0 && p < bones && in_subtree[p]) {
            in_subtree[i] = true;
            flags[i] |= hka_pose::kFlagModelDirty;
        }
    }
    pose[hka_pose::kModelInSync] = 0;
    g_heads++;
}

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

    g_target = handle;
    g_target_frame = hooks::frame_count();
    if (g_hide_head) head_hide::attach(handle);

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
    MC2VR_LOG("fp:   head: controllers attached=%u failed=%u | OnPose=%llu head hidden=%llu (bone %d) no head bone=%llu",
              head_hide::attached_count(), head_hide::failed_count(), (unsigned long long)g_poses,
              (unsigned long long)g_heads, g_head_index, (unsigned long long)g_no_head);
    g_frames = g_no_pose = 0;
    g_poses = g_heads = g_no_head = 0;
}

} // namespace mc2vr::first_person

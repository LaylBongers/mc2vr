// Gameplay cameras and the pose store (docs/reverse_engineering/camera_classes.md,
// docs/reverse_engineering/character_pose.md): typed data and the original
// functions the carrier calls.
#pragma once

#include <cstdint>

#include "game/function.hpp"

namespace mc2vr::game {

// A pose as the pose store hands it out (and the camera objects keep it):
// position, a change serial, rotation quaternion. Pose_Copy semantics: the
// serial becomes max(source, destination) + 1.
struct Pose {
    float pos[3];
    uint32_t serial;
    float quat[4];  // x, y, z, w
};
static_assert(sizeof(Pose) == 0x20);

// The pose of the object behind a handle (EDI) into out (ESI); AL = found.
// 427 call sites; reads only (the record's serial is not bumped).
inline constexpr Function<bool(uint32_t handle, Pose *out)> PoseStore_GetPoseByHandle{
    "PoseStore_GetPoseByHandle", 0x00665a60u, {EDI, ESI}};

// The on-foot camera (class type name "human", vtable CamHuman_vtbl). Fields
// the carrier touches; layout in Ghidra (plates on CamHuman_Update, Cam_Update).
namespace cam_human {
inline constexpr uintptr_t kVtbl = 0x00bd20b8u;
inline constexpr uintptr_t kTargetHandle = 0x20;  // u32: the character it follows (pose store / PhysicsActor key)
inline constexpr uintptr_t kWorkingPos = 0x24;    // float[3]; +0x30 serial, +0x34 quat
inline constexpr uintptr_t kObstruction = 0x4fc;  // u8: Cam_Update runs slot 15 (pull-in + near-plane clearance)
}  // namespace cam_human

}  // namespace mc2vr::game

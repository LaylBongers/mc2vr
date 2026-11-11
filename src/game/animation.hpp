// Animation: the pose (Havok hkaPose layout), skeleton, bone-name map and the
// bone-controller mechanism (docs/reverse_engineering/character_pose.md).
// Typed data and the original functions the carrier calls.
#pragma once

#include <cstdint>

#include "game/function.hpp"
#include "game/global.hpp"

namespace mc2vr::game {

// hkQsTransform: translation, rotation (quaternion x, y, z, w), scale.
struct QsTransform {
    float t[4];
    float q[4];
    float s[4];
};
static_assert(sizeof(QsTransform) == 0x30);

// The pose a controller's OnPose gets (AnimSystem_PoseTask's stack hkaPose).
// model[i] = model[parent[i]] ∘ local[i] (Pose_CalcBoneModelSpace); scale
// composes per component, so a bone's local scale shrinks its subtree.
// syncModelSpace (FUN_00878d30) runs right after the controllers: while
// modelInSync is 0 it rebuilds every model-dirty bone, parents first.
namespace hka_pose {
inline constexpr uintptr_t kSkeleton = 0x00;     // -> skeleton
inline constexpr uintptr_t kLocal = 0x04;        // QsTransform[bones]
inline constexpr uintptr_t kModel = 0x10;        // QsTransform[bones]
inline constexpr uintptr_t kFlags = 0x1c;        // uint32[bones]
inline constexpr uintptr_t kModelInSync = 0x28;  // u8
inline constexpr uint32_t kFlagModelDirty = 0x2;
}  // namespace hka_pose

namespace hka_skeleton {
inline constexpr uintptr_t kParents = 0x04;    // int16[bones], -1 = root; parents precede children
inline constexpr uintptr_t kBoneCount = 0x10;  // int
}  // namespace hka_skeleton

// The skeleton's bone-name map (an open-addressing hash of name hashes; the
// OnPose name-map argument): bone index = values[HashTable_FindSlot(...)], or
// the default when the slot is < 0.
namespace bone_name_map {
inline constexpr uintptr_t kCapacity = 0x14;  // u32
inline constexpr uintptr_t kValues = 0x1c;    // int32*
inline constexpr uintptr_t kKeys = 0x20;      // u32*
inline constexpr uintptr_t kDefault = 0x24;   // int32
}  // namespace bone_name_map

// Slot of key (ECX) in keys (ESI) of the given capacity (stack); < 0 = absent.
inline constexpr Function<int32_t(uint32_t key, const uint32_t *keys, uint32_t capacity)> HashTable_FindSlot{
    "HashTable_FindSlot", 0x008244e0u, {ECX, ESI, STK}};

using g_BoneHash_Head = Global<uint32_t, 0x0117bfd0u>;  // the "Bone_Head" name hash (static init)

// Appends a bone controller to the object's BoneControllerRuntime (EAX =
// object handle; creating the runtime if needed, for objects with an animated
// model), sorted by the controller's priority (vtable slot 7). AL = added.
inline constexpr Function<bool(uint32_t handle, void *controller)> BoneControllerRuntime_AddController{
    "BoneControllerRuntime_AddController", 0x00685ad0u, {EAX, STK}};

// A bone controller object (BoneController_vtbl base; plate there): the vtable,
// then the runtime's list node embedded at +0x04 {next, prev, payload = the
// controller, key = priority}. Slots (thiscall, callee-popped): 0 dtor(flags)
// ret 4; 1 (1 arg) ret 4; 2 Update(dt, object handle) ret 8; 3 (4 args) ret
// 0x10; 4 () ret; 5 OnPose(name map, pose) ret 8; 6 IsDone() -> AL (true =
// BoneControllerRuntime_Update unlinks and deletes it via slot 0); 7 priority
// -> EAX; 8 type hash -> EAX; 9..15 (1 arg) ret 4.
namespace bone_controller {
inline constexpr uintptr_t kNodeOff = 0x04;
inline constexpr uintptr_t kSize = 0x14;  // vtable + node; derived data follows
inline constexpr int kSlots = 16;
// The game's own no-op bodies, for the slots a controller doesn't use.
inline constexpr uintptr_t kNoopRet4 = 0x004ccd40u;   // ret 4
inline constexpr uintptr_t kNoopRet10 = 0x00840fc0u;  // ret 0x10
inline constexpr uintptr_t kNoopRet0 = 0x006b9360u;   // ret (VirtHook_NoOp)
}  // namespace bone_controller

}  // namespace mc2vr::game

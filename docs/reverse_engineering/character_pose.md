# Components, character pose & bone controllers

How C++ can reach a character's parts and its animated pose. Mapped statically 2026-11-10. The names, plates and `g_CompType_*` labels are in Ghidra. The camera side is in [camera_classes.md](camera_classes.md), the mod-side use in [../first_person.md](../first_person.md).

## Components

- Every component type has a static descriptor `g_CompType_<Name>`. Its name pointer sits at `+0x3c` and is written by the type's static init; the `[presize]` string at `0x00bad488` lists all ~337 types.
- Instances live in the type's pooled store and are found by object handle with **`ComponentStore_Find`** (ECX = descriptor, EAX = handle). It has 1583 call sites and is plaintext.
- Descriptors found so far: `PhysicsActor`, `BoneControllerRuntime`, `RtDamageFlags`, `RuntimeHeadLookAt`.
- `CamHuman` reads its target's `PhysicsActor` component.
- Script-level `Object.GetHardpointPosition` is **VM-virtualized** (thunk `0x00688640` → `0x02567a90` → the VM entry `0x01b950a0`). The script camera's eye-from-hardpoint uses it too. Don't build on hardpoints.

## Pose

- The pose layout matches Havok's `hkaPose`:
  - `+0x10`: model-space transforms, 0x30 each (`hkQsTransform`: translation, rotation, scale);
  - `+0x1c`: per-bone flag bytes. Bit `0x2` means dirty, recomputed lazily by `Pose_CalcBoneModelSpace`.
- Bones are found by name hash (`0x008244a0`) in the skeleton's name map (`HashTable_FindSlot`).
- `g_BodyBoneHashes` holds 12 body bones (thighs, shins, Spine2, Chest, biceps, forearms, Head). `g_BoneHash_Head` is the head.
- `BodyBones_Sample` caches those 12 bones' current and previous positions per object. It's probably motion or hit data (inference).

## Bone controllers

- Each animated object has a `BoneControllerRuntime` (0x8c bytes) holding a list of controllers (the `BoneCtrl*` component types: LookAt, LocalRotation, LocalTranslation, Jostle, PhysicsActor, …).
- Controllers get a callback with (skeleton name map, pose) and edit the pose after sampling.
- `BoneControllerRuntime_AddController` appends one (used by `Object.PlayAnimation`). `BoneControllerRuntime_FindByType` finds them by type hash (used by `Human.SetJostleEnabled`).
- **This is the engine's own hook for post-animation pose edits.**
- **Who runs them (live-verified 2026-11-11, `debug_gameplay_census`)**, both on the update thread inside the animation phase, before `CameraTable_FillLoop` (122,983 `OnPose` calls, none stale or late):
  - `AnimSystem_PoseTask`, a per-object task body run by the animation task queues, builds a **stack-local `hkaPose`** from the sampled animation;
  - it calls every controller's slot 4, then **slot 5 `OnPose(name map, &pose)`**;
  - then it syncs model space (`FUN_00878d30`, Havok `syncModelSpace`), publishes it with `AnimSystem_PublishBonePoses`, and queues the object for `AnimSystem_PostUpdate`.

  **The publish keeps translation and rotation only:** each bone becomes a 0x20-byte `{position, serial, rotation}` record, so **bone scale never reaches rendering**. Confirmed live: a `Bone_Head` local scale of 0.001 set in `OnPose` every frame had no visible effect.

  So a controller edits the final pose, and translation/rotation edits reach skinning the same frame (scale doesn't, see above). A mod controller was tried for first-person head hiding ([../first_person.md](../first_person.md)). Separately, `BoneControllerRuntime_Update` calls slot 2 `Update(dt, object handle)` and then slot 6 `IsDone()`: true deletes the controller.
- **Contract** (base vtable `BoneController_vtbl`; the slot layout is on its plate):
  - pure slots: 2 (`Update(dt, object handle)`), **5 `OnPose(name map, pose)`**, and 8 (type hash, for `FindByType`); slot 6 `IsDone()` defaults to false (keep), and slot 4 is a pre-pose hook (default no-op);
  - slot 7 is the priority (default 10). The runtime keeps its controllers in a list sorted by it (`PrioList_InsertSorted`), so a later controller sees the earlier ones' edits. `BodyBoneTracker` (type `0x64bef946`) uses 25.
  - So a mod controller is a subclass: a high priority to run after the stock look-at and similar controllers, its own type hash, and the edit in slot 5. The engine hands it the pose, so no character → pose pointer path is needed.

## Frame order (game thread, `InGameShellState_FramePipeline`)

1. `AnimSystem_FrameUpdate` (`0x004c990c`): three animation task queues, then `AnimSystem_PostUpdate`, which applies `RuntimeHeadLookAt`.
2. `CameraTable_FillLoop` (`0x004c993a`): camera updates and the `g_CameraTable` fill.
3. Render packet build.

So the pose and the head look-at are computed before the camera reads its target. This is live-verified (2026-11-11): the bone controllers ran in the animation phase on every frame, never after the camera fill.

## OPEN

- Whether `AnimSystem_PoseTask` ever runs on a worker thread (it checks a TLS thread index). In the 2026-11-11 run it was always the update thread; a controller of ours should not assume that.
- `RenderQueue_SubmitFrameStats` (`0x00695550`, named earlier) sits in the animation module, calls the animated-object gatherer `FUN_00694d00`, and shares the per-frame block ring `0x014b6e6c` with `AnimSystem_FrameUpdate`. Its "profiler stats" name is suspect; re-check it.
- Character → skeleton name map / pose pointer: not traced, and not needed if we work as a controller (see the contract). It's only needed for reading the pose outside the callback.
- Player stance: the `Human` Lua module (21 entries) has no crouch/stance API, but the physics has crouch capsules and the camera modifier crouch tuning ([human_physics.md](human_physics.md#stances)), and the player has a crouch-zoom aim mode in stock gameplay. How it's entered (input, state machine) is unexplored.
- Where the skinning palette (`BoneMatrixArray`, [view_context_records.md](view_context_records.md)) is built from the pose.

# Player input → movement intent

How controller input becomes character movement, and where the aim ray comes from. Mapped statically 2026-11-10. The names and plates are in Ghidra. Physics side: [human_physics.md](human_physics.md). Camera and player modes: [camera_classes.md](camera_classes.md). Mod-side use: [../first_person.md](../first_person.md).

## Frame order (`InGameShellState_FramePipeline`)

| call site | step |
|---|---|
| `0x004c9811` | `Player_FrameUpdate`: input handlers, the aim ray, the mode's controller (posts commands) |
| `0x004c982b` | `PhysicsActor_DispatchMessages`: drains the command queue into the actors' movement intent |
| `0x004c984c` | `HumanPhysics_FrameStep`: turn and move (proxy integrate) |
| `0x004c990c` | `AnimSystem_FrameUpdate` |
| `0x004c993a` | `CameraTable_FillLoop`: camera updates and the table fill |

The controller and the aim ray read the camera before it updates this frame, so they see the previous frame's camera.

## Move path (all plaintext)

1. `ControllerPlayer_UpdateOnFoot`: the on-foot mode's helper, using the `ControllerPlayer` component.
   - Reads the move stick (axis pairs `9/10`, `0xb/0xc`) and rotates it into the world by the camera heading.
   - Posts **character commands** with `CharCmdQueue_Post`: `{target handle, id, vec3}`, where id 1 = move vector, 2 = facing, 3 = zero/stop.
2. `g_CharCmdQueue` is a multi-consumer queue (1024 × 0x18). Each consumer pops its channel with `CharCmdQueue_Pop`.
3. `PhysicsActor_DispatchMessages` hands human targets to `HumanPhysics_ApplyCommand`, which writes the intent fields that `HumanPhysics_ComputeMoveAndTurn` consumes.

AI presumably posts the same commands. That's unverified, but it's why the queue is keyed by target handle.

## Look path

The look stick (`0xf/0x10`, `0xe/0xd`) goes through the **VM-virtualized** thunk `0x004bdd50` → `0x02533190`. It ends up in the human camera's own input fields (`+0x610`/`+0x614`), which `CamHuman_ComputeLookDeltas` turns into yaw/pitch deltas ([camera_classes.md](camera_classes.md#camhuman-on-foot)). The delivery in between is unconfirmed. The dispatcher's second message type (`FUN_004067b0` → `0x00426e70` → `0x02562430`) is VM-virtualized too.

## Aim ray

`Player_UpdateAimRay` casts from a camera-table slot through the reticle, using the fov tan table and a reticle offset:
- origin at player `+0x130`;
- direction at `+0x13c…+0x144`, aim point 200 m out;
- the input is `[0x011751b0] + slot*0x620`, laid out like `g_CameraTable`.

`Player_CollectAimTargets` gathers objects in front of that ray.

**The aim reads `g_CameraTable`.** `[g_pCameraManager]` is set to `g_CameraManager` (`0x014A2EC0`) in `GameState3_Update`, and `+0x20` is the table. So the stock aim already follows the HMD-injected camera, from the previous frame's fill ([../camera.md](../camera.md)).

## Fire site (live-verified 2026-11-11, `debug_watch=player:13c+player:124`)

- **`Weapon_ComputeShotTarget`** reads the shooter's **aim point** (`+0x124`), only in firing windows. Its caller `FUN_00531964` spawns the shot.
  - With weapon flag `0x1000` it also reads the aim origin and the weapon's world pose, and places the target **along the weapon's own forward axis at the muzzle-to-aim-point distance**. So the barrel sets the direction and the aim point the range.
- Other readers of the aim point, while aiming or firing:
  - `FUN_0051ffb0` / `FUN_00697700` (animation range; probably the upper-body aim pose);
  - `CamHuman`'s pivot logic `FUN_0071c7b0` (the aim-mode camera).
- The aim direction (`+0x13c`) is read every frame by the target collection and the aim code, but not by the shot itself.

## Weapon model (schemas; values in the data archives)

- `WeaponProjectileBase`:
  - `FireType` (projectile type), `RateOfFire`, `iBulletsPerShot`;
  - **`FireFromReticle`**: per weapon, whether shots go along the reticle ray or from the muzzle;
  - **`MaxAimAngle`** / `MaxAimAngleAi`: probably how far the shot may deviate from the body or weapon.
- `WeaponScatter`: `ScatterMin/Max` and **`ScatterAimModeMin/Max`** (spread while aiming), `ScatterPerShot`, `CenterBias`, `LowSkillScatter`.
- `ProjectilePhysics`: `Damage` with dropoff, `GravityScale` (ballistic rounds).
- Muzzle and shell-eject hardpoints (`MuzzleFlashHardpoint`): effects only, as far as the schema shows.
- Human camera modifier (matched by content): horizontal/vertical **aim assist** (close/far), yaw/pitch speeds, **crouch yaw/pitch speed and `CrouchFov`**, `CameraShakeScale`, `AimDisruptScale`. The crouch fields point to a stock player crouch.

## OPEN

- The fire site's branch without weapon flag `0x1000` (does it aim straight at the aim point?), what that flag is (`FireFromReticle`?), and where `MaxAimAngle` / scatter apply.
- What `HumanPhysics_ApplyCommand` does per command id, and the ids beyond 1–3.
- How the look stick gets from the VM thunk into the camera's `+0x610`/`+0x614` (the camera event `0x574220ac`?); the census logs the fields.
- Fire direction: muzzle versus aim ray ([../first_person.md](../first_person.md) aim item).

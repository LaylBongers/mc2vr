# Gameplay camera classes & player modes

The gameplay side upstream of `g_CameraTable` ([draw_camera_chain.md](draw_camera_chain.md)): which object produces the camera pose, and how it's chosen. Mapped statically 2026-11-10. Names, vtable labels and plates are in Ghidra (`Cam*_vtbl`, `PlayerMode*_vtbl`, `CamHuman_Update`, …). Mod-side use: [../first_person.md](../first_person.md).

## The camera object is the controller

- Each `g_CameraTable` slot (5 × 0x620) holds its current camera object at `+0x1e0`. `CameraTable_FillFromPose` calls the object's vtable slot 4, `Update(float dt)`, and then reads its output pose through `CameraEntity_GetPoseRecord`. So **the gameplay camera logic runs inside `CameraTable_FillLoop`**, on the game thread, in `InGameShellState_FramePipeline`, once per live slot per frame, in plaintext.
- Pose fields:
  - Working pose: `+0x24` pos, `+0x30` serial, `+0x34` quat. The class's update writes it.
  - Output pose: `+0x44`, `+0x50`, `+0x54`. `Cam_Update` copies the working pose here and bumps the serial. Everything downstream reads this one (`CameraEntity_GetPoseRecord`, which has 13 callers incl. `LuaCamera_GetYaw/GetPitch` and `LuaPg_SpawnFromCamera`).
- Base `Cam_Update` (vtable slot 4) is a template:
  1. Slot 15 (`Cam_ObstructionAdjust`), if byte `+0x4fc` is set.
  2. Slot 19.
  3. Velocities.
  4. Working → output copy.
  5. Slot 18.
- Derived classes compute the working pose and then call `Cam_Update`.
- Vtables are 20 slots. Slot 14 returns the class's type-name string, which is how the classes below were named.

| vtable | type name | notes |
|---|---|---|
| `0x00bce338` | — | abstract base (purecalls) |
| `0x00bce388` | `nullcam` | pushed on `Player_AttachCharacter` |
| `0x00bce3d8` | `free eye` | |
| `0x00bce428` | `binoculars` | binoculars and sniper (`bSniper`), focus distances |
| `0x00bce478` | `pda` | |
| `0x00bce4c8` | `ragdoll` | |
| `0x00bd2018` / `2068` / `2160` / `21b8` | `car` / `helicopter` / `tank` / `turret` | `PlayerModeVehicle_CreateCamera` |
| `0x00bd20b8` | `human` | **on-foot third-person orbit**, `CamHuman_Update` |
| `0x00bd2110` | `script` | look-at / hold shots |

Live census (2026-11-11, `debug_gameplay_census`), all in `g_CameraTable` slot 0, the only slot used:
- `human` on foot, including the crouch-zoom aim mode;
- `car` for the car and the boat;
- `turret` for the tank gunner seat, `tank` for the driver;
- `binoculars`;
- `pda` for the satellite / airstrike view;
- `nullcam` briefly at load;
- `ragdoll` when the character was thrown from a car (2 s, then `human`).

- `script` for the fixed cameras of the vehicle-hijack QTE (it hands over straight to the hijacked vehicle's camera, here `tank`) and of an NPC interaction.

The video cutscenes (Bink) use no game camera. Fixed-camera in-engine cutscenes weren't reached; they are presumably `script` too.

## Player modes

- The player object keeps a mode stack: `+0x1c0`, up to 8 entries, count `+0x1bc`, active-bitmask byte `+0x1e0`.
- Each mode is a small object. `+0xc` is its active camera. Vtable slot 4 creates its cameras, slot 3 is a transition.
- Mode classes found statically: null, PDA, binoculars, script, on-foot (`0x00bce750`), vehicle (`0x00bce7a8`/`0x00bce800`).
- **Live (2026-11-11): this stack is an overlay stack, not the gameplay camera's owner.**
  - In gameplay the primary player's stack is **empty**. It stayed empty on foot, in every vehicle and in binoculars. The `null` mode only showed at load, and on the unused co-op slot.
  - Opening the PDA pushes `PlayerModePdaMenu_vtbl` (`0x00bce5f0`), then `pda` for the map/satellite view.
  - The on-foot and vehicle modes never appeared here, so who owns the `human`/vehicle cameras is OPEN.
- The game updates every player slot each frame, including the unused co-op slot, which has no character (`+0x20` = 0).
- Mode code reads the controller input (`ControllerInput`, `RightAnalogX/Y` in `FUN_006ab250`) and posts UI events (`BinocularsEnter/Update/Exit`, `nZoomLevel`).
- `PlayerModeOnFoot_CreateCameras` builds a `CamHuman` with pivot offset **(0, 1.75, 0)** and a `CamRagdoll`.
- Some mode constructors have no static caller: e.g. the script mode's constructor at `0x006b41b0` is mutated plaintext that is reached only from VM/runtime code. The camera classes themselves are plaintext.

## CamHuman (on-foot)

- Target character: via the `+0x20` handle (the pose-store and `PhysicsActor` key; the pivot code also reads `+0x1c`).
- Orbit yaw: `+0x618`. Normalized pitch: `+0x61c`, in [0, 1].
- Rotation: `RotationX(−pitch)·RotationY(yaw+π)`.
- Pivot and offset, then collision (`FUN_0071e710`). It writes the working pose, then calls `Cam_Update`, which may run the obstruction pull-in and `CamCtrl_NearPlaneClearance` ([view_frustum.md](view_frustum.md)).
- Every callee is plaintext. No SecuROM involvement.
- Look input: `CamHuman_ComputeLookDeltas` turns the stick at `+0x610`/`+0x614` and the **mouse** at `+0x10`/`+0x14` (live: deltas up to ~24 per frame while the mouse moved, stick 0 without a gamepad) into the yaw/pitch deltas. Yaw is in radians, wrapping at ±π; pitch is in [0, 1], 0.5 at rest. `+0x7dd` was set in the crouch-zoom aim windows. The stick gets there through VM code ([player_input.md](player_input.md#look-path)), probably as the camera event `0x574220ac` that `CameraTable_FillLoop` dispatches through vtable `+0x18`. `debug_gameplay_census` logs the fields.

## OPEN

- The fields still to decode, in priority order:
  - the pivot/offset path (`FUN_0071c7b0`, `FUN_0071c360`);
  - `+0x7dd` (set in the crouch-zoom aim mode; aim, crouch, or both?);
  - the zoom/aim offsets (the `CameraZoomOffset`/`CameraYawOffset`/`CameraPitchOffset` schema at `FUN_0065a3c0`, the `CameraOffset*` / `CameraBlendTime` schema at `FUN_0065e570`/`FUN_0065e7d0`);
  - how blending between cameras works on a mode switch.
- Whether aim and character heading read the camera's output pose or CamHuman's yaw/pitch fields directly.
- Who owns and switches the gameplay cameras (`human` ↔ vehicle), given that the player's mode stack doesn't.
- Whether the gamepad stick lands in `+0x610`/`+0x614` (the run used mouse and keyboard).

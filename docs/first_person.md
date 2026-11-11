# First person (OPEN — in progress)

Goal: a VR first-person mode, built into the engine, where **the user's real head is the truth and the character follows it**:
- Head motion moves the character's head.
- Roomscale walking moves the character.
- The user's real height carries over.
- Crouching, and even a slight lean down, bends the character by the same amount. It's continuous, not a stance switch.
- Hitboxes follow the body.
- Both standing and seated modes are supported.
- Motion-controller aim comes later.

Game side: [reverse_engineering/camera_classes.md](reverse_engineering/camera_classes.md), [reverse_engineering/character_pose.md](reverse_engineering/character_pose.md), [reverse_engineering/human_physics.md](reverse_engineering/human_physics.md), [reverse_engineering/player_input.md](reverse_engineering/player_input.md).

## VR play-mode practice (current choices, from the sources below)

Sources: Meta's [seven design decisions](https://developers.meta.com/horizon/resources/seven-design-decisions/) and [modern VR design patterns](https://developers.meta.com/horizon/blog/lessons-from-the-frontlines-modern-vr-design-patterns/) (2017, still the reference), the Road to VR [comfort-settings checklist](https://roadtovr.com/vr-comfort-settings-checklist-glossary-developers-players/), [IEEE VR 2018 on head–wall collisions](https://doi.ieeecomputersociety.org/10.1109/VR.2018.8446221), Godot's [room-scale XR](https://docs.godotengine.org/en/stable/tutorials/xr/xr_room_scale.html) (basis for the per-frame algorithm in step 1 below). Practice from VR bodies/mods is noted where it's convention, not a study.

- **Both seated and standing**, switchable mid-session (about half of players sit).
  - **Standing** uses a floor origin: the real head height counts, and the body is fitted to it.
  - **Seated** uses an eye origin: we set the virtual eye height and lose the floor.
- Offer a quick recenter / height recalibration, and prompt for it when the posture changes, since calibrating seated and playing standing is off.
- **Crouch, two kinds.** Real crouch and lean are continuous, from the measured head drop below the calibrated standing height. An artificial crouch button is also available, for seated play and accessibility. Gameplay never *requires* a physical crouch.
- **Walls: never stop or push the camera.** The camera always is the HMD. The body is what collides. When the head enters geometry, fade to black by penetration depth (fully black where the polygon could be seen through). Block teleport/locomotion shortcuts while faded. A slightly thicker head/body volume keeps the fade rare.
- **Turning.**
  - Snap turn (30–45°) is the comfort default; smooth turn is an option.
  - Movement runs at fixed velocity (no acceleration).
  - An optional vignette.
  - A single **Comfort vs Immersive** preset sets these, rather than individual knobs alone.
- **Body.** The head and eyes follow the player. Body yaw follows the head with a dead zone, then turns smoothly toward it; this is the convention from VR bodies and mods, not a study. Legs come from animation, with crouch done by pelvis drop and bent legs. Waist-rooted spine IK alone can't crouch under things. "Incorrect arms are worse than no arms": this matters for the later motion-controller arms.

## Pipeline (per game frame, game thread)

HMD pose (play-space) → **character placement** → **body pose** → **camera** = the HMD eyes.

1. **Placement.** This is Godot's *character-body-centric* room scale: the game's character stays authoritative, and the play-space origin is re-derived around it. Godot rates it "easier to combine with traditional game mechanics" than its origin-centric variant. Per frame:
   1. Body target = HMD position minus a neck offset rotated by the head, with height zeroed, so nodding or tilting doesn't move the body. Move the body there through its controller, then shift the play-space origin back by the distance the body *actually* moved, so the player stays centred over the body. In this engine that is `HumanPhysics_IntegrateProxy`, the game's `hkpCharacterProxy` integrate inside `HumanPhysics_FrameStep` ([human_physics.md](reverse_engineering/human_physics.md)): the room-scale displacement goes in as velocity over the step, and the position after the substeps is the actual motion.
   2. Body yaw follows the head (dead zone, per the body bullet above); the origin counter-rotates so the view doesn't turn.
   3. If the body fell short of its target by more than about 1 cm, it's **blocked**. Skip stick turn and locomotion that frame but keep gravity, and fade by the head–body gap (from a few cm). The camera is never pulled back: the head can stay in the wall behind the fade until the player steps back.
   4. Otherwise apply stick locomotion along the body heading. Snap/smooth turn rotates *around the body*, not around the play-space centre, which avoids the circular sweep when the player stands off-centre.

   Stick locomotion and body yaw map onto the engine's **character commands**. `ControllerPlayer_UpdateOnFoot` posts move (id 1) and facing (id 2) commands; they're drained into the physics actor's movement intent before the physics step ([player_input.md](reverse_engineering/player_input.md)). Ours keep that path, which keeps speed tiers, turning, slopes and animation selection stock:
   - the move direction is relative to the body/head heading instead of the third-person camera;
   - the facing comes from the body-yaw rule.

   The room-scale displacement is the one part that bypasses commands, because it's measured motion, not a request.
2. **Body pose.** After animation, our bone controller ([character_pose.md](reverse_engineering/character_pose.md#bone-controllers)) bends spine/neck/head so `Bone_Head` lands at the HMD, and lowers the pelvis with bent legs for crouching and leaning. Havok Behavior foot/hand/look-at IK classes are in the binary (`hkbFootIkModifier`, `hkbHandIkModifier`, `hkbLookAtModifier`); whether they're used and callable is OPEN. The same controller could hide the head (see Head hiding below).
3. **Height and calibration** (per the practice above).
   - Standing (floor origin): real height is calibrated against the character's eye height.
   - Seated (eye origin): a fixed offset lifts the head to the character's standing eye height, and head motion stays relative.
   - Crouch depth: the measured drop from the calibrated standing height, plus the artificial crouch input.
4. **Camera.** In first person, `CamHuman` becomes the HMD camera: its position is the character's eye point after step 2 (which tracks the HMD by construction). The render-side per-eye delta stays as it is ([camera.md](camera.md)). Its yaw/pitch feed gameplay heading and aim.
5. **Hitboxes and capsule.**
   - The hit bodies follow the bent pose, if they're pose-driven (`BodyBones_Sample` / `BoneCtrlPhysicsActor`, unverified).
   - The movement capsule follows the HMD height. The engine has discrete stance capsules: standing `ProxyCapsuleHeight`, crouch `ProxyCapsuleCrouchHeight`, and short ones. It also has a penetration-checked switch, `HumanPhysics_TrySetStance`, so standing up under an obstacle fails cleanly ([human_physics.md](reverse_engineering/human_physics.md#stances)).
   - Two options:
     - map head height onto those stances with hysteresis, using the engine's own shapes and animation states;
     - resize the capsule continuously (Havok capsule vertex edit plus a phantom re-cache, unverified).

     Start with the stances; the visual body bend (step 2) stays continuous either way.
   - When standing up is refused, the real head is above the allowed height, so the penetration fade covers it.
   - Godot suggests separate head and body shapes as the more accurate option.
6. **Calibration moments.** Record the standing/seated reference on a "ready" confirmation (loading screen or menu), not at spawn: the headset may not be on yet, or the player may be away from the tracking centre. Recalibrate on request. Develop and test standing and walking early; seated testing hides room-scale bugs.

## Implemented

**Camera** (`first_person=on`, default off; live-verified 2026-11-11):
- A MidHook in `CamHuman_Update` where its final world matrix is ready (`hooks/first_person.cpp` → `vr/first_person.cpp`, [hooks.md](hooks.md)) sets the matrix translation to the eye point: the character's pose position + `first_person_eye_height` (1.65 m) up + `first_person_eye_forward` (0.15 m) along the view's horizontal forward.
- It clears the camera's obstruction flag (`+0x4fc`), so the pull-in and near-plane clearance don't run.
- Rotation, look input, aim mode and the pose publication stay the camera's own. The HMD stays layered on top through `g_CameraTable` ([camera.md](camera.md)).
- The character's pose position is at its feet (ground level). The resulting view height is feet + 1.65 m plus the HMD's offset in the OpenXR `LOCAL` space, whose origin is the head position at session start / recentre. No SteamVR height is used. Seated, that puts the eye at about the right height.
- Vehicle, QTE (`script`), NPC-interaction (`script`) and ragdoll cameras keep their own classes, and switching to and from them works.
- Report: `fp: window` (frames, character and eye positions, the stock boom length).

**Head hiding:** not implemented. A bone controller that scaled the head down in `OnPose` attached and ran every frame, but the published pose drops scale ([character_pose.md](reverse_engineering/character_pose.md#bone-controllers)), so the head stayed; that controller (our own engine bone controller with a hand-built vtable, attached through `BoneControllerRuntime_AddController`) is in git history as the starting point for the body pose. Hiding needs another way (OPEN).

**Why a hook, not a reimplementation of `CamHuman_Update` (yet):** it computes the position inline (536 instructions; 28 calls to private helpers with custom register conventions; mixed SSE/x87, including x87 float returns the call shim doesn't support), so no small function owns that decision. A faithful reimplementation is the next stage for the camera, once its internals need to change (head-driven yaw, body coupling); the hook then becomes a direct call inside it. Rejected: a new camera class swapped into the on-foot mode (mode creation partly runs from VM/runtime code), and overriding at `g_CameraTable` (render-only, so gameplay stays third-person).

## Frame order

`HumanPhysics_FrameStep` (`0x004c984c`) → animation (`0x004c990c`, the bone controllers run synchronously inside it, live-verified) → `CameraTable_FillLoop` (`0x004c993a`) ([character_pose.md](reverse_engineering/character_pose.md#frame-order-game-thread-ingameshellstate_framepipeline)). So placement (step 1) goes into the physics step, the body pose (step 2) into a bone controller, and the camera (step 4) last. The HMD sample used must be the one the frame is rendered with, or the head and body lag the view; the render-side per-eye delta absorbs a residual, but only the rendered view's, not the body's.

## OPEN

- **Head offset policy.** The HMD's `LOCAL` offset is relative to wherever the last recentre happened, so horizontally the eye isn't always over the character's head. Right for seated height; the horizontal part needs a policy: the room-scale placement (body follows the head) or a recentre on the character.
- **Hiding the head.** Bone scale doesn't survive publication. Options: the render side (zero the head bones' rows in the player mesh's `BoneMatrixArray` on the eye passes only; needs the skin-bone → skeleton mapping and the player's records, and keeps the shadow), or a mesh/part-level hide if the character model has separable head parts (unexplored). Moving the head bone away instead would stretch vertices that are partly weighted to the neck.
- **Own body in view.** Looking down, the body is visible and the near plane clips through parts of it. Decide how much of the body to show, and how to keep it clear of the near plane.
- **Strafing.** A/D makes the character turn and walk sideways; the game has no strafe animations. Needs a design, e.g. keep the body facing the view with a sideways walk, or turn the body under a fixed view (animation RE).
- **Standing height mapping.** Meta's advice is that the virtual body scales to the player. With a fixed-size character and its hitboxes, the options are proportional (real standing eye height → the character's eye height, crouch depth scaled with it) or 1:1 plus a body fit (a tall player sees over walls).
- **Movement** (mapped statically, [human_physics.md](reverse_engineering/human_physics.md)): the per-id semantics of `HumanPhysics_ApplyCommand`; the player's stock crouch and the stance indices 2–6; how the displacement is injected into `HumanPhysics_IntegrateProxy` (velocity field, substep split). The look stick lands in `CamHuman`'s own input fields ([camera_classes.md](reverse_engineering/camera_classes.md#camhuman-on-foot)).
- Whether the Havok IK is usable for the spine/legs, or the pose needs our own analytic IK.
- **Heading:** whether body facing follows the head yaw (dead zone) or the stick, and what gameplay "camera heading" (`Player.GetCameraXZHeading`, aim) should follow.
- **Aim** ([player_input.md](reverse_engineering/player_input.md#aim-ray)). The stock aim ray is cast from the `g_CameraTable` slot through the reticle, so in first person it follows the HMD camera, one frame late, and the reticle belongs at the view centre. Shooting in first person is not tested yet.
  - For motion controls, the substitution point is `Player_UpdateAimRay`: write the controller's ray into the player's aim origin/direction.
  - The shot takes the player's aim point; for flag-`0x1000` weapons the direction is the weapon's own forward axis at that range ([player_input.md](reverse_engineering/player_input.md#fire-site)). Not read: the no-flag branch, `MaxAimAngle`, aim assist.
- **Mode coverage:** vehicles, turrets, binoculars and script shots keep their own classes; decide which get first-person variants, and how seated/standing work in a vehicle seat.
- Near-camera fade/hide logic for the player model, if any.

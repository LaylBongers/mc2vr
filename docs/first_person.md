# First person (OPEN — research)

Goal: a VR first-person mode, built into the engine, where **the user's real head is the truth and the character follows it**:
- Head motion moves the character's head.
- Roomscale walking moves the character.
- The user's real height carries over.
- Crouching, and even a slight lean down, bends the character by the same amount. It's continuous, not a stance switch.
- Hitboxes follow the body.
- Both standing and seated modes are supported.
- Motion-controller aim comes later.

Game side: [reverse_engineering/camera_classes.md](reverse_engineering/camera_classes.md), [reverse_engineering/character_pose.md](reverse_engineering/character_pose.md).

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
2. **Body pose.** After animation, a bone controller of ours: a `BoneController_vtbl` subclass added with `BoneControllerRuntime_AddController`, high priority, the edit in its `OnPose(name map, pose)` ([character_pose.md](reverse_engineering/character_pose.md#bone-controllers)). It bends spine/neck/head so `Bone_Head` lands at the HMD, and lowers the pelvis with bent legs for crouching and leaning. Havok Behavior foot/hand/look-at IK classes are in the binary (`hkbFootIkModifier`, `hkbHandIkModifier`, `hkbLookAtModifier`); whether they're used and callable is OPEN. The same controller hides the head (collapsed `Bone_Head` scale; the cost is the head's shadow).
3. **Height and calibration** (per the practice above).
   - Standing (floor origin): real height is calibrated against the character's eye height.
   - Seated (eye origin): a fixed offset lifts the head to the character's standing eye height, and head motion stays relative.
   - Crouch depth: the measured drop from the calibrated standing height, plus the artificial crouch input.
4. **Camera.** In first person, `CamHuman` becomes the HMD camera: its position is the character's eye point after step 2 (which tracks the HMD by construction). The render-side per-eye delta stays as it is ([camera.md](camera.md)). Its yaw/pitch feed gameplay heading and aim (see OPEN).
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

## Where the camera plugs in

`CamHuman_Update` (plaintext, all callees plaintext) writes the on-foot camera's working pose each frame inside `CameraTable_FillLoop`. Everything downstream reads it: the render camera, culling, Lua `Camera.GetYaw`, spawn-from-camera, and the HMD injection at `g_CameraTable`.

Recommended: **reimplement `CamHuman_Update`** ([reimpl.md](reimpl.md)) with a first-person branch:
- the position comes from the pipeline above;
- no obstruction pull-in or near-plane clearance (`+0x4fc`, `Cam_ObstructionAdjust`);
- the third-person path stays in the same function.

Rejected:
- A new camera class swapped into the on-foot mode: mode creation partly runs from VM/runtime code.
- Overriding at `g_CameraTable`: render-only, so gameplay stays third-person.

## Ordering hazard

The animation update runs before `CameraTable_FillLoop` ([character_pose.md](reverse_engineering/character_pose.md#frame-order-game-thread-ingameshellstate_framepipeline)).

- The frame order fits: `HumanPhysics_FrameStep` (`0x004c984c`) → animation (`0x004c990c`) → `CameraTable_FillLoop` (`0x004c993a`). The bone controllers run synchronously inside the animation phase, before the camera (live-verified 2026-11-11). Placement (step 1) goes into the physics step, the body pose (step 2) after animation, and the camera (step 4) last.
- The body pose (step 2) has to be done before skinning and before the camera reads the eye point.
- The HMD sample used must be the one the frame is rendered with, or the head and body lag the view. The render-side per-eye delta absorbs a residual, but only the rendered view's, not the body's.

## OPEN

- Standing height mapping. Meta's advice is that the virtual body scales to the player. With a fixed-size character and its hitboxes, the gameplay-safe options are:
  - proportional: real standing eye height → the character's eye height, and crouch depth scales with it;
  - 1:1 plus a body fit (a tall player sees over walls).

  Decide; probably proportional as the default, with an option.
- Live:
  - is the pose final at `CameraTable_FillLoop`? Check the task-queue joins;
  - the camera-class and mode census.
- Movement, mapped statically ([human_physics.md](reverse_engineering/human_physics.md)). Still OPEN:
  - the per-id semantics of `HumanPhysics_ApplyCommand`;
  - whether the player has a stock crouch, and the stance indices 2–6;
  - the look stick lands in the human camera's own input fields ([camera_classes.md](reverse_engineering/camera_classes.md#camhuman-on-foot)), so in first person the reimplemented `CamHuman_Update` simply ignores it in favour of snap/smooth turn. Only the delivery path through VM code is unconfirmed.
  - how the displacement is injected into `HumanPhysics_IntegrateProxy` exactly (velocity field, substep split).
- Whether the bone controllers' order and the Havok IK are usable for the spine/legs, or we need our own analytic IK on the pose.
- Heading: whether body facing follows the head yaw (with a dead zone, like typical VR bodies) or the stick. What gameplay "camera heading" (`Player.GetCameraXZHeading`, aim) should follow.
- Aim ([player_input.md](reverse_engineering/player_input.md#aim-ray)). The stock aim ray is cast from the `g_CameraTable` slot through the reticle, so in first person it already follows the HMD camera, one frame late. Gaze aim works without changes, and the reticle belongs at the view centre.
  - For motion controls, the clean substitution point is `Player_UpdateAimRay`: write the controller's ray into the player's aim origin/direction instead of the camera's. Every downstream consumer (the target collection, and presumably fire) follows.
  - The fire site is live-verified ([player_input.md](reverse_engineering/player_input.md#fire-site-live-verified-2026-11-11-debug_watchplayer13cplayer124)). Shots aim at the player's **aim point**, and for flag-`0x1000` weapons along the weapon's own forward axis at that range. So a hand-held weapon whose pose follows the controller would fire where its barrel points; the aim point then only sets the range. Still OPEN: the no-flag branch, `MaxAimAngle`, and whether aim assist bends the ray. Aim assist is usually off or reduced in VR.
- Mode coverage: vehicles, turrets, binoculars and script shots keep their own classes. Decide which ones get first-person variants, and how seated/standing work in a vehicle seat.
- Near-camera fade/hide logic for the player model.

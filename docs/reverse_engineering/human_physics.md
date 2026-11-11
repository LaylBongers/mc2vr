# Human physics (character movement)

How an on-foot character moves: a Havok character proxy driven by a movement intent, with capsule shapes per stance. Mapped statically 2026-11-10. The names (`HumanPhysics_*`, `HumanPhysicsActor_*`) and plates are in Ghidra. The mod-side use is in [../first_person.md](../first_person.md); the component and pose side is in [character_pose.md](character_pose.md).

## Controller type

- Havok **`hkpCharacterProxy`**: a phantom-based character proxy with its state machine (`hkpCharacterState{OnGround,InAir,Jumping}`, `hkpCharacterContext`) and caching/simple shape phantoms. These are the RTTI names; there's no `hkpCharacterRigidBody`.
- `HumanPhysicsActor_Init` builds the actor (`HumanPhysicsActor_vtbl`), its capsules and its proxy (`hkpCharacterProxy_Ctor`).
- Tuning comes from the `_HumanPhysics` component (0x84 bytes = 33 floats, defaults in `HumanPhysics_RegisterFields`):
  - `ProxyCapsuleRadius` / `ProxyCapsuleHeight` / `ProxyCapsuleCrouchHeight` (0.4 / 1.8 / 1.35);
  - walk/jog/run/sprint speeds, `CrouchWalkSpeed`, `MaxRotationSpeed`, `Gravity`, `ImpassableSlopeAngle`, and the fall and stagger fields.
  - The real values are in the data archives.

## Frame step

`HumanPhysics_FrameStep` is called from `InGameShellState_FramePipeline` at `0x004c984c`, **before** the animation update and `CameraTable_FillLoop`. It runs per-actor callbacks:

1. `HumanPhysics_PreStep` (once). Some actors are throttled to half rate, unless the byte at `+0x419` is set. It runs `HumanPhysics_ComputeMoveAndTurn`.
2. Up to 5 substeps of `HumanPhysics_IntegrateProxy`, then `FUN_0042da90`. `HumanPhysics_IntegrateProxy` is the game's own copy of `hkpCharacterProxy::integrate`: manifold, contact planes, simplex solve. Velocity goes in; the position after it is the actual motion.
3. A post-step (`FUN_004292e0`).

All of this is plaintext.

## Movement intent

Controllers post character commands, which `HumanPhysics_ApplyCommand` turns into intent fields on the physics actor ([player_input.md](player_input.md)):
- desired move vector `+0x140…+0x148`;
- desired facing `+0x160…+0x168` (XZ).

`HumanPhysics_ComputeMoveAndTurn` turns toward the facing and moves along the move vector. When either vector is about zero it falls back to the current forward.

## Stances

- The actor keeps 7 shape slots (`+0x2b8`): standing, crouch, two short ones, two from a constant, and one unknown. `+0x414` is the current stance index.
- `HumanPhysics_TrySetStance(index, checkPenetration)` swaps the phantom's shape. With the check on, it reverts and returns false when the new shape would overlap geometry: "can't stand up here".

## OPEN

- Where the stick magnitude is scaled into speed tiers (the controller or `HumanPhysics_ComputeMoveAndTurn`). The player-side writer is resolved ([player_input.md](player_input.md)).
- What the stance indices 2–6 are (prone? swim? carry?); who calls the stance switch for the player; whether the player can crouch in stock gameplay (`CrouchWalkSpeed` suggests so, maybe in cover).
- Whether a capsule's vertices can be changed in place for a continuous height (Havok `hkpCapsuleShape::setVertex` plus a phantom re-cache), compared with the discrete stance shapes.
- The roles of `FUN_0042da90` and the post-step; where the actor's world transform is published to the pose store (`Pose_Copy`).
- How hits are tested: the capsule or pose-driven bodies ([character_pose.md](character_pose.md)).

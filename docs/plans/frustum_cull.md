# Frustum Culling Alignment (HMD-following, HMD-FOV)

Goal: the engine's frustum culling (and the LOD/shadow work tied to it) covers what the HMD sees,
instead of the game's 16:9 widescreen frustum — no pop-in when turning or looking up/down in VR.

**Status: DONE (2026-10-08, live-verified).** Both halves work in the HMD, and the third-person
camera behaves as stock.

| Half | Mechanism | Status |
|---|---|---|
| Rotation (cull follows the head) | `view_table_inject=on` — union HMD pose written into `g_CameraTable` (`camera_table.cpp`) | Done, live-verified 2026-10-07 |
| FOV (cull covers the HMD's angles) | `cull_hmd_fov=on` — HMD frustum extents written into the view-context builder, camera clearance kept stock (`cull_frustum.cpp`) | Done, live-verified 2026-10-08 (cull 62.0°/59.7° vs game 36.7°/22.7°) |

## The mechanism (cull_hmd_fov)

`ViewContext_BuildCameraConstants` (`0x008591ac`, plate in Ghidra) builds every view's camera
constants once per view per frame. It turns the camera entry's fov (`entry+0x58`) into two frustum
half-extents:

```
halfH = fov * 0.375 * aspect          (radians; stock fov 0.95975 at 16:9 -> 36.65 deg)
tanH  = tan(halfH)                    -> XMM2, stored to ctx+0x30 @ 0x00859425
tanV  = tanH / aspect                 -> XMM0, stored to ctx+0x34 @ 0x00859436   (22.71 deg)
```

At `0x0085943B` (`LEA EAX,[EBX+0xB20]`, `MC2_VCCAM_TANS_READY`), everything the rest of the build
produces derives from XMM2/XMM0 only (raw-decoded):
- the projection at ctx+0xb20 → VP → `g_ViewContextTable` records;
- the four frustum-corner vectors (±tanH, ±tanV) handed to `FUN_00857140` (frustum structs at
  ctx+0x38 and ctx+0x250) and `FUN_0085a3f0` (whole view → ctx+0x954/0xa70/0xa80; four shadow
  cascades → ctx+0x46c/0x8c0/0x900);
- the ctx tans themselves (read by `FUN_0047ded0`'s render constants and the snapshot).

The carrier MidHooks that instruction. For HMD-driven views only, it sets
`tanH = max(|tanH|, tan(hmdHalfH + margin))` (same for V; signs kept), in XMM2/XMM0 and in
ctx+0x30/+0x34. `hmdHalf*` is the outermost half-angle across both eyes (`camera_table::get_fov_union`),
and the margin is `cull_fov_margin` (default 5°). Each extent is shaped on its own axis, so the
vertical is no longer tied to 16:9. The result is never narrower than the game's frustum.

**View discrimination**: the builder works on copies of the `g_CameraTable` entries. The fill hook
records every entry it rewrites (rows + position, seqlocked 8-slot history).
`camera_table::is_union_camera` matches the builder's active camera entry (`[ctx+0x28]`, self-indexed
0x70 stride) against that history. Shadow, reflection and aux views never match, so they keep the
game's frustum. Live: about one widened build per frame (the main view); `otherView`/`noCam` are the
remaining views.

**Why the game's projection may change**: for the main view the HMD never sees the game's
projection. `view_row_rewrite=hmd_delta` decomposes every main-pass VP upload and rebuilds it with
the per-eye OpenXR FOV, which is independent of the game's a/b terms. The monitor shows the left
eye (`eye_monitor_pin`).

Side effects (accepted):
- `view: gameproj` reports the HMD angles.
- Shadow cascades are fitted to the wider frustum, so shadow texels get coarser.
- Screen-size LOD metrics see the wider projection.
- ADS zoom no longer narrows culling, because the max() keeps the HMD extents.

## The one consumer that must stay stock: camera clearance

`FUN_00857140` writes the frustum struct at ctx+0x38: `{+0x00 near, +0x04 far, +0x0c 8 world-space
corners — near 0..3 = (+H,+V)(+H,−V)(−H,−V)(−H,+V), then far 0..3; +0x6c side-plane normals, ...}`
(0x218 bytes; `MC2_FRUSTUM_*`). The third-person camera reads it through
**`CamCtrl_NearPlaneClearance` (`0x007107d0`)**. That function block-copies the main slot's struct
and keeps running maxima of the near-plane diagonal |c2−c0| and vertical edge |c1−c0| in the
controller (+0x4d0/+0x4d4). These are the camera's obstacle clearance. The HMD quad's diagonal is
~3x the stock one, so the camera backed off from obstacles behind the player far too early.

Fix: a second MidHook at `0x007107F9` (`MC2_CAMCLEAR_FRUSTUM_COPIED`), the first instruction after
the copy; the copy is at [ESP+0x18]. If the copied near quad carries the extents the builder hook
wrote (1% match, measured from the corners), its four near corners are scaled about their center
back to the game's own extents. The builder hook publishes both extent pairs through atomics; the
camera runs on the game thread. Only that function's private copy changes.

How it was found (2026-10-08):
1. A/B: no pull-in with `cull_hmd_fov=off`.
2. A `debug_watch` on the slot-ctx tans and near box found only render readers.
3. A `debug_watch` on the frustum struct caught these `rep movsd` copies:
   - `0x00472E2F` (render thread, 0x86 dwords) and `0x005314C7` (0x45 dwords): cull-side readers,
     which should see the HMD frustum;
   - `0x005046C3`: per-player focus;
   - `0x007107F7`: the camera function above.

The old fov-scale experiments' camera pull-in above ~1.75x was this same clearance, not
`Fov_TanIndexHelper`.

## Regression checks

Conf: `view_table_inject=on`, `view_row_rewrite=hmd_delta`, `cull_hmd_fov=on`, HMD tracked, gameplay.

1. Boot log: `cullfov: installed builder MidHook @ 0085943b (ARMED)` and
   `cullfov: installed camera-clearance MidHook @ 007107f9`.
2. `cullfov: window:` per 10 s:
   - `widened` ≈ one per frame;
   - `game 36.7°/22.7° -> cull 62.0°/59.7°` (Quest-class FOV + 5°);
   - `camClear restored` ≈ `widened`/2 (one per camera update) with `stock≈0`.

   Failure signatures:
   - `widened=0, otherView>0`: the union-camera match failed (tolerances in `camera_table.cpp`
     `is_union_camera`).
   - `camClear stock>0` while widened: the copy match failed, and the pull-in returns.
   - `noFov>0`: no tracked pose in the last second.
3. In the HMD: no pop-in at the edges when turning or nodding, including water top and bottom, and
   no camera pull-in near obstacles.

## Open items

- **Crash once in run 3**: about 20 s after the OpenXR session dropped VISIBLE→SYNCHRONIZED, with
  4-target full-mode watchpoints trapping ~50k/10 s. The carrier log has no fault record. It did
  not recur in the verification run, which had no watch and no session drop. If it reappears
  without `debug_watch`, test a session drop (headset off) deliberately.
- The cull-side copiers `0x00472E2F` / `0x005314C7` were not named. Which one is the operative
  per-object cull does not matter while the builder hook covers both.

## History: what was tried and why it is gone (2026-10-08, ~26 rounds)

All of the following were removed from the carrier in the 2026-10-08 cleanup (git history has the
code). The Ghidra plates on the named functions keep the RE facts.

| Approach | Verdict |
|---|---|
| `entry_fov_scale` (patch `[0x00BEAB5C]`) / `boom_pin` (repoint the `0x0071BBC6` MULSS at a scaled value) | The only thing that ever moved the visible cull, because it scales the fov *before* the builder's tan conversion. But the scale is 16:9-locked (vertical stayed ~39° at 1.5x), and the camera pulled in above ~1.75x (camera clearance, above). Superseded by the builder hook. |
| `cull_tan_override` (write HMD tans at `FUN_0047ded0`'s DIVSS reads) and the snapshot/source-ctx writes (rounds 16/18/21) | The writes were consumed but did not remove pop-in. ctx+0x30/0x34 are just one frustum product of the build, read only by render constants and the snapshot. The frustum structs and corner rays stayed stock. |
| `cull_fov_widen` (ViewEntry fov triple at `0x0048a97a`) | Moved nothing visible. The triple comes from the fixed gcam corner (v/h = 0.3), not the projection. |
| `record_fov_widen` (rewrite record VP projection rows, 3 sites) | Inconclusive: the hooks never matched a main record. Records are downstream of the builder anyway. |
| `entry_fov_decouple` | Wrong value semantics (wrote the 300.0 scale constant into fov slots), which produced garbage projections. |
| Entry-fov census, `cull_snap_dump`, `gcamtan` watch target | Diagnostics for the above. Removed. |

Earlier-round misreadings worth not repeating:
- `[0x00BEAB5C]` is the base fov in radians, 4:3-normalized (0.95975). It is not a cosine and not
  "300 engine units": `[0x00BAD260]` = 300.0 is a separate constant.
- `Fov_TanIndexHelper` is not the projection's fov reader. It is camera-controller boom math.
- `FUN_0047ded0` (reads the slot-ctx tans) builds render constants (fog/atmosphere-style terms),
  not a per-object cull.
- `ViewCtx_BuildSnapshot`'s snapshot is a stack temporary used only for a viewport rect
  (`FUN_0061b7e0`), not a cull input.

## Hard-won gotchas

- Replace a derived value where it is derived, not at one consumer's copy. The builder recomputes
  every frame and consumers read sibling products. Then exempt the consumers that must stay stock
  at their own private copy (the camera clearance).
- `debug_watch` on a struct's first fields catches block copies (`rep movsd`, ESI just past the
  watched dword). That is how the camera consumer was found after static xref hunting came up
  empty.
- Raw byte decode beats decompiler output in SecuROM-mutated regions. The decompiler
  constant-folds memory loads and shifts stack offsets (BuildCameraConstants: decompiler
  `stack0x24` = raw `[esp+0x30]`). Verify hook sites against raw bytes, and hook only exact
  instruction boundaries; one byte off crashes at boot.
- Multi-DR watchpoint attribution under Wine is unreliable (`field=?`). EIPs and registers are
  still exact, which is enough to identify readers.
- When a run "does nothing", first check that the conf took effect in the log (the round-8 `bool`
  vs `float` no-op).

## VR smearing: occluder boxes (2026-10-09, live-verified fix)

Symptom: while turning in the HMD, regions at fixed in-world seams stayed undrawn (stale colour
pixels — the game never clears colour); never in mono, not where the user looked directly.

Ruled out: terrain frustum (`debug_terrain_cull_probe`: slot 1 gets the widened 62.0°/59.7° corners
and side planes 56° apart), Z pre-pass / ZFUNC=EQUAL (`debug_zstate_probe`: the game never sets
ZFUNC; only a small depth-only pass), occlusion queries (the only `CreateQuery` is the per-frame
GPU sync).

Cause: the engine's per-object occluder box pass. `OcclusionBox_EmitPrimRecord` (0x0046edf0)
emits an `OcclusionMaterial` / `PgOcclusionVP` unit-box record per flagged object (flag
`(obj+0x12>>2)&1`, plus a list at ctx+0x1430), called from `ViewObjects_DrawAndEmitOcclusionBoxes`
(0x00468ea0) at 0x00468eed and 0x00468f12.

What the boxes are (static RE 2026-10-09, disassembly of the mutated region): **hardware occlusion
queries**. The emitter frustum-tests the object's padded AABB, appends its matrix to the per-view
instance-matrix array, allocates a 10-bit query ring id (`g_OcclusionQueryRingCounter`
0x011697c4), stores it in the object's 4-deep history (`obj+0x1c4..`), and submits the box prim
(type byte 4, sort bucket 3, material `OcclusionMaterial`, shader `PgOcclusionVP`). The VM
packet interpreter wraps the draw in the query. `ObjectOcclusionQueries_PollAndMarkVisible`
(0x00468c10, same pattern at 0x004a67c4) polls `g_OcclusionQueryArray[id]->GetData(&px, 4,
FLUSH)` for the oldest outstanding query: not-ready or px != 0 => `lastVisibleFrame = frame`;
S_OK with 0 pixels keeps the old stamp. The object is drawn only while `frame - lastVisibleFrame
<= 3` (then `flags |= 8`). No query id (0xffff) counts as visible.

Consequence: `skip_occluder_boxes` == disabling occlusion culling (extra draw calls only), which
is why nothing else changed. Why VR breaks it is still a hypothesis: (a) the replayed per-eye
pass re-issues the same ring ids before their results are read; (b) the PgOcclusionVP technique
carries no `viewContextData` register map, so its boxes use the un-rewritten game camera (union
pose, widened game projection) while the scene is drawn with the per-eye projection -> boxes land
at the wrong screen position -> 0 visible pixels -> wrongly occluded objects. (b) would be
testable by logging that technique's constant map. A proper fix (per-eye queries or the box
projection rewritten per eye) is not worth it unless draw-call cost becomes a problem.

The `debug_terrain_cull_probe` and `debug_zstate_probe` diagnostics that ruled out the terrain
frustum and a Z pre-pass were removed after the fix (git history has them; the terrain-pass facts
are in the Ghidra plates on `Terrain_BuildFrustumPlaneSet` / `Terrain_QuadtreeWalk`). The box skip
lives in `occluder_boxes.{cpp,hpp}`.

# Frustum-culling alignment

Goal: the engine's culling (and the LOD/shadow work tied to it) covers what the HMD sees instead of the game's 16:9 frustum — no pop-in when turning or looking up/down. Both halves live-verified (2026-10-07/08); the third-person camera behaves as stock.

- **Rotation**: union HMD pose written into `g_CameraTable` ([camera.md](camera.md)) — culling follows the head.
- **FOV** (always on, `src/carrier/vr/cull_frustum.cpp`): HMD frustum extents written at the view-context builder.

## The builder mechanism

`ViewContext_BuildCameraConstants` (`0x008591ac`) builds every view's camera constants once per view per frame: `halfH = fov * 0.375 * aspect`, `tanH = tan(halfH)` → ctx+0x30 (`0x00859425`), `tanV = tanH/aspect` → ctx+0x34 (`0x00859436`) — stock 36.65°/22.71° at 16:9. At `0x0085943B` (`LEA EAX,[EBX+0xB20]`, `MC2_VCCAM_TANS_READY`) everything downstream derives from XMM2/XMM0 only: the projection (ctx+0xb20 → VP → `g_ViewContextTable` records), the four frustum-corner vectors (frustum structs ctx+0x38 and ctx+0x250 via `Frustum_Build`; whole view + four shadow cascades via `ViewContext_BuildCascadeFrusta`), and the ctx tans themselves (render constants + snapshot).

The carrier MidHooks that instruction. For HMD-driven views only it sets `tanH = max(|tanH|, tan(hmdHalfH))` (same for V; signs kept) in XMM2/XMM0 and ctx+0x30/+0x34. `hmdHalf*` = the FOV union (`camera_table::get_fov_union`): both eyes' frustum-corner rays rotated into the mid pose's frame (the union camera's axes), outermost |x/z| and |y/z| over all eight. Rotating matters on canted-display headsets, where each eye's FOV is relative to an eye turned outward. There is **no margin**: the union of the two eye frustums is already what the eyes see (only the eyes' ±IPD/2 offset from the mid point, a few centimetres, is uncovered). A margin is harmful, not just unneeded: objects in it are off-screen in both eyes yet get occlusion boxes, which read as occluded — the turning smear ([occluder_boxes.md](occluder_boxes.md)). The former `cull_fov_margin` key (default 5°) was removed 2026-11-10 after a live run at 0 showed neither edge pop-in nor smearing. Each extent is shaped on its own axis; the result is never narrower than the game's frustum. Live (2026-11-10, Index-class HMD, 2016x2240 per eye): cull 57.0°/54.7°, cant 0.04°. The game's own extents are 18.5°/20.4° there, because the engine builds at the eye's render aspect, 0.9 ([render_resolution.md](render_resolution.md)); at 16:9 they were 36.7°/22.7°.

**View discrimination**: the builder works on copies of the `g_CameraTable` entries; the fill hook records every entry it rewrites (rows + position, seqlocked 8-slot history); `camera_table::is_union_camera` matches the builder's active camera entry (`[ctx+0x28]`, self-indexed 0x70 stride) against that history. Shadow/reflection/aux views never match, so they keep the game's frustum. Live (2026-11-09): four builds per frame, two widened (both carry a rewritten entry; the camera table alternates sub-entries 0.0/0.1 per frame), one other view, one without a camera.

Why the game's projection may change: for the main view the HMD never sees the game's projection — `hmd_delta` rebuilds it per eye ([view_rewrite.md](view_rewrite.md)); the monitor shows the left eye.

Accepted side effects: `view: gameproj` reports the HMD angles; shadow cascades are fitted to the wider frustum (coarser texels); screen-size LOD metrics see the wider projection; ADS zoom no longer narrows culling (the max() keeps the HMD extents).

## The consumer that must stay stock: camera clearance

`Frustum_Build` writes the frustum struct at ctx+0x38 (0x218 bytes: near `+0x00`, far `+0x04`, 8 world-space corners `+0x0c` — near 0..3 = (+H,+V)(+H,−V)(−H,−V)(−H,+V), then far; side-plane normals `+0x6c`; `MC2_FRUSTUM_*` in Ghidra). `CamCtrl_NearPlaneClearance` (`0x007107d0`) block-copies the main slot's struct and keeps running maxima of the near-plane diagonal |c2−c0| and vertical edge |c1−c0| in the controller (+0x4d0/+0x4d4) — the camera's obstacle clearance. The HMD quad's diagonal is ~3× stock, so the camera backed off from obstacles behind the player far too early (the old fov-scale experiments' pull-in above ~1.75× was THIS, not `Fov_TanIndexHelper`).

Fix: a second MidHook at `0x007107F9` (`MC2_CAMCLEAR_FRUSTUM_COPIED` — first instruction after the copy; copy at [ESP+0x18]; the source is always `[g_RenderShellPtr]+0xec8`). If the copied near quad carries the widened extents (1% corner match, measured), its four near corners are scaled about their center to the **16:9 reference extents of the game's fov**. The builder hook publishes both extent pairs through atomics; the camera runs on the game thread; only that function's private copy changes.

Why 16:9 and not the game's own extents: in VR the engine builds at the eye's render aspect (0.9), whose quad (18.5°/20.4°, near diagonal ≈ 1.0·near) is much smaller than the 16:9 one the camera was tuned against (36.7°/22.7°, ≈ 1.71·near), so restoring the game's own extents let the camera sit ~40% closer to obstacles than on a monitor. The builder's own formula gives the reference: `aspect = tanH/tanV`, `fov·0.375 = atan(tanH)/aspect`, then `tanH' = tan(fov·0.375·16/9)`, `tanV' = tanH'/(16/9)` — ADS zoom narrows it as it does on a monitor. (The render-size change caused this; the hook itself kept matching.)

Found by (2026-10-08): A/B (no pull-in with `cull_hmd_fov=off`); `debug_watch` on the slot-ctx tans (render readers only); `debug_watch` on the frustum struct catching the `rep movsd` copies — `0x00472E2F` (render thread) / `0x005314C7` = cull-side readers (should see the HMD frustum), `0x005046C3` per-player focus, `0x007107F7` the camera.

## Regression checks

HMD tracked, gameplay.

1. Boot: `cullfov: installed builder MidHook @ 0085943b` + `cullfov: installed camera-clearance MidHook @ 007107f9`.
2. Per 10 s `cullfov: window:` — `widened` ≈ two per frame; `game 18.5°/20.4° -> cull 57.0°/54.7°` (game angles follow the render aspect); `camClear 36.7°/22.7°` (unzoomed) with `restored` ≈ one per frame and `stock≈0`. `camtable: window:` ends with `fov union` (= cull) and `cant` (0 on parallel-display headsets). Failure signatures: `widened=0, otherView>0` (union-camera match failed — tolerances in `camera_table.cpp` `is_union_camera`); `camClear stock>0` while widened (copy match failed, pull-in returns); `noFov>0` (no tracked pose in the last second).
3. In the HMD: no pop-in at the edges when turning/nodding (incl. water top and bottom); no camera pull-in near obstacles.

## Open items

- Cull-side copiers `0x00472E2F`/`0x005314C7` were never named — irrelevant while the builder hook covers both.
- Occlusion culling runs per eye — [occluder_boxes.md](occluder_boxes.md).
- The builder can't be reimplemented: it is reached only through a VM thunk (`0x00506a26`) and calls VM-region code itself (`thunk_FUN_02537cb0`), so it stays original with the MidHook ([reimpl.md](reimpl.md)). `CamCtrl_NearPlaneClearance` (286 bytes, only `Float_Sqrt`) could be, which would turn the clearance MidHook into a direct call. Not done: the hook matches 1:1.

## History: what was tried and why it is gone (removed 2026-10-08, ~26 rounds; git has the code)

| Approach | Verdict |
|---|---|
| `entry_fov_scale` (patch `[0x00BEAB5C]`) / `boom_pin` (repoint the `0x0071BBC6` MULSS) | The only thing that ever moved the visible cull (scales fov BEFORE the builder's tan conversion) — but 16:9-locked (vertical stayed ~39° at 1.5×) and camera pull-in above ~1.75×. Superseded by the builder hook. |
| `cull_tan_override` + snapshot/source-ctx writes | The writes were consumed but removed no pop-in: ctx+0x30/0x34 are one frustum product of the build, read only by render constants and the snapshot — the frustum structs and corner rays stayed stock. |
| `cull_fov_widen` (ViewEntry fov triple `0x0048a97a`) | Moved nothing: the triple comes from the fixed gcam corner (v/h = 0.3), not the projection. |
| `record_fov_widen` (rewrite record VP projection rows, 3 sites) | Hooks never matched a main record; records are downstream of the builder anyway. |
| `entry_fov_decouple` | Wrong value semantics (wrote the 300.0 scale constant into fov slots) — garbage projections. |
| Entry-fov census, `cull_snap_dump`, `gcamtan` watch target | Diagnostics for the above; removed. |

Misreadings worth not repeating: `[0x00BEAB5C]` is the base fov in radians, 4:3-normalized (0.95975) — not a cosine, not "300 engine units" (`[0x00BAD260]` = 300.0 is a separate constant). `Fov_TanIndexHelper` is camera-controller boom math, not the projection's fov reader. `PgClouds_EmitViewRecords` (reads the slot-ctx tans) builds render constants (fog/atmosphere terms), not a per-object cull. `ViewCtx_BuildSnapshot`'s snapshot is a stack temporary used only for a viewport rect.

Lesson: **replace a derived value where it is DERIVED, not at one consumer's copy** (the builder recomputes every frame and consumers read sibling products); then exempt consumers that must stay stock at their own private copy. Method gotchas (raw byte decode, watchpoints): [debugging.md](debugging.md). VR smearing: [occluder_boxes.md](occluder_boxes.md).

## Unmatched views

One builder view per frame never matches the union camera (`otherView` ≈ 1 per frame in `cullfov: window`), so it keeps the game's extents. Measured (2026-11-11, `cullfov:   other view` lines): its camera is 0–25 m from the HMD camera and 25–75° off its forward axis, at times coinciding with it; which view it is is not identified. Widening it whenever its camera was within 1 m of the HMD camera (a since-removed A/B) made objects pop frame to frame, near ones included. Don't widen views that don't match.

## OPEN: sky and far-terrain flicker

Observed (2026-11-11), long-standing: a rapid flicker of the sky and of distant mountains above a certain height, also in the tank gunner seat; it persists with occlusion culling off ([occluder_boxes.md](occluder_boxes.md)). In appearance, the fog/cloud layer that normally hides the distant mountains is what flickers. Relevant fact: `PgClouds_EmitViewRecords` reads the view's frustum tangents (ctx+0x30/+0x34) as render constants, and the builder hook widens those ([reverse_engineering/view_frustum.md](reverse_engineering/view_frustum.md)).

# Frustum-culling alignment

Goal: the engine's culling (and the LOD/shadow work tied to it) covers what the HMD sees instead of the game's 16:9 frustum — no pop-in when turning or looking up/down. Both halves live-verified (2026-10-07/08); the third-person camera behaves as stock.

- **Rotation**: union HMD pose written into `g_CameraTable` ([camera.md](camera.md)) — culling follows the head.
- **FOV** (always on, `src/carrier/vr/cull_frustum.cpp`): HMD frustum extents written at the view-context builder.

## The builder mechanism

`ViewContext_BuildCameraConstants` (reimplemented, `reimpl/render/build_camera_constants.cpp`) builds every view's camera constants once per view per frame: `halfH = fov * 0.375 * aspect`, `tanH = tan(halfH)` → ctx+0x30, `tanV = tanH/aspect` → ctx+0x34 (stock 36.65°/22.71° at 16:9). Everything downstream derives from those two values only: the projection (ctx+0xb20 → VP → `g_ViewContextTable` records), the four frustum-corner vectors (frustum structs ctx+0x38 and ctx+0x250 via `Frustum_Build`; whole view + four shadow cascades via `ViewContext_BuildCascadeFrusta`), and the ctx tans themselves (render constants + snapshot).

The builder calls `cull_frustum::on_tans_ready` right after computing them. For HMD-driven views only it sets `tanH = max(|tanH|, tan(hmdHalfH))` (same for V; signs kept), in the builder's values and ctx+0x30/+0x34. `hmdHalf*` = the FOV union (`camera_table::get_fov_union`): both eyes' frustum-corner rays rotated into the mid pose's frame (the union camera's axes), outermost |x/z| and |y/z| over all eight. Rotating matters on canted-display headsets, where each eye's FOV is relative to an eye turned outward. There is **no margin**: the union of the two eye frustums is already what the eyes see (only the eyes' ±IPD/2 offset from the mid point, a few centimetres, is uncovered). A margin is harmful, not just unneeded: objects in it are off-screen in both eyes yet get occlusion boxes, which read as occluded — the turning smear ([occluder_boxes.md](occluder_boxes.md)). Without one there is neither edge pop-in nor smearing. Each extent is shaped on its own axis; the result is never narrower than the game's frustum. On an Index-class HMD (2016x2240 per eye): cull 57.0°/54.7°, cant 0.04°. The game's own extents are 18.5°/20.4° there, because the engine builds at the eye's render aspect, 0.9 ([render_resolution.md](render_resolution.md)); at 16:9 they are 36.7°/22.7°.

**View discrimination** (exact, by the camera's source): four builds per frame. The shell's main view (`ctx` = shell view slot 1) has its camera object pointing straight into `g_CameraTable` (slot 0). `FUN_00480640` builds a second view on a stack copy of its source view's camera; a MidHook right after that copy (`0x004806d1`) records the copy's source, thread-locally. The builder resolves its camera to the `g_CameraTable` entry it comes from and asks `camera_table::rewritten_entry` whether the fill hook rewrote that entry's address with the HMD pose in the last few frames. Those two views are widened. `FUN_00486460`'s view (a camera mirrored off the main one, most likely the water reflection: 0–6 m from the HMD camera, 35–150° off its forward axis) and shell view slot 0 (no camera) keep the game's frustum. The per-window `cullfov:   view ctx=… camera=… (…)` lines list each view, its camera's location and its widened/other/no-camera counts.

Why the game's projection may change: for the main view the HMD never sees the game's projection — `hmd_delta` rebuilds it per eye ([view_rewrite.md](view_rewrite.md)); the monitor shows the left eye.

Accepted side effects: `view: gameproj` reports the HMD angles; shadow cascades are fitted to the wider frustum (coarser texels); screen-size LOD metrics see the wider projection; ADS zoom no longer narrows culling (the max() keeps the HMD extents).

## The consumer that must stay stock: camera clearance

`Frustum_Build` writes the frustum struct at ctx+0x38 (0x218 bytes: near `+0x00`, far `+0x04`, 8 world-space corners `+0x0c` — near 0..3 = (+H,+V)(+H,−V)(−H,−V)(−H,+V), then far; side-plane normals `+0x6c`; `MC2_FRUSTUM_*` in Ghidra). `CamCtrl_NearPlaneClearance` (`0x007107d0`) block-copies the main slot's struct and keeps running maxima of the near-plane diagonal |c2−c0| and vertical edge |c1−c0| in the controller (+0x4d0/+0x4d4) — the camera's obstacle clearance. The HMD quad's diagonal is ~3× stock, so the camera backed off from obstacles behind the player far too early (the old fov-scale experiments' pull-in above ~1.75× was THIS, not `Fov_TanIndexHelper`).

Fix: a second MidHook at `0x007107F9` (`MC2_CAMCLEAR_FRUSTUM_COPIED` — first instruction after the copy; copy at [ESP+0x18]; the source is always `[g_RenderShellPtr]+0xec8`). If the copied near quad carries the widened extents (1% corner match, measured), its four near corners are scaled about their center to the **16:9 reference extents of the game's fov**. The builder hook publishes both extent pairs through atomics; the camera runs on the game thread; only that function's private copy changes.

Why 16:9 and not the game's own extents: in VR the engine builds at the eye's render aspect (0.9), whose quad (18.5°/20.4°, near diagonal ≈ 1.0·near) is much smaller than the 16:9 one the camera was tuned against (36.7°/22.7°, ≈ 1.71·near), so restoring the game's own extents let the camera sit ~40% closer to obstacles than on a monitor. The builder's own formula gives the reference: `aspect = tanH/tanV`, `fov·0.375 = atan(tanH)/aspect`, then `tanH' = tan(fov·0.375·16/9)`, `tanV' = tanH'/(16/9)` — ADS zoom narrows it as it does on a monitor. (The render-size change caused this; the hook itself kept matching.)

Found by (2026-10-08): A/B (no pull-in with `cull_hmd_fov=off`); `debug_watch` on the slot-ctx tans (render readers only); `debug_watch` on the frustum struct catching the `rep movsd` copies — `0x00472E2F` (render thread) / `0x005314C7` = cull-side readers (should see the HMD frustum), `0x005046C3` per-player focus, `0x007107F7` the camera.

## Regression checks

HMD tracked, gameplay.

1. Boot: `reimpl: ViewContext_BuildCameraConstants @ 008591a0 replaced` + `cullfov: installed camera-clearance MidHook @ 007107f9`.
2. Per 10 s `cullfov: window:` — `widened` ≈ two per frame; `game 18.5°/20.4° -> cull 57.0°/54.7°` (game angles follow the render aspect); `camClear 36.7°/22.7°` (unzoomed) with `restored` ≈ one per frame and `stock≈0`. `camtable: window:` ends with `fov union` (= cull) and `cant` (0 on parallel-display headsets). Failure signatures: `widened=0, otherView>0` (the exact view identification failed: see the per-view `cullfov:   view` lines); `camClear stock>0` while widened (copy match failed, pull-in returns); `noFov>0` (no tracked pose in the last second).
3. In the HMD: no pop-in at the edges when turning/nodding (incl. water top and bottom); no camera pull-in near obstacles.

## Open items

- The two cull-side copiers of the frustum struct: `ViewObject_DrawSubmeshes` (`0x00472E2F`: per-submesh `Frustum_TestTransformedBox` beyond 10 m) and the SecuROM-mutated terrain pass (`0x005314C7` → `Terrain_BuildFrustumPlaneSet`: left/right/near/far planes only). Both are plane tests, correct for any frustum `Frustum_Build` makes.
- Occlusion culling runs per eye — [occluder_boxes.md](occluder_boxes.md).
- The builder is reimplemented ([reimpl.md](reimpl.md)); the widening is a direct call in it, at the point the game computes the tangents. `CamCtrl_NearPlaneClearance` (286 bytes, only `Float_Sqrt`) could be too, turning the clearance MidHook into a direct call.

## History: what was tried and why it is gone (removed 2026-10-08, ~26 rounds; git has the code)

| Approach | Verdict |
|---|---|
| `entry_fov_scale` (patch `[0x00BEAB5C]`) / `boom_pin` (repoint the `0x0071BBC6` MULSS) | The only thing that ever moved the visible cull (scales fov BEFORE the builder's tan conversion) — but 16:9-locked (vertical stayed ~39° at 1.5×) and camera pull-in above ~1.75×. Superseded by the builder hook. |
| `cull_tan_override` + snapshot/source-ctx writes | The writes were consumed but removed no pop-in: ctx+0x30/0x34 are one frustum product of the build, read only by render constants and the snapshot — the frustum structs and corner rays stayed stock. |
| `cull_fov_widen` (ViewEntry fov triple `0x0048a97a`) | Moved nothing: the triple comes from the fixed gcam corner (v/h = 0.3), not the projection. |
| `record_fov_widen` (rewrite record VP projection rows, 3 sites) | Hooks never matched a main record; records are downstream of the builder anyway. |
| `entry_fov_decouple` | Wrong value semantics (wrote the 300.0 scale constant into fov slots) — garbage projections. |
| Entry-fov census, `cull_snap_dump`, `gcamtan` watch target | Diagnostics for the above; removed. |

Misreadings worth not repeating: `[0x00BEAB5C]` is the base fov in radians, 4:3-normalized (0.95975) — not a cosine, not "300 engine units" (`[0x00BAD260]` = 300.0 is a separate constant). `Fov_TanIndexHelper` is camera-controller boom math, not the projection's fov reader. `PgClouds_EmitCloudRender1`/`2` (read the slot-ctx tans) build the clouds' sky matrix from them, not a per-object cull ([below](#clouds-pass-and-the-ctx-tans)). `ViewCtx_BuildSnapshot`'s snapshot is a stack temporary used only for a viewport rect.

Lesson: **replace a derived value where it is DERIVED, not at one consumer's copy** (the builder recomputes every frame and consumers read sibling products); then exempt consumers that must stay stock at their own private copy. Method gotchas (raw byte decode, watchpoints): [debugging.md](debugging.md). VR smearing: [occluder_boxes.md](occluder_boxes.md).

## Views that are not HMD-driven

One builder view per frame is not HMD-driven (`otherView` ≈ 1 per frame in `cullfov: window`), so it keeps the game's extents: `FUN_00486460`'s mirrored camera, most likely the water reflection (see View discrimination). Its frustum is the game's narrow one, so reflections may miss objects near the edges of the HMD view (OPEN, unchecked). Widening it whenever its camera was within 1 m of the HMD camera (a since-removed A/B) made objects pop frame to frame, near ones included. Don't widen views that don't match.

## Clouds pass and the ctx tans

`PgClouds_EmitCloudRender1`/`2` map the screen to view directions with the ctx tans (ctx+0x30/+0x34), so the widened values must be written there too: with the game's own (18.5°/20.4° at the eye aspect) cloud render 2 slides with head pitch. They are stable frame to frame (one read per frame, render thread). On HMD views its sky matrices are replaced per eye with the eye's own FOV ([view_rewrite.md](view_rewrite.md#clouds)), so the widened ctx tans reach the screen only through the mono fallback (no stamp or pose).

## OPEN: scenery culled and ghost images when looking steeply up or down

Looking up steeply (around 50–60° in the HMD), parts of the scenery stop being drawn, without the stale-colour smear of an occlusion miss ([occluder_boxes.md](occluder_boxes.md)): something rejects them before drawing. Sweeping slowly through that band excludes different parts of the scene in turn, as if a single cull plane sweeps through the view; past it they come back. Separately, at steep pitch ghost images of nearby trees and terrain appear in the sky, behind all other scenery and in the wrong place, fading in until solid: drawn with a wrong camera or depth, not a culling result.

Measured (a temporary per-pitch record count): draw records per frame fall gently up to 50° and drop sharply between 50° and 60°; no single frame loses half its records.

Ruled out:
- The widened frustum passing the vertical: it would start at 90° − 54.7° ≈ 35°, and the frustum's plane tests (`Frustum_TestPoints`, the terrain pass's `Terrain_BuildFrustumPlaneSet`) are correct for any frustum `Frustum_Build` makes.
- The per-player focus point (`FUN_00504040`, camera position + 5 or 15 m ahead): its direction is already horizontal.

- Frustum culling: past 50° the extra `Frustum_TestPoints` rejections are by the view's bottom plane (`debug_cull_probe`: caller, frustum, plane by pitch), but 10° of extra vertical cull extent changed nothing, and the scenery cuts out anywhere in the view, not near the bottom edge.
- The per-eye rewrite using a wrong head pose: every main-pass camera upload matches an HMD view stamp exactly ([view_rewrite.md](view_rewrite.md#hmd-view-stamps)).
- For the ghosts: `g_ViewProjMtx` draws (the clouds' sky matrices) — the ghosts remain with all of them hidden (`debug_camera_census_skip=g_ViewProjMtx`).

- `ParticleRecord_UpdateBounds` (`0x00876a90`, formerly misnamed `ViewEntry_DeriveCullTask`): an effect update and bounds step, not a view cull.

Pitching the game camera (mouse, head level) in the HMD cuts scenery out too; with the HMD off (no widening, no per-eye passes) it doesn't. So it needs the VR rendering state, at steep pitch, but not through the plane-based cullers. Next leads: a coarse spatial or streaming query built from the camera position and forward direction (the `Matrix_MakeWorldToView` call sites that feed `Box_ExpandByLocalPoint`); `LocalToProj` draws for the ghosts (`debug_camera_census_skip=LocalToProj`).

# Frustum-culling alignment

Goal: the engine's culling (and the LOD/shadow work tied to it) covers what the HMD sees instead of the game's 16:9 frustum — no pop-in when turning or looking up/down. Both halves live-verified (2026-10-07/08); the third-person camera behaves as stock.

- **Rotation**: union HMD pose written into `g_CameraTable` ([camera.md](camera.md)) — culling follows the head.
- **FOV** (`cull_hmd_fov=on`, `src/carrier/cull_frustum.cpp`): HMD frustum extents written at the view-context builder.

## The builder mechanism

`ViewContext_BuildCameraConstants` (`0x008591ac`) builds every view's camera constants once per view per frame: `halfH = fov * 0.375 * aspect`, `tanH = tan(halfH)` → ctx+0x30 (`0x00859425`), `tanV = tanH/aspect` → ctx+0x34 (`0x00859436`) — stock 36.65°/22.71° at 16:9. At `0x0085943B` (`LEA EAX,[EBX+0xB20]`, `MC2_VCCAM_TANS_READY`) everything downstream derives from XMM2/XMM0 only: the projection (ctx+0xb20 → VP → `g_ViewContextTable` records), the four frustum-corner vectors (frustum structs ctx+0x38 and ctx+0x250 via `FUN_00857140`; whole view + four shadow cascades via `FUN_0085a3f0`), and the ctx tans themselves (render constants + snapshot).

The carrier MidHooks that instruction. For HMD-driven views only it sets `tanH = max(|tanH|, tan(hmdHalfH + margin))` (same for V; signs kept) in XMM2/XMM0 and ctx+0x30/+0x34. `hmdHalf*` = outermost half-angle across both eyes (`camera_table::get_fov_union`); margin = `cull_fov_margin` (default 5°). Each extent is shaped on its own axis (the vertical is no longer tied to 16:9); the result is never narrower than the game's frustum. Live: cull 62.0°/59.7° vs game 36.7°/22.7°.

**View discrimination**: the builder works on copies of the `g_CameraTable` entries; the fill hook records every entry it rewrites (rows + position, seqlocked 8-slot history); `camera_table::is_union_camera` matches the builder's active camera entry (`[ctx+0x28]`, self-indexed 0x70 stride) against that history. Shadow/reflection/aux views never match, so they keep the game's frustum. Live: ~one widened build per frame (the main view).

Why the game's projection may change: for the main view the HMD never sees the game's projection — `hmd_delta` rebuilds it per eye ([view_rewrite.md](view_rewrite.md)); the monitor shows the left eye.

Accepted side effects: `view: gameproj` reports the HMD angles; shadow cascades are fitted to the wider frustum (coarser texels); screen-size LOD metrics see the wider projection; ADS zoom no longer narrows culling (the max() keeps the HMD extents).

## The consumer that must stay stock: camera clearance

`FUN_00857140` writes the frustum struct at ctx+0x38 (0x218 bytes: near `+0x00`, far `+0x04`, 8 world-space corners `+0x0c` — near 0..3 = (+H,+V)(+H,−V)(−H,−V)(−H,+V), then far; side-plane normals `+0x6c`; `MC2_FRUSTUM_*` in Ghidra). `CamCtrl_NearPlaneClearance` (`0x007107d0`) block-copies the main slot's struct and keeps running maxima of the near-plane diagonal |c2−c0| and vertical edge |c1−c0| in the controller (+0x4d0/+0x4d4) — the camera's obstacle clearance. The HMD quad's diagonal is ~3× stock, so the camera backed off from obstacles behind the player far too early (the old fov-scale experiments' pull-in above ~1.75× was THIS, not `Fov_TanIndexHelper`).

Fix: a second MidHook at `0x007107F9` (`MC2_CAMCLEAR_FRUSTUM_COPIED` — first instruction after the copy; copy at [ESP+0x18]). If the copied near quad carries the widened extents (1% corner match, measured), its four near corners are scaled about their center back to the game's own extents. The builder hook publishes both extent pairs through atomics; the camera runs on the game thread; only that function's private copy changes.

Found by (2026-10-08): A/B (no pull-in with `cull_hmd_fov=off`); `debug_watch` on the slot-ctx tans (render readers only); `debug_watch` on the frustum struct catching the `rep movsd` copies — `0x00472E2F` (render thread) / `0x005314C7` = cull-side readers (should see the HMD frustum), `0x005046C3` per-player focus, `0x007107F7` the camera.

## Regression checks

Conf `view_table_inject=on view_row_rewrite=hmd_delta cull_hmd_fov=on`, HMD tracked, gameplay.

1. Boot: `cullfov: installed builder MidHook @ 0085943b (ARMED)` + `cullfov: installed camera-clearance MidHook @ 007107f9`.
2. Per 10 s `cullfov: window:` — `widened` ≈ one per frame; `game 36.7°/22.7° -> cull 62.0°/59.7°`; `camClear restored` ≈ widened/2 with `stock≈0`. Failure signatures: `widened=0, otherView>0` (union-camera match failed — tolerances in `camera_table.cpp` `is_union_camera`); `camClear stock>0` while widened (copy match failed, pull-in returns); `noFov>0` (no tracked pose in the last second).
3. In the HMD: no pop-in at the edges when turning/nodding (incl. water top and bottom); no camera pull-in near obstacles.

## Open items

- One-time crash ~20 s after an OpenXR session dropped VISIBLE→SYNCHRONIZED, with 4-target full-mode watchpoints trapping ~50k/10 s; no fault record in the carrier log; did not recur in a clean run. If it reappears without `debug_watch`, deliberately test a session drop (headset off).
- Cull-side copiers `0x00472E2F`/`0x005314C7` were never named — irrelevant while the builder hook covers both.

## History: what was tried and why it is gone (removed 2026-10-08, ~26 rounds; git has the code)

| Approach | Verdict |
|---|---|
| `entry_fov_scale` (patch `[0x00BEAB5C]`) / `boom_pin` (repoint the `0x0071BBC6` MULSS) | The only thing that ever moved the visible cull (scales fov BEFORE the builder's tan conversion) — but 16:9-locked (vertical stayed ~39° at 1.5×) and camera pull-in above ~1.75×. Superseded by the builder hook. |
| `cull_tan_override` + snapshot/source-ctx writes | The writes were consumed but removed no pop-in: ctx+0x30/0x34 are one frustum product of the build, read only by render constants and the snapshot — the frustum structs and corner rays stayed stock. |
| `cull_fov_widen` (ViewEntry fov triple `0x0048a97a`) | Moved nothing: the triple comes from the fixed gcam corner (v/h = 0.3), not the projection. |
| `record_fov_widen` (rewrite record VP projection rows, 3 sites) | Hooks never matched a main record; records are downstream of the builder anyway. |
| `entry_fov_decouple` | Wrong value semantics (wrote the 300.0 scale constant into fov slots) — garbage projections. |
| Entry-fov census, `cull_snap_dump`, `gcamtan` watch target | Diagnostics for the above; removed. |

Misreadings worth not repeating: `[0x00BEAB5C]` is the base fov in radians, 4:3-normalized (0.95975) — not a cosine, not "300 engine units" (`[0x00BAD260]` = 300.0 is a separate constant). `Fov_TanIndexHelper` is camera-controller boom math, not the projection's fov reader. `FUN_0047ded0` (reads the slot-ctx tans) builds render constants (fog/atmosphere terms), not a per-object cull. `ViewCtx_BuildSnapshot`'s snapshot is a stack temporary used only for a viewport rect.

Lesson: **replace a derived value where it is DERIVED, not at one consumer's copy** (the builder recomputes every frame and consumers read sibling products); then exempt consumers that must stay stock at their own private copy. Method gotchas (raw byte decode, watchpoints): [debugging.md](debugging.md). VR smearing: [occluder_boxes.md](occluder_boxes.md).

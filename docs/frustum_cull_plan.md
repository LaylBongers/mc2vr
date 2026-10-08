# Frustum Culling Alignment (HMD-following, HMD-FOV)

Goal: the engine's frustum culling (and the LOD/shadow work tied to it) covers what the HMD sees,
instead of the game's 16:9 widescreen frustum — no pop-in when turning or looking up/down in VR.

Two halves:

| Half | Mechanism | Status |
|---|---|---|
| Rotation (cull follows the head) | `view_table_inject=on` — union HMD pose written into `g_CameraTable` (`view_table.cpp`) | **Done**, live-verified |
| FOV (cull covers the HMD's angles) | `cull_hmd_fov=on` — HMD frustum extents written into the view-context builder (`cull_frustum.cpp`) | **Implemented 2026-10-08, awaiting live test** |

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
- the four frustum-corner vectors (±tanH, ±tanV) handed to `FUN_00857140` and `FUN_0085a3f0`
  (whole-view frustum → ctx+0x954/0xa70/0xa80; four shadow cascades → ctx+0x46c/0x8c0/0x900);
- the ctx tans read by the per-object cull (`FUN_0047ded0`) and copied by `ViewCtx_BuildSnapshot`.

The carrier MidHooks that instruction and, for HMD-driven views only, sets
`tanH = max(|tanH|, tan(hmdHalfH + margin))` (same for V; signs kept), in XMM2/XMM0 and in
ctx+0x30/+0x34. `hmdHalf*` is the outermost half-angle across both eyes (`camtable::get_fov_union`),
and the margin is `cull_fov_margin` (default 5°). Each extent is shaped on its own axis, so the
vertical is no longer tied to 16:9. The result is never narrower than the game's frustum.

**View discrimination**: the builder works on copies of the `g_CameraTable` entries. The fill hook
records every entry it rewrites (rows + position, seqlocked 8-slot history).
`camtable::is_union_camera` matches the builder's active camera entry (`[ctx+0x28]`,
self-indexed 0x70 stride) against that history. Shadow, reflection and aux views never match, so
they keep the game's frustum.

**Why the game's projection may change**: for the main view the HMD never sees the game's
projection. `view_row_rewrite=hmd_delta` decomposes every main-pass VP upload and rebuilds it with
the per-eye OpenXR FOV, which is independent of the game's a/b terms. The monitor shows the left eye
(`eye_monitor_pin`).

Expected side effects:
- `view: gameproj` reports the HMD angles instead of 36.65°/22.71°.
- Shadow cascades are fitted to the wider frustum, so shadow texels get coarser.
- Screen-size LOD metrics see the wider projection.
- ADS zoom no longer narrows culling, because the max() keeps the HMD extents.

## Verification (next live run)

Conf: `view_table_inject=on`, `view_row_rewrite=hmd_delta`, `cull_hmd_fov=on`, HMD tracked, gameplay.

1. `cullfov: installed builder MidHook @ 0085943B (ARMED)` at boot, and no crash entering gameplay.
2. `cullfov: window:`: `widened > 0` every window, `game 36.7°/22.7° -> cull ~60°/~58°`.
   - `widened=0, otherView>0` means the union-camera match failed. Check whether the builder entry
     is a transformed copy rather than a raw one; the tolerances are in `view_table.cpp`
     `is_union_camera`.
   - `noFov>0` means there was no tracked pose in the last second.
3. `view: gameproj` shows the HMD angles, and the HMD image is unchanged in framing and stereo
   (hmd_delta owns it).
4. **Acceptance**: no pop-in at the HMD edges while turning or nodding, in either axis, including
   water at the top and bottom of the view.
5. A/B: `cull_hmd_fov=off` should bring back edge pop-in. That confirms this is the operative input.

If step 4 fails while steps 2–3 hold, the cull consumes something not derived from this build.
The remaining known candidate is the per-view box from `ViewEntry_DeriveCullTask` (`0x00876a90`),
fed by the fov-independent gcam corner triple (plates on `ViewEntry_MatrixFromGlobalCam` and
`ViewEntry_DeriveCullTask`). Investigate there next, and widen at the task's own read site
(`0x00877162`–`0x0087718b`) rather than at the writer.

## History: what was tried and why it is gone (2026-10-08, ~26 rounds)

All of the following were removed from the carrier in the 2026-10-08 cleanup (git history has the
code). The Ghidra plates on the named functions keep the RE facts.

| Approach | Verdict |
|---|---|
| `entry_fov_scale` (patch `[0x00BEAB5C]`) / `boom_pin` (repoint the `0x0071BBC6` MULSS at a scaled value) | The only thing that ever moved the visible cull, because it scales the fov *before* the builder's tan conversion, which is upstream of the hook above. But the scale is 16:9-locked (vertical stayed ~39° at 1.5x), and the camera controller's boom math (`Fov_TanIndexHelper`, which reads the same fov) pulls the third-person camera in above ~1.75x. Superseded by the builder hook. |
| `cull_tan_override` (write HMD tans at `FUN_0047ded0`'s DIVSS reads) and the snapshot/source-ctx writes (rounds 16/18/21) | The writes were consumed (data-verified) but did not remove pop-in. ctx+0x30/0x34 are only one of several frustum products of the build; the corner rays and cascade frusta stayed stock. |
| `cull_fov_widen` (ViewEntry fov triple at `0x0048a97a`) | Moved nothing visible. The triple comes from the fixed gcam corner (v/h = 0.3), not the projection. |
| `record_fov_widen` (rewrite record VP projection rows, 3 sites) | Inconclusive: the hooks never matched a main record. Records are downstream of the builder anyway, so the builder hook covers them. |
| `entry_fov_decouple` | Wrong value semantics (wrote the 300.0 scale constant into fov slots), which produced garbage projections. |
| Entry-fov census, `cull_snap_dump`, `gcamtan` watch target | Diagnostics for the above. Removed. |

Earlier-round misreadings worth not repeating:
- `[0x00BEAB5C]` is the base fov in radians, 4:3-normalized (0.95975). It is not a cosine and not
  "300 engine units": `[0x00BAD260]` = 300.0 is a separate constant.
- `Fov_TanIndexHelper` is not the projection's fov reader. It is camera-controller boom math.

## Hard-won gotchas

- Raw byte decode beats decompiler output in SecuROM-mutated regions. The decompiler
  constant-folds memory loads and shifts stack offsets (BuildCameraConstants: decompiler
  `stack0x24` = raw `[esp+0x30]`). Verify hook sites against raw bytes, and hook only exact
  instruction boundaries; one byte off crashes at boot.
- Multi-DR watchpoint attribution under Wine is unreliable. Reason from single targets.
- When a run "does nothing", first check that the conf took effect in the log (the round-8 `bool`
  vs `float` no-op).
- Writing one consumer's copy of a derived value is fragile: the builder recomputes every frame,
  and other consumers read sibling products. Replace the value where it is derived.

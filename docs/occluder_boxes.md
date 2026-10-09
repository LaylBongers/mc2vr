# Occluder boxes (VR smearing fix, 2026-10-09)

Symptom (fixed, live-verified): while turning in the HMD, regions at fixed in-world seams stayed undrawn (stale colour pixels — the game never clears colour); never in mono, not where the user looked directly.

Ruled out: terrain frustum (`debug_terrain_cull_probe`: slot 1 gets the widened 62.0°/59.7° corners and side planes 56° apart); Z pre-pass / ZFUNC (`debug_zstate_probe`: the game never sets ZFUNC; only a small depth-only pass); our own occlusion queries (the only `CreateQuery` is the per-frame GPU sync).

## Cause: the engine's per-object occluder box pass

`OcclusionBox_EmitPrimRecord` (`0x0046edf0`) emits an `OcclusionMaterial` / `PgOcclusionVP` unit-box record per flagged object (flag `(obj+0x12>>2)&1`, plus a list at ctx+0x1430), called from `ViewObjects_DrawAndEmitOcclusionBoxes` (`0x00468ea0`) at `0x00468eed` / `0x00468f12`.

What the boxes are (hardware occlusion queries): the emitter frustum-tests the object's padded AABB, appends its matrix to the per-view instance-matrix array, allocates a 10-bit query ring id (`g_OcclusionQueryRingCounter` `0x011697c4`), stores it in the object's 4-deep history (`obj+0x1c4..`), and submits the box prim (type byte 4, sort bucket 3). The VM packet interpreter wraps the draw in the query. `ObjectOcclusionQueries_PollAndMarkVisible` (`0x00468c10`; same pattern at `0x004a67c4`) polls `g_OcclusionQueryArray[id]->GetData(&px, 4, FLUSH)` for the oldest outstanding query: not-ready or px != 0 ⇒ `lastVisibleFrame = frame`; S_OK with 0 pixels keeps the old stamp. The object is drawn only while `frame − lastVisibleFrame <= 3` (then `flags |= 8`). No query id (0xffff) counts as visible.

## Why VR breaks it (hypothesis, unconfirmed)

(a) the replayed per-eye pass re-issues the same ring ids before their results are read; or (b) `PgOcclusionVP` carries no `viewContextData` register map, so its boxes use the un-rewritten game camera (union pose, widened game projection) while the scene is drawn with the per-eye projection → boxes land at the wrong screen position → 0 visible pixels → wrongly occluded objects. (b) is testable by logging that technique's constant map. A proper fix (per-eye queries, or the box projection rewritten per eye) is not worth it unless draw-call cost becomes a problem.

Consequence: `skip_occluder_boxes` == disabling occlusion culling (extra draw calls only), which is why nothing else changed when it was introduced. Lives in `src/carrier/occluder_boxes.{cpp,hpp}`.

The `debug_terrain_cull_probe` / `debug_zstate_probe` diagnostics were removed after the fix (git history has them; terrain-pass facts are in the Ghidra plates on `Terrain_BuildFrustumPlaneSet` / `Terrain_QuadtreeWalk`).

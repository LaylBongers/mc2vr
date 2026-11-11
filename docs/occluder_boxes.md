# Occluder boxes (VR smearing fix, 2026-10-09)

Symptom (fixed 2026-10-09 by turning occlusion culling off; per-eye queries since 2026-11-10): while turning in the HMD, regions at fixed in-world seams stayed undrawn (stale colour pixels — the game never clears colour); never in mono, not where the user looked directly.

Ruled out: terrain frustum (`debug_terrain_cull_probe`: slot 1 gets the widened 62.0°/59.7° corners and side planes 56° apart); Z pre-pass / ZFUNC (`debug_zstate_probe`: the game never sets ZFUNC; only a small depth-only pass); our own occlusion queries (the only `CreateQuery` is the per-frame GPU sync).

## Cause: the engine's per-object occluder box pass

`OcclusionBox_EmitPrimRecord` (`0x0046edf0`) emits an `OcclusionMaterial` / `PgOcclusionVP` unit-box record per flagged object (flag `(obj+0x12>>2)&1`, plus a list at ctx+0x1430), called from `ViewObjects_DrawAndEmitOcclusionBoxes` (`0x00468ea0`) at `0x00468eed` / `0x00468f12`.

Box geometry: the unit box is `[-1,1]³`. Its instance matrix is scale (half extents of the object's local AABB `obj+0x94…+0xa8`, padded) × translate (its centre) × the object's world matrix (`obj+0x40`), stored row-major in `g_PerViewInstanceMatrices`. Its index is the record's `envIdx`, so at the draw the record's env entry (`g_EnvTable40`, set to the published buffer's matrices in `PgPrimitive_PublishFrame_body`) is the box's world matrix; `objIdx` is the query ring id (disassembly-checked, plate on `OcclusionBox_EmitPrimRecord`).

What the boxes are (hardware occlusion queries): the emitter frustum-tests the object's padded AABB, appends its matrix to the per-view instance-matrix array, allocates a 10-bit query ring id (`g_OcclusionQueryRingCounter` `0x011697c4`), stores it in the object's 4-deep history (`obj+0x1c4..`), and submits the box prim (type byte 4, sort bucket 3). The VM packet interpreter wraps the draw in the query. `ObjectOcclusionQueries_PollAndMarkVisible` (`0x00468c10`; same pattern at `0x004a67c4`) polls `g_OcclusionQueryArray[id]->GetData(&px, 4, FLUSH)` for the oldest outstanding query: not-ready or px != 0 ⇒ `lastVisibleFrame = frame`; S_OK with 0 pixels keeps the old stamp. The object is drawn only while `frame − lastVisibleFrame <= 3` (then `flags |= 8`). No query id (0xffff) counts as visible.

## Why the single query broke VR

The query draws run in our `PgPrimitive_SubmitToGPU` (view type 4: `Issue(BEGIN)`, draw, `Issue(END)` on `g_OcclusionQueryArray[rec->objIdx]`, [reimpl.md](reimpl.md)). Both eye passes issued the same query, and re-issuing `BEGIN` restarts it, so the poll only saw the RIGHT eye's count: objects visible only to the left eye were culled there. A second suspect, boxes drawn with the mono camera (a box technique without `viewContextData`), is watched by the report's `boxesNoVcd`.

Since 2026-10-09 the box calls were NOPed (occlusion culling off). That cost draw calls, and every draw is submitted twice: ~7,400 draws per frame, with the main scene dominating GPU time ([pacing.md](pacing.md)).

## Per-eye query pairs (`src/carrier/vr/occlusion_queries.{hpp,cpp}`)

Ring facts (plates on the functions; summary in `src/game/pg_primitive.hpp`): 1024 queries at RenderShell+0x2e8c, created by `FUN_0074a180` (from `SetupRenderTargets`, `CreateQuery(OCCLUSION)` ×0x400), released and NULLed by `LtiRenderer_ReleaseRenderTargets` (device release / Reset). Ring ids come only from `OcclusionBox_EmitPrimRecord`. Three GetData readers: `ObjectOcclusionQueries_PollAndMarkVisible` (`0x00468c10`), `FUN_004a6730` (`0x004a67c4`), and `OcclusionQuery_GetPixelCount` (`0x00854840`: the pixel count on S_OK, else `0xffffffff`; from `FUN_00487070`, which wants ≥ 64 pixels, and `FUN_00487610`, which wants ≠ 0 — per-object query slots at +0xcb0, likely a visibility-faded effect such as a glow/flare; with the boxes NOPed they read `0xffffffff` and always passed, even behind walls). None of the three readers waits: one `GetData(FLUSH)` per object per frame, not ready = visible, so queries never stall the CPU on the GPU. They run on the render thread in the scene build, before pass 1. (The plate's `0x00477f04`/`0x004a8b84` don't read the ring.)

Design: at the start of every `SubmitToGPU`, each ring entry the engine (re)created is replaced by a pair object: the engine's query (RIGHT, pass 2) plus a carrier query (LEFT, pass 1). `Issue` goes to the current pass's eye. `GetData` is not ready if either eye isn't, otherwise it returns the larger eye's count, so an object stays hidden only when both eyes saw none of its box, and the ≥ 64-pixel reader keeps per-eye magnitudes (a sum would double them). All three readers go through `GetData` and stay original; a combine at the readers would have meant reimplementing all three, one of them a 501-byte function. The engine's own `Release` frees both eyes, so the pairs follow the ring's lifetime.

Regression checks (HMD, gameplay), per 10 s `occlusion: window:`:
- `wrapped=1024` once after boot / a render-size switch, then 0; `createFailed=0`.
- `pairDrift` (cumulative `issuedL` − `issuedR`) at 0; `boxesNoVcd=0`. Per window `issuedL`/`issuedR` can differ when a frame straddles the report (the counters are reset by the report thread); such differences cancel over the next window, so only a lasting `pairDrift` is a desync.
- `polls hidden > 0` (culling works; `hidden=0` with `visible > 0` = boxes always pass, e.g. drawn without depth); `leftOnly` = objects the single query would have culled wrongly.
- In the HMD: no stale-colour regions while turning near seams (the old smearing). If they return, `debug_occlusion_off=on` tells whether the pairs are at fault.
- Perf: compare `pace:` / `debug_gpu_census` against `debug_occlusion_off=on` in the same spot.

Perf A/B (2026-11-10, same spot, standing still, `debug_gpu_census=on`, off = `debug_occlusion_off=on`): main draws per pass 2,540 → 2,260 (−11%); GPU pass 1 4.08 → 3.94 ms, pass 2 2.88 → 2.85 ms; pass 2 CPU 1.68 → 1.34 ms; scene-build CPU 2.77 → 3.10 ms (the polls and ~490 boxes per eye); render-gate slack 4.24 → 4.62 ms. A small net win (~0.4 ms per frame), kept on. 72% of polls are hidden but only 11% of draws go: the culled objects are cheap, and most of the CPU saved in submission is spent again in the scene build. Not a lever for 120 Hz ([pacing.md](pacing.md)). The 3-frame visibility hysteresis is stock (a newly revealed object can take a few frames to appear).

## Smearing with the pairs: the cull margin (closed 2026-11-10)

With the pairs in place the smearing came back (`wrapped=1024`, `issuedL == issuedR` ≈ 560 boxes per eye per frame, `boxesNoVcd=0`, `pending=0`, 76% of polls hidden). Ruled out statically: a camera baked into the boxes (the instance matrix is unit-box scale/translate × the object's world matrix, `0x0046f950`), and an off-main pass gate (entering view type 4 binds no target, `0x856901`, so it inherits the depth prepass RT0).

Cause: the emitter issues a box for every object in the **cull** frustum, then the eye union plus a 5° `cull_fov_margin`. A box in the margin is off-screen in both eyes and counts 0 pixels, which reads as "occluded". Stock never does that, because its cull frustum is the screen. When the head turns, those objects enter view still marked hidden and stay undrawn until a fresh visible result is polled (the oldest of 4 outstanding queries). The original 2026-10-09 smearing was found the day after the margin was added, so it was probably this too, plus the shared query.

Verdict: no margin (`cull_fov_margin` removed). The two eye frustums' union (left −57°…+46.5°, right −46.5°…+57°, same vertical) is nearly exactly the cull frustum then. Live 2026-11-10: no smearing, no edge pop-in, hidden polls 7–73% by location. `leftOnly` reached 433k polls in one 10 s window: objects only the left eye saw, which a single query would have culled. A margin could only come back if the pairs reported boxes off-screen in both eyes as not ready (= visible), which needs a per-eye on-screen test per box (the box→record matrix link is not reversed).

## OPEN: objects pop in and out with occlusion culling on

Observed (2026-11-11, HMD, occlusion culling on):
- In one area, whole objects pop in and out or leave stale-colour smears, near ones included, in first person and third person (on foot and in a car). Scenery in the tank driver seat flickers too.
- With `debug_occlusion_off=on`, both are gone.
- The same build as the 2026-11-10 check above (`ebd3eab`, same conf) shows it in that area, so it predates the carrier changes made after that check. The 2026-11-10 check's location wasn't recorded.
- In that area the report showed ~560 boxes issued per eye per frame (vs ~55 elsewhere in the same runs), `pairDrift` 0, `boxesNoVcd` 0, `pending` 0.

Tested and not the cause:
- Per-eye pair desync: `pairDrift` stays 0.
- The eye inside or within 0.6 m of a box (a guard that skipped those queries never triggered in that area).
- The tank gunner-seat flicker persists with occlusion off, so it's a different issue ([culling.md](culling.md#open-sky-and-far-terrain-flicker)).

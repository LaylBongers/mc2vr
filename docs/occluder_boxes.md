# Occluder boxes (occlusion culling in VR)

The engine culls objects with hardware occlusion queries: each flagged object draws a box inside a query, and a later frame's poll hides the object if no pixel of its box passed the depth test. In VR this needs two changes, both in the carrier-owned query ring: a query per eye, and a larger ring. Failure signature of either: regions where objects should be stay undrawn, showing stale colour (the game never clears colour), most visibly while turning.

## The engine's box pass

`OcclusionBox_EmitPrimRecord` (`0x0046edf0`) emits an `OcclusionMaterial` / `PgOcclusionVP` unit-box record per flagged object (flag `(obj+0x12>>2)&1`, plus a list at ctx+0x1430), called from `ViewObjects_DrawAndEmitOcclusionBoxes` (`0x00468ea0`) at `0x00468eed` / `0x00468f12`.

Box geometry: the unit box is `[-1,1]³`. Its instance matrix is scale (half extents of the object's local AABB `obj+0x94…+0xa8`, padded) × translate (its centre) × the object's world matrix (`obj+0x40`), stored row-major in `g_PerViewInstanceMatrices`. Its index is the record's `envIdx`, so at the draw the record's env entry (`g_EnvTable40`, set to the published buffer's matrices in `PgPrimitive_PublishFrame_body`) is the box's world matrix; `objIdx` is the query ring id. No camera is baked into the box: it is drawn with the view's `viewContextData`, rewritten per eye like any main-pass draw.

Per box, the emitter frustum-tests the object's padded AABB, appends its matrix to the per-view instance-matrix array, takes the next ring id (`g_OcclusionQueryRingCounter` `0x011697c4`, masked at `0x0046fa0d`), stores it in the object's history, and submits the box prim (type byte 4, sort bucket 3). Our `PgPrimitive_SubmitToGPU` draws it inside `Issue(BEGIN)` / `Issue(END)` on `g_OcclusionQueryArray[rec->objIdx]` ([reimpl.md](reimpl.md)); entering view type 4 binds no render target, so the boxes test against the depth prepass.

**History:** each object keeps 3 ids (`obj+0x1c4`, write index `obj+0x1c2`, read index `obj+0x1c0`), both indices advanced mod 3 once per frame in `RenderObject_BeginFrame` (`0x0046e5a0`) with read = write + 1. So the poll reads the id issued **two frames earlier**.

**Readers** (render thread, scene build, before pass 1; none waits): `ObjectOcclusionQueries_PollAndMarkVisible` (`0x00468c10`) and `FUN_004a6730` (`0x004a67c4`) call `GetData(&px, 4, FLUSH)`: not ready or px ≠ 0 ⇒ `lastVisibleFrame = frame`; S_OK with 0 pixels keeps the old stamp. The object is drawn while `frame − lastVisibleFrame <= 3`; no id (0xffff) counts as visible. `OcclusionQuery_GetPixelCount` (`0x00854840`) returns the pixel count on S_OK, else `0xffffffff`; its callers `FUN_00487070` (wants ≥ 64 pixels) and `FUN_00487610` (wants ≠ 0), likely a visibility-faded effect such as a glow or flare, keep their ids in the same kind of 3-slot history (ids `+0xcb0`, read index `+0xca8`, write index `+0xcac`, advanced in `PixelCountQueryHistory_Advance` (`0x004862c0`)): two frames from issue to read there too.

**The engine's own ring:** 1024 queries at RenderShell+0x2e8c, created by `FUN_0074a180` (from `SetupRenderTargets`), released and NULLed by `LtiRenderer_ReleaseRenderTargets` (`0x0074a850`, device release / Reset). `RenderShell_InitDrawRecordTables` points `g_OcclusionQueryArray` (`0x01163768`) at it, the pointer's only writer; everything else reads the ring through that pointer.

## Why VR needs its own ring

- **Two eyes.** Every box is drawn in both eye passes, and re-issuing a query restarts it: one query per id only measures the right eye, so objects only the left eye sees would be culled.
- **Ring size.** The HMD cull frustum ([culling.md](culling.md)) covers about 7× the stock solid angle, so far more objects get boxes: up to ~580 per frame in dense areas, against ~80 for stock. With the two-frame read latency, more than 512 boxes per frame means an id comes round again before its owner reads it, and the owner reads another object's box: 0 pixels hides it.

## The carrier ring (`src/carrier/vr/occlusion_queries.{hpp,cpp}`, hooks in `src/carrier/hooks/occlusion_queries.cpp`)

- `g_OcclusionQueryArray` points at our ring of 4096 slots: a MidHook right after `RenderShell_InitDrawRecordTables` sets the pointer (`0x0085405f`) replaces it. The emitter's id mask is patched from `0x3ff` to `0xfff`, only when both ring hooks are installed (wider ids would overrun the engine's ring).
- Each slot is a query object with one query per eye: `Issue` goes to the current pass's eye (pass 1 LEFT, pass 2 RIGHT). `GetData` is not ready while any issued eye isn't, otherwise S_OK with the larger eye's count: an object is hidden only when neither eye saw its box, and the ≥ 64-pixel reader keeps per-eye magnitudes. All readers go through `GetData` and stay original.
- An id is never handed out while it may still be read: issued in frame F, read in F+2, its history slot overwritten in F+3. A MidHook on the emitter's new id (`0x0046fa22`) gives a box `0xffff` ("no query", visible to every reader; our `SubmitToGPU` doesn't draw it) when its slot was handed out fewer than 3 frames ago: the ring is full (`ringFull`). Reads are checked the other way: a slot read less than 2 frames after it was handed out was reissued over its owner's box (`aliasedReads`). Capacity: about 1,365 boxes per frame.
- A slot without queries is never ready, which every reader treats as visible, so the ring is safe from the moment the pointer is swapped. The queries are created at the first `SubmitToGPU` after the engine set up its ring, and released at `LtiRenderer_ReleaseRenderTargets`' entry (MidHook), together with the engine's. The engine's own 1024 queries are created and released as before, unused.
- The cull frustum must have no margin beyond the eyes' union: a box off-screen in both eyes counts 0 pixels and reads as occluded, so such objects would enter view hidden ([culling.md](culling.md)).

## Regression checks

HMD, gameplay, per 10 s `occlusion: window:`:
- Boot: `occlusion: carrier ring installed (4096 slots, per-eye queries)`.
- `created=1` after boot and after each render-target setup (with `released=1` before it on a render-size switch), then 0; `createFailed=0`.
- `pairDrift` (cumulative `issuedL` − `issuedR`) at 0; `boxesNoVcd=0` (else boxes are drawn with the mono camera). Per window `issuedL`/`issuedR` can differ by a frame that straddles the report; only a lasting `pairDrift` is a desync.
- `ringFull=0` (else raise the ring size) and `aliasedReads=0` (else objects read other objects' boxes: wrong hides, smears).
- `polls hidden > 0` (culling works; `hidden=0` with `visible > 0` = the boxes always pass, e.g. drawn without depth); `leftOnly` = objects only the left eye saw.
- In the HMD, in a dense area (`issuedL` above ~5,000 per 10 s): no undrawn objects or stale-colour smears, near ones included.
- `debug_occlusion_off=on` (NOPs both emitter calls) turns occlusion culling off, to tell whether a visual issue comes from it.

## Cost

Measured in one spot, standing still, against `debug_occlusion_off=on`: main draws per pass −11%; GPU per pass −0.1 ms; submission CPU (pass 2) −0.3 ms; scene-build CPU +0.3 ms (the polls and ~490 boxes per eye). A small net win (~0.4 ms per frame). Most polls come back hidden, but the culled objects are cheap, and most of the CPU saved in submission is spent again in the scene build: not a lever for 120 Hz ([pacing.md](pacing.md)). The 3-frame visibility hysteresis is stock: a newly revealed object can take a few frames to appear.

# View rewrite (the `viewContextData` channel)

Code: `src/carrier/vr/view_rewrite.cpp`. The visible view lives in the VS constant `viewContextData` (layout: VP rows 0..3, optional camPos, optional world-fixed extra row, row-major, `clip_i = dot(VP_row_i, worldpos)`; the count-6 extra row is left alone — RE: [view_context_records.md](reverse_engineering/view_context_records.md)).

## Register map

Exact registers come from the game's own resolver, not shape matching: a MidHook at the upload gate (`0x00855a78`, `cmp [edi+0xd8],0` in `PgPrimitive_SubmitToGPU`, EDI = current technique) publishes the resolved map (`+0xd4`/`+0xd8` viewContextData reg/count, `+0xdc`/`+0xe0` ViewProj reg/count); the device hook runs immediately after on the same thread.

## Modes

- `hmd_delta` (always on): per-eye OpenXR-FOV projection + per-eye position/rotation delta relative to the union pose the uploaded VP carries ([HMD view stamps](#hmd-view-stamps)), applied on main-pass VP uploads via `vp_camera::decompose` + `apply_eye` on the register map. Without a tracked HMD the uploads pass through.
- `debug_view_identity=on` (oracle, formerly `hmd_identity`): rebuilds the game's own camera and logs the max residual — the one-shot consistency check; also the calibration input for the camtable row-sign measurement.

Historical verification modes — `on`/`pulse` (row-shift pans), `stereo` (±right·IPD/2 A/B with the right-axis cache, `view_asym` NDC shift, `view_stereo_hold`), `hmd` (full-VP replacement) — were REMOVED 2026-10-07, superseded by the union injection + `hmd_delta` (git history has them). The math below survives as the machinery `hmd_delta` runs on.

## Math

- World-space pan `D = (dx,dy,dz)`: camPos `xyz += D`, and every VP row `w -= dot(row.xyz, D)` (rigid world shift on screen; a uniform clip-w shift does NOT work — the divide scales it per-vertex). The camera right axis is `normalize(row0.xyz)` (view row0 = camera right; refresh on main-pass row0 uploads only — offscreen passes must not seed it).
- Full VP decomposition/rebuild: `clip = [a·x_v + c·z_v, b·y_v + d·z_v, A·z_v + B, z_v]` with `x_v/y_v/z_v = dot(R/U/F, p−C)`. So rows are `row0 = aR + cF`, `row1 = bU + dF`, `row2 = A·F`, `row3 = F` (xyz), with `w = −dot(xyz, C)` (row2.w additionally `+B`). Decompose: `F = row3.xyz` (must be unit — else pass the technique through and count it), `R,a` from row0 minus its F component, `U,b` likewise, `c,d` the F components, `A = row2·F` (row2 must be ∥ F), `C` from solving `clip.x=clip.y=clip.w=0` (3×3 Cramer, independent of the optional camPos row), `B = row2.w + row2·C`. Pure math: `src/carrier/vr/vp_camera.{hpp,cpp}` on `src/common/vec_math.hpp`; native test `tools/test/test_vp_camera.cpp` covers decompose/rebuild round trip, identity eye, eye-shift/head-turn directions, scale.

## HMD view stamps

When the view builder finishes an HMD-driven view ([culling.md](culling.md): identified by its camera's source), it stamps that view's VP with the union pose and HMD sample of the camera-table entry its camera came from (`view_rewrite::on_hmd_view_built`, an 8-entry seqlocked ring: two HMD views per frame, a few frames back). Every record the main pass uploads is an exact copy of a view's VP, made by several writers (the builder's own record, `Scene_EmitViewPrimitives`' copies), and `viewContextData` carries its transpose. So an upload is looked up by exact equality with a stamp's transposed VP: the pose the delta is taken against is the one that VP was built with, whichever writer made the record and however late it is uploaded. An upload without a stamp isn't an HMD view and passes through. `view/hmd: HMD views hit=… notHmd=…` counts both per window; the camera table returns the newest rewrite of an entry address (the same entries are rewritten every frame or two).

## Pose age

The frame is built (view, culling, records) with the HMD sample the camera-table fill took; its eye passes render with the OpenXR frame's newer views. The build-time sample is 2 OpenXR frames older (~22 ms at 90 Hz); the head turns 0.2–1.5° on average between the two, up to ~15° in fast turns (`view/hmd: poseAge`, once per frame). `hmd_delta`'s per-pass eye delta absorbs that rotation: per-eye camera data produced at build time needs the same late, rotation-only correction per pass, or it adds that latency.

## Explicit camera matrices

Shaders without `viewContextData` take the camera as `LocalToProj` / `g_ViewProjMtx` (CTAB names, `vr/shader_constants`). These get `D·M`, where `D` maps this pass's mono clip space to the eye's (`D·VP_mono = VP_eye`). `D` is built from the two cameras' parameters (`vp_camera::clip_delta`: `P_eye · T · P_mono⁻¹`, with `T` the eye's rigid offset from the mono camera in camera space and `P⁻¹` in closed form), not by inverting the world-space VP: its ~1e3-unit translations would round into every corrected matrix. An identity eye gives `D = I` to the axes' float rounding, wherever the camera is (`tools/test/test_vp_camera.cpp`).

**Camera-relative matrices** are the exception: rows 0, 1 and 3 have w = 0 (the camera at the origin of the input space), row 3 is the unit forward axis, and row 2 is the matrix's own depth row `A'·F, B'`. They are rebuilt directly from this pass's eye camera at the origin (eye rotation + per-eye projection, no translation), keeping `A'`/`B'`. The only ones seen are the clouds/fog pass's sky matrices (`PgClouds_EmitViewRecords` → tech 84 `g_ViewProjMtx` c12, `PgClouds_EmitFogLayer` → tech 85 `g_ViewProjMtx` c0: the fog layer drawn into main with scene depth, LESSEQUAL, no z-write). The `view/hmd: camera matrices` line counts them (`cameraRelative`, ≈ 16 rows per frame).

Why not `D·M` for them: `D` maps the main view's clip space, so `D·M` reinterprets M's depth with the main view's depth terms. The result takes the main view's depth slope (`A` ≈ 1.0004 instead of `A'` = 1.0000019), and distant fog (out to `cQuad3.x` = 7000) lands past the far plane: clipped, a sky / distant-mountain flicker. (With the inverse-VP `D` it was also float rounding of the same size, varying per frame.) Producing these matrices per eye at their source (the clouds emitters) would retire the special case. `debug_gui_census=clouds` shows these records' constants before and after the rewrite in both eye passes. The ctx tans the pass reads are covered in [culling.md](culling.md#cloudsfog-pass-and-the-ctx-tans).

Rewrites are done on a **scratch copy** returned to the driver call — the game's upload buffer may alias the persistent per-view record and is never modified. `view_world_scale` scales the HMD position offset (units are metres, s = 1.0 — [camera.md](camera.md)).

## Pass gate

Shadow-map, reflection and other offscreen passes upload their own `viewContextData` (the shadow pass's "camera" is the light); shifting them moved shadow maps relative to receivers (visible shadow fading). The rewrite applies only while RT0 (observed via device SetRenderTarget, slot 37) has the engine's LIVE render size (RenderShell `+0x2b98/+0x2b9c` = `g_ScreenWidth/Height`, read per check). (A size cached once goes stale after a resolution change and silently stops all rewriting — `rewritten rows=0`.) Seen RT0 sizes: 2560x1440 main; skipped: 1024x4096 shadow atlas, 512², 128², 64², 853x480, and the 1280x720 → 1x1 downsample chain. Caveat: an offscreen pass with exactly the render size would be shifted (none seen). Keying on RT identity (`g_RT_MainColorSurf` + the composite front) is the stricter alternative. Shadow receivers look up in world space (the VS passes world position to the PS) — eye-invariant.

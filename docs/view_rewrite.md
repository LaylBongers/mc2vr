# View rewrite (the `viewContextData` channel)

Code: `src/carrier/view_rewrite.cpp`. The visible view lives in the VS constant `viewContextData` (layout: VP rows 0..3, optional camPos, optional world-fixed extra row, row-major, `clip_i = dot(VP_row_i, worldpos)`; the count-6 extra row is left alone — RE: [view_context_records.md](reverse_engineering/view_context_records.md)).

## Register map

Exact registers come from the game's own resolver, not shape matching: a MidHook at the upload gate (`0x00855a78`, `cmp [edi+0xd8],0` in `PgPrimitive_SubmitToGPU`, EDI = current technique) publishes the resolved map (`+0xd4`/`+0xd8` viewContextData reg/count, `+0xdc`/`+0xe0` ViewProj reg/count); the device hook runs immediately after on the same thread.

## Modes (`view_row_rewrite`)

- `off`
- `hmd_delta` (live mode): per-eye OpenXR-FOV projection + per-eye position/rotation delta relative to the same-frame union pose ([camera.md](camera.md)), applied on main-pass VP uploads via `vp_camera::decompose` + `apply_eye` on the register map.
- `hmd_identity` (oracle): rebuilds the game's own camera and logs the max residual — the one-shot consistency check; also the calibration input for the camtable row-sign measurement.

Historical verification modes — `on`/`pulse` (row-shift pans), `stereo` (±right·IPD/2 A/B with the right-axis cache, `view_asym` NDC shift, `view_stereo_hold`), `hmd` (full-VP replacement) — were REMOVED 2026-10-07, superseded by the union injection + `hmd_delta` (git history has them). The math below survives as the machinery `hmd_delta` runs on.

## Math

- World-space pan `D = (dx,dy,dz)`: camPos `xyz += D`, and every VP row `w -= dot(row.xyz, D)` (rigid world shift on screen; a uniform clip-w shift does NOT work — the divide scales it per-vertex). The camera right axis is `normalize(row0.xyz)` (view row0 = camera right; refresh on main-pass row0 uploads only — offscreen passes must not seed it).
- Full VP decomposition/rebuild: `clip = [a·x_v + c·z_v, b·y_v + d·z_v, A·z_v + B, z_v]` with `x_v/y_v/z_v = dot(R/U/F, p−C)`. So rows are `row0 = aR + cF`, `row1 = bU + dF`, `row2 = A·F`, `row3 = F` (xyz), with `w = −dot(xyz, C)` (row2.w additionally `+B`). Decompose: `F = row3.xyz` (must be unit — else pass the technique through and count it), `R,a` from row0 minus its F component, `U,b` likewise, `c,d` the F components, `A = row2·F` (row2 must be ∥ F), `C` from solving `clip.x=clip.y=clip.w=0` (3×3 Cramer, independent of the optional camPos row), `B = row2.w + row2·C`. Pure math: `src/carrier/vp_camera.{hpp,cpp}` on `src/common/vec_math.hpp` (shared with the host); native test `tools/test/test_vp_camera.cpp` covers decompose/rebuild round trip, identity eye, eye-shift/head-turn directions, scale.

Rewrites are done on a **scratch copy** returned to the driver call — the game's upload buffer may alias the persistent per-view record and is never modified. `view_world_scale` scales the HMD position offset (units are metres, s = 1.0 — [camera.md](camera.md)).

## Pass gate

Shadow-map, reflection and other offscreen passes upload their own `viewContextData` (the shadow pass's "camera" is the light); shifting them moved shadow maps relative to receivers (visible shadow fading). The rewrite applies only while RT0 (observed via device SetRenderTarget, slot 37) has the backbuffer size. Seen RT0 sizes: 2560x1440 main; skipped: 1024x4096 shadow atlas, 512², 128², 64², 853x480, and the 1280x720 → 1x1 downsample chain. Caveat: an offscreen pass with exactly the backbuffer size would be shifted (none seen); if render resolution ever differs from the backbuffer, key the gate on RT identity. Shadow receivers look up in world space (the VS passes world position to the PS) — eye-invariant.

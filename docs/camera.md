# HMD camera (union injection + per-eye delta)

The implemented camera system, live-verified 2026-10-07. Two parts:

1. **Union injection at `g_CameraTable`** (`view_table_inject=on`, `src/carrier/camera_table.cpp`): MidHook at `0x0070AEF8` — the instruction after the fill's `Matrix_Copy3x4` in `CameraTable_FillFromPose` (`0x0070ae50`) — rewriting the JUST-filled entry (keyed by EAX = entry+0x10; table is 5 slots × 0x620, a slot is live iff its `+0x1e0` camera-object ptr exists). Composes the HMD-union pose (mid-point of the two [IPC](ipc.md) eyes, sampled once per game frame). Steers the draw camera + culling + LOD together; ordering guaranteed by construction (after every fill, before every consumer — the VP builder AND the culling/fov readers, `0x0048067E` family).
2. **Per-eye at the record level** (`view_row_rewrite=hmd_delta`): per-eye projection + position/rotation delta relative to camtable's same-frame union (`camera_table::get_union`), applied on the main-pass VP uploads — [view_rewrite.md](view_rewrite.md).

## Entry convention (probe-derived — static analysis could NOT settle this)

Measured by the `debug_camtable_probe` transfer-function instrument (fixed +10° local-axis injections at the fill site, entry + rendered response logged):

- The rendered camera's axes are the entry's **ROWS**: rendered basis = (−row0, row1, row2) of the 3×3 at entry+0x10. Every earlier "columns" reading was an axis-aligned coincidence (rows ≈ columns near identity).
- Writing `E' = E·M` renders axes' = `M⁻¹·axes` — WORLD-side application of the INVERSE. The builder's 4×4 matrix inverse (`FUN_008225c0`, called at `0x008593db` between the entry and the view·proj multiply) inverts whatever rotation is composed into the entry. This one fact produced an entire misleading history of "shifting sign conventions" and pitch/roll coupling on yawed cameras.

Implemented composition for the desired aim-following LOCAL head rotation: with

```
L = (−qx, −qy, +qz, qw)   (rendered local frame: x right, y up, z FORWARD — left-handed
                           D3D pipeline, clip.w = +z_view; XR LOCAL is z-back)
```

write the closed form **`E' = S_r·L⁻¹·S_r·E`** (a LEFT multiply; `S_r = diag(measured row signs)` = diag(−1,1,1) in this game), whose composite quaternion is `qB = (qx, −qy, +qz, qw)`, with position `ΔC = (−px·r0 + py·r1 − pz·r2)·view_world_scale` (r_j = entry rows). Nod always pitches, lean always rolls, yaw always yaws — regardless of aim.

Why static analysis failed (remember for future injections): the E2b probe's `fabsf(dot)` evidence was sign- and transpose-blind; the game's projection is nonstandard (clip.w z-coefficient +1.0 LH, x-scale from a runtime tan-table — the disambiguating signs are data, not constants); the builder's inverse makes the transfer `E·M → M⁻¹` state-dependent. When symptoms keep fitting different conventions, **measure the transfer function instead of guessing**.

The runtime row-sign calibration (`camtable: camera frame MEASURED` log line; expected row0.R=−1, row1.U=+1, row2.F=+1 — a different result means the convention changed, re-run the probe) and the probe itself remain as per-run verification instruments.

## Eye-offset frame (2026-10-09 fix, live-verified)

Two `hmd_delta` bugs caused near-field focus discomfort (this was NOT the HUD — [hud.md](hud.md)): (1) the per-eye offset was expressed in XR-local space but applied along the head-rotated camera axes — now rotated into the head frame (`conj(union rot)` applied); (2) a legacy `R = −R` flip put each eye on the wrong side (pseudoscopic parallax: invisible at distance, uncomfortable inside ~2 m) — removed; the decomposed R is screen-right, no flip needed.

## World scale: s = 1.0 (CLOSED 2026-10-07)

Units are METRES: Havok world gravity is 9.8–9.81 game units/s², zero feet-convention constants in the image (evidence: [pandemic_engine.md](reverse_engineering/pandemic_engine.md) § World units). Live IPD verified end-to-end: applied parallax = ipd_runtime·s/z; asymmetric-frustum offset −451 px (predicted −460), per-eye FOV span scale 1.7%, door z agrees between width and parallax.

## Verdicts (do not re-litigate)

- The `viewContextData` records are PLAINTEXT-filled, once per frame, BOTH double-buffer bases, not per submit (E1) — the fill lives in SecuROM-MUTATED `.text` (~0x004671xx, no static callers, found by hardware watchpoints). A record-level per-eye rewrite is interference-free; the [stereo.md](stereo.md) replay pass re-reads unchanged records.
- Upstream ViewEntry pose injection does NOT steer the draw camera (E2): `pos7c4`/`quat7d4` are OUTPUT channels of the staged round-trip — every write+bump is answered by the copy-back re-asserting the VM's pose within one frame (regression slope 0.00). This also killed the old ViewEntry D1/D2 culling design.
- `g_CameraTable` (`0x014A2EE0`) is the single upstream injection point (E1b + E2b): static VA, runtime-live, plaintext-filled ~1.7/frame from the camera entity's quat+pos, read by BOTH the draw-camera builder AND the culling/fov consumers.

Full chain (plates in Ghidra; narrative: [render_path.md](reverse_engineering/render_path.md) § Draw-camera constant chain): camera entity pose (heap record) → `CameraTable_FillFromPose` → `g_CameraTable` → per-builder stack entry (clear from `g_CameraPoseClearBlock` `0x00DFBBD0`, live copy from the table, fov from static `0x00B9B688`) → VM thunk `0x00506a26` → `ViewContext_BuildCameraConstants` (`0x008591ac`, plaintext VP build) → VP → record fill (`0x018c45e0 + (bufIdx*32 + viewIdx)*0x70`, bufIdx = `[0x00ff364c]`) → upload gate → device slot 94 → GPU; the same table feeds the culling/fov consumers.

## Pose-hold (2026-10-09)

`camera_table::sample_pose` / `view_rewrite::set_pass_eye` hold the last good HMD pose for 250 ms on a failed sample (instead of dropping that frame to the head-less mono camera, which would also cull from the wrong view). `camtable: window` ends with `poseMiss read=… untracked=… insane=… held=…` (why samples failed / how many the hold bridged). ~17–20% of fills had `noPose` before the hold; the cause was never separated.

## Open

- E3 classification run (`debug_watch=addr:0x014A2EF0` full-mode) → decide the fate of the "stays hard" texgen/matViewMat derivations (if they read the table, the union injection already fixed them).
- apply_eye's remaining R-flip in/out is empirically validated (correct 3D) but theory is still pending under the rows model.

## Implementation gotchas (recurring failure modes)

- `on_set_vs_constant` must dispatch HmdDelta into `hmd_rewrite` — a missed dispatch silently passes everything through (blocks=0 with no other symptom).
- `get_union` requires an actual rewrite in the current game frame, so hmd_delta's delta never rides records whose pose didn't get the union.
- A row/column indexing bug in the final B·E multiply produced a clean unmixed L⁻¹ ("all flipped, no mixing" signature): row_i(B·E) takes the i-th component of EACH column of B, not all components of one column.

## Experiments (evidence, one line each — 2026-10-06)

- **E1**: record census via the upload-gate MidHook (`view: rec` tally): gameplay world-scale records idx 6/12 (same VP row0 = same camera, both bases in parity); the fills are plaintext (`Matrix_Copy3x4` `0x0046718c`, inline fstp `0x004673bf`); record base `g_ViewContextTable` (`0x01169774`) → double-buffered 32×0x70 array at `0x018c45e0`; walk counts cannot discriminate views — upload dominance can.
- **E1b**: fill-source scratch `0x017D04E0` is written by `ViewContext_BuildCameraConstants` (plaintext VP builder, camera object from ctx+0x28, called ONLY from the VM via the no-xref thunk `0x00506a26`); scratch cadence ~4.4/frame vs record fill ~1.0–1.7/frame.
- **E2**: inject ±right·sin + serial bumps into ALL 16 matched live views → slope 0.00, refreshes≈injects (copy-back reverts each frame). Requires `hmd_identity`/decompose to be pose-gated live.
- **E2b**: builder-entry layout decoded (0x70 stride: status, rotation rows, position, near, far, fovCos); gameplay instances are STACK-LOCAL (menu uses the static table directly); entry pose clear comes from the static zero template; the LIVE pose comes from `g_CameraTable` via another `Matrix_Copy3x4` — chain closed.

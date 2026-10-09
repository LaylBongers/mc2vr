# Stereo Injection Improvements — findings, verdicts and the decided architecture

Status: **IMPLEMENTED + LIVE-VERIFIED (2026-10-07).** The union HMD-pose injection at
`g_CameraTable` and the per-eye `hmd_delta` record rewrite are live and correct: head rotation
tracks through aim changes, HMD FOV/aspect and stereo 3D confirmed, culling/LOD follow the head.
A final cleanup pass the same day removed the superseded paths (the S2 `on`/`pulse`/`stereo`
verification modes, the `hmd` full-VP-replacement mode, the E2/E2b inject_probe module) and
was verified by a green run on the cleaned build (row-sign calibration locked at the recorded
convention, fills=rewritten, noPose=0, decompFail=0). Open items (none blocking) are in
Remaining work below. This doc started as an improvements plan after the culling RE changed
old premises; the experiment log at the bottom is the evidence for the verdicts.

## The entry convention — probe-derived final model (2026-10-07)

Eight live runs settled this (the round-by-round history lives in git history if ever
needed); what is kept here is the final model, why static analysis could not have
resolved it, and the failure modes that will recur in any future injection.

**Measured facts** (debug_camtable_probe transfer-function run — fixed +10° local-axis
injections at the fill site, entry + rendered response logged; the instrument stays in the
carrier, conf `debug_camtable_probe=on`):

1. **The rendered camera's axes are the entry's ROWS**: rendered basis = (−row0, row1, row2)
   of the 3x3 at entry+0x10 — verified exactly (3 decimals) on yawed-camera samples. Every
   earlier "columns" reading was an axis-aligned coincidence (rows ≈ columns near identity).
2. **Writing E' = E·M renders axes' = M⁻¹·axes — WORLD-side application of the INVERSE.**
   The builder's 4x4 matrix inverse (FUN_008225c0, called at 0x008593db between the entry and
   the view·proj multiply) inverts whatever rotation is composed into the entry. This one fact
   produced the entire misleading history: pure reversals on aligned cameras ("ever-shifting
   sign conventions") and pitch/roll COUPLING once the aim is yawed/pitched (world-side
   inverse rotation about the wrong axes mixes components exactly as reported).

**The implemented composition** (`src/carrier/camera_table.cpp`, verified live, round 8): to
apply the desired aim-following LOCAL head rotation

   L = (−qx, −qy, +qz, qw)   (rendered local frame = x right, y up, z FORWARD — left-handed
                             D3D pipeline, clip.w = +z_view; XR LOCAL is z-back)

write the closed form **E' = S_r·L⁻¹·S_r·E** (a LEFT multiply; S_r = diag(measured row signs)
= diag(−1,1,1) in this game), whose composite quaternion is qB = (qx, −qy, +qz, qw), with
position ΔC = (−px·r0 + py·r1 − pz·r2)·view_world_scale (r_j = entry rows). Nod always
pitches, lean always rolls, yaw always yaws — regardless of aim. The runtime row-sign
calibration ("camtable: camera frame MEASURED" log line) and the probe remain as per-run
verification instruments.

**Why static analysis failed** (worth remembering for future injections):
- The E2b probe's fabsf(dot) evidence was sign- and transpose-blind; near-identity cameras
  make rows ≈ columns and every sign hypothesis locally consistent.
- The game's projection is nonstandard: clip.w z-coefficient +1.0 (LH) and x-scale from a
  runtime tan-table — the disambiguating signs are data, not constants.
- The builder's matrix inverse makes the transfer E·M → M⁻¹ — a state-dependent error that
  no fixed sign convention can model. When symptoms keep fitting different conventions,
  measure the transfer function instead of guessing.

**Other implementation fixes made along the way** (still relevant):
- `on_set_vs_constant` must dispatch HmdDelta into `hmd_rewrite` (a missed dispatch silently
  passes everything through — blocks=0 with no other symptom).
- `get_union` requires an actual rewrite in the current game frame, so hmd_delta's per-eye
  delta never rides records whose pose didn't get the union.
- apply_eye (used by hmd_delta) flips the decomposed R in/out (empirically validated by
  correct 3D; theory still pending under the rows model — see Remaining work).
- A row/column indexing bug in the final B·E multiply produced a clean unmixed L⁻¹ (run 7's
  "all flipped, no mixing" signature) — row_i(B·E) takes the i-th component of EACH column
  of B, not all components of one column.

## The complete draw-camera chain (all hardware-watch-proven, plates in Ghidra)

```
camera entity pose (quat + pos, heap record; VALUES originate in game logic / VM)
  -> CameraTable_FillFromPose (0x0070ae50, PLAINTEXT, ~1.7/frame; from CameraTable_FillLoop
       FUN_0070f430's 5-slot loop, stride 0x620): CameraEntity_GetPoseRecord (0x0042ee50)
       -> D3DXQuaternionNormalize -> D3DXMatrixRotationQuaternion -> position + w
       -> Matrix_Copy3x4 -> g_CameraTable entry (0x014A2EE0, STATIC — call site ~0x0070AEF3)
  -> per builder call (mutated block ~0x004665xx-0x004666xx):
       CamPose_ClearEntryPose (0x004665b0) clears the stack entry's pose from the static
         zero template g_CameraPoseClearBlock (0x00DFBBD0)
       -> Matrix_Copy3x4 copies the LIVE pose from g_CameraTable -> the per-call stack
          camera entry (rotation +0x10..0x3c, position +0x40..0x4c)
       -> CamPose_FillEntryFov (0x00466615) fills fov fields from static 0x00B9B688
  -> VM'd packet interpreter (ORCHESTRATES only)
  -> VMThunk_ViewContext_BuildCameraConstants (0x00506a26, no static xref = VM-called)
  -> ViewContext_BuildCameraConstants (0x008591ac): reads the camera object (*(ctx+0x28) ->
       self-indexed 0x70-stride entries: status +0x00, rotation rows +0x10..0x3c, position
       +0x40, near +0x50, far +0x54, fovCos +0x58), reads near/far/fov + view matrix,
       builds the projection in plaintext, D3DXMatrixMultiply(view, proj) = VP
       -> Matrix_Copy3x4(VP -> scratch 0x017D04E0)      [call site 0x00859562]
  -> record fill (once per frame, BOTH double-buffer bases, NOT per submit):
       0x0046718c Matrix_Copy3x4 (idx-6 record), inline fstp 0x004673bf (idx-12)
       record = 0x018c45e0 + (bufIdx*32 + viewIdx)*0x70, bufIdx = [0x00ff364c]
  -> upload gate 0x00855a78 ([esp+0x18] = this pass's record ptr)
  -> Dx9_SetVertexShaderConstantF -> device slot 94 -> GPU
       ↘ the SAME g_CameraTable is read by the culling/fov consumers
         (0x0048067E family, global frame-ctx 0x017cf980)
```

Per-address facts live on the Ghidra plates (`g_CameraTable`, `CameraTable_FillFromPose`,
`CamPose_ClearEntryPose`, `CamPose_FillEntryFov`, `ViewContext_BuildCameraConstants`,
`g_CameraPoseClearBlock`, `ViewContextRecord_Fill_*`); the chain narrative also lives in
`reverse_engineering/render_path.md` § Draw-camera constant chain.

## Verdicts

1. **The `viewContextData` records are PLAINTEXT-filled, once per frame** (E1) — the old "VM-produced
   VP rows / no plaintext writer" claim is disproven. The fill lives in SecuROM-MUTATED `.text`
   (~0x004671xx, undefined function, no static callers — invisible to decompiler-text hunts; found by
   hardware watchpoints). Nothing rewrites the records between pass 1 and pass 2, and the S2c replay
   pass re-reads unchanged records — a record-level per-eye rewrite is interference-free.
2. **Upstream ViewEntry pose injection does NOT steer the draw camera** (E2). The ViewEntry
   `pos7c4`/`quat7d4` fields are OUTPUT channels of the staged round-trip: every serial bump is
   answered by the copy-back re-asserting the VM's pose within one frame (write+bump, into all 16
   camera-adjacent views, measured regression SLOPE 0.00). This also kills `frustum_cull_plan.md`'s
   original D1/D2 entry-injection design.
3. **`g_CameraTable` (0x014A2EE0) is the single injection point** (E1b + E2b). Static VA, runtime-live,
   plaintext-filled ~1.7/frame from the camera entity's quat+pos (via D3DX), and read by BOTH the
   draw-camera builder path AND the culling/fov consumers. It sits UPSTREAM of the output-only
   ViewEntry fields — I2 is realized, at a different address than originally hoped.

## Decided architecture (as implemented)

- **Union injection at the table** (replaces S6's ViewEntry design and the "producer side"
  hunt): MidHook at 0x0070AEF8 — the instruction after the fill's Matrix_Copy3x4 in
  CameraTable_FillFromPose — rewriting the JUST-filled entry (keyed by its EAX = entry+0x10;
  the table is 5 slots × 0x620, a slot is live iff its +0x1e0 camera-object ptr exists).
  The rewrite composes the HMD-union pose (mid-point of the two IPC eyes, sampled once per
  game frame) using the probe-derived closed form — see § The entry convention above.
  Ordering is guaranteed by construction (after every fill, before every consumer — the
  draw-camera builder AND the culling/fov readers 0x0048067E family).
- **Per-eye at the record level** (`view_row_rewrite=hmd_delta`, I1 as designed): per-eye
  OpenXR-FOV projection + per-eye position/rotation delta relative to camtable's same-frame
  union (`camera_table::get_union`), applied on the main-pass VP uploads. The old `hmd`
  full-VP-replacement mode was REMOVED 2026-10-07 — superseded by this pair
  (it double-applied the pose on top of the table union).
- **I3 as the checker, not the source**: the per-upload VP decompose remains the one-shot
  consistency oracle (`hmd_identity`), and `view_rewrite::get_game_camera` is the calibration input
  for the camtable row-sign measurement.
- **Verification instruments** (all conf-gated, default off): `debug_camtable_probe` — the
  transfer-function probe that settled the entry convention (injects fixed +10° local-axis
  rotations, logs entry + rendered response; run standing-still, no HMD needed, pair with
  `view_row_rewrite=hmd_identity`), plus the runtime row-sign calibration and the per-window
  oracles (fills≈rewritten, noPose, deltaNoUnion, decompFail); `debug_watch` for
  E3-classification runs. (The E2/E2b inject_probe module — ViewEntry entry-injection probe
  and builder-entry dump — was REMOVED 2026-10-07: E2's channel is disproven and both were
  superseded by the above.)
- The S2 verification modes (`on`/`pulse`/`stereo` row-shift pans) and the old `hmd`
  full-VP-replacement mode were REMOVED 2026-10-07 — superseded by view_table_inject +
  hmd_delta. Remaining view_row_rewrite values: off | hmd_delta | hmd_identity.

## Remaining work

1. **DONE 2026-10-07 (verified live, round 8)** — union injection + hmd_delta live and
   correct; oracles green (fills≈rewritten, noPose=0, deltaNoUnion=0, decompFail=0).
2. **DONE 2026-10-07** — fill loop mapped: 5 slots × 0x620 (FUN_0070f020 ctor), live slots
   keyed by +0x1e0; the two same-spot draw views (records idx 6/12) map to the live slots
   (slot 0.0 / 0.1 in the runtime census).
3. ~~**Measure `view_world_scale`**~~ — **STATIC VERDICT 2026-10-07: units are METRES,
   s = 1.0 is CORRECT** (Havok world gravity is 9.8–9.81 game units/s² — the Earth value:
   hkpWorld ctor `0x008d8f40` defaults the cinfo gravity magnitude to 9.81 when zero
   (const `0x00b58ff0`), and 21 stored `(0,−9.8,0)` hkClass default member vectors sit in
   .rdata; ZERO feet-convention constants (32.174/32.2/0.3048/3.28084) in the whole
   51.4 MB initialized image; plate + labels on hkpWorld_ctor, evidence in
   `reverse_engineering/pandemic_engine.md` § World units). **LIVE IPD VERIFIED
   2026-10-07** (eye-dump pair analysis): applied parallax = ipd_runtime·s/z
   end-to-end — asymmetric-frustum offset −451 px (predicted −460), per-eye FOV span
   scale 1.7%, door z agrees between width and parallax (3.5–3.9 m). s = 1.0 CLOSED
   (static + live). The near-field focus discomfort is therefore NOT scale/IPD/stereo —
   root-caused to the HUD/crosshair's crossed disparity (see docs/hud_plan.md);
4. ~~A/B `hmd_delta` vs the `hmd` upload-level fallback~~ — RETIRED 2026-10-07: the
   `hmd` mode was removed (superseded); `hmd_delta` + the union injection is the
   live-verified system.
5. ~~**Culling-fov widening**~~ — DONE 2026-10-08 (`frustum_cull_plan.md`): `cull_hmd_fov=on`
   writes the HMD frustum extents at the view-context builder's tan site 0x0085943B, from which
   the projection, cull corners and shadow cascades all derive. The third-person camera
   clearance is kept stock at 0x007107F9. Live-verified. The record-path apply_eye R-flip was REMOVED 2026-10-09: it put each eye on the wrong side (pseudoscopic, near-field discomfort); decomposed R is screen-right, so no flip is needed. The per-eye delta is also now expressed in the head frame (conj(union rot) applied) — live-verified.
6. **E3 classification run** (`debug_watch=addr:0x014A2EF0` full-mode) → decide I4's fate
   (the I4 "stays hard" texgen/matViewMat derivations — if they read the table, the union
   injection already fixed them).

### Pose-hold + poseMiss oracle (2026-10-09)

`camera_table::sample_pose` and `view_rewrite::set_pass_eye` now hold the last good HMD pose for
250 ms when a sample fails (instead of dropping that frame to the game's head-less mono camera,
which also culls from the wrong view). The `camtable: window` line ends with
`poseMiss read=… untracked=… insane=… held=…`: why samples failed (seqlock read gave up / host
reports untracked / eye failed sanity) and how many misses the hold bridged. A run showed ~17-20%
of fills with `noPose` before the hold; the cause of those misses was never separated (smearing
turned out to be unrelated — see `frustum_cull_plan.md` § VR smearing).

### Phase oracles (reading a run's log — learned 2026-10-06)

- `view: record` census: menu = idx 2/3 (immediate after attach), load-in = idx 9/10 (~+13 s),
  gameplay = idx 4,5,6,7,8,12,13 (~+20 s) — the world-scale set (4/5/6) appearing at all is the
  "truly in-world" signal.
- The menu/loading vista camera parks at ~(-1730,-33,2064) with slow rotation — a decomposed
  `camC` frozen there across a run means the world pass never engaged, regardless of what was on
  screen.
- `view/hmd: blocks=0` + `poseId=0` for whole windows = the HMD was never tracked (everything
  downstream of the hmd rewrite's decompose — matching, `get_game_camera` — is blind without it).
- `camtable: camera frame MEASURED` = the row-sign calibration locked (expected
  row0.R=−1, row1.U=+1, row2.F=+1 in this build; a different result means the convention
  changed — re-run the probe before trusting the union pose).

## Experiment log (evidence — 2026-10-06, one line of method per result)

- **E1 (runs 1-3)** — record census via the upload-gate MidHook (`view: record/...`, `view: rec ...`
  tally lines): gameplay's world-scale records are idx 6 and idx 12 (60-87k main-RT uploads/10s each,
  same VP row0 = same camera, both double-buffer bases in parity — the base flips per frame);
  shadow atlas idx 7/8; idx 5 = exactly-once-per-submit main+offscreen pair; idx 4/13/11 minor;
  idx 2/3/9/10 transient. Walk counts cannot discriminate views (all 24 walked uniformly) —
  upload dominance can. Watch (write mode) on idx-6/12 row0, both bases: the fills are PLAINTEXT —
  `Matrix_Copy3x4` call 0x0046718c (idx-6) + inline fstp 0x004673bf (idx-12), once per frame, both
  bases, not per submit. Record base resolved statically: g_ViewContextTable (0x01169774) holds a
  POINTER to the double-buffered 32x0x70 array at 0x018c45e0 + frameIdx*0xe00 (initializer
  FUN_00854da8 0x00854e6d, sole plaintext xref); the per-pass record pointer is [esp+0x18] at the
  upload gate.
- **E1b** — watch the fill-source scratch 0x017D04E0: written by `Matrix_Copy3x4` at 0x00859562 from
  a STACK-LOCAL matrix inside `ViewContext_BuildCameraConstants` (0x008591ac) — the plaintext VP
  builder (camera object from ctx+0x28, projection built in plaintext, D3DX view×proj multiply),
  called ONLY from the VM via the no-xref thunk 0x00506a26. Scratch cadence ~4.4/frame (per active
  view) vs record fill ~1.0-1.7/frame.
- **E2 (6 runs; the probe)** — match live type-2 views against the decomposed game camera
  (position within 100 units; slot0 basis match is unreliable — live views' slot0 is IDENTITY or
  zero, and an identity basis false-positives against axis-aligned cameras), inject
  ±right·A·sin oscillation + serial bumps (D2 protocol) into ALL 16 matched views. RESULT: SLOPE
  0.00 (phase-immune regression of the decomposed RAW draw camera vs the injected offset) across
  static-camera windows; refreshes≈injects (copy-back reverts each frame). Camera pos carriers are
  the idx 19-34 family (20-23 at posd 3-7 with live quats). NOTES: requires view_row_rewrite=hmd
  AND the HMD tracked (the decompose is pose-gated); a minute of true in-world gameplay suffices.
- **E2b step 1** — builder-entry MidHook (debug_cambuilder_dump): camera object entry layout decoded
  (0x70 stride: status/rotation rows/position/near/far/fovCos — MC2_VCCAM_ENTRY_* in
  game_addresses.h); gameplay instances are STACK-LOCAL (menu uses the static g_CameraTable
  directly); the main camera entry identified by byte-match to the decomposed VP camera.
- **E2b step 2 (partially retracted)** — the entry pose writes are a CLEAR from a static zero
  template (g_CameraPoseClearBlock 0x00DFBBD0, never written at runtime), via
  CamPose_ClearEntryPose (0x004665b0) — the initial "canonical live pose global" reading was wrong
  (misled by a meaningless stack-address value snapshot). LESSONS: never trust value snapshots of
  stack addresses outside the owning call; same-EIP aggregates hide multiple callers
  (Matrix_Copy3x4's 131 call sites all watch as one EIP).
- **E2b steps 4-5** — split the fill from the clear (watch entry+0x58, untouched by the clear's 0x40
  copy): the LIVE pose comes from **g_CameraTable (0x014A2EE0)** via another Matrix_Copy3x4; fov
  fills from static 0x00B9B688 (0x00466615); the culling/fov readers (0x0048067E) read the same
  table. Then watching the table: filled by `CameraTable_FillFromPose` (FUN_0070ae50) from the
  camera entity's quat+pos (CameraEntity_GetPoseRecord 0x0042ee50 → D3DX quat→rotation),
  ~1.7/frame. Chain closed.

## Retired premises (do not re-litigate)

- "The `viewContextData` VP rows are produced by the VM'd code" — disproven (E1).
- "The producer side cannot work / ViewEntry injection steers anything" — the ViewEntry path
  specifically is dead (E2), but the producer side WORKS at `g_CameraTable` (E2b); the M3-era
  negative was an address error, not a category error.
- `frustum_cull_plan.md` D1/D2 (ViewEntry injection) — superseded by the table injection; the
  plan doc carries the resolution note.

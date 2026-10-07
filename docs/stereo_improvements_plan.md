# Stereo Injection Improvements — findings, verdicts and the decided architecture

Status: **EXPERIMENTS COMPLETE (2026-10-06); UNION INJECTION + hmd_delta (I1)
IMPLEMENTED (2026-10-07), PENDING LIVE VERIFICATION.** E1, E1b, E2 and E2b (5 steps) are
all answered; the draw-camera chain is mapped end-to-end, the injection architecture is
DECIDED (below) and now IMPLEMENTED (`src/carrier/view_table.cpp` + the `hmd_delta` record
mode in `view_rewrite.cpp`, conf `view_table_inject=on` + `view_row_rewrite=hmd_delta`);
the fill loop is mapped statically (remaining work 2 below). The 2026-10-07 first live run
found and fixed two defects (see "Live-run corrections" below). What remains is the live-run
re-verification. This doc started as an improvements plan drafted after the culling RE changed
old premises; the experiment log at the bottom is the evidence for everything above it.

## Live-run corrections (2026-10-07 — keep for the record)

**Round 2** (pitch OK, yaw flipped; FOV/aspect wrong; no stereo fusion; culling
follows the camera):

3. **hmd_delta never fired**: `on_set_vs_constant`'s dispatch only routed
   Hmd/HmdIdentity into `hmd_rewrite` — HmdDelta fell through to the legacy
   pan path and did nothing (`view/hmd: blocks=0`, `rewritten rows=0` the
   whole run, deltaNoUnion=0). That alone explains "no 3D" (both eyes got
   the identical union image, no per-eye projection/IPD) and "FOV wrong"
   (game projection untouched). Fixed: HmdDelta joins the dispatch.
4. **Camera local frame is x=LEFT**: with the record path dead, the observed
   pose behavior was pure table-union. Pitch correct + yaw flipped +
   x-translation inverted uniquely identify col0 = -R (local x = LEFT;
   right-handedness of the entry's columns with col2=B forces
   (col0, col1) = (-R, U) or (R, -U), and (R, -U) would flip pitch). The
   game's proj_xx is negative, canceling the mirror (decompose's R is still
   the physical right via the double negative — the record path is
   frame-safe). Fixed in the table composition: XR->camera x-mirror,
   q_cam = (q.x, -q.y, -q.z, q.w), v_cam = (-v.x, v.y, v.z).

**Round 1** (head turn opposite, widescreen FOV):

1. **Entry convention**: the camera-table/camera-object entry is the camera-to-WORLD
   transform — COLUMN j = local axis j, position +0x40 = positive world C.
   Builder-disassembly-proven: ViewContext_BuildCamera
   Constants copies entry rot+pos VERBATIM into ctx+0xaa0 (call 0x008592a1), then INVERTS
   it (FUN_008225c0 = Matrix_Inverse4x4, call 0x008593db) back into the view slot
   (0x008593e4) before the view×proj multiply. The E2b probe's "rows = right/up/fwd"
   reading was a transpose error invisible to its fabsf(dot) match — the first union
   implementation composed the HMD rotation onto the ROWS (= inverse rotation = head
   motion in the OPPOSITE direction). Fix: compose onto the columns (E' = E·M(q)).
2. **Widescreen FOV regression**: expected, not a bug — the game's projection (aspect from
   the RenderShell screen dims, fov from entry+0x58 fovCos) is untouched by the union
   injection BY DESIGN (the table has no projection). Fix = I1 implemented as the new
   `view_row_rewrite=hmd_delta` mode: per-eye projection from the OpenXR FOV + per-eye
   position delta (relative to camtable's same-frame union pose) applied at the record
   level on the already-union VP. `hmd` (full-pose replacement) stays as the fallback
   and now correctly warns NOT to combine with view_table_inject (double pose).

## The complete draw-camera chain (all hardware-watch-proven, plates in Ghidra)

```
camera entity pose (quat + pos, heap record; VALUES originate in game logic / VM)
  -> CameraTable_FillFromPose (0x0070ae50, PLAINTEXT, ~1.7/frame; from FUN_0070f430's loop,
       stride 0x620): CameraEntity_GetPoseRecord (0x0042ee50)
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
   camera-adjacent views, measured regression SLOPE 0.00). This also kills `frustrum_cull_plan.md`'s
   original D1/D2 entry-injection design.
3. **`g_CameraTable` (0x014A2EE0) is the single injection point** (E1b + E2b). Static VA, runtime-live,
   plaintext-filled ~1.7/frame from the camera entity's quat+pos (via D3DX), and read by BOTH the
   draw-camera builder path AND the culling/fov consumers. It sits UPSTREAM of the output-only
   ViewEntry fields — I2 is realized, at a different address than originally hoped.

## Decided architecture

- **Union injection at the table** (replaces S6's ViewEntry design and the "producer side" hunt):
  MidHook immediately after the `Matrix_Copy3x4` call in `CameraTable_FillFromPose` (~0x0070AEF3);
  rewrite the JUST-filled entry's rotation (+0x10..0x3c) and position (+0x40..0x4c) with
  game pose + HMD rotation/translation (mid-point between eyes — the union is exactly what culling
  wants; reuse `apply_hmd_eye` math + `view_world_scale`). Ordering is guaranteed by construction
  (after every fill, before every consumer). Rewrite only the entry the hooked call filled (keyed by
  its EAX) — the "inject only matched views" discipline maps to that. Optionally widen the fov
  fields (+0x50/+0x54/+0x58) for the HMD FOV union (semantics of the widened values TBD — the
  fovCos field matches the ViewEntry fovCos observation).
- **Per-eye stays at the I1 record level** — the table is once-per-frame union. I1 (record-level
  per-pass rewrite behind a conf flag, keep the upload-level path as fallback) proceeds as designed,
  but shrinks to the per-eye projection/position delta because the pose now comes from the union.
- **I3 becomes the checker, not the source**: keep the per-upload VP decompose as a one-shot
  consistency/regression oracle (`hmd_identity` style), retire it as the camera source.
- **Verification oracles**: `hmd_identity` residuals, shadow-glue per eye, culling pop-in on head
  turn (before/after the union injection), the carrier's `view: rec` census tally, and a
  post-implementation full-mode `debug_watch=addr:0x014A2EF0` run to classify the
  texgen/matViewMat readers (E3 folded in — if the I4 "stays hard" derivations read the table,
  the union injection fixes them too).
- Probe tooling is retained as `src/carrier/debug/inject_probe.cpp` (renamed from `e2_probe`):
  the entry-injection probe (negative result, kept for re-tests; its SLOPE regression verdict
  metric is the reusable injection-verification instrument) and the builder-entry camera-object
  dump (address discovery). Conf keys: `debug_entry_inject*`, `debug_cambuilder_dump` — see
  `conf/mc2vr.conf`.

## Remaining work

1. **DONE 2026-10-07** — union injection at the table implemented, conf-gated
   `view_table_inject=on` (`src/carrier/view_table.cpp`): fill-site MidHook at
   0x0070AEF8 (instruction after the fill copy), pose = shortest-path mid-point of
   the two IPC eyes sampled once per frame, composition = `vpcam::apply_eye`'s
   body+offset model without projection terms, `view_world_scale` shared. NOT
   combined with per-eye or projection changes (I1's scope). Census + noPose
   counters in the 10s `camtable: window` report; warns when
   `view_row_rewrite=hmd` double-applies the pose (until I1 lands).
2. **DONE 2026-10-07** — fill loop mapped statically (plates: `CameraTable_FillLoop`
   0x0070f430, `CameraTable_FillFromPose`): the table is **5 slots x 0x620**
   (constructed by FUN_0070f020: `FUN_00401890(&g_CameraTable,0x620,5,ctor)`); the
   loop fills every slot whose +0x1e0 camera-object ptr exists — so typically
   1-2 live slots at ~1.7 fills/frame, and the two same-spot draw views (records
   idx 6/12) map to the live slots. Injection keys on the copy's EAX = the
   just-filled entry+0x10 (self-indexed: slot + [slot+4]*0x70 + 0x10), so every
   live slot gets the union pose and no dead slot is touched. The runtime entry
   census in the `camtable: window` report confirms which slot.sub fire live.
3. **NEXT — live re-verification run** (`view_table_inject=on` + `view_row_rewrite=hmd_delta`,
   gameplay, HMD tracked): head turn in the SAME direction, draw camera follows (decompose
   residual), FOV/aspect back to the HMD's (the old hmd-mode look), culling/LOD follow the
   head (S6's original goal — pop-in gone on head turn), no revert fights (the fill is
   ~1.7/frame — the post-fill hook must win every race by construction), `camtable: window`
   fills≈rewritten and noPose=0, `view/hmd:` blocks nonzero and deltaNoUnion=0.
4. **DONE 2026-10-07** — I1 implemented as `view_row_rewrite=hmd_delta`: the record-level
   per-eye rewrite on top of the union pose (per-eye OpenXR-FOV projection + per-eye
   position delta relative to camtable's same-frame union via `camtable::get_union`);
   `hmd` (full VP replacement) kept as the fallback path. Still to do: the A/B against the
   `hmd` upload-level path with the established oracles once the live run is green.
5. E3 classification run (full-mode watch on the table entry) → decide I4's fate.
   Also open: the fov source global `0x00B9B688` (read by CamPose_FillEntryFov) —
   semantics unexplored. Moot for the DRAW path since hmd_delta replaces the projection
   at the record level, but still relevant to the CULLING consumers: they cull by the
   table's fov fields (game's widescreen fov), so objects may pop in late at the edges of
   the HMD's wider fov — if the live run shows edge pop-in, widening the table fov fields
   (+0x50/+0x54/+0x58) is the fix and needs those semantics mapped first.
6. Update `stereo_design.md` (S4-4 open items) + `frustrum_cull_plan.md` with
   implementation results; retire this doc's "remaining work" as it completes.

### Phase oracles (reading a run's log — learned 2026-10-06)

- `view: record` census: menu = idx 2/3 (immediate after attach), load-in = idx 9/10 (~+13 s),
  gameplay = idx 4,5,6,7,8,12,13 (~+20 s) — the world-scale set (4/5/6) appearing at all is the
  "truly in-world" signal.
- The menu/loading vista camera parks at ~(-1730,-33,2064) with slow rotation — a decomposed
  `camC` frozen there across a run means the world pass never engaged, regardless of what was on
  screen.
- `view/hmd: blocks=0` + `poseId=0` for whole windows = the HMD was never tracked (everything
  downstream of the hmd rewrite's decompose — matching, `get_game_camera` — is blind without it).

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
- `frustrum_cull_plan.md` D1/D2 (ViewEntry injection) — superseded by the table injection; the
  plan doc carries the resolution note.

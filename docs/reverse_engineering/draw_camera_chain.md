# Draw-camera constant chain (E1/E1b)

The `viewContextData`/VP production chain, mapped end-to-end — **all PLAINTEXT after the VM's orchestration thunk** (hardware-watch-proven 2026-10-06; plates on the named symbols in Ghidra; record system: [view_context_records.md](view_context_records.md); mod context: [../camera.md](../camera.md)).

```
camera entity pose (quat + pos, heap record; values originate in game logic/VM)
  -> CameraTable_FillFromPose (0x0070ae50, plaintext; called from CameraTable_FillLoop
     FUN_0070f430's 5-slot x 0x620 loop):
       CameraEntity_GetPoseRecord (0x0042ee50) -> D3DXQuaternionNormalize
       -> D3DXMatrixRotationQuaternion -> position + w=1.0
       -> Matrix_Copy3x4 -> g_CameraTable entry (0x014A2EE0, STATIC, ~1.7 fills/frame)
       [g_CameraTable = the SINGLE injection point: also read by the culling/fov
        consumers (0x0048067E family, global frame-ctx 0x017cf980). IMPLEMENTED +
        live-verified (2026-10-07): carrier MidHook at 0x0070AEF8 rewriting the
        just-filled entry with the HMD-union pose — see the conventions below]
  -> per builder call: CamPose_ClearEntryPose (0x004665b0) clears the stack entry's pose
     from g_CameraPoseClearBlock (0x00DFBBD0, static zero template), then a Matrix_Copy3x4
     copies the live pose from g_CameraTable -> stack entry, then CamPose_FillEntryFov
     (0x00466615) fills fov from static 0x00B9B688
  -> VM'd packet interpreter (orchestrates only)
  -> VMThunk_ViewContext_BuildCameraConstants (0x00506a26, no static xref = VM-called)
  -> ViewContext_BuildCameraConstants (0x008591ac):
       camera object = *(ctx+0x28)          (null -> identity defaults)
       near/far/fov via self-indexed obj[obj[0]*0x1c + 0x14/0x15/0x16/0x1d]
       entry rot+pos copied VERBATIM into ctx+0xaa0 (Matrix_Copy3x4 @0x008592a1)
       -> INVERTED into the view slot (FUN_008225c0 = Matrix_Inverse4x4,
          call @0x008593db, result copied back @0x008593e4)
       projection built in plaintext (LH: clip.w z-coeff +1.0 @ctx+0xb4c; m00 =
         -1/xscale from the fov tan table DAT_00cf1900, aspect from g_RenderShell
         fields 0xae6/0xae7/0xaf2/0xaf3/0xaf5 — always the SCREEN aspect's)
       D3DXMatrixMultiply(view@ctx+0xaa0, proj@ctx+0xb20) = VP matrix
       Matrix_Copy3x4(VP -> scratch 0x017D04E0)            [call site 0x00859562]
  -> record fill (once per frame, frame-build time, ~1.0-1.7 events/frame):
       scratch -> g_ViewContextTable record
       0x0046718c = Matrix_Copy3x4 (idx-6 record); 0x004673bf = inline fstp (idx-12)
       record addr = 0x018c45e0 + (bufIdx*32 + viewIdx)*0x70, bufIdx = [0x00ff364c]
       BOTH double-buffer bases written per event; NOT per submit
  -> upload gate 0x00855a78 ([esp+0x18] = this pass's record ptr)
  -> Dx9_SetVertexShaderConstantF -> device slot 94
```

## Camera VALUES enter at the camera object

- **The draw camera is external to the view system.** The plaintext consumer path (the `PgPrimitive` record walk, 0x58 stride) carries only table indices (material/technique/env/view-context/view-scale/screen) and draw params. Patching `ViewEntry` fields, staging slots, the camera ring, frame-ctx blocks or the pose-record store never moved the draw camera in the early patching experiments — re-interpreted 2026-10-06: those patches predate the serial change-gating and staged round-trip knowledge ([camera_data_flow.md](camera_data_flow.md)); **E2 (live-proven 2026-10-06): the ViewEntry pose fields are NOT a draw-camera input** — injecting pos + serial bump into ALL 16 camera-adjacent views never moved the decomposed draw camera (regression slope 0.00 across static windows); the round-trip copy-back re-asserts the VM's pose within one frame (refreshes≈injects). The `pos7c4`/`quat7d4` entry fields are OUTPUT channels.
- **Camera-object pose writer — RESOLVED (E2b complete, 2026-10-06)**: camera VALUES originate in the camera entity's quat+pos (heap record, game logic / VM-side), flow through PLAINTEXT `CameraTable_FillFromPose` (`0x0070ae50`, D3DX quat→rotation) into the STATIC `g_CameraTable` (`0x014A2EE0`), which feeds the VP builder AND the culling/fov consumers (`0x0048067E`). The single injection point is g_CameraTable post-fill — IMPLEMENTED (2026-10-07): carrier MidHook at `0x0070AEF8`, the instruction after the fill's `Matrix_Copy3x4` call; the E2-disproven ViewEntry entry-injection protocol is retired. Verdicts + implementation notes: [../camera.md](../camera.md).
- There is NO fixed-function projection — `SetTransform` is never called. The projection is folded into the `viewContextData` VP rows. The game's projection is always the screen aspect's.

## Matrix & handedness conventions (probe-proven 2026-10-07)

These could NOT be settled by static analysis (sign-blind probe evidence, runtime-signed projection coefficients) — a runtime transfer-function probe (carrier `debug_camtable_probe`) measured them; six live rounds of "sign convention" fixes were all consistent with the facts below being unknown. Details + the full derivation: [../camera.md](../camera.md) § Entry convention; plates on CameraTable_FillFromPose / ViewContext_BuildCameraConstants. Do NOT re-derive from static sign analysis.

- **LEFT-HANDED pipeline** (classic D3D LH): the projection's clip.w z-coefficient is **+1.0** (ctx+0xb4c store in the builder) ⇒ `clip.w = +z_view`, view z is positive IN FRONT ⇒ the camera local z axis is FORWARD (not backward). `m00 = -1/xscale` with the tan-table sign runtime-dependent.
- **Matrix majority / entry semantics** (the non-obvious one): the camera entries (g_CameraTable slots, builder stack entries) are row-major 3x4s (rotation + pos + w=1), but they are NOT view matrices — the builder copies the entry verbatim and INVERTS it into the view. The RENDERED camera's axes are the entry's **ROWS**: R = −row0, U = row1, F = row2. Transfer law for any rewrite: writing `E' = E·M` renders `axes' = M⁻¹·axes` (world-side application of the INVERSE). To apply a desired aim-following LOCAL rotation L, write the closed form `E' = S_r·L⁻¹·S_r·E` (S_r = diag(measured row signs) = diag(−1,1,1) in this build; composite quat `(qx,−qy,+qz,qw)` for `L = (−qx,−qy,+qz,qw)`).
- **Consequence for consumers of decomposed VP rows** (e.g. the carrier's `vp_camera::decompose`, identity-verified on real rows): its basis is consistent with the rows above only up to the projection-coefficient signs — near-identity cameras make every wrong row/column/handness reading locally consistent. Validate sign-sensitive consumers against a YAWED camera or the probe, not an aligned one.

## Record census & cadences

- **Record census** (carrier per-record upload tally, `view: rec ...` log lines): world-scale records **idx 6 and idx 12** — same VP row0 = same camera, 60–87k main-RT uploads/10s each; shadow-atlas (1024x4096) idx 7/8; idx 5 exactly-once-per-submit main+offscreen pair; idx 4/13/11 minor; idx 2/3/9/10 transient (menu/load). Stable across sessions.
- **Cadences**: scratch ~4.4 writes/frame (builder runs per active view) vs record fill ~1.0–1.7/frame — the builder runs more often than records refresh (consumption is change-gated).
- **Consequences**: record-level per-eye rewrites at pass boundaries are interference-free (the fill is once per frame, before the passes; the frame-replay pass re-reads unchanged records — consistent with its proven state-safety).

## Methodology lesson

The fill code lives in SecuROM-mutated `.text` (~0x004671xx–0x004674xx) with NO defined function and no static callers — invisible to decompiler-text static hunts (why the 2026-10-03 static writer hunt was negative). `debug_watch=addr:` on the target address is the probe that finds writers in such regions; watch the EIP + surrounding bytes, then decode by hand ([re_methodology.md](re_methodology.md)).

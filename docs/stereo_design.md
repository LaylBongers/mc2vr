# Stereo Submission Design

Design for M4+: dual-eye world rendering + HMD presentation. Consumes S0
(loop-body RE) and S1 (draw-camera hunt, `docs/s1_camera_hunt.md`).
Per-address facts live in Ghidra; this doc carries design decisions.
Mechanism rules: `docs/launcher_plan.md`. Runtime frame chain:
`docs/render_path.md`.

## Facts this design builds on

- Frame chain (producer side all plaintext, main thread only): `GameShell_FrameTick` →
  frame pipeline → `RenderQueue_SubmitWorldPackets` (`0x0048e620`, walks active
  `ViewEntry`s, publishes packet elements into the `g_RenderQueue` ring) →
  [SecuROM-VM'd packet interpreter, stub `0x0050f660` at `0x004c99f9`] →
  `RenderShell_RenderFrame` (`0x00855690`) → `BeginSubmit` → `RenderCmd_ExecuteStream`
  (`0x008569d0`, 27-opcode plaintext interpreter, ~1.5–3.4k cmds/frame) → `EndSubmit`.
- **S1 answer**: the draw camera is external to the view system; the consumer
  reads it from VM-internal state; per-eye injection happens at the GPU
  boundary (SetVertexShaderConstantF uploads). Producer-side camera channels
  are proven negative — do not re-litigate (s1_camera_hunt.md). Post-S1 RE
  (2026-10-02) made this structural: the entire plaintext consumer path — the
  `PgPrimitive` record walk — carries only table indices (material/technique/
  env/light-env/view-scale/screen) and draw params, no camera data at all; the
  camera crosses plaintext code only as interpreter-issued D3D constant uploads.
- Post-S1 classification of the constant traffic (see `pandemic_engine.md` +
  `g_MaterialTable`/`g_PrimitiveBase` plates): the position/matrix rows that
  carry the camera but move only effects (c51 shadow/LOD cascades, w≠1.0
  near-identity families) are **`PgMaterial` texture-projection (texgen)
  transforms** — shadow/reflection/sky materials project FROM the camera.
  They are not the view; the view transform remains the target.
- The camera on the GPU: position row `c21` (exact camera position; bases
  slide per shader phase) + view-matrix rows `c23–c26` (near-identity
  rotation, camera position in the translation). Rewriting w==1.0
  world-position rows in the uploads visibly moves effects (verified,
  run 16/17); the view itself is driven by the matrix rows (w≠1.0 — not
  matched by the current filter).
- Mechanisms proven at runtime: trap-based inline/Mid/Vmt installs (no
  suspension), device VmtHook surviving device-lost + `Reset`, `g_RenderShell`
  slots 4/5 claimable 1:1 with frames, Present-hook caller attribution,
  SetVertexShaderConstantF interception with in-buffer modification
  (draws consume the modification — the S2 mechanism).
- Queue: `g_RenderQueue` ring (elem 96, cap 4096; position = +0x10 low16 % cap).
  `g_RenderQueue2` carries 2D/overlay submissions.
- Special cameras (satellite) submit up to 608 views/frame; gameplay tens.
- S0: active views = intrusive list (head `DAT_00d29e60`, link `ViewEntry+0x4`);
  per-view element = three `{size, ptr}` pairs ({0x30 staging}, {0x810 entry},
  {0x680 ctx}); the consumer derefs POST-walk.
- `LtiRenderer_EndSubmit` already StretchRects RT0 → backbuffer
  (`LtiRenderer+0x3ea4`) whenever they differ (S2; plates on `g_LtiRenderer` /
  `LtiRenderer_EndSubmit`) — an existing RT→backbuffer copy path the S4
  compositor can co-opt instead of adding its own blit.

## Architecture (REVISED by S1)

**GPU-boundary per-eye injection**: the frame renders once per eye. During
the eye pass, camera constant rows in the SetVertexShaderConstantF uploads
are rewritten per eye (`pos ± right·IPD/2` + matching view-matrix rows).
The second draw pass replays the frame's command stream through the
plaintext interpreter. Compositor at `Present` delivers both eye textures
to the HMD.

Supersedes the producer-side design (duplicate each view's element with
shadow ViewEntry/staging and per-eye camera fields): S1 proved the consumer
never reads view camera data, so shadow entries cannot carry the eye
offset to the draw. The S0 staging/clone sites (`0x0048ef71`, `0x0048f013`)
remain mapped for queue-level duplication if ever needed, but camera
control is GPU-boundary only.

## Phases

### S0 — Loop-body RE — COMPLETE (2026-10-02)

All facts in Ghidra (plate comment on `SubmitWorldPackets` + site comments).
Summary: intrusive linked-list view walk; one 96-byte element per type-2/4
view = header + three `{u32 size, ptr}` pairs: `{0x30, ctx+0xc2110+idx*0x30}`
(camera staging), `{0x810, ViewEntry*}` (live entry by pointer),
`{0x680, ctx+0xd2950}` (frame-ctx block). Pairs deref POST-walk (consumer
is VM'd) — patching between passes cannot work. Viewport/RT is not a
ViewEntry field (per-record targets at consume time). Staging cap 768
elements. Iterator reload `0x0048f013` / back-edge `0x0048f01f` mapped.

### S1 — Draw-camera source hunt — COMPLETE (2026-10-02)

Answer and evidence: `docs/s1_camera_hunt.md`. Producer-side channels are
exhausted (runs 4–15); the GPU-boundary rewrite is proven (runs 16–17).

### S2 — Per-eye injection at the GPU boundary (S2a DONE, S2b VALIDATED 2026-10-03)

1. **S2a — SOLVED (static; no runtime identification needed).** The view is
   the VS constant `viewContextData`, a 4–5 register block per technique:
   count-5 = `[camPos (w==1.0) | VP row0..row3]`, count-4 = VP rows only;
   row-major, `clip_i = dot(VP_row_i, worldpos)` (proven by shader bytecode,
   `tools/shader_disasm.py`). Exact (reg,count) per technique comes from the
   game's own resolver (plate on `Technique_ResolveConstantRegisters`
   0x0085b260: viewContextData +0xd4/+0xd8, ViewProj +0xdc/+0xe0), published
   at upload time by the MidHook at the upload gate (0x00855a78,
   `MC2_VCD_UPLOAD_CMP`) — no shape heuristics (two failed: split-call
   arrival; per-technique register reuse).
2. **S2b — VALIDATED (run 22)**: rewriting exactly those registers in the
   upload buffer pans the camera correctly. Consistent pan formulas:
   camPos `x += D.x`; every VP row `w -= row.x*D.x` (generalizes to
   `w -= dot(row.xyz, D)`). Per-eye: `D = ±right*IPD/2` (~0.032m vs the
   ±4-unit verification pulse). **Per-eye asymmetric projection = editing
   the VP rows at the same site** (no FFP projection exists — `SetTransform`
   never fires; the projection is folded into viewContextData). Upstream
   source is the per-view light-env record (`g_LightEnvTable` 0x01169774,
   plate) — VM-written, hence the GPU boundary is the injection point
   (S1 conclusion upheld). IPD + pose from the HMD runtime (S4).
3. **S2c — the second draw pass**: replay the frame's command stream once
   per eye through the plaintext interpreter `RenderCmd_ExecuteStream`
   (`0x008569d0`, 27 opcodes; the M3 opcode MidHook at `0x008569f5` is
   already installed as the stream tap): buffer the frame's stream, replay
   per eye with that eye's rewritten constants and eye render targets.
   Per-draw RT/viewport switching already happens (SetViewport fires
   2–3.6k×/frame) — the replay redirects draw targets to eye RTs.
   Engineering list: stream buffering, draw-state reapplication on replay,
   RT plumbing. S0 ring/element facts are the replay's timing inputs.
   Per-object re-uploads during replay are classified (objectData =
   local→world, BoneMatrixArray = skinning — no view content).
4. **Verification modes** (`mc2vr.conf`): `gpu_boundary_rewrite=off|on|pulse`
   (w==1.0 rows — moves effects only; diagnostic) and `view_row_rewrite=
   off|on|pulse` (the exact-register camera pan — run 22). `off` =
   pass-through.

### S3 — (SUPERSEDED) First duplication via clone-at-stage

The S0 design (clone the staged element with shadow ViewEntry/staging)
assumed the entry was the draw camera source — S1 proved it is not.
Kept for reference: staging site `0x0048ef71` (count `[ESP+0x19a00]`,
array `[ESP+0x79a0]`, cap 768), re-emit fallback `0x0048f013` →
`0x0048e9d0`. The second draw pass now lives in S2c (stream replay).

### S4 — Presentation / HMD runtime

- `Present` VmtHook (proven) as the compositor entry: submit the frame to
  OpenVR/OpenXR (both eye textures, or the SBS target). Interop blit needs
  no swapchain changes — the game's Present continues to the monitor
  untouched (debug-friendly).
- Slot-5 (`PostUpdateHook`) claim as the per-frame VR orchestration point:
  sample HMD pose, marshal to the main thread before the producer loop,
  feed S2. Slot-4 (`EndOfFrameHook`) for end-of-frame bookkeeping
  (timewarp input, frame pacing — see `main_game_loop.md`).
- OpenXR preferred on Proton/RADV via wineopenxr (present in the prefix;
  verify at integration).
- UI/2D (`g_RenderQueue2`): render once; composite over both eyes in the
  compositor. Per-eye rects are compositor-owned (S0: not a producer field).
- Fallback if per-eye RTs can't differ at the D3D level per view:
  Present-hook interop blit of the single backbuffer into per-eye targets.

### S5 — Motion controls (separate track)

Follows the logic-mod track in `docs/launcher_plan.md` (XInput stubs
`0x00a64d56/0x00a64d5c`, idle-reset buffer pair `0x017d30e8`/`0x00f7fb90`
first). Pose marshal point is the slot-5 hook (S4).

## Hook inventory (proven mechanisms)

| Site | Mechanism | Phase | Status |
|---|---|---|---|
| Device `SetVertexShaderConstantF` (slot 94) | VmtHook | S1 attribution + **S2 per-eye injection** | **installed — the S2 mechanism; in-buffer modification verified visible (runs 16–17)** |
| `RenderCmd_ExecuteStream` opcode `0x008569f5` | MidHook | M3 histogram + **S2c stream tap** | installed (M3) |
| Device `Present` (17) / `Reset` (16) | VmtHook | S4 compositor / params | installed (M2) |
| `g_RenderShell` slots 4/5 | cloned-vtable claim | S4 orchestration | installed (M3, counting noop) |
| `SubmitWorldPackets` loop head `0x0048e9ea` | MidHook | M3 view aggregation | installed (M3) |
| Element staging `0x0048ef71` / iterator `0x0048f013` | MidHook | (superseded S3) | S0-mapped, uninstalled |

**Removed with the S1 closeout** (negative-result instruments; evidence in
`docs/s1_camera_hunt.md`): the patch-window state machine (A–O), consumer
brackets, walk-entry hook, pose capture family (`Pose_Copy`, f5c0 copy,
record getter, VM-getter post-calls, framecam), matrix classification, exfil,
SetTransform/SetViewport slots, vsclock/vspose controls.

**Never hook**: VM entry stub `0x0050f660`, VM pose-getter thunk
`0x0048bf00` (plaintext call sites instead), anything at `0x01a48000+`.

## Open questions

- **Material texgen stays mono for eye 2** (post-S1 RE): `PgMaterial`
  texture-projection transforms (shadow cascades, water/sky reflections,
  blob shadows) are derived CPU-side by VM'd code from the mono camera and
  uploaded via SetPixelShaderConstantF (matViewMat, wrapper slot 109 / device
  110) + material VS consts. The S2 channel rewrites VS camera rows only, so
  eye 2's projected shadows/reflections keep mono projection (skew grows
  toward the periphery; geometry itself is correct). Accept initially; a
  later SetPixelShaderConstantF VmtHook could transform identified
  camera-derived material rows by the eye delta (affine transforms, but the
  full derivation is VM'd so correctness is not guaranteed).
- Camera-row identification robustness (S2a): register bases slide per
  shader phase; identification must track them. Input: owin register
  families + dynamics analysis. Post-S1 RE: slides correlate with
  technique/pass/material population changes.
- `g_RenderQueue2` consumption timing relative to Present (compositor
  needs the 2D stream's frame timing) — extend the S1 bracket with
  queue2 counters when S4 starts.
- Frame pacing: game vsync-locked 60 Hz; HMD typically 90 Hz. Present-hook
  compositor can run at HMD cadence independently (pose extrapolation via
  the HMD runtime). Decide in S4.
- Stream replay state (S2c): which of the 27 opcodes carry draw state that
  must reset between eye passes; RT plumbing for eye targets.

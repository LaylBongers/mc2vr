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
  are proven negative — do not re-litigate (s1_camera_hunt.md).
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

### S2 — Per-eye injection at the GPU boundary (CURRENT PHASE — handoff)

1. **S2a — camera-row identification**: per frame, identify the camera
   position row and view-matrix rows in the VS uploads. Known signatures:
   position row w==1.0 holding the live camera position (c21 in static
   phases; bases slide per phase); matrix rows follow (c23–c26 family:
   near-identity rotation, camera position in the translation, w≠1.0).
   Identification input: the verification run's owin register families
   (s1_camera_hunt.md §handoff) + the existing register cache/dynamics
   analysis. Memoize per shader base (bases are phase-stable).
2. **S2b — per-eye rewrite**: in `on_set_vs_constant`
   (`src/carrier/s1_probe.cpp`), during the eye pass, rewrite the
   identified rows in the upload buffer: `pos ± right·IPD/2`, and the
   view-matrix translation rows consistently (negated/rotated position per
   the run-6 signature). Mechanism identical to the proven ambient rewrite
   (`mc2vr.conf gpu_boundary_rewrite`), narrowed to the camera rows.
   IPD + pose from the HMD runtime (S4).
3. **S2c — the second draw pass**: replay the frame's command stream once
   per eye through the plaintext interpreter `RenderCmd_ExecuteStream`
   (`0x008569d0`, 27 opcodes; the M3 opcode MidHook at `0x008569f5` is
   already installed as the stream tap): buffer the frame's stream, replay
   per eye with that eye's rewritten constants and eye render targets.
   Per-draw RT/viewport switching already happens (SetViewport fires
   2–3.6k×/frame) — the replay redirects draw targets to eye RTs.
   Engineering list: stream buffering, draw-state reapplication on replay,
   RT plumbing. S0 ring/element facts are the replay's timing inputs.
4. **Verification mode** (already implemented): `mc2vr.conf`
   `gpu_boundary_rewrite=off|on|pulse` — persistent ambient rewrite for
   visual verification; `off` restores the instrumented window sequence.

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

- Camera-row identification robustness (S2a): register bases slide per
  shader phase; identification must track them. Input: owin register
  families + dynamics analysis.
- `g_RenderQueue2` consumption timing relative to Present (compositor
  needs the 2D stream's frame timing) — extend the S1 bracket with
  queue2 counters when S4 starts.
- Frame pacing: game vsync-locked 60 Hz; HMD typically 90 Hz. Present-hook
  compositor can run at HMD cadence independently (pose extrapolation via
  the HMD runtime). Decide in S4.
- Stream replay state (S2c): which of the 27 opcodes carry draw state that
  must reset between eye passes; RT plumbing for eye targets.

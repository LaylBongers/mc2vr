# S1 — Draw-Camera Source Hunt (COMPLETE)

Objective: find the data channel that feeds the visible draw camera, as the
per-eye injection point for stereo. **Answer: there is no patchable plaintext
channel — the draw camera reaches the GPU through VM-internal state; per-eye
injection happens at the GPU boundary (SetVertexShaderConstantF uploads).**
17 runs, 2026-10-02. Run history and per-run evidence below; design consumes
the result in `stereo_design.md` §S2.

## Method

Patch windows A–O on the patch state machine in `src/carrier/s1_probe.cpp`
(5 frames each, ~1 s apart, gameplay-gated): patch one candidate channel,
watch for a visible nudge + a GPU proof (`vspatched`/`vspose`: does the
patched value appear in a shader-constant upload?). All patches in-place on
live game data unless noted.

## Structural conclusion (runs 4–15)

The visible camera is **external to the view system**. The VM'd consumer
(`0x0050f660`, called at `0x004c99f9`) never reads camera data from:
ViewEntry fields (m0–m8, pos7c4, pos7ac, quat, at any timing incl. before
every walk copy — window K), camera staging slots, camera-record ring,
frame-ctx block + subobjects, camData, the pose record store, the
Pose_Copy chain, or the frame-ctx+0x30 block. Entries/staging/ring are
derived copies that feed streaming/culling only (patching them caused
hitches, never visual camera motion). GPU proof fired zero times on all of
them. Corroborating: view 0/1 pos7c4 == the GPU camera position exactly;
the gameplay "frame camera" ptr resolves to a NaN block.

## Run log

| Run | Window(s) | Result |
|---|---|---|
| 1 | bracket, SetTransform, m[1] | consumer runs at pipeline time; SetTransform never called (shader-driven); elements are {size,ptr} descriptors |
| 2 | SetVertexShaderConstantF, A/B | world element {0x30,0x810,0x680} verified in-ring; ring pos = +0x10 low16 % cap; A/B negative |
| 3 | A–E + exfil | perf fixed (memo+budgets); windows mistimed; exfil emit dead code |
| 4 | A–E fixed | A/B/D/E negative on ~20 live views; camera-ring record layout cracked (ViewEntry* at +0x18); satellite m[0]/m[6] reach GPU exact (c8/c9/c12); main camera matrices never appear exactly (derived) |
| 5 | C fixed, F | A–F ALL negative; per-view entry field space exhausted |
| 6 | GPU proof, G, H | **vspatched = ZERO** (no patched matrix ever reaches the GPU); main camera located on GPU: c21 pos + c23–c26 view matrix, dynamic |
| 7 | I, J | no nudge; J starved — camData holds parameters, not pose; hitches = patches land in game-consumed state (streaming), not the draw |
| 8 | K (walk-entry) | K NEGATIVE (200 field writes/frame before every walk copy) → camera external to view system |
| 9 | vsclock, burst, L sites | state770∈{2,3,7} VM pose-getter path NEVER fires live (0 captures); vsclock armed (transposed) → VSCLEAN (consumer never reads patched entry fields at consume time) |
| 10 | record-store capture | store live (~300 getter calls/frame, keys never match views); record banks heap-resident; registry saturated (instrument bug); L never ran (K-end→Done bug) |
| 11 | L (key-mapped) | L ran, starved: seen=0 — d160/891e0 else branches never process active views either; getter-keyed model dead |
| 12 | Pose_Copy capture | every walked view's source captured: gameplay entries are refreshed `Pose_Copy(staging→entry)` — circular with the walk's `entry→staging`; static-phase sources = frame-camera objects (pos == GPU camera exactly) |
| 13 | staging-writer capture | zero staging writers via Pose_Copy; only two entry-writer sites ever (`0x00488fdd` activate, `0x0048f732` f5c0 restore) |
| 14 | M/N (frame camera) | patched a stale NaN block via the global — negative; global written per task call at `0x0084ae3b`, can be stale |
| 15 | M/N (live capture) | live per-call capture = same NaN block; frame-camera chain falsified → **PIVOT to GPU boundary** |
| 16 | O (GPU-boundary) | **POSITIVE — visible shift** (first in 16 runs): rewriting w==1.0 world-position rows in the VS uploads changes the render |
| 17 | ambient verify | continuous pulse (`mc2vr.conf`): confirmed by user — noticeable cyclical effect; camera itself does not move (see handoff) |

## Deliverables (post-closeout state)

The S1 hunt instrumentation was REMOVED from the carrier at closeout
(patch-window machine, consumer brackets, pose captures, classification,
exfil, SetTransform/SetViewport slots) — all of it produced negatives, and
the evidence lives in this doc + Ghidra. What the carrier keeps:

- **The GPU-boundary channel** (the S2 mechanism): `SetVertexShaderConstantF`
  VmtHook slot 94 → `on_set_vs_constant` in `src/carrier/s1_probe.cpp`.
  Ambient rewrite mode (`mc2vr.conf gpu_boundary_rewrite=off|on|pulse`)
  shifts world-position rows (w==1.0, |x|>5) in the upload buffer; never
  touches game state. `off` = pass-through.
- **VS register cache** (`g_vs_rows`) — the S2a identification input.
- The M1/M2/M3 ambient telemetry (FrameTick, Present/Reset, queue poller,
  opcode histogram, view aggregation).
- **Static RE in Ghidra** (permanent): camera-pose system decoded and named
  (pose record store `DAT_00df8d00[idx>>8] + (idx&0xff)*0x38`; `Pose_Copy`;
  `ViewManager_*`/`ViewEntry_*`; plate comments throughout).

## S2 handoff (identification data from the verification run)

- Rewriting w==1.0 rows visibly moves **effects, not the camera**: the
  visible view is driven by the view-matrix rows (c23–c26 family, run 6:
  near-identity rotation, camera position in the translation) whose w≠1.0
  — the current filter does not match them. **S2a = extend the rewrite to
  the view-matrix rows** (identify: 3–4 consecutive dynamic rows following
  a w==1.0 position row; bases slide per shader phase — runs 10/16).
- Registers observed carrying world-position rows (w==1.0, one-shot owin
  log, verification run): `c0`, `c11`, `c21` = camera position
  (−1470.16, −18.53, 2803.86 — the exact GPU-camera value); `c18` =
  (20000, 10000, 20000) sun/light; `c28`, `c213`, `c221`, `c32`, `c36`,
  `c51` = cascade of camera-line positions (shadow/LOD cascades — these
  produced the visible "effects moving"); `c58`, `c39` = small-scale
  world positions. Rewrite volume in gameplay: ~6–13k matching rows/frame.
- `c21` = camera position row (static phases; gameplay bases slide — run
  10 burst). The position row alone does not move the view (per-pixel
  effects only); the view transform is the matrix rows.
- Known-good wiring: device VmtHook slot 94 (SetVertexShaderConstantF),
  bulk + row-wise upload paths both pass through `on_set_vs_constant`
  (modify before the driver call — the draw consumes the modification).
- Do not re-litigate the producer side (runs 4–15 above). The page-guard
  write-watch on an entry/staging slot remains possible but would land in
  the VM — no patchable target.

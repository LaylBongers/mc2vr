# Stereo Injection Improvements Plan

The S4-4 HMD camera replacement works but is deliberately low-level: per-upload scratch rewrites of the
`viewContextData` VP rows inside the device `SetVertexShaderConstantF` hook, with per-upload VP
decomposition, technique register maps from the upload-gate MidHook, and shape-fragility workarounds
(split uploads, decompFail counters). The 2026-10-06 culling RE changed the premises that forced that
design. This plan captures what can now be improved, what the decisive experiments are, and what stays
hard. Status: **drafted, not implemented** — experiments E1–E3 below should run first.

## What changed (facts from the culling RE)

Evidence: `reverse_engineering/view_and_camera.md` § Camera-data accessors (all watch-proven), the
Ghidra plates referenced there.

1. **The staged camera block is a VM↔plaintext ROUND-TRIP, not one-way.** The walk stages
   entry→staged (`0x0048EC95`), and the walk's mutated tail copies staged→entry back
   (`0x0048F72D`, `Pose_Copy` semantics, serial+1). New camera-pose VALUES originate in the VM'd
   consumer; there is no plaintext writer of pose values.
2. **The whole camera pipeline is serial-gated (change-detection), not per-frame.** Copies only fire
   when source serials advance (`serial = max(src,dest)+1`).
3. **`ViewEntry` camera state is plaintext and live during gameplay**: `pos7c4`/`quat7d4`/`serial7d0`
   (pose block), `fovCos2ec`/`fovSin2f4`, slot matrices — all plaintext-writable, refreshed by the
   gated cycle above.
4. **The old negative result is re-interpreted** — "patching `ViewEntry` never moved the draw camera"
   (M3-era) was tested WITHOUT serial bumps and WITHOUT understanding the round-trip; patches died at
   the change-gate (and/or were overwritten by the next copy-back). The producer side is NOT proven
   dead for the draw camera — it is untested under the correct protocol (write + bump serial every
   frame, before the staging).
5. What remains true: the draw camera's `viewContextData` VP rows are produced ~~by the VM'd code~~
   **[CORRECTED by E1, see the E1 RESULT under Experiments: the record fill is PLAINTEXT
   (Matrix_Copy3x4 call 0x0046718c / inline fstp 0x004673bf, source scratch 0x017D04E0, once per
   frame)]** into the per-view render-context record (`g_ViewContextTable`, `0x01169774`, 0x70
   stride, indexed by `prim+0x49`; +0x00 viewContextData, +0x40 PS view consts) and reach the GPU
   as per-pass constant uploads. The record itself is plaintext memory.

## Improvements

### I1 — Record-level per-eye rewrite (replaces the per-upload scratch rewrite)

Instead of rewriting every `viewContextData` upload at the device hook, rewrite the **render-context
record itself** at pass boundaries:

- Once per frame, before pass 1: write eye-1 (LEFT) camera into the main view's record
  (VP rows, camPos, and record+0x40 PS view consts).
- Between pass 1 and pass 2 (the existing `eye_replay` pass boundary): rewrite with the eye-2 (RIGHT)
  camera.

Why this is better:

- **PS-side camera data becomes per-eye for free** where the pass uploads the whole record to the PS
  (`Dx9_SetPixelShaderConstantF(pViewContext)`): the record+0x40 view consts carry our per-eye camera.
  Today that path stays mono — a standing S4-4 open item ("PS camera data / texgen stays mono").
- **One write per pass instead of math per upload**: no scratch-copy dance, no per-technique register
  targeting for the record path, no split-upload fragility. The upload-gate MidHook remains only to
  learn WHICH record index the main pass uses (prim+0x49).
- The record is the single source both passes read — per-pass rewriting gives per-eye cameras with no
  per-upload interception at all (the device slot-94 VmtHook could be retired to observe-only).

Requirements / risks:

- Record base: RESOLVED statically (2026-10-06, E1 prep). `0x01169774` holds a
  POINTER, not the array — sole plaintext xref is the initializer (`FUN_00854da8`,
  write `0x00854e6d`): `g_ViewContextTable = 0x018c45e0 + DAT_00ff364c * 0xe00`, a
  double-buffered array of 32 records × 0x70 stride (readers all VM-side, hence no
  plaintext read xrefs). Constants in `game_addresses.h`
  (`MC2_G_VIEWCONTEXTTABLE` + stride/size). The main-pass record VA needs no
  `prim+0x49` hunt either: `[esp+0x18]` at the upload gate IS the pass's record
  pointer, and the carrier MidHook now logs each distinct record VA + index +
  pass-gate context (run 1 of E1).
- **VM write timing** (experiment E1): the VM fills the record before the passes; verify it does not
  rewrite it mid-frame/per-pass. `debug_watch=addr:<record VP row VA>` (read+write) answers this in one
  run — the tooling exists.
- Keep the main-pass gating discipline: only rewrite the record index used by the main pass
  (offscreen passes — shadow atlas, reflections — have their own records and must stay untouched;
  the existing RT-size gate logic maps onto record-index gating).
- Aliasing caveat from `view_rewrite.cpp` (the upload buffer may alias the record) becomes moot: we
  WANT the uploads to read our values.

### I2 — Upstream pose injection (decisive experiment; big payoff if it works)

Write the HMD pose into the main view's `ViewEntry` pose block (+ bump `serial7d0`) every frame, before
the walk staging — same protocol the S6 culling plan (`frustrum_cull_plan.md` D2) uses. Then check
whether the draw camera follows:

- **If YES**: the VM's camera derivation consumes the staged pose, and a single upstream injection
  would steer the draw camera AND culling AND LOD together. The S4-4 GPU/record machinery reduces to
  per-eye projection-only differences (per-eye still needs a per-pass rewrite somewhere — the
  staged/entry data is once-per-frame, not per-pass — but the per-upload decompose/rebuild disappears:
  eye cameras = ViewEntry pose + per-eye XR projection).
- **If NO**: the staged block is an output channel only (VM recomputes pose from game state); upstream
  injection stays culling-only (S6 plan unchanged) and the draw camera keeps the I1 record-level path.
  Either way the S6 culling plan is unaffected.

This is the experiment that the old "producer side cannot work" verdict prematurely closed. Direction
evidence so far: the copy-back (staged→entry) proves the VM WRITES the staged block; whether it also
READS it as input to the draw camera is unknown — E2 answers it.

### I3 — Camera source simplification (ViewEntry-direct, no VP decomposition)

Today `vp_camera` decomposes VP rows per upload to recover the game camera (basis R/U/F, pos C) —
needed because the camera was believed unreachable in plaintext. It now is: the main view's
`ViewEntry` carries live `pos7c4`/`quat7d4`/fov (plaintext, watch-proven). Proposed:

- Read the game camera once per frame from the matched ViewEntry (matching = S6 plan D1: compare the
  entry's slot-matrix basis against the decomposed VP basis once, for identification only).
- Build the HMD eye cameras directly from pose + XR angles (`apply_hmd_eye` math unchanged).
- Keep `view_row_rewrite=hmd_identity` as a regression oracle, and keep a one-shot-per-window
  decompose as a consistency check (residual logging), not as the primary source.

This removes the decompFail/split-upload fragility class entirely and shrinks the per-upload hook to a
no-op observer.

### I4 — What stays hard (unchanged expectations)

- VM-derived material constants that do NOT read the record at upload time: `matViewMat` (device slot
  109), PS `cameraPos` (c92 in material PSes), blob-shadow/water texgen matrices. If these are
  pre-derived once per frame (before the passes), I1 does not fix them; they need either their own
  upload-time rewrites (extend the existing scratch-rewrite technique to those constants) or acceptance
  as mono. E3 (below) classifies them cheaply.

## Experiments (run before implementation, all with existing tooling)

| # | Question | Method | Cost |
|---|---|---|---|
| E1 | Does the VM rewrite the render-context record per frame or per pass? When is it stable? | Prep DONE (2026-10-06): the upload-gate MidHook logs each distinct record VA + index + pass; run 1 = one gameplay run reading that log (picks the main-pass record VA), run 2 = `debug_watch=addr:<main record VP row>` (full mode); read the accessor EIPs/cadence | two runs, no new code |

E1 RESULT (2026-10-06, COMPLETE — run 3, debug_watch write-mode on idx-6/idx-12 row0, both bases):
**The premise "records are filled by the VM'd producer, no plaintext writer" is DISPROVEN.**
The record VP rows are written by PLAINTEXT code in the SecuROM-mutated .text block
~0x004671xx–0x004674xx (undefined function; the 2026-10-03 static hunt missed it because the
region has no defined function/decompilation — reachable only via mutated control flow):
- `0x0046718c` = `Matrix_Copy3x4(EAX=record, ECX=0x017D04E0)` — record addr computed as
  `0x018c45e0 + (bufIdx*32 + viewIdx)*0x70` (bufIdx from `0x00ff364c`). Fills the idx-6 record.
- `0x004673bf` = inline `fstp [eax]` — same event, fills the idx-12 record.
- Cadence: both double-buffer bases written back-to-back per event, ~1.0–1.7 events/frame at
  34 fps — **once per frame, NOT per submit** (frame_replay's second submit re-reads unchanged
  records; consistent with S2c's proven state-safety).
- Window value snapshots: all four watched row0 dwords identical — **idx 6 and idx 12 carry the
  SAME VP row0** (same camera; two same-camera views, e.g. world opaque + a second layer).

Consequences for the plan:
- **I1 is safe as designed**: nothing rewrites the record between pass 1 and pass 2; a
  pass-boundary rewrite lands after the once-per-frame fill and stays for both submits.
- **I1's rewrite targets are idx 6 AND idx 12** (both bases refresh together, so rewriting the
  current base suffices; the RT-size gate maps onto record-index gating as planned).
- **New, better option opened (E1b)**: the fill copies from a plaintext scratch source
  (`0x017D04E0`). Finding ITS writer tells us where camera VALUES enter this chain — if that
  is also plaintext (or the staged round-trip), a single upstream injection could feed the
  records, culling AND the draw camera at once (supersedes/simplifies I2+E2).
- The GPU/record path stays the proven channel until E1b/E2 say otherwise.

E1b RESULT (2026-10-06, COMPLETE — debug_watch=addr:0x017D04E0, write mode): the fill source
scratch is written by **`Matrix_Copy3x4` again** (`0x00859562`), from a STACK-LOCAL matrix, inside
**`ViewContext_BuildCameraConstants` (`0x008591ac`)** — THE plaintext draw-camera VP builder:
- Called ONLY from the VM via the no-xref thunk `VMThunk_ViewContext_BuildCameraConstants`
  (`0x00506a26`): **the VM orchestrates, plaintext does the math.**
- Reads the CAMERA OBJECT (`*(ctx+0x28)`, self-indexed `obj[obj[0]*0x1c + 0x14/15/16/1d]` =
  near/far/fov) + view matrix; builds the projection in plaintext (fov tan table, aspect from
  g_RenderShell); `D3DXMatrixMultiply(view@ctx+0xaa0, proj@ctx+0xb20)` = VP; copies it to the
  scratch.
- Scratch write cadence ~4.4/frame (per active view) vs record fill ~1.0–1.7/frame.

**The full draw-camera chain is now mapped end-to-end, all plaintext after the VM's orchestration
thunk:** camera object → VP build (0x008591ac) → scratch 0x017D04E0 → record fill (0x0046718c /
0x004673bf, both bases) → GPU upload gate (0x00855a78). Camera VALUES enter at the camera object
(ctx+0x28) — whose writer is the remaining unknown, and the I2/E2 injection candidate.

E2 is now strongly motivated: if the matched ViewEntry's pose round-trip (or this camera object)
carries the game camera, one upstream injection steers records + PS view consts + culling together.

E1 progress notes (runs 1–2): record census — gameplay's world-scale records are **idx 6 and
idx 12** (60–87k main-RT uploads/10s each, both double-buffer bases in parity — the base flips
per frame; all other records ≥10x smaller). Offscreen (shadow atlas 1024x4096): idx 7/8 (+9/10
transient). Idx 5 is an exactly-once-per-submit main+offscreen pair; idx 4/13/11 minor. Run 1 vs
run 2 censuses agree — idx assignment stable across sessions. Walk counts cannot discriminate
views (all 24 walked uniformly every frame), upload dominance can.
| E2 | Does upstream ViewEntry pose injection steer the draw camera? | Temporary `cull_probe`-style carrier feature: write big yaw + serial bump into the matched view's pose each frame; observe the rendered view (unmistakable), then revert | one run + code |
| E3 | Which PS camera constants are record-derived at upload time vs pre-derived? | After I1 (or with a debug flag that perturbs only record+0x40): watch water reflections/blob shadows per eye — visible check; or `debug_watch=addr:` on the candidate derived-constant staging | one run |

## Rollout order

1. E1 → record base + index RESOLVED (see I1); remaining: run the two-watch gameplay runs
   for VM write timing (pure RE, no further code).
2. I1 record-level per-eye rewrite behind `view_record_rewrite=on` (keep the upload-level path as
   default/fallback; A/B via `hmd_identity`-style verification: shadows glued at each eye, monitor
   image sane, PS reflections per-eye where E3 said record-derived).
3. E2 (upstream) → decide I2; if positive, fold into the S6 culling injection (single point).
4. I3 cleanup once I1 is proven (decompose becomes the checker, not the source).
5. Update `stereo_design.md` §S4-4 + `frustrum_cull_plan.md` with results; retire the contradicted
   "producer side cannot work" claim (already re-worded in `view_and_camera.md`).

## Doc reconciliation done alongside this plan

- `view_and_camera.md` § "Where the draw camera lives" — the "consumer never reads view camera data"
  and "producer-side duplication cannot work" absolutes are re-worded to point at the round-trip
  evidence and this plan's E2.

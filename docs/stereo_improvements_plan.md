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
5. What remains true: the draw camera's `viewContextData` VP rows are produced by the VM'd code into
   the per-view render-context record (`g_ViewContextTable`, `0x01169774`, 0x70 stride, indexed by
   `prim+0x49`; +0x00 viewContextData, +0x40 PS view consts) and reach the GPU as per-pass constant
   uploads. The record itself is plaintext memory.

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

- Resolve the record base first: `0x01169774` may hold the 0x70-stride array directly or a pointer to
  it (the RE doc is ambiguous; check in Ghidra + a live dump).
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
| E1 | Does the VM rewrite the render-context record per frame or per pass? When is it stable? | `debug_watch=addr:<main record VP row>` (full mode) one gameplay run; read the accessor EIPs/cadence | one run |
| E2 | Does upstream ViewEntry pose injection steer the draw camera? | Temporary `cull_probe`-style carrier feature: write big yaw + serial bump into the matched view's pose each frame; observe the rendered view (unmistakable), then revert | one run + code |
| E3 | Which PS camera constants are record-derived at upload time vs pre-derived? | After I1 (or with a debug flag that perturbs only record+0x40): watch water reflections/blob shadows per eye — visible check; or `debug_watch=addr:` on the candidate derived-constant staging | one run |

## Rollout order

1. E1 → resolve record base + index + timing (pure RE, no code risk).
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

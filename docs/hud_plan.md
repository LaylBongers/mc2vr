# HUD Depth Plan — fix the HUD/crosshair rendering in stereo

**Status: OPEN (opened 2026-10-07).** Spawned from the world-scale/IPD verification
track (closed same day): world scale s = 1.0 and the per-eye stereo pipeline are
verified end-to-end, and the near-field focus discomfort was root-caused to the HUD.
This plan is the fix track. Evidence numbers below are from the 2026-10-07 eye-dump
run (frames 11414…11566, runtime ipd 0.0647, `fovL` (−0.994 0.812 0.953 −0.954) rad).

## The finding

User-perceived, live-confirmed: **the HUD/crosshair reads as placed way too close**
("in your face"), which makes fixating near objects uncomfortable.

## Root cause (analysis; one step still to verify — see Verification)

The HUD is composited into the per-eye pass images (part of the backbuffer/eye-RT
final, before the `eye_share` blits and the monitor pin). It is screen-space drawing
that does NOT go through the per-eye camera rewrite (`hmd_delta` only rewrites
`viewContextData` camera rows), so it lands at **identical NDC coordinates in both
eyes**. But the world content is NOT at identical NDC per eye — the per-eye OpenXR
projection gives each eye an asymmetric frustum with a different center:

- left-eye frustum center: `cL = (tan(−0.994)+tan(0.812))/2 = −0.233` tangent units
- right-eye center (mirrored): `cR ≈ +0.234`

Measured live: the world's L/R images are related by a **−451 px** horizontal
offset (predicted −460 from the centers above) plus a 1.7% per-eye span scale
difference. A screen-fixed HUD ignores all of that, so:

1. **Crossed disparity**: the left eye places the HUD at world direction −0.233,
   the right eye at +0.234 — inverted relative to real objects (real points always
   have the left eye seeing them right of the right eye). The HUD is *crossed* by
   ~0.467 tangent units ⇒ perceived at `z = ipd/0.467 ≈ 0.14 m` — a floating HUD
   ~14 cm in front of the user's face. This matches the reported "way too close"
   exactly. (Pre-VR / on the monitor this was invisible: mono output.)
2. **Aspect distortion (second defect)**: the HUD is drawn as 16:9 square-pixel
   screen-space, but the RT carries the XR frustum squeezed horizontally (tangent
   spans ≈ 2.59 H vs 2.80 V ⇒ true eye aspect ≈ 0.93 vs the RT's 1.78) and the host
   un-stretches the full image ≈1.9× horizontally — so HUD icons/text render ~1.9×
   too WIDE in the HMD.

## Fix directions (in order of preference)

1. **Per-eye HUD projection shift at the existing upload gate** (the register-map
   MidHook machinery `hmd_delta` runs on): detect screen-space techniques during the
   per-eye passes and shift their constants per eye by that eye's frustum-center
   offset (+0.233 left / −0.234 right, in tangent/viewport units) — this cancels the
   crossed disparity and puts the HUD back at zero disparity (optical infinity,
   the normal flat-HUD state). Optionally add an uncrossed comfort disparity
   (+`ipd/z_comfort`, e.g. z ≈ 2 m) to simulate a panel at a readable distance.
   Detection hooks that exist already: the technique constant map
   (`Technique_ResolveConstantRegisters 0x0085b260`: `InvViewport +0xb4`,
   `UVMatrix +0xbc` — screen-space signatures; camera rows absent), the per-record
   upload census (`view: rec` lines), the main-RT pass gate, and the S4-5 HUD
   boundary sampling in `eye_replay.cpp` (`set_pass`).
2. **Aspect compensation** (can ride the same shift): scale the HUD's horizontal
   screen-space coordinates by the reciprocal squeeze so the host stretch restores
   square HUD pixels.
3. Fallback (bigger surgery, only if 1 fails): lift the HUD out of the per-eye
   path and composite it per-eye in the carrier at the `eye_share` blit — requires
   separating the HUD draws from the composite, likely fragile.

## Verification

- **Pre-fix (uses the EXISTING dump pair, no run needed)**: measure a HUD element's
  L/R offset in `mc2vr_eye_{left,right}_frame11414.bmp` — expect ≈ 0 px where the
  world shows −451 px. That number IS the crossed disparity (~0.467 tangent ⇒ 14 cm
  perceived) and confirms the "identical NDC" premise (currently inference).
- **Post-fix**: HUD offset should equal the applied per-eye HUD shift (±0.233
  tangent ± comfort term), and the world must be unchanged. Live oracles: HUD reads
  at a sane distance; near-object focus comfort restored; HUD not horizontally fat.

## Measurement recipe (learned 2026-10-07 — do NOT re-derive)

For any per-eye dump pair analysis:

1. Phase-correlate the pair for the GLOBAL offset (the frustum-center offset is
   ~−450 px at 1440p — a ±64 px search window reports "no parallax" wrongly).
2. Fit the per-eye horizontal scale (kx ≈ 0.983 in this run) from per-eye FOV spans.
3. Measure parallax of target strips vs a **true far reference (sky)** — a wall
   coplanar with the target gives ~0 parallax and nonsense distances. Convert with
   `px_per_tan = W_px / (tan(fov.right) − tan(fov.left))` per eye (from the
   `ipc: first tracked pose ... fovL=` log line), `z = ipd·s·px_per_tan / parallax`.
4. Backlog: upgrade `tools/analyze_dumps.py` with this recipe (it currently does
   horizontal-only ±64 px SAD).

## Open items

- [ ] Pre-fix HUD-offset measurement on the existing dumps (which pixels are HUD:
  crosshair/reticle, minimap — pick a high-contrast edge).
- [ ] HUD technique census at the upload gate (screen-space records list, registers,
  cadence — is the crosshair a separate technique from menus/HUD?).
- [ ] Decide the comfort distance / whether zero-disparity (infinity) suffices.
- [ ] Aspect compensation factor from the runtime FOV (per-eye, dynamic — the FOV
  comes over IPC every frame).
- [ ] Interaction with `eye_monitor_pin` (monitor keeps the LEFT image — fine) and
  the S4-5 HUD-timing work.

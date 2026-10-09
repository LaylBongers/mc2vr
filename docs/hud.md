# HUD depth (OPEN track, low priority)

The one open implementation track (opened 2026-10-07). NOTE: the near-field focus discomfort that spawned it turned out to be two `hmd_delta` eye-offset bugs ([camera.md](camera.md) § Eye-offset frame), both fixed — the HUD is only a minor remaining defect. The analysis below stands; the "root-caused to the HUD" claim does not.

## Finding

User-perceived, live-confirmed: the HUD/crosshair reads as placed way too close ("in your face"), making fixating near objects uncomfortable. Evidence numbers: 2026-10-07 eye-dump run (frames 11414…11566, runtime ipd 0.0647, `fovL` (−0.994 0.812 0.953 −0.954) rad).

## Root cause (analysis; one step still to verify)

The HUD is composited into the per-eye pass images (screen-space drawing before the `eye_share` blits and the monitor pin). It does NOT go through the per-eye camera rewrite (`hmd_delta` only rewrites `viewContextData` camera rows — [view_rewrite.md](view_rewrite.md)), so it lands at **identical NDC coordinates in both eyes**. But the world is not: the per-eye OpenXR frusta are asymmetric with different centers — left `cL = (tan(−0.994)+tan(0.812))/2 = −0.233` tangent units, right `cR ≈ +0.234` (measured world L/R offset −451 px, predicted −460; 1.7% span scale). So:

1. **Crossed disparity**: the left eye places the HUD at world direction −0.233, the right at +0.234 — inverted relative to real objects (real points always have the left eye seeing them right of the right eye). Crossed by ~0.467 tangent units ⇒ perceived at `z = ipd/0.467 ≈ 0.14 m` — a floating HUD ~14 cm in front of the face. Invisible pre-VR (mono output).
2. **Aspect distortion**: the HUD is drawn 16:9 square-pixel screen-space, but the RT carries the XR frustum squeezed horizontally (tangent spans ≈ 2.59 H vs 2.80 V ⇒ true eye aspect ≈ 0.93 vs the RT's 1.78) and the host un-stretches the full image ≈1.9× horizontally — HUD icons/text render ~1.9× too wide in the HMD.

## Fix directions (preference order)

1. **Per-eye HUD projection shift at the existing upload gate** (the register-map MidHook machinery `hmd_delta` runs on): detect screen-space techniques during the per-eye passes and shift their constants per eye by that eye's frustum-center offset (±0.233 tangent/viewport units) — cancels the crossed disparity, puts the HUD at zero disparity (optical infinity = the normal flat-HUD state); optionally add an uncrossed comfort disparity (`+ipd/z_comfort`, e.g. z ≈ 2 m) to simulate a panel at a readable distance. Detection hooks that already exist: the technique constant map (`Technique_ResolveConstantRegisters` `0x0085b260`: `InvViewport +0xb4`, `UVMatrix +0xbc` — screen-space signatures, camera rows absent), the per-record upload census (`view: rec` lines), the main-RT pass gate, the HUD boundary sampling in `eye_replay.cpp` (`set_pass`).
2. **Aspect compensation** (can ride the same shift): scale the HUD's horizontal screen-space coordinates by the reciprocal squeeze so the host stretch restores square HUD pixels.
3. Fallback (bigger surgery, only if 1 fails): lift the HUD out of the per-eye path and composite it per-eye in the carrier at the `eye_share` blit — requires separating HUD draws from the composite, likely fragile.

## Verification

- **Pre-fix** (existing dump pair, no run needed): measure a HUD element's L/R offset in `mc2vr_eye_{left,right}_frame11414.bmp` — expect ≈ 0 px where the world shows −451 px. That number IS the crossed disparity (~0.467 tangent ⇒ 14 cm) and confirms the identical-NDC premise (currently inference). Pick a high-contrast HUD edge (crosshair/reticle, minimap).
- **Post-fix**: HUD offset should equal the applied shift (±0.233 tangent ± comfort term), world unchanged. Live oracles: HUD at a sane distance; near-object focus comfortable; HUD not horizontally fat.

## Open items

- [ ] Pre-fix HUD-offset measurement on the existing dumps
- [ ] HUD technique census at the upload gate (screen-space records list, registers, cadence — is the crosshair a separate technique from menus/HUD?)
- [ ] Comfort distance vs zero-disparity (infinity) decision
- [ ] Aspect compensation factor from the runtime FOV (per-eye, dynamic — arrives over IPC every frame)
- [ ] Interaction with `eye_monitor_pin` (monitor keeps LEFT — fine) and the HUD boundary sampling ([stereo.md](stereo.md) § HUD/2D)

Dump-pair measurement recipe (do NOT re-derive): [debugging.md](debugging.md).

# Frustum Culling Alignment Plan (HMD-following, FoV-extended)

Goal: make the engine's frustum culling (and its LOD selection) cover the HMD's field of view,
instead of the game's fixed projection — eliminating pop-in when turning/looking in VR.

Status (2026-10-08, after ~21 investigation rounds):
- **Rotation half: DONE** — `view_table_inject=on` (the camera-table union injection,
  live-verified): culling/LOD follow the head through aim changes.
- **Fov half: INTERIM SHIPPED** — `boom_pin=on` + `entry_fov_scale=1.5`: horizontal clipping
  fully corrected, rendering/camera acceptable, no smearing. **KNOWN DEFECT: the vertical cull
  is short** (~39° halfV vs the HMD's ~55°) — water clips at the top/bottom.
- **Round 25 DONE (the clean fix is LIVE and data-verified)**: with the HMD tracked, the
  cull-site write mechanism ran the whole session: `tantest: overrides` sustained (~1500-2500/
  window) with `mismatches=0 skips=0`; gameproj 100% stock (36.65°/22.71°); union injection
  healthy (fills==rewritten, noPose=0). The per-frame cycle is now fully mapped: the game
  rewrites the ctx tans stock each frame → **site A reads stock, writes the HMD union**
  (tanH=1.8795/tanV=1.7095 ≈ 62°/60° half-angles incl. the 5° margin) → site B reads OURS
  (same frame, A's write) and rewrites idempotently → BOTH DIVSS divides consume the HMD
  frustum. This also retro-explains rounds 16/21: any write at another frame time is
  overwritten before the cull reads — only the in-place write survives.
- **Round 26 VERDICT (visual test): the round-25 in-place write did NOT fix pop-in.** The data
  proved both DIVSS sites in `FUN_0047ded0` consumed our HMD tans ("OURS" at site B, overrides
  sustained, mismatches=0) — yet the visible cull stayed stock. **`FUN_0047ded0` is therefore NOT
  the pop-in gate.** Combined with rounds 16/18/21: the entire ctx/snapshot tan path is
  conclusively excluded. The cull-site hooks stay installed and armed (proven harmless,
  rendering 100% stock, gameproj 36.65°/22.71°) — they may still contribute once the operative
  path is fixed; flip `cull_tan_override=off` for a clean single-variable A/B if preferred.
- **The [BEAB5C] reader enumeration (the next thread, statically mapped this session)** — the
  constant patch is the ONLY mechanism that ever moved the pop-in cull, so the operative input
  is among its 8 readers (xref-proven):
  - `CamPose_CopyGlobal_To_Entry` 0x004665b0 @ 0x004665fb, `FUN_004660a6` @ 0x00466101/0x00466188
    — the camera-entry fillers;
  - `FUN_0070aff6` @ 0x0070b039/0x0070b0bd/0x0070b144, `FUN_0070aa60` @ 0x0070aa86,
    `FUN_0070a910` @ 0x0070a94b — the "0x0070axx cull-fov derivations".
  Decoded: `FUN_0070aff6` writes TEMPLATE blocks into a **0x70-stride table** (the camera-entry
  stride), three blocks at +0x40/+0xb0/+0x120, each carrying **[BEAB5C] at entry+0x48**
  (decompiler constant-folds it to 0x3f75b22c = 0.95975 — the mutated-region fold gotcha;
  raw decode shows the [BEAB5C] loads), plus +0x40=0.2, +0x44=2400, +0x50=0.1, +0x54=100,
  +0x58=300, +0x5c=0.3, +0x60=1.0. Single caller: FUN_004a89ec (init/level-setup, not
  per-frame) — so the scaled constant lands in the entries when the constant patch runs at
  boot. **Candidate: the pop-in cull reads a 0x70-stride camera-table entry's fov field.**
  Corroborating 10-06 note (E2b plate): `0x0048067E` reads a 0x70-stride table (as 0x1c-float
  rows) — fields **+0x50/+0x54/+0x58** via `FLD [ECX+EAX+0x50/0x54/0x58]` — with
  esi=0x017CF980; container `FUN_00480640` builds a large per-view stack structure from
  them (calls FUN_008591a0, box/range logic). Which field is the operative fov (+0x48?
  +0x58?) and which table (the g_CameraTable 0x014a2ee0 vs the ESI-walked one) = the next
  decode/watch target.
- **The DeriveCullTask in-place option (pre-decoded for next session)** — the 10-06-proven cull
  builder `ViewEntry_DeriveCullTask 0x00876a90` reads the fov triple at (plate-corrected raw
  addresses): `LEA ECX,[EBX+0x8C]` (= entry+0x2ec) @ **0x00877162**, `FLD [EBX+0xA8]` @
  **0x00877177**, `FMUL [ESP+0x2C]` @ 0x00877180, `Vector_Scale` 0x00401750 call @
  **0x0087718b**, then box-union `FUN_0040b4c0` @ 0x00877146-51 region (raw-decoded; the
  plate's old "0x0087712c" was off — mutated block boundaries). The cull_fov_widen no-op
  (triple write at the writer, 0x0048a97a) is now best explained by the same overwrite/ordering
  trap as rounds 16/21 — the in-place write at the task's OWN read site (0x00877162-0x0087718b)
  is the natural next mechanism, exactly the round-25 technique aimed at the right consumer.

## The decoded architecture (watch-proven, the session's main asset)

The game's entire fov chain, end to end:

```
[0x00BEAB5C]  fov MULTIPLIER slot (file 1.0 -> boot writes 300 -> gameplay 0.95975, a COS)
[0x00BAD260]  300.0 — a SEPARATE scale constant (feeds entry +0x44 and tan math). NOT the fov.
              (The early "300 engine-unit fov" model was wrong; 0x95975 is the real runtime fov.)
      |
      v
EffectiveFov_ZoomCompose 0x0071BBC6:
   [EAX+0x5E8] = [EAX+0x9F8] (ZOOM/ADS factor) x [0x00BEAB5C]     <- THE effective fov
      |
      v  (ONE reader in the whole image — scan-proven)
Fov_TanIndexHelper 0x0071a430:
   idx = ([obj+0x5E8] * 0.75 * aspect * (8192/pi) + 1)/2;  tanTable 0x00CF1900 step pi/4000;
   tanTable[idx & 0x1fff] / -tanTable[(idx-0x800) & 0x1fff]; atan2 -> projection terms
      |
      v
Render-slot ctx tan extents (+0x30 = tanHalfH, +0x34 = tanHalfV) and the projection matrix
      |                                   (built from REGISTER copies — the ctx slots are
      |                                    not re-read by the projection build)
      v
ViewCtx_BuildSnapshot 0x0061b930 (~5.5/frame, caller FUN_0061b7e0):
   clones the view's render-slot ctx (param_2 = g_RenderShellPtr + idx*0x3a0 + 4) into the
   PER-VIEW SNAPSHOT (param_1, in EBX at the tail): tans +0x30/+0x34, header +0x10..0x1c,
   matrices +0x8C0/+0x900 (from ctx+0x8c0/0x900, the builder's FUN_0085a3f0 pair), +0xa70 block.
      |
      v
THE PER-OBJECT CULL (round-22 CORRECTION — the snapshot chain below it is NOT operative):
   FUN_0047ded0 (vtable slot 0x00bab5bc) builds the cull VP and reads its tan extents from
   the GLOBAL CAMERA OBJECT gcam = [[0x00e79dfc]+0x104] at +0x30/+0x34 (EAX provenance
   raw-decoded: MOV EAX,[EAX+0x104] @ 0x0047E0EB, no EAX write until the DIVSS pair
   0x0047E35C/0x0047E367). Live-falsified negatives: static ctx 0x017CF980 tans (r16,
   clean no-op), snapshot copy tans (r18, clean no-op), SOURCE slot-ctx tans (r21 live A/B:
   ~64 rewrites/frame, mismatches=0, cull stock — see the round-22 plate on FUN_0047ded0).
```

- **Cull and projection are FUSED** at `[obj+0x5E8]` (one writer, one reader, one chain) —
  widening the fov value widens BOTH. The only split points are downstream: the slot-ctx tans
  (the cull's direct input) vs the register-built projection.
- **ADS narrows the culling** via the `[EAX+0x9F8]` zoom factor (user-observed, decoded).
- In HMD play the game's own projection is INVISIBLE for the main view (per-eye hmd_delta owns
  it); the scale's side effects are the non-HMD monitor view, offscreen passes (shadow/
  reflection stretching = the "smearing"), and the third-person camera boom (pulls in above
  ~1.75x, fine at 1.5x).

## The value<->angle map (measured)

| scale s | gameproj halfH | halfV (tan-16:9-locked) | notes |
|---|---|---|---|
| 1.0 | 36.65° | 22.71° | stock |
| 1.333 | 48.87° | 32.78° | playable |
| 1.5 | 54.98° | 38.75° | session best state |
| 2.0 | 73.30° | 61.93° | cull fully wide; camera/smearing break |
| ~2.455 | 90° | — | tan-table pole — UPSIDE-DOWN flip |

The tan-table is angle-linear in the multiplier (idx = angle x 4000/pi); the flip at halfH=90°
is a hard ceiling for ANY chain through the projection. The vertical therefore CANNOT be
covered via projection-shaped widening at a playable scale — the clean fix (below) is required.

## Shipped mechanisms (carrier, view_table.cpp)

- **`boom_pin=on` + `entry_fov_scale=<s>`** (THE INTERIM): repoints the imm32 at
  0x0071BBCA (EffectiveFov_ZoomCompose's `MULSS XMM0,[0x00BEAB5C]`) at a carrier
  VirtualAlloc page holding `0.95975 x s`. The whole chain widens consistently (no
  derived-math breakage). Game's own [BEAB5C] stays stock. Best playable: 1.5.
- **`cull_tan_override=on`** (THE CLEAN FIX, round 24): two MidHooks at the operative cull's own
  tan-extent read sites (A 0x0047E35A, B 0x0047ED44 — the `DIVSS [EAX+0x30]/[EAX+0x34]` pair,
  EAX = the main slot ctx 0x017CF980, live-proven at both sites every window). The hooks
  discriminate the main view (ctx tanH vs 1/g_game_cam.a, 30% tolerance, + exact-bit-match to
  our last write for leak-robustness) and write tan(hmdHalf+margin) into [EAX+0x30]/[EAX+0x34]
  IN PLACE, immediately before the DIVSS consumes them — ordering-free by construction. The
  projection (register-built), snapshot, monitor and offscreen passes all stay stock.
  NOTE: needs a tracked HMD (no pose → skip → cull stock). The snapshot-tail hook
  (0x0061BB40) is now observer-only (cull_snap_dump diagnostics).
- **`cull_fov_margin=<deg>`** (default 5) — the safety margin on the HMD-union half-angles
  (via `camtable::get_fov_union`, 1s staleness window, outermost bounds over both eyes).
- `cull_snap_dump=on` — log-only snapshot field dump (the diff diagnostic).

## Dead ends (verdicts only — details in git history if ever needed)

| Approach | Verdict |
|---|---|
| ViewEntry fov-triple widening (`cull_fov_widen`, hook 0x0048a97a) | the triple feeds the per-view BOX (LOD/coarse), NOT the operative object cull — a margin-30 test moved nothing. Hook works mechanically; conf-off. |
| Record-VP widening (`record_fov_widen`, 3 hook sites) | the operative cull does not read the records (and the fill sites never matched main records). conf-off. |
| entry_fov_decouple (repoint fillers at 300.0) | RETIRED — 300.0 in a cos-form slot produces garbage projections (the "84° upside-down" runs). |
| Constant patch without pin (`entry_fov_scale` alone) | couples cull+projection+boom; >1.75x breaks the camera and offscreen passes. Superseded by boom_pin. |
| Static-ctx tan write (round 16) | wrong object — 0x017CF980 is not read by the cull. |
| Snapshot-copy tan write (round 18) | wrong object — the copy is not read by the cull. |
| SOURCE slot-ctx tan write (round 21, live A/B) | wrong object — rewrites landed ~64/frame
  with mismatches=0 and the cull stayed stock; the round-19 "copy carried the widen" diff was
  downstream correlation, not the operative input. |
| [EBP+0xC] source write (round 20) | the mutated convention keeps param_2 in a REGISTER — **round-21 decode: EBP IS param_2**, so [EBP+0xC] was param_2+0xC, a header field (crash, then the hardened validation rejected all 3244 candidates/window). |

## Hard-won gotchas (do not relearn these)

- `.rdata`/`.text` writes need `VirtualProtect`; multi-DR watchpoint attribution under Wine is
  UNRELIABLE (equal-count EIP tables were an artifact) — single-target or value-guarded
  reasoning only.
- Raw byte decode > decompiler output in SecuROM-mutated regions: the decompiler constant-folds
  memory loads into immediates and mis-attributes inlined helpers (cost us ~5 rounds of wrong
  "entry+0x58" models). Verify every imm32 site against raw bytes; the `patch_imm32s` value
  guard is MANDATORY (a six-byte mis-decode caused both a boot crash and a silent no-op).
- Hook addresses must be exact instruction boundaries — one byte off = crash at boot
  (0x0046662e lesson) or garbage trampoline.
- `bool` vs `float` declarations compile silently through implicit conversion — the round-8
  no-op run. When a run "does nothing", check the conf actually took effect in the logs.
- The tan slots and matrices live in MULTIPLE ctx objects (static 0x017CF980, render-slot
  per-view, per-call stack) — always verify WHICH object a read/write addresses via the
  hit registers, not the static xref.

## Next session (round 27: two pre-decoded leads — pick by cost, A/B one at a time)

1. **Lead A — the DeriveCullTask in-place triple write** (cheapest, fully spec'd): MidHook in the
   0x00877162-0x0087718b window (pre-`Vector_Scale` call), discriminate the main view (match
   the triple against the live gcam fov pair / 1/cam.a as in cullsite_probe), and rewrite the
   fovTriple vector at [EBX+0x8C] in place so the case-0 bound = the HMD-union corner ray.
   Same write-at-the-consumer's-read-site technique as round 25 (the only one that survives the
   per-frame overwrites). Registers: EBX = entry+0x260-ish base (verify at hook: EBX+0x8C
   must hold {fovH*k, 0, fovV*k}); raw bytes already decoded.
2. **Lead B — the 0x70-stride camera-table fov field**: determine which field the cull reads
   (entry+0x48 where [BEAB5C] lands via FUN_0070aff6, or the +0x50/+0x54/+0x58 triple read by
   FUN_00480640 at 0x0048067E) and which table; a live `debug_watch` write-mode run on the
   candidate field (need the runtime table address — the gcamtan-style deref targets or
   addr: with a logged address) finds the per-frame writer; then hook the consumer's read site
   in place (same technique). Note the view_table_inject fill-site hook (0x0070aef8) already
   rewrites pose in this family — the natural place for an fov companion write if the table is
   the g_CameraTable.
3. Keep `cull_tan_override=on` during Lead A/B (proven harmless) or flip off for single-variable
   A/Bs; either way report `cullsiteA/B` lines to confirm both consumers see the HMD values.
4. When a lead lands visually: delete the dead-end hooks after a clean A/B (`cull_fov_widen`,
   `record_fov_widen`, possibly the cullsite hooks if they prove redundant), retire
   boom_pin/entry_fov_scale plumbing from the documented conf, and close this plan.
5. If BOTH leads fail despite data-verified in-place consumption: the pop-in may be LOD/box
   driven rather than object-cull driven — re-examine the DeriveCullTask LOD arrays
   (param_3+0x164/0x178/0x1a0/0x1b4/0x1c8, LOD_DistanceSelect 0x00490b80 kin) and the
   second-pass array at entry+0x62c.

Ghidra carries the anchors: `EffectiveFov_ZoomCompose` (0x71BBC6), `Fov_TanIndexHelper`
(0x71a430), `ViewCtx_BuildSnapshot` (0x61b930) — all with full plates; the record-fill sites
(`RecordFillB_VP_Copy` 0x004673be) and the fov-constant defines live in `game_addresses.h`.

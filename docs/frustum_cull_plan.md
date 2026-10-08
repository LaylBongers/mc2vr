# Frustum Culling Alignment Plan (HMD-following, FoV-extended)

Goal: make the engine's frustum culling (and its LOD selection) cover the HMD's field of view,
instead of the game's fixed projection — eliminating pop-in when turning/looking in VR.

Status (2026-10-08, after ~20 investigation rounds):
- **Rotation half: DONE** — `view_table_inject=on` (the camera-table union injection,
  live-verified): culling/LOD follow the head through aim changes.
- **Fov half: INTERIM SHIPPED** — `boom_pin=on` + `entry_fov_scale=1.5`: horizontal clipping
  fully corrected, rendering/camera acceptable, no smearing. **KNOWN DEFECT: the vertical cull
  is short** (~39° halfV vs the HMD's ~55°) — water clips at the top/bottom.
- **The clean fix is one decode away** — see "Next session" below. The complete architecture is
  decoded (below); nothing needs re-discovery.

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
THE PER-OBJECT CULL reads this snapshot chain. KEY FACTS (three surgical negatives):
   - it does NOT read the STATIC ctx tans (0x017CF980 — round-16 write: clean no-op)
   - it does NOT read the SNAPSHOT COPY's tans (round-18 write: clean no-op)
   - it reads the SOURCE render-slot ctx's tans (round-19 diff: at a known-widen state the
     snapshot copy demonstrably carried 1.4269/0.8026 yet the copy-write moved nothing)
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
- **`cull_tan_override=on`** (THE CLEAN FIX, 95% built, currently conf-off): MidHook at
  0x0061BB4E (the snapshot tail, EBX = the snapshot): matches the main view (snapshot tanH vs
  1/g_game_cam.a, 30% tolerance — shadow views pass through), then writes tan(hmdHalf+margin)
  into the SOURCE slot ctx's +0x30/+0x34. The source pointer recovery is the missing 5% (below).
  The write path is fully crash-hardened: VirtualQuery page guard + EXACT BITWISE content check
  (a true source must hold the tan that was just copied from it) + harmless copy-write fallback.
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
| Static-ctx tan write (round 16) | wrong object — the snapshot reads the render-SLOT ctx. |
| Snapshot-copy tan write (round 18) | wrong copy — the cull reads the source. |
| [EBP+0xC] source write (round 20) | the mutated convention keeps param_2 in a REGISTER; the stack slot was garbage (crash, then the hardened validation rejected all 3244 candidates/window). |

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

## Next session (the clean fix, one decode away)

1. **Decode `ViewCtx_BuildSnapshot`'s prologue (0x0061b930-0x0061bb5f)** to find param_2's
   REGISTER at the tail hook (0x0061BB4E). Candidate: ESI (0x1FE10E40 seen in the copy-loop hit
   registers; EDI/ESI are POPped right after the hook point — read them BEFORE the pops, i.e.
   at 0x0061BB4E). Validate with the existing bitwise check ([reg+0x30] == snapshot tanH).
2. Enable `cull_tan_override=on` (everything else off) → the hook writes the HMD-union tans
   into the SOURCE slot ctx. CHECKS: rendering 100% stock everywhere (gameproj 36.65°/22.71°),
   water fully covered INCLUDING top/bottom, no edge pop-in, ADS no longer narrows the cull,
   `camtable: tantest: overrides>0 mismatches=0`.
3. If the source-tans theory fails there too (overrides>0 but cull stock): the enumerated
   fallbacks, same hook: the snapshot's matrices (+0x8C0/+0x900 — scale m00/m11 by the tan
   ratio) or the +0xa70 block. The round-19 dump lines (`snapdump`) show their live values.
4. When it lands: retire boom_pin/entry_fov_scale from the conf, auto-compute the margin
   from the live HMD fov (already done via get_fov_union), delete the dead-end hooks
   (cull_fov_widen, record_fov_widen) after a clean A/B, and close this plan.

Ghidra carries the anchors: `EffectiveFov_ZoomCompose` (0x71BBC6), `Fov_TanIndexHelper`
(0x71a430), `ViewCtx_BuildSnapshot` (0x61b930) — all with full plates; the record-fill sites
(`RecordFillB_VP_Copy` 0x004673be) and the fov-constant defines live in `game_addresses.h`.

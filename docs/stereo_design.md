# Stereo Submission Design

Design for dual-eye world rendering + HMD presentation. Per-address facts live
in Ghidra plates (`PgPrimitive_SubmitToGPU`, `Technique_ResolveConstantRegisters`,
`g_ViewContextTable`, `g_PrimitiveBase`, `RenderQueue_SubmitWorldPackets`).
Mechanism rules and hook list: `docs/launcher_plan.md`. Runtime frame chain:
`docs/render_path.md`; overview diagram: `docs/render_diagram.svg`. Code: `src/carrier/view_rewrite.cpp`.

## Status

| Phase | State |
|---|---|
| S0 loop-body RE | complete |
| S1 draw-camera hunt | complete — the camera is only reachable at the GPU boundary |
| S2 per-eye injection | camera pan **done and visually clean at game scale**; real HMD offsets, stream replay (S2c) pending |
| S4 HMD presentation, S5 motion controls | not started |

## Handover — state and next steps (2026-10-03)

Done and verified in-game: per-view camera pan at the GPU boundary (layout-corrected, scratch-copy
upload, RT0 pass gate); shadows/materials visually clean at game scale. Ghidra + docs consolidated
(the `ViewContextRecord`/`ViewEntry`/`ViewRef`/`PgPrimitive` structs, full `Dx9StateWrapper_vtbl`
names, corrected comments). Tooling: `tools/shader_*.py`, `tools/analyze_dumps.py`, optional stub
tracer (`stub_trace=on`).

Next, roughly in order:
1. Real per-eye offsets from the HMD pose (replace the pulse) + asymmetric projection (VP rows).
2. Shaders without `viewContextData` (billboards/rain/quads) — check which lag, then implement.
3. PS-side camera data (the pass uploads the view record to the PS; `cameraPos` c92; texgen
   matrices are mono) — hook slot 109 if reflections/shadows skew at IPD scale.
4. S2c second draw pass (stream buffering/replay, eye RTs); note the per-frame GPU sync in
   `LtiRenderer_BeginSubmit` and that rendering runs as a registered task (`RenderTask_RenderFrame`).
5. S4 compositor (OpenXR via wineopenxr), pacing.
Open RE items: what the stub's plaintext callbacks do (`FUN_0050c106` recursive handle-tree walk,
see its Ghidra plate); why `ViewManager_Update` never fired in the traced run; `FUN_00858980`,
`FUN_00852740` (opcode 0x08 draw) etc. still unnamed.
Camera-matrix writer hunt (2026-10-03, after the thunk-target census): NEGATIVE. None of the 405
runtime-native thunk-target functions references `g_ViewContextTable` (`0x01169774`) or the
`ViewEntry` table (`0x012865e0`); the view/camera code (`ViewEntry_Activate`, `FUN_0048a3b0`,
`FUN_00489e50`, `FUN_004d2a50`) still calls thunks that stay VM at runtime; the three native
`FramePipeline` callees (`0x0057de60`, `0x0059de70`, `0x00624f70`) are handle-table helpers. Writes
to `g_ViewContextTable` +0x10..+0x48 from the function-less `0x8564xx..0x856dxx` blocks are
state-cache flags, not VP rows. `FUN_024fe0d0` (`.securom`, readable in Ghidra) maintains the
active-view list (`ViewEntry` +0x0/+0x4 links, head `DAT_00d29e60`) — not matrices. Static xrefs
cannot find pointer-based matrix writes; proposed next step: carrier hardware-write watch (debug
registers + VEH, or PAGE_GUARD) on one live `ViewContextRecord`'s VP rows to log the writer's EIP
(SecuROM anti-debug is documented inert, but untested for DRx).

## Facts this design builds on

- **Frame chain** (producer side all plaintext, main thread only):
  `GameShell_FrameTick` → frame pipeline → `RenderQueue_SubmitWorldPackets`
  (`0x0048e620`, walks active `ViewEntry`s, publishes packet elements into the
  `g_RenderQueue` ring) → [SecuROM-VM'd packet interpreter, stub `0x0050f660`
  at `0x004c99f9`] → `PgPrimitive_SubmitToGPU` (`0x00855690`) → `BeginSubmit` →
  `RenderCmd_ExecuteStream` (`0x008569d0`, 27-opcode plaintext interpreter,
  ~1.5–3.4k cmds/frame) → `EndSubmit`.
- **The draw camera is external to the view system.** The plaintext consumer
  path (the `PgPrimitive` record walk, `0x58` stride) carries only table
  indices (material/technique/env/view-context/view-scale/screen) and draw
  params. Patching ViewEntry fields, staging slots, the camera ring, frame-ctx
  blocks or the pose-record store never moved the draw camera (they are
  derived copies feeding streaming/culling). The camera crosses plaintext code
  only as interpreter-issued D3D constant uploads — **the GPU boundary is the
  only per-eye injection point.** Do not re-litigate the producer side.
- **There is NO fixed-function projection** — `SetTransform` is never called.
  The projection is folded into the `viewContextData` VP rows, so per-eye
  asymmetric projection is an edit to the same rows.
- **Source of `viewContextData`**: the per-view render-context record
  (`g_ViewContextTable` `0x01169774`, 0x70 stride, indexed by `prim+0x49`):
  +0x00 viewContextData, +0x40 PS view consts, +0x60 atmosphereData*, +0x64
  globalLightData*. No plaintext writer — filled by the SecuROM-VM'd
  producer; plaintext code only zeroes it and copies it to the GPU.
- **Constant-name → technique-field map** (plate on
  `Technique_ResolveConstantRegisters` `0x0085b260`): reg at technique+X,
  count/gate at +X+4 — objectData +0x94, LocalToWorld +0x9c, PrevLocalToWorld
  +0xa4, BoneMatrixArray +0xac (N bones × 3 rows of 3x4 skinning matrices; no
  view content), InvViewport +0xb4, UVMatrix +0xbc, BlendWeight +0xc4,
  globalLightData +0xcc, **viewContextData +0xd4**, **ViewProj +0xdc**,
  atmosphereData +0xe4, Atmos.ScatteringTermMultiplier +0xec, User +0xf4,
  ObjectIDScaleArray +0xfc, WindMatrix +0x104, shader ptr +0x10c.
- **Shader register-role lookup**: `docs/shader_ctab_map.md` (generated by
  `tools/shader_ctab.py` from the CTAB tables in `data/shader*.bin`; every
  constant named per register, `viewContextData` in 180/194 VS shaders, base
  slides per shader). `tools/shader_disasm.py` disassembles the vs_3_0
  bytecode and is the layout oracle (its swizzle/writemask decoding is rough —
  trust register usage, not component masks).
- Queue: `g_RenderQueue` ring (elem 96, cap 4096; position = +0x10 low16 %
  cap); `g_RenderQueue2` carries 2D/overlay submissions. Special cameras
  (satellite) submit up to 608 views/frame; gameplay tens.
- S0 view walk: active views = intrusive list (head `DAT_00d29e60`, link
  `ViewEntry+0x4`); per-view element = three `{size, ptr}` pairs
  ({0x30 staging `ctx+0xc2110+idx*0x30`}, {0x810 live `ViewEntry*`}, {0x680
  frame-ctx `ctx+0xd2950`}); the VM'd consumer derefs POST-walk, so patching
  between passes cannot work. Staging cap 768 elements; staging site
  `0x0048ef71`, iterator reload `0x0048f013`, back-edge `0x0048f01f` (mapped,
  hooks not needed for the GPU-boundary design).
- `LtiRenderer_EndSubmit` already StretchRects RT0 → backbuffer
  (`LtiRenderer+0x3ea4`) whenever they differ — an existing RT→backbuffer copy
  path the compositor can co-opt.

## The view channel (implemented)

The visible view lives in the VS constant `viewContextData`, a 4–6 register
block the engine resolves per technique. Layout (proven from shader bytecode:
count-4 `shader3.bin @0x28c8`, count-5 `@0x1fd3f8`, count-6 `@0x1de598`):

```
count 4: [VP row0..3]
count 5: [VP row0..3 | camPos (w==1.0)]
count 6: [VP row0..3 | camPos | extra row]   (most common)
row-major, clip_i = dot(VP_row_i, worldpos)
```

`ViewProj` (count 4) sits at the same register as `viewContextData` in every
technique, confirming VP-first. The count-6 extra row is a world-fixed plane
(unit-length xyz unrelated to the VP rows) and is left alone.

**Rewrite** (device VmtHook on `SetVertexShaderConstantF`, slot 94): for a
world-space pan `D = (dx,dy,dz)` — camPos `xyz += D` (per-pixel effects follow
the eye) and every VP row `w -= dot(row.xyz, D)` (rigid world shift on screen;
a uniform clip-w shift does NOT work, the divide scales it per-vertex).
Rewriting is done on a scratch copy returned to the driver call — the game's
upload buffer may alias the persistent per-view record and is never modified.

**Exact registers come from the game's own resolver, not shape matching**
(blocks arrive split across upload calls, and register numbers are reused
across techniques, so shape heuristics conflate techniques). A MidHook at the
upload gate (`MC2_VCD_UPLOAD_CMP` `0x00855a78`, EDI = current technique)
publishes its resolved map (`+0xd4`/`+0xd8` viewContextData reg/count,
`+0xdc`/`+0xe0` ViewProj reg/count); the device hook runs immediately after on
the same thread.

**Pass gate**: shadow-map, reflection and other offscreen passes upload their
own `viewContextData` (the shadow pass's "camera" is the light). Shifting them
moved shadow maps relative to their receivers (visible shadow fading). The
rewrite therefore applies only while RT0 (observed via device `SetRenderTarget`,
slot 37) has the backbuffer size. Seen RT0 sizes: 2560x1440 main; skipped:
1024x4096 shadow atlas, 512², 128², 64², 853x480, and the 1280x720 → 1x1
downsample chain. Caveat: an offscreen pass with exactly the backbuffer size
would be shifted (none seen); if render resolution ever differs from the
backbuffer, key the gate on RT identity. Shadow *receivers* look up in world
space (the VS passes world position to the PS), so they are eye-invariant.

**Controls** (`mc2vr.conf`): `view_row_rewrite=off|on|pulse`, `view_row_amp=`
world units (default 4.0, unmistakable; ~0.05 for game-scale checks, 0.032 =
IPD scale). The real per-eye offset is `D = ±right·IPD/2` (≈0.032 m).

## Architecture

**GPU-boundary per-eye injection**: the frame renders once per eye. During an
eye pass the `viewContextData` registers in the uploads are rewritten for that
eye (pan + asymmetric-projection VP edits). The second draw pass replays the
frame's command stream through the plaintext interpreter. A compositor at
`Present` delivers both eye textures to the HMD. (A producer-side design —
duplicating each view's element with shadow ViewEntry/staging — cannot work:
the consumer never reads view camera data.)

## Remaining phases

### S2 — remaining work

1. **Real per-eye offsets from HMD pose** (S4 supplies the pose): replace the
   test pulse with `±right·IPD/2` and per-eye asymmetric projection.
2. **Shaders without `viewContextData` are not rewritten** and will lag the
   pan (not yet observed as visibly wrong — check billboards, rain, particles,
   quads before implementing): explicit `g_ViewProjMtx` (`c0-3`, `0x9fb8`:
   same per-row `w` shift); `LocalToProj` (`0x6cc8`: view folded in per object
   — needs the view-space eye offset, `clip.x -= P00*e.x`, with P00 derivable
   from cached VP rows); `Mvp`/`TexGen` (`0x200278`); rain (`0x1fe198`).
3. **S2c — second draw pass**: replay the frame's command stream once per eye
   through `RenderCmd_ExecuteStream` (the M3 opcode MidHook at `0x008569f5` is
   already installed as the stream tap): buffer the frame's stream, replay per
   eye with that eye's rewritten constants and eye render targets. Per-draw
   RT/viewport switching already happens (SetViewport fires 2–3.6k×/frame) —
   the replay redirects draw targets to eye RTs. Engineering list: stream
   buffering, draw-state reapplication on replay, RT plumbing; S0 ring/element
   facts are the timing inputs. Per-object re-uploads during replay are safe
   (objectData = local→world, BoneMatrixArray = skinning; no view content).
   With two passes the RT gate must also distinguish the two eye RTs.

### S4 — Presentation / HMD runtime

- `Present` VmtHook (proven) as the compositor entry: submit both eye textures
  (or an SBS target) to OpenVR/OpenXR. Interop blit needs no swapchain changes;
  the game's Present continues to the monitor untouched.
- Slot-5 (`PostUpdateHook`) claim as the per-frame VR orchestration point:
  sample HMD pose, marshal to the main thread before the producer loop, feed
  S2. Slot-4 (`EndOfFrameHook`) for end-of-frame bookkeeping (timewarp input,
  frame pacing — `main_game_loop.md`).
- OpenXR preferred on Proton/RADV via wineopenxr (present in the prefix;
  verify at integration).
- UI/2D (`g_RenderQueue2`): render once; composite over both eyes in the
  compositor. Per-eye rects are compositor-owned.
- Fallback if per-eye RTs can't differ at the D3D level per view: Present-hook
  interop blit of the single backbuffer into per-eye targets.

### S5 — Motion controls (separate track)

Follows the logic-mod track in `docs/launcher_plan.md` (XInput stubs
`0x00a64d56/0x00a64d5c`, idle-reset buffer pair `0x017d30e8`/`0x00f7fb90`
first). Pose marshal point is the slot-5 hook (S4).

## Hook inventory

| Site | Mechanism | Purpose |
|---|---|---|
| Device `SetVertexShaderConstantF` (slot 94) | VmtHook | the view rewrite (scratch-copy upload) |
| Device `SetRenderTarget` (slot 37) | VmtHook (observe only) | main-pass gate |
| Upload gate `MC2_VCD_UPLOAD_CMP` `0x00855a78` | MidHook | publish the technique's exact viewContextData/ViewProj map |
| `RenderCmd_ExecuteStream` opcode `0x008569f5` | MidHook | M3 histogram + S2c stream tap |
| Device `Present` (17) / `Reset` (16) | VmtHook | S4 compositor / params |
| `g_RenderShell` slots 4/5 | cloned-vtable claim | S4 orchestration (counting no-op now) |
| `SubmitWorldPackets` loop head `0x0048e9ea` | MidHook | M3 view aggregation |
| Stub call `0x004c99f9`/`0x004c99fe` + ~15 plaintext helper entries | MidHook | optional callback tracer (`stub_trace=on`, see `render_path.md`) |

Proven mechanisms: trap-based inline/Mid/Vmt installs (no suspension), device
VmtHook surviving device-lost + `Reset`, slot 4/5 claim 1:1 with frames,
in-buffer-style constant interception where the draw consumes the modified
data. **Never hook**: VM entry stub `0x0050f660`, VM pose-getter thunk
`0x0048bf00` (use plaintext call sites), anything at `0x01a48000+`.

## Open questions

- **Material texgen stays mono for eye 2**: `PgMaterial` texture-projection
  transforms (water/sky reflections, blob shadows, shadow cascades' fitted
  matrices) are derived CPU-side by VM'd code from the mono camera and uploaded
  via `SetPixelShaderConstantF` (matViewMat; device slot 109) and
  material VS consts; PS `cameraPos` (c92 in material PSes) is likewise mono. The pass object also uploads the whole `g_ViewContextTable` record to the PS (`Dx9_SetPixelShaderConstantF(pViewContext)`, pass pair +0xDC/gate +0xE0; PS register/count are runtime values) — so VP/camPos data reaching the PS is unshifted too (open).
  The view rewrite touches VS camera rows only. No visible issue at 0.05
  units; at IPD scale and for the periphery, re-check. A later
  `SetPixelShaderConstantF` hook could shift camera-derived rows by the eye
  delta (the derivation is VM'd, so correctness is not guaranteed).
- `g_RenderQueue2` consumption timing relative to Present (the compositor needs
  the 2D stream's frame timing) — add queue2 counters when S4 starts.
- GPU sync: every frame begins by waiting for all prior GPU work (event-query spin in `LtiRenderer_BeginSubmit`, see `render_path.md`). Per-eye passes inherit it; the pacing design must account for it (S2c replay happens after this point).
- Frame pacing: game vsync-locked 60 Hz; HMD typically 90 Hz. A Present-hook
  compositor can run at HMD cadence independently (pose extrapolation via the
  HMD runtime). Decide in S4.
- Stream replay state (S2c): which of the 27 opcodes carry draw state that must
  reset between eye passes; RT plumbing for eye targets.

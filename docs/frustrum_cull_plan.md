# Frustum Culling Alignment Plan (HMD-following, FoV-extended)

Goal: make the engine's frustum culling (and its LOD selection) follow HMD head rotation and cover the
HMD's field of view, instead of the fixed, narrow game-camera frustum — eliminating the pop-in that
appears when turning the head in VR.

Status: **RE COMPLETE (2026-10-06)** — every producer/consumer of the culling inputs has been identified
live (hardware-watchpoint evidence; `docs/reverse_engineering/view_and_camera.md` § Camera-data accessors,
per-address facts in the Ghidra plates). **Design drafted below — not yet implemented.**

## What the RE proved (evidence summary)

The culling data flow, all sites watch-proven on live gameplay:

```
VM'd packet consumer (ORIGIN of camera-pose VALUES; post-walk deref of the staged block
  ctx+0xc2110+idx*0x30, serial-gated round-trip)
        │  copy-back at 0x0048F72D (walk tail; Pose_Copy semantics, serial+1)
        ▼
ViewEntry {pos7c4, quat7d4, serial7d0}   [plaintext, change-gated ~1/10s]
        │  Pose_Copy relay 0x00824a10 (0x20 block, serial = max(src,dest)+1)
        ▼
camera object (ViewRef.camData chain)
        │  ViewEntry_MatrixFromGlobalCam 0x0048a8f0 (via ViewManager_Update -> PropagateMatrices):
        │    writes slot matrices (+0x20, stride 0xc0, via Matrix_Copy3x4 0x00836120)
        │    and fovCos/fovSin (+0x2ec/+0x2f4)
        ▼
ViewEntry_DeriveCullTask 0x00876a90  (task-queued, was undefined in Ghidra until the watch run)
        reads fov + slot matrices + camera-object params, per slot kind (kindA4 0..4):
        LOD distance selection, D3DX transforms, Vector_Scale (0x00401750) by fov,
        Box_Union helpers (0x0040b250/0x0040b4c0)  =>  CULLING BOUNDS + per-slot LOD states
```

Key facts the design must respect:

1. **No VM involvement in the culling math.** The culling volume is built by `ViewEntry_DeriveCullTask`,
   a plaintext function. The VM only originates *pose values* (and produces the draw camera's
   `viewContextData` VP rows — unchanged by this plan; the S4-4 GPU-boundary rewrite stays the draw-camera
   channel).
2. **Everything is change-gated (serial handshake), NOT per-frame.** Copies only happen when the source
   serial advances (`serial = max(src,dest)+1`, `Pose_Copy` semantics). Observed cadence ~once per 10 s
   for a static camera — the gate opens on change. Injection must write fields AND advance the serials,
   or nothing downstream will notice.
3. **View indices are per-run allocations** (live view was idx0/idx13/idx14/idx23 across runs). Never pin
   an index; identify at runtime (below). The companion liveness byte `g_ViewTable3 + idx*0x20 + 0x18`
   (t3) marks live camera data (00 = loading template, 01 = live).
4. **The slot matrices feed general per-object view-transform math** (`Matrix_MakeWorldToView`
   0x004017d0, 23 call sites) — not only culling. Overwriting them is a wide-reaching change; prefer
   the minimal-injection variant below.
5. ~24 live type-2 views are walked per frame; the main draw view must be identified among them.

## Design

### D1. Identify the main draw view at runtime (no pinning)

The carrier already decomposes the draw camera from the GPU uploads (S4-4 `vp_camera` decomposer:
`F = row3.xyz`, basis R/U/F, position C). Each frame, for each live (t3=1) type-2 view, compare the
view's slot-0 basis (`slot mtx[0]` rows ≈ camera right/up/forward + pos at row 3) against the decomposed
draw-camera basis/pos (tolerance match). The view that matches is the culling view of the draw camera.
- Match inputs are all plaintext; comparison runs in the existing view-MidHook walk
  (`render_dump.cpp` view_midhook) or the FrameTick path.
- Log mismatches/ambiguity (several views share one camera — e.g. water views); if multiple views match,
  inject into all matched views (cheap, and culling must hold for each).
- The active-view list head is `g_ActiveViewListHead (0x00d29e60)`; entries at `g_ViewTable (0x012865e0)`,
  stride 0x810.

### D2. Injection point — write the culling inputs, bump the serials

Inject once per frame (in the existing slot-5 `PostUpdateHook` or the view MidHook, i.e. BEFORE the
producer walk's staging, so the staged round-trip carries our values):

- **Pose**: `pos7c4` += HMD head offset (same body-on-camera mapping as S4-4 `apply_hmd_eye`, scaled by
  `view_world_scale`); `quat7d4` = q_hmd ∘ q_game (HMD rotation applied on the game camera). Union, not
  per-eye: use the mid-point between the eyes (IPD contributes negligibly to culling bounds; include a
  +IPD/2 margin in the bound scale if paranoid).
- **FOV**: `fovCos2ec`/`fovSin2f4` ← half-angle widened to the HMD's per-eye FOV union: half-angle =
  max over both eyes of the OpenXR angle bounds (+ a safety margin). NOTE: the exact semantic of these
  two floats is not yet decoded (observed fovCos = 0.957826 on a live view — plausibly cos(half-angle) of
  a sub-view, but VERIFY: watch them while zooming/changing the game FOV option; alternatively read them
  alongside the slot `paramA8` params in `MatrixFromGlobalCam`).
- **Serials**: `serial7d0` += 1 (and pos7ac = old pos7c4 first, to keep the "previous position" semantics
  consistent). This opens the change-gate so `MatrixFromGlobalCam` re-derives slots+fov and
  `ViewEntry_DeriveCullTask` re-derives the culling bounds.
- **Slot matrices**: only if D3 shows the derive does not cover rotation (see open items). Writing slots
  directly affects general per-object transforms — avoid unless proven necessary; the pose path should
  propagate rotation through `MatrixFromGlobalCam` automatically.

Alternative considered and rejected: hooking inside `ViewEntry_DeriveCullTask` to rewrite its bounds
outputs. Wider risk surface (task runs for ALL views), and the inputs are cleanly writable anyway.

### D3. Ordering & forcing the derive

Open question: within the change-gated cycle, does `ViewEntry_DeriveCullTask` run after
`MatrixFromGlobalCam` unconditionally on a serial bump, or does the task have its own gate?
- Watch evidence: both fire ~1/window for a static camera (gate = change).
- Implementation probe: inject + bump serial once, watch the window reports (`debug_watch=fov`,
  `debug_watch=slot0`) — if the derive runs on our bump, ordering is fine; if not, also bump the entry
  fields the cull task gates on (its per-slot serials / the 0x62c-array serials at entry+0x62c,
  count = flags804>>4 & 0xF).
- Fallback if gating can't be satisfied cleanly: MidHook `ViewEntry_DeriveCullTask`'s entry (plaintext,
  task-queued — hookable) and re-run it for the matched view after injection (the task is idempotent
  derive math).

### D4. What stays untouched

- The draw camera itself (S4-4 GPU-boundary `viewContextData` rewrite) — unchanged.
- The staged round-trip and the VM consumer — we inject INTO the pipeline upstream; the VM relays our
  values like its own (values are indistinguishable: it never validates them, it just copies serials).
- Shadow atlas culling (light frustum), satellite/PDA/water views — inject only into matched views.

### D5. LOD consideration

`ViewEntry_DeriveCullTask` also does per-slot LOD distance selection using the same camera inputs, so
HMD-aligned injection automatically aligns LOD with the head (removing periphery LOD pop). The widened
FOV must NOT scale the LOD distances (or distant LODs would drop everywhere): inject pose + FOV for the
*volume*, keep the LOD distance metric on the true head position. Verify by reading the task's
distance-selection inputs during the verification run.

## Conf flags (proposed)

- `cull_align=off|on` — master switch (default off).
- `cull_fov_margin=<deg>` — extra degrees on the widened half-angle (default 5).
- `cull_view_debug=on` — log the runtime view matching (basis residuals per candidate view).

## Verification plan

1. `view/hmd` + `cull_align=on`, stand still, turn the head ±90° quickly: no geometry pop-in at the
   periphery (today: heavy pop-in). Watch the `watch:`-style logs (extend the view MidHook to log the
   matched view idx + injected values once per window).
2. FOV: with `cull_fov_margin` deliberately huge (e.g. 30°), culling should never pop even on fast
   spins; with 0 margin, slight periphery pop is acceptable/expected.
3. Regression: monitor stereo (shadows glued, IPD parallax) and LOD behavior at the screen center
   (unchanged).
4. If a culling artifact appears only for one eye: the union margin is too small (increase IPD margin).

## Open items (before implementation)

- [ ] Decode the exact semantics of `fovCos2ec`/`fovSin2f4` (cos/sin of half-angle? which angle —
      horizontal/vertical/diagonal? does the game FOV setting change it? watch run while changing zoom).
- [ ] Confirm `MatrixFromGlobalCam` re-derives on our serial bump (D3 probe) and the ordering vs the
      cull task.
- [ ] Confirm whether the widened FOV reaches the box-union vectors via `Vector_Scale` alone
      (i.e. the bounds scale with fov as intended) — read the task's outputs after injection.
- [ ] The camera-object params (+0x128 kind, +0xa8/+0xac/+0xb0 slot params) — check whether any of
      them encode frustum extents that also need widening (kind 3/4 paths in the cull task).
- [ ] Unit scale `view_world_scale` (shared with S4-4, still unverified).

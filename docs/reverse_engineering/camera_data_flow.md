# Camera-data accessors (watch-proven data flow)

Live evidence from `debug_watch` full sweeps (DR0-3 hardware watchpoints, read+write), 2026-10-06. Watchpoint arming/delivery lessons: `src/carrier/debug/watch.hpp`, [../debugging.md](../debugging.md). The culling picture this feeds: [view_frustum.md](view_frustum.md); injection verdicts: [draw_camera_chain.md](draw_camera_chain.md), [../camera.md](../camera.md).

## Every observed accessor is plaintext

(quat/pos/fov/slot0 watches on live views; ~416 hits each while the view was active, zero VM-section accessors, zero writers after activation):

- `0x0048EC46` / `0x0048EC5E` (`RenderQueue_SubmitWorldPackets`, the `0x0048ec3e` staging block): `movss xmm0,[eax+0x01286DA4]` (pos.x) and `movd xmm3,[eax+0x01286DB4]` (quat.x), `eax = idx*0x810` — the per-view staging copy into the frame-ctx block (`this+0xc2110+idx*0x30`), i.e. the plaintext writer of the VM consumer's staged camera input, read once per walk (~1/frame).
- `0x0048A851` / `0x0048A87E` (`ViewEntry_PropagateMatrices`): `fld [esi+0x7c4]` ×2 — the companion-table pos copies.
- `0x0048EC95` (staging block): `movq [ecx+0x10],xmm3` — the staged-quat WRITE (inlined Pose_Copy semantics; the serial `max+1` logic sits right before it).
- `~0x00824A23` = **`Pose_Copy` (`0x00824a10`)** — the game's universal pose relay (EAX=dest, ECX=src, copies {pos3, serial, quat4} with `serial = max(src,dest)+1`; 59 call sites) — reads the staged quat and copies it onward. **The staged camera pose participates in the ordinary plaintext pose pipeline.**

Zero VM-section accessors anywhere in the watched pose chain (entry fields AND staged copies). The ~1/10s cadence (not ~120/frame) confirms the chain is **serial-gated change-detection**: camera pose data moves on CHANGE, not per frame.

## The staging is a ROUND-TRIP

(stagingquat rerun with register capture, live view idx13): `Pose_Copy`'s caller at the staged slot is the producer walk's MUTATED TAIL (call at `0x0048F72D`, undefined split-block region past the walk loop; an inlined fld/fstp + `add [esi+0xc],eax` serial-bump variant right above) with SRC = staged slot, DEST = `ViewEntry+0x7c4` — the staged camera pose is copied BACK into the entry, serial+1.

- New pose VALUES originate in the VM'd consumer (post-walk deref of the staged block `ctx+0xc2110+idx*0x30`) and reach the plaintext ViewEntry only through this copy-back — there is no plaintext writer of camera-pose VALUES; the plaintext chain only relays (explains the long-negative static writer hunt of 2026-10-03).
- Consequence proven by E2: every external write to the entry fields (injection + serial bump) is reverted within one frame — the fields are OUTPUT channels ([draw_camera_chain.md](draw_camera_chain.md)). Injection implication: write the entry's pose/slot/fov fields directly and bump the serials — the relay pipeline (staging round-trip + Pose_Copy chain + DeriveCullTask) propagates on change; no VM fight needed.
- The VM consumer reads camera data from the STAGED copies, not ViewEntry (zero VM hits on the entry fields). Direct consumer probe for culling: the staged 0x30 block ({pos3, serial@+0xc, quat@+0x10}; carrier targets `stagingpos/stagingquat/stagingserial`). Expected writers: the plaintext staging code (~1/frame); the interesting hits are its READERS.

## View refresh is change-gated, not per-frame

- No per-frame ViewEntry pose refresh: pos/quat stayed byte-identical for 20+ s (through two window snapshots) while the view was still staged every walk; fields appear written once at activation. The camera pos carriers (idx 19-34 family; 20-23 at posd 3-7 with live quats) ARE refreshed — by the round-trip copy-back on camera CHANGE.
- View-liveness marker: companion byte `g_ViewTable3 + idx*0x20 + 0x18` (t3) is 00 for the 12 loading templates (shared obj ptr 0x1f758960) and 01 for persistent live views (e.g. idx14: obj 0x1fe28c80, camera pos folded into its worldToView slot matrix). Select views by t3=01 + walk counts (most-walked after a 10s settle). View indices are NOT stable across runs (live view was idx11 in one run, idx23 transient in another, idx13/idx14 in others) — pin only with the logged idx of that run. 24 t3-live views active per frame with near-equal walk counts.

## The culling derive pass (fov/slot0 runs)

- **fov run (live view idx13)** — fovCos2ec was LIVE (0.957826) and the whole derive/cull pass was caught, all plaintext, all change-gated (~1 hit/site/10s window, 5 sites): writers both in `ViewEntry_MatrixFromGlobalCam` (`0x0048A972` `movss [edi],xmm1`; `0x0048A9BC` `fstp [edi]`, `edi = entry+0x2ec`) — the slot-derive pass via ViewManager_Update → PropagateMatrices. Readers: MatrixFromGlobalCam itself (`0x0048A97E`), math helpers `FUN_00401630` (at `0x0048A9A6`) and `FUN_00401750` (Vector_ScaleByScalar: `[eax] = fov * src[0]`), whose caller return `0x0087718b` identified **`ViewEntry_DeriveCullTask` (`0x00876a90`, renamed 2026-10-06; plate there)** — a 5.8KB task body that was UNDEFINED in Ghidra until this run (no static callers: dispatched via the task queue, `TaskQueue_Dispatch 0x00876810` adjacent). Structure: sets the t3 liveness byte, iterates the view's camera slots (switch on kindA4 0..4) with per-kind LOD distance-selection, D3DX transforms, fov-scaled vectors and **bounding-box unions** (`FUN_0040b250/0x0040b4c0`), then a second pass over an array at entry+0x62c (count = flags804>>4 & 0xF, stride 5 floats). **This is the per-view culling/LOD derive: the culling volume is built HERE from the view's fov/slots/camera object — the primary HMD-alignment target.** All its inputs (fovCos/fovSin, slot matrices, camera-object params) are plaintext-writable.
- **slot0 run (live view idx0 — indices are per-run allocations; t3 selection adapts)** — slot matrices are also fully plaintext and change-gated, 3 sites: (1) writer = ViewEntry_MatrixFromGlobalCam at `0x0048AC2B` via **`Matrix_Copy3x4` (`0x00836120`, renamed; `fld [ecx]; fstp [eax]` m32 copy)** with dest=slot0, src = a computed matrix on the camera-object scratch; (2) reader = **`Matrix_MakeWorldToView` (`0x004017d0`, renamed)** at `0x004017E1` — copies viewToWorld out, transposes + negates translation into worldToView (23 call sites incl. box helpers `FUN_0040b6b0` and per-object D3DXMatrixMultiply paths — the slot matrices feed general per-object view-transform math, not just culling); (3) a system-DLL memcpy touching the entry (block copy, same cadence).

## Complete culling-input picture (2026-10-06)

Upstream camera object (camData chain) → MatrixFromGlobalCam writes slot matrices + fovCos/fovSin (plaintext, change-gated) → ViewEntry_DeriveCullTask reads them and builds the culling bounds + per-slot LOD states. Pose reaches culling via the same change-gated serial chain (Pose_Copy; staged copies). ZERO VM involvement in any camera-culling data path observed across quat/pos/fov/slot0/stagingquat watches. Injection design consequence: an HMD-aligned culling frustum must be injected into this plaintext chain respecting the serial change-gating (write the slot/fov/pose fields, then make the cull task re-run — e.g. by advancing the pose serials so the gate opens), and the main draw view can be identified at runtime by matching the cull task's view slot matrix against the decomposed draw-camera VP basis (the vp_camera decomposer already computes it).

## Wine watchpoint facts

DR0-3 arm + verify works on 27 threads; Dr6 is NOT delivered with the trap (ownership via EFlags.TF; per-field attribution needs single-target runs).

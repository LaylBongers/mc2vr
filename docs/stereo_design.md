# Stereo Submission Design

Design for M4+: dual-eye world rendering + HMD presentation. Consumes the M3 outputs
(`ViewEntry` struct + plate comment in Ghidra, hook mechanism proofs, command histogram).
Per-address facts live in Ghidra; this doc carries design decisions, phases, and open work.
Mechanism rules: `docs/launcher_plan.md`. Runtime-mapped frame chain: `docs/render_path.md`.

## Facts this design builds on

- Frame chain (all plaintext, main thread only): `GameShell_FrameTick` → frame pipeline →
  `RenderQueue_SubmitWorldPackets` (`0x0048e620`, producer: walks active `ViewEntry`s at
  `g_ViewTable` `0x012865e0`, publishes packet blocks into the `g_RenderQueue` ring) →
  `RenderShell_RenderFrame` (`0x00855690`) → `BeginSubmit` (Present prev + BeginScene) →
  `RenderCmd_ExecuteStream` (`0x008569d0`, 27-opcode interpreter, ~1.5–3.4k cmds/frame) →
  `EndSubmit` (EndScene).
- `ViewEntry` (Ghidra `/RenderPath`): nine D3D-style 4×4 matrices at `+0x020` (m[0]
  view-to-world with camera pos, m[1] world-to-view with negated pos, others near-identical
  camera variants); FOV half-angle sin/cos at `+0x2ec/+0x2f4`; camera position copies at
  `+0x7ac/+0x7c4`; near-plane-ish params near `+0x188`; `ViewRef` pointers at
  `+0x7e4..+0x7ec`. Entries are template/zero at load; camera data populates when live.
- Mechanisms proven at runtime: trap-based inline/Mid/Vmt installs (no suspension), device
  VmtHook surviving device-lost + `Reset`, `g_RenderShell` slots 4/5 claimable (called 1:1
  with frames), Present-hook caller attribution. DXVK runtime (MinGW-built, Itanium vtables).
- Two primary per-view sub-objects exist inside `g_RenderShell` (at `+0xFDC`, stride `0x3a0`,
  count `*(WORD*)(g_RenderShell+0x2b90)` = 2) — copied per frame into the producer's frame
  context. Unknown but suspicious: 2 = main camera + secondary (UI/overlay?) camera.
- Queue: `g_RenderQueue` ring (elem 96, cap 4096, write position at `+0x10`, observed wrapping
  freely — large headroom). `g_RenderQueue2` carries 2D/overlay submissions.
- Special cameras (satellite designation) submit up to 608 views in one frame; normal
  gameplay tens per frame.

## Architecture

**Producer-side eye duplication**: inside the `SubmitWorldPackets` per-view loop, submit
each type-2 world view's packets twice — once per eye — with eye-offset camera data. 2D/overlay
(`g_RenderQueue2`) renders once. Compositor at `Present` delivers the result to the HMD.

Chosen over the alternatives:

- Consumer-side replay (`ExecuteStream` opcode dispatch duplication): one interception point,
  but the stream is consumed linearly (replay needs buffering), draw state would apply twice,
  and per-eye render-target switching mid-replay is harder than doing it at the source.
  Keep as fallback if the producer loop body proves unmappable.
- Whole-frame duplication via the slot-4/5 hooks: duplicates game logic ticks and 2D UI;
  rejected.

Rationale for producer-side: the game's own pipeline already handles per-view visibility, LOD,
and state caching correctly — duplicating at the source inherits all of it, per-eye, for free.

## Phases

### S0 — Loop-body RE (prerequisite, static)

Map the per-view loop body (`0x0048e9ea` → loop back-edge, plus the packet-emit sites) to
answer the two questions the design depends on:

1. **Packet data residency**: do the emitted packets carry inline copies of the camera
   data (matrices/FOV/viewport), or pointers back into `ViewEntry`/the frame context?
   - Pointers → patching `ViewEntry` between eye passes suffices.
   - Inline → intercept the copy site (MidHook) or patch the source fields in-place per eye.
2. **Viewport/render-target flow**: where the per-eye pixel rect comes from (entry field vs
   frame-context global). This decides whether SBS halves fall out naturally or need a
   patched field per eye.

Also: identify the loop's back-edge instruction (the second MidHook site for S3) and whether
the two primary sub-objects (`g_RenderShell+0xFDC`, count 2) participate per view.

### S1 — Field-use instrumentation (one run)

MidHook at the S0 copy sites; log (one-shot + throttled) which `ViewEntry` offsets are read
per view per frame. Cross-check against the M3 steady-state dumps (m[0]/m[1], FOV sin/cos,
`+0x7ac/+0x7c4` positions expected). Classify the ~608-view satellite frames (are the extra
views world-renderable, or e.g. minimap/scope RTs that should NOT be duplicated?).

### S2 — Eye math

Per type-2 view: `pos' = pos ± right * IPD/2` for the camera position in m[0] (and the negated
position in m[1]), rotation unchanged; FOV/aspect → HMD projection (replace `+0x2ec/+0x2f4`
half-angle sin/cos per-eye if the pipeline consumes them directly, else patch projection
where S0 finds it). IPD + pose from the HMD runtime. All patching happens on the main thread
inside the producer loop — same-thread writes, no synchronization.

### S3 — First duplication (first visual milestone)

Mechanism: MidHook at the loop back-edge — after a type-2 view's packets are emitted, patch
the view's camera fields for the other eye (S2 math), re-run that iteration's emit, restore.
Iterate until each eye renders into its target (SBS halves of the backbuffer, or separate
eye RTs — per S0 answer). Skip duplication for views classified non-world in S1.

Risks to watch: queue pressure at 2× peak (608-view frames → 1216 submissions; cap 4096,
observed headroom large — poller confirms); the render-state cache is per-stream (render_path
"cache desync" warning) — duplicating whole per-view packet blocks should stay cache-coherent
since blocks are self-contained, but verify.

### S4 — Presentation / HMD runtime

- `Present` VmtHook (proven) as the compositor entry: submit the frame to OpenVR/OpenXR
  (submit both eye textures, or the SBS target). Interop blit needs no swapchain changes —
  the game's Present continues to the monitor untouched (debug-friendly).
- Slot-5 (`PostUpdateHook`) claim as the per-frame VR orchestration point: sample HMD pose,
  marshal to the main thread before the producer loop runs, feed S2. Slot-4
  (`EndOfFrameHook`) for end-of-frame bookkeeping (timewarp input, frame pacing via the
  game's own `g_FrameDeltaSec`/framerate policy bypass — see `main_game_loop.md`).
- Decide OpenVR vs OpenXR when integrating (OpenXR preferred on Proton/RADV via
  wineopenxr, which the prefix already has; verify runtime availability in the prefix —
  `openvrpaths.vrpath` and `wineopenxr64.json` exist from prior use).
- UI/2D layer (`g_RenderQueue2`): render once; composite over both eyes via a world-space
  panel or screen-space overlay in the compositor (decide in S4 prototypes).

### S5 — Motion controls (separate track)

Follows the logic-mod track in `docs/launcher_plan.md` (XInput stubs `0x00a64d56/0x00a64d5c`,
idle-reset buffer pair `0x017d30e8`/`0x00f7fb90` resolution first). Pose marshal point is the
slot-5 hook (S4). No new mechanism — this doc ends at visual VR.

## Hook inventory (all sites proven mechanisms)

| Site | Mechanism | Phase | Status |
|---|---|---|---|
| SubmitWorldPackets loop head `0x0048e9ea` | MidHook | S0/S1/S2 anchor | installed (M3) |
| SubmitWorldPackets back-edge / copy sites | MidHook | S0 find, S3 use | pending S0 |
| `ViewEntry` camera fields | in-loop same-thread patch | S2/S3 | pending S0 residency answer |
| device `Present` (slot 17) | VmtHook | S4 compositor | installed (M2) |
| device `Reset` (slot 16) | VmtHook | S4 param changes | installed (M2) |
| `g_RenderShell` slot 5 (`PostUpdateHook`) | cloned-vtable claim | S4 pose sampling | installed (M3, counting noop) |
| `g_RenderShell` slot 4 (`EndOfFrameHook`) | cloned-vtable claim | S4 bookkeeping | installed (M3, counting noop) |

## Open questions (tracked, non-blocking for S0/S1)

- Whether the two primary sub-objects (`g_RenderShell+0xFDC`, count 2) are eyes, cameras, or
  something else — if they are already eye-slots, patching THERE may be the cleaner S2/S3
  site than `ViewEntry`. S0 answers.
- `g_RenderQueue2` consumer (second `ExecuteStream` pass inside `RenderFrame`?) — S4 needs
  to know when the 2D stream is consumed relative to Present.
- Whether the nine matrices are per-view redundant copies of one camera or distinct camera
  roles (LOD/shadow) — S1 classification; duplicate only the ones that feed packets (S0).
- Frame pacing: game runs vsync-locked 60 Hz (`interval=0`, DISCARD); HMD typically 90 Hz.
  Present-hook compositor can run at HMD cadence independent of the game loop (pose
  extrapolation/timewarp via the HMD runtime), leaving game pacing untouched. Decide in S4.

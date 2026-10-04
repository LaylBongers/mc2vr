# S2c Handover — Second Draw Pass (Stream Replay)

Self-contained brief for the agent picking up S2c. Everything here is also in the
referenced docs; per-address facts live in Ghidra plates, not repeated here.

## Mission

Render the frame **twice — once per eye**. During an eye pass the
`viewContextData` VS constants are rewritten for that eye (already working,
see "Current state"); the S2c work is to make the game draw the whole frame a
second time with the other eye's constants, into an eye render target. The
mechanism: buffer the frame's command stream and replay it through the
plaintext interpreter `RenderCmd_ExecuteStream` (`0x008569d0`, 27 opcodes,
~1.5–3.4k cmds/frame). The M3 opcode MidHook at `0x008569f5` is ALREADY
installed as the stream tap — extend it, don't re-site it.

## Read first (in order)

1. `AGENTS.md` (project root) — rules, iteration loop, doc conventions.
2. `docs/stereo_design.md` — architecture (GPU-boundary per-eye injection), §S2
   remaining work, §Open questions (several are S2c-specific), the verified
   view-channel facts (VP-row layout, pass gate, register resolver).
3. `docs/launcher_plan.md` — mechanism rules (do-not-re-litigate list) + hook
   inventory + build/test commands.
4. `docs/render_path.md` — frame chain and timing (esp. the per-frame GPU-sync
   event-query spin in `LtiRenderer_BeginSubmit`).
5. `docs/ghidra-reva.md` — how to use the ReVa MCP tools correctly.

## Current state (2026-10-04, all verified unless noted)

- M0–M3 complete; S0/S1 complete; S2 camera channel **done and verified**:
  - `view_row_rewrite=stereo` pans `D = ±right·IPD/2` along the camera right
    axis, derived per frame from raw VP row0 uploads (`right =
    normalize(row0.xyz)`, view row0 = camera right). VERIFIED in-game: unit-length,
    tracks yaw through 180° turns and oblique headings.
  - Eye sign alternates every `view_stereo_hold` s (A/B hold — this is what S2c
    replaces with real per-eye draw passes). Conf keys: `view_ipd`,
    `view_stereo_hold`, `view_asym_x/y` (asym-projection NDC channel, default 0).
  - Pass gate: rewrite applies only while RT0 == backbuffer size (observed via
    device SetRenderTarget, slot 37). **With two eye passes the gate must also
    distinguish the two eye RTs** — same size, so key on RT identity.
  - Constant-map publisher: MidHook at `MC2_VCD_UPLOAD_CMP` `0x00855a78`
    (EDI = current technique) publishes exact `viewContextData`/`ViewProj`
    (reg, count); the device VmtHook on SetVertexShaderConstantF (slot 94)
    applies the rewrite on a scratch copy. Same-thread, immediately after.
- S4 direction settled: **OpenVR/SteamVR, NOT OpenXR** (Valve's OpenXR driver
  lacks 32-bit+DX9; wineopenxr is present+registered in the prefix but dead —
  do not re-investigate). The OpenVR bridge for i386 IS installed in this
  prefix (`C:\vrclient\`, 32-bit `vrclient.dll` + `openvr_api_dxvk.dll` in
  syswow64). S2c itself needs none of this — it's D3D-local.

## S2c engineering list (from stereo_design.md, expanded)

1. **Stream capture**: tap the frame's command stream at the `0x008569f5`
   MidHook (EAX=opcode there; the stream pointer/parse state must be recovered
   from the surrounding function — see Ghidra `RenderCmd_ExecuteStream`).
   First deliverable: a payload census per opcode (what state each carries),
   logged or dumped to `<GAME_DIR>/mc2vr/`. Note `FUN_00858980` and
   `FUN_00852740` (opcode 0x08 draw) are still unnamed — expect RE work here.
2. **Buffering**: copy the stream (and any referenced draw data) for the frame.
   Per-draw RT/viewport switching already happens in-stream (SetViewport fires
   2–3.6k×/frame) — assume the replay must re-apply draw state, not just re-execute.
3. **Replay**: re-run the buffered stream through the interpreter per eye with
   that eye's rewrite active and draws redirected to that eye's RT. Per-object
   re-uploads during replay are safe (objectData/BoneMatrixArray carry no view
   content). Open question: which of the 27 opcodes carry draw state that must
   be reset between eye passes.
4. **Eye RTs**: create at carrier init (device creation params are fixed
   post-boot, but ADDITIONAL render targets are fine — no Reset needed).
   Backbuffer-size textures; the existing `LtiRenderer_EndSubmit` StretchRect
   (RT0→backbuffer, `LtiRenderer+0x3ea4`) is the co-optable copy path.
5. **Timing**: replay happens after the BeginSubmit GPU-sync point each frame;
   game is vsync-locked 60 Hz. Pacing design is S4's, but keep the replay on
   the render thread, bracketed by the existing frame chain.

## Suggested staging (each step verifiable on the monitor, no HMD)

- **S2c-0**: capture + census only (read-only), no behavior change.
- **S2c-1**: replay the buffered stream UNCHANGED (same eye, into the same RT)
  — proves buffering/replay is state-safe: any visual regression = state bug.
- **S2c-2**: replay into a second eye RT with the OTHER eye's rewrite active;
  A/B via the existing hold timer driving eye selection, dump both RTs to PNG
  (extend `tools/analyze_dumps.py` if needed) and check parallax geometry.
- **S2c-3**: drive eye selection per-frame deterministically (producer of the
  replay chooses; the conf hold stays for debugging).

## Hard rules (violating these wastes days — see launcher_plan.md for why)

- Plaintext `.text` only. NEVER hook anything at `0x01a48000+`, the VM entry
  stub `0x0050f660`, or any VM-stub thunk (calling thunks is fine, proven).
  The packet interpreter behind `0x0050f660` is VM'd — bracket its plaintext
  call sites (`0x004c99f9`/`0x004c99fe`), never the stub.
- No thread suspension at hook install (SafetyHook trap-based; suspension
  deadlocks on the CRT heap lock).
- The device vtable is the only sanctioned vtable patch (cloned-vtable VmtHook).
- Hook objects leak by design; handlers must match the original convention.
- The game's upload buffers are never modified in place — rewrite on scratch
  copies returned to the driver call (see `view_rewrite.cpp`).

## Build / test / iterate

- Build: `cmake -B build/win32 -DCMAKE_TOOLCHAIN_FILE=cmake/i686-w64-mingw32.cmake && cmake --build build/win32`.
- Chain selftest (needs unsandboxed terminal — wineserver uses Unix sockets,
  and `/tmp/opencode` must exist first): `tools/selftest/run.sh` — run after
  every carrier change.
- Live: the human runs `./launch.sh` into GAMEPLAY (not menu) and reports;
  audit `<GAME_DIR>/mc2vr/mc2vr_*.log` (path from `launch.conf`).
- The DEPLOYED `mc2vr.conf` at `<GAME_DIR>/mc2vr/` is never overwritten by
  `launch.sh` (`cp -n`) — conf changes there are manual (bitten us once).

## Open questions the new agent inherits (S2c-relevant)

- Opcode draw-state reset set (item 3) — needs the S2c-0 census.
- `g_RenderQueue2` (2D/overlay) consumption timing vs Present — needed for the
  S4 compositor, add counters when S4 starts.
- Why `ViewManager_Update` never fired in the S1 traced run (curiosity, not a
  blocker).

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

- **S2c-0 DONE (code)** — built + selftested, awaiting first live census run.
  `src/carrier/stream_capture.cpp` extends the `0x008569f5` opcode MidHook (via
  `render_dump.cpp`; same site, not re-sited). At the cmp, EBP = current command
  and `[esp+0x14]` = stream base (entry pushes ecx/ebp/esi/edi; stream is stack
  arg 1) — EBP==base identifies a stream's first command; the whole stream is then
  walked + copied with the RE'd size table (op 0 terminator INCLUDED — replay
  copies must stay terminated). Read-only, default off. Conf keys:
  `stream_capture=on`, `stream_dump_frames=N` (raw dumps to
  `mc2vr_stream_frame<N>.txt`, `stream_dump_delay` s after capture start), parsed
  by `tools/analyze_dumps.py`. Census = per-opcode "S2c census" payload samples
  (logged once each) + a 10s "S2c window" line whose walk-vs-hook delta MUST be
  0 (nonzero = size-table bug or mid-frame stream mutation). Full opcode table +
  semantics on the `RenderCmd_ExecuteStream` plate in Ghidra.
- Key interpreter facts (Ghidra plate): (a) **dedupe** — a stream pointer equal to
  `g_LastExecuteStream` `0x011697b8` is skipped whole, so replay MUST execute a
  COPY (different pointer); (b) op 0x02 VS-constant upload carries
  `{op, startReg, dataPtr, vec4count}`, count passed in EDX (custom convention);
  op 0x03 PS likewise; (c) op 0x08 = screen-constant refresh (viewport-derived,
  uses ExecuteStream args 2/3 = VS/PS technique objects) — view-dependent, needs
  eye-aware handling at replay; (d) op 0x13 is a 2-dword NO-OP (jump-table entry
  lands on case 0x0d's advance tail — Ghidra's decompiler drops the case);
  (e) ops 0x10/0x11 are the in-stream RT/viewport switchers (gated `c[4] ∈ {1,2}`)
  — the S2c-2 eye-RT redirect targets these.

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

1. **Stream capture**: DONE (S2c-0, `src/carrier/stream_capture.cpp`) — census
   runs at the `0x008569f5` MidHook; `FUN_00858980` (→ `RenderCmd_ResetPassState`)
   and `FUN_00852740` (→ `RenderCmd_SetScreenConstants`, opcode 0x08) are now
   named in Ghidra with the full opcode plate on `RenderCmd_ExecuteStream`.
2. **Buffering**: DONE in-mechanism (S2c-1) — but NOTE: streams contain NO
   geometry draws (op 0x0f is a device Clear, all others are state; draws are
   SubmitToGPU steps 7-8 per record). The second draw pass therefore re-invokes
   PgPrimitive_SubmitToGPU wholesale (InlineHook @ entry 0x00855690, conf
   `frame_replay=on`, `src/carrier/stream_capture.cpp`) — the game's own mutated
   walk re-runs all state + streams + binds + draws from a cleared scene, so
   nothing is reimplemented. Raw stream capture (S2c-0) remains the census/
   validation instrument.
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

- **S2c-0: DONE AND LIVE-VERIFIED** (run 2026-10-04 ~13:20): delta=0 in all 7
  windows (~1.5M commands) — the walk table matches execution exactly (the M3
  hook histogram agrees per-window, per-opcode). No runaway/truncation. Dumped
  frames 895-899. Live census facts (gameplay): ~75-207 streams/frame,
  ~500-900 cmds/frame, 21-22 opcodes active; streams are SHORT (2-7 cmds,
  mostly `06 08 00` / `06 03 00` shapes: RT0+DS set, then screen-const or PS
  upload, then halt) and live in a reused pool at 0x2029xxxx-0x2033xxxx
  (constant data at 0x2038xxxx, clip-plane source static at 0x00d69f40) —
  S2c-1 replay copies must be taken per frame at first command (the capture
  already does this). Per-stream arg2/arg3 = VS/PS technique objects (arg2
  0x0196eef8 = the main technique from the view: logs). op 0x08 payloads carry
  the CURRENT pass viewport size (observed 1x1 .. 2560x1440) — per-eye replay
  with differently-sized eye RTs must rewrite op 0x08, else use backbuffer-sized
  eye RTs (preferred; also keeps the RT pass gate simple).
- **S2c-1 (DONE + VERIFIED, 2026-10-04)**: second draw pass via double-invoke
  of PgPrimitive_SubmitToGPU (`frame_replay=on`). Live run: visuals clean,
  replay 1:1 with frames, ~2x streams/frame, delta=0, no runaways; cost
  avgMs=16.6 -> the game holds a stable 30 Hz (each frame 33.3ms = both
  passes). The 2s eye-flip cadence during the run showed no mid-frame split
  artifacts.
- **S2c-2 (IMPL, 2026-10-04, awaiting live run)**: `eye_pass=on` +
  `eye_rt=on` + `eye_dump_frames=5` (all require frame_replay=on):
  pass 1 = LEFT, pass 2 = RIGHT, deterministic per frame
  (view_rewrite per-pass override replaces the hold timer while active).
  Pass-2 device SetRenderTarget(0, mainRT) and StretchRect sources pointing
  at the main RT are redirected to a carrier-created backbuffer-sized eye RT
  (src/carrier/eye_replay.cpp; main RT recorded from the first slot-0 set,
  re-recorded after Reset; eye RT dropped on Reset). EndSubmit's own
  RT0->backbuffer StretchRect is ALSO redirected in pass 2 (its source is the
  game-cached g_CurRenderTarget pointer == mainRT), so the monitor shows the
  RIGHT (pass-2) eye each frame; the LEFT image is dumped from mainRT at the
  1->2 pass boundary. Depth is shared (BeginSubmit clears RT+depth each pass).
  VERIFICATION: `mc2vr_eye_left/right_frame<N>.bmp` pairs after
  stream_dump_delay; `tools/analyze_dumps.py <log>` reports the measured
  horizontal parallax shift per pair (synthetic-fixture tested; nonzero shift
  with SAD < shift-0 SAD = stereoscopy proven). **First live attempt CRASHED
  at the dump window (2026-10-04, 2x): the surface-vtable slot guess for
  LockRect/UnlockRect was wrong (10/11 — slot 10 is GetType, whose small
  positive D3DRESOURCETYPE return passed SUCCEEDED() and left pBits garbage
  -> read through it segfaulted). RESOLVED from the d3d9.h interface
  (surface vtable: GetContainer=11, GetDesc=12, LockRect=13, UnlockRect=14;
  device slots re-verified: CreateRenderTarget=28, GetRenderTargetData=32,
  StretchRect=34, CreateOffscreenPlainSurface=36) plus a format guard
  (32-bit RGB only) and a pBits/pitch sanity check. Everything BEFORE the
  dump was already proven in that run: rtRedirects=482 blitRedirects=298 per
  10s window at the main menu, replay 1:1, delta=0, Present=2x frames.
  Remaining watch item: pass-2 post-effects reading the main RT via paths
  other than StretchRect (UpdateSurface/UpdateTexture NOT redirected yet).
- **S2c-2 LIVE-VERIFIED (run 2026-10-04 ~13:45)**: stable run after the slot
  fix (30 Hz, replay 1:1, delta=0, gameplay rtRedirects ~1500 / blitRedirects
  ~900 per 10s). VISUAL "camera moves rapidly left and right" = the SUCCESS
  signal: EndSubmit#1 copies pass-1 LEFT -> backbuffer, EndSubmit#2's
  redirected copy writes pass-2 RIGHT -> the same backbuffer, and the two
  per-frame Presents alternate L/R on the monitor at the Present rate. Both
  per-eye passes render fully every frame with distinct view constants.
  BMP dumps SKIPPED by the format guard: the main scene RT is
  D3DFMT_A16B16G16R16F (113) — fp16 HDR 2560x1440 (new RE fact, plated on
  LtiRenderer_EndSubmit; the EndSubmit RT0->backbuffer StretchRect is an
  fp16->backbuffer blit).
- **S2c-2 NEXT STEPS** (order): (1) dump_surface: decode fp16
  (A16B16G16R16F, 8 bytes/px) + simple tonemap -> BMP, quantify parallax
  with tools/analyze_dumps.py; (2) optional monitor pin: in device.cpp's
  stretchrect_hook, if pass==2 and the source was redirected, SKIP the
  original call (return S_OK) so the backbuffer keeps the LEFT image (kills
  the temporal flicker for monitor debugging; the S4 compositor consumes the
  eye RT instead); (3) S2c-3 is effectively satisfied (deterministic
  per-frame eye pair) — S4 Present-hook compositor is the consumer; audit
  UpdateSurface/UpdateTexture redirects only if a visual artifact appears.
- **S2c-2 next steps IMPLEMENTED (2026-10-04, awaiting live run)**:
  (1) fp16 decode — `dump_surface` now accepts A16B16G16R16F (113, 8 B/px:
  R,G,B,A halves; decode host-verified exhaustively incl. subnormals) plus
  the old 32-bit RGB path, tone-mapped per channel (Reinhard + ~sRGB gamma
  via 8192-entry LUT over [0,16), brighter clamps near-white) into the same
  24-bit BMPs, so tools/analyze_dumps.py's parallax report works unchanged.
  (2) monitor pin — conf `eye_monitor_pin=on` (needs eye_rt=on): pass-2
  StretchRects whose source was redirected AND whose dst is the swapchain
  backbuffer (GetBackBuffer slot 18, cached, ref released on Reset) are
  skipped (return S_OK) so the monitor holds a stable pass-1 LEFT image
  instead of alternating L/R each frame; mid-frame pass-2 reads still run
  (only the EndSubmit RT->backbuffer copy is skipped). Window report now
  includes pinSkips. This is also the S4 steady state (compositor eats the
  eye RT, Present keeps showing pass 1). Conf template updated; the
  DEPLOYED conf at <GAME_DIR>/mc2vr/ is manual — add the key there.
  LIVE-RUN RECIPE: keep frame_replay=on eye_pass=on eye_rt=on
  eye_dump_frames=5, eye_monitor_pin=on, stream_dump_delay=60 (be in
  GAMEPLAY by then; black frames are auto-skipped), expected: stable LEFT
  image on the monitor (no L/R flicker), 10 BMP dumps (5 pairs), log lines
  "eye: dumped left/right ...", pinSkips/bbRtRedirects>0 in the eye window
  report, delta=0, Present=2x frames. Then
  `tools/analyze_dumps.py <GAME_DIR>/mc2vr/mc2vr_carrier.log` reports the
  measured horizontal parallax per pair at 1-px resolution (upgraded
  2026-10-04: full-res numpy shift search, +/-64px window, sign convention
  documented in shift_sad; pure-Python 4px-grid fallback if numpy is
  missing; regression fixture tools/eye_pair_fixture.py mirrors the
  carrier's fp16 decode + tonemap and is recovered exactly). SAD at best
  shift << shift-0 SAD = stereoscopy proven. If dumps come out washed out,
  note it — a future `eye_dump_exposure` key is the knob (not needed for
  parallax math).
- **S2c-2 FIRST LIVE RUN of the fp16/pin build (2026-10-04 14:00, menu ->
  gameplay ~50s in)**: fp16 dump pipeline WORKED (10 BMPs, 2560x1440,
  delta=0, replay 1:1, rewrite active in gameplay ~190k rows/10s window)
  but two failures + one new RE fact:
  (a) BMPs were pitch-black — the dump window opened at 15s = intro/loading
    (gameplay starts ~52s after boot; streams/frame jumps to 264+ only
    then). FIXED: dumps are now CONTENT-GATED (nonempty probe; black pairs
    don't count against eye_dump_frames, retries at 0.5s cadence) and the
    deployed conf moved stream_dump_delay 15 -> 60.
  (b) BMP filenames were mojibake (e.g. mc2vr_eye_<CJK>_frame453.bmp):
    `_snwprintf(L"...%s...", narrow_tag)` — plain %s in a WIDE printf means
    a WIDE string (MSVCRT + C99 agree), so the narrow "left"/"right" literal
    bytes (plus GCC-merged adjacent literals, e.g. "eye: dump window
    closed") were reinterpreted as UTF-16. Also truncated the log's %ls
    display (CJK unmappable in cp1252). FIXED: tag widened by hand (no
    printf-format ambiguity). Same-class lesson: never pass narrow strings
    to wide printf formats in this codebase.
  (c) NEW RE FACT — gameplay's final composite into the backbuffer is NOT
    a StretchRect from mainRT: the pin (v1: skip pass-2 blits with
    src==mainRT && dst==backbuffer) fired 112/79/301/172x per 10s window in
    MENU/loading (backbuffer identity via GetBackBuffer(0) slot 18 is
    CORRECT — menu EndSubmit copy is mainRT->backbuffer and got pinned) but
    ZERO in gameplay while the monitor still alternated L/R — so in
    gameplay no pass-2 StretchRect writes the backbuffer at all; the pass-2
    final image reaches the presented surface via another path (draw with
    RT0=backbuffer, or UpdateSurface/UpdateTexture). Present params plated:
    backbuffer 2560x1440 fmt=22 (X8R8G8B8), fullscreen, swapeffect=1
    (DISCARD), count=1 — the EndSubmit copy is an fp16->X8R8G8B8 conversion
    blit. FIXES SHIPPED (awaiting next run): pin v2 skips ANY pass-2 blit
    into the backbuffer; pass-2 SetRenderTarget(0, backbuffer) is
    redirected to a carrier sink RT (draw-path pin, backbuffer-desc-sized);
    UpdateSurface (slot 30, same rules as StretchRect) + UpdateTexture
    (slot 31, counter only — texture src can't be matched vs the RT surface)
    are now hooked; window report adds bbRtRedirects / updRedirects /
    updPinSkips / updTex / dumpEmpty so the next audit pinpoints the actual
    gameplay mechanism if the monitor still alternates.
- **S2c-2 (staging bullet, historical)**: replay into a second eye RT with the OTHER eye's rewrite active;
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

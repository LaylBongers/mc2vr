# Launcher + Carrier Plan (TEMPORARY)

The M4 design now lives in `docs/stereo_design.md` (S1 evidence: `docs/s1_camera_hunt.md`); this doc remains the launcher/mechanism/hook reference. Per-address facts live in Ghidra; not repeated beyond the hook list below.

## Architecture

`launch.sh` → `mc2vr_launcher.exe` (win32 i386, in-prefix via Proton) → `CreateProcess` game → inject `mc2vr_carrier.dll` (win32 i386) → carrier installs SafetyHook hooks in-process.

- Carrier = delivery vehicle only; mechanism stays "patch the code itself". Rejected: external cross-process `WriteProcessMemory` patching (no prologue relocation, no MidHook, messier under Wine).
- Everything 32-bit; image can't relocate, base fixed `0x00400000` — all hook VAs are literal runtime addresses.
- DXVK runtime in this prefix (D3D9 → Vulkan → RADV): all hooked objects/vtables are DXVK implementations; same COM contract, expect quirk-level behavior differences.

## Build / deploy / test

- `cmake -B build/win32 -DCMAKE_TOOLCHAIN_FILE=cmake/i686-w64-mingw32.cmake && cmake --build build/win32` → `build/win32/bin/{mc2vr_launcher.exe,mc2vr_carrier.dll}`. `launch.sh` deploys to `<GAME_DIR>/mc2vr/` and runs under Proton.
- SafetyHook v0.7.0 vendored in `vendor/safetyhook/` (amalgamated + Zydis, BSL-1.0, C++23).
- Build lock: carrier verifies exe size + SHA-256 on disk against `src/carrier/build_lock.h`, refuses to hook on mismatch. Regenerate after re-RE: `tools/gen-build-lock.sh <exe>`.
- `tools/selftest/run.sh`: full chain test under plain Wine using a sleeper stand-in (base 0x400000 + ticking counter at the literal VA). Also verifies build-lock refusal. Run after carrier changes.

## Launch sequence (launcher)

1. Resolve paths from deploy location (`<game dir>/mc2vr/`).
2. `CreateProcessW` game (no suspension).
3. Boot gate: poll frame counter `0x011755bc` until TWO value changes (a single early init write could fake one). Counter starts at 0 and spins uncapped pre-D3D (~1400 Hz); gate fires ~0.2–1s after start. Main loop alive ≠ rendering up, but the D3D device already exists by carrier-init time. Strict "render ready" gate if ever needed: `g_D3D9` (`0x01175284`) ≠ 0.
4. Inject: `CreateRemoteThread` + `LoadLibraryW`. Works under Proton (proven). Fallbacks (manual mapping, import stub) never needed.
5. Carrier `DllMain` only spawns the init thread (loader lock). Logs: `<deploy dir>/mc2vr_{launcher,carrier}.log`.

## Mechanism (proven, do not re-litigate)

- SecuROM is inert under everything we do: live `.text` inline patches, in-process `.data` writes, direct VM-stub thunk calls, DLL injection, cross-process reads. Verified over multi-minute runs. Still: never attach a debugger/ptrace; carrier is the only sanctioned probe.
- Mod code may CALL VM-stub thunks (`GetD3DDevice` etc.); never HOOK them or anything at `0x01a48000+`.
- Hook install needs NO thread suspension: SafetyHook v0.7.0 install is trap-based (page guard + VEH IP fixup) — atomic w.r.t. execution. External suspension is FORBIDDEN (v0.7.0 heap-allocates during `create_inline`; suspended thread holding the CRT heap lock would deadlock).
- VmtHook (cloned-vtable vptr swap) works on DXVK's MinGW-built objects (`VMT_HEADER=2` matches DXVK's Itanium vtables); safe from a foreign thread (aligned pointer store; in-flight calls keep the old valid vtable). Survives device-lost + `Reset` cycles.
- MidHook = register-context probe at arbitrary instructions — the tool for thiscall/unknown-convention sites (read ECX/ESP from context, no dispatch semantics).
- Hook objects are leaked by design: destructors would restore vptrs/bytes during process teardown after the target objects may be freed.
- Hook handlers must match the original convention; zero-arg void functions are convention-agnostic on i386.

## Hook sites

| Site | VA | Type | Status |
|---|---|---|---|
| `GameShell_FrameTick` | `0x00630e10` | InlineHook | DONE (M1): frame counter, 10s timing reports, `hooks::frame_count()` |
| `GetD3DDevice` thunk | `0x0047f2f0` | direct call | DONE (M2): device capture at init (device pre-exists; thunk is hot-path — don't hook it) |
| device vtable | runtime | VmtHook | DONE (M2): Present 17 / BeginScene 41 / EndScene 42 / Reset 16 — slots pinned (SetRenderState@57 anchor + runtime 1:1 call-pattern confirmation); present params via swapchain `GetPresentParameters` (slot 9) on first Present; Reset logs new params. DONE (S1): SetTransform 44 / SetViewport 47 added — run result: SetTransform is NEVER called (shader-driven). DONE (S1b): SetVertexShaderConstantF 94 added as the real camera channel (register-row cache + matrix classification) |
| `LtiRenderer_BeginSubmit` entry | `0x0074aaa0` | MidHook probe | DONE (M2.5): answered driver + vtable questions; one-shot, currently dormant |
| `RenderCmd_ExecuteStream` | `0x008569d0` | MidHook at opcode cmp `0x008569f5` (EAX=opcode, 27 ops) | M3 histogram + S1 bracket tap + S2c stream tap — installed |
| `RenderQueue_SubmitWorldPackets` | `0x0048e620` | MidHook at view-loop lea `0x0048e9ea` (ESI=idx, ECX=type, EAX=off; entry+0x7e4 = per-view object; 3rd table `0x014095e0` stride 0x20) | M3 view aggregation — installed. S0-mapped, uninstalled: staging `0x0048ef71` (superseded S3 clone site), iterator `0x0048f013` (re-emit fallback) |
| `g_RenderShell` slots 4/5 (`EndOfFrameHook`/`PostUpdateHook`) | live vtable = base `LtiRenderer_vtbl` `0x00bd38e8`; object `0x017ceaf0` (=`*g_RenderShellPtr` `0x00dfb2f8`) | cloned-vtable swap (VmtHook), counting no-op handlers | M3 claim PROVEN 1:1 with frames. Risk (unobserved): the `0x00a7d950` reinit fragment reinstalls the original vtable — re-claim on device-lost if counts stop |
| `g_RenderQueue` counters | `0x00ff3618` | poller thread (250ms), 10s window reports | M3 producer rhythm — installed (ambient telemetry). S0: halves resolved — producers do `countersB(+0x14).low += count`, `countersA(+0x10).high += count`; `countersA.low` advanced only by the VM'd consumer (explains M3's "ring position" wrap). S1: countersA now also sampled in-hook at 4 bracket points (see pipeline pre-VM-stub row) |
| viewContextData upload gate | `0x00855a78` | MidHook | DONE (S2 run 22): `cmp [edi+0xd8],0` in PgPrimitive_SubmitToGPU before the gated Dx9_SetVertexShaderConstantF call; EDI = CURRENT technique — publishes exact viewContextData reg/count (+0xd4/+0xd8) + ViewProj (+0xdc/+0xe0) to the device-level rewriter |

Rules: plaintext `.text` only; never `0x01a48000+` or VM-stub thunks; the device vtable is the one sanctioned vtable patch (via clone). NOTE (S0): the ring/packet interpreter is itself VM-protected (stub `0x0050f660` via call site `0x004c99f9`) — probe it by bracketing the plaintext call sites, never by hooking the stub.

## Milestones

- M0 (toolchain, launcher, injection, carrier attach) — DONE.
- M1 (FrameTick stability; probes: `.data` write+restore, VM-stub call) — DONE. SecuROM live-patching caveat DISCHARGED.
- M2 (device capture, VmtHook, Present/EndScene/Reset pinning, present params) + M2.5 (BeginSubmit probe: frame driver + live vtable) — DONE. Per-frame submit chain is fully plaintext; see `render_path.md` "Frame driver chain".
- M3 (**COMPLETE**): view-table dump + command histogram + slot claim + queue poll — verified in-mission (runs incl. cutscene, boat/crouch cameras, PDA, satellite designation, alt-tabs, mission load).
  - Slot 4/5 claim (VmtHook clone on `g_RenderShell`): **PROVEN** — 1:1 with frames through everything; no vtable reinstalls.
  - View entry field map: `ViewEntry` struct created in Ghidra (`/RenderPath`, applied at `g_ViewTable` `0x012865e0`; plate comment has the evidence). Load-time entries are template/zero; camera data — **nine D3D-style 4×4 matrices at +0x020** (m[0] viewToWorld w/ camera pos, m[1] worldToView w/ negated pos, others near-identical camera variants; identity-diag or orthonormal 3×3 rotations), **FOV half-angle sin/cos at +0x2ec/+0x2f4**, camera position copies at +0x7ac/+0x7c4, near-plane-ish params near +0x188 (5 normal / 20 satellite-style / 9.81 water view), `ViewRef` pointers +0x7e4..+0x7ec (vtable `0x00bac1c8` = `ViewRef_vtbl`), handle-id mirror at +0x800.
  - View loop: active views only, per-frame submissions 1..608 (satellite designation), indices to 239 (~256-entry table). ~~`DAT_00d29e60` is a dynamic registered-view count~~ — **corrected in S0**: it is the active-list HEAD index.
  - Type-4 views: never observed in any scenario — deprioritized.
  - Command histogram: gameplay ~21 opcodes (menu 9), 1.5k–3.4k cmds/frame; bins 00/01/02/03 dominate; 27 opcodes defined.
  - Queue+0x10 is a ring POSITION (wraps 0..cap-1; capacity 4096, elem 96); +0x14 stayed 0. — **reinterpreted in S0**: +0x10 = countersA (consumer-advanced), +0x14 = countersB (producer-advanced); see `render_path.md`.
  - Carrier keeps the M3 instrumentation as ambient telemetry for future runs.
- M4: stereo rendering. **S0 COMPLETE** (loop-body RE, facts in Ghidra: intrusive view list, per-view {size,ptr} elements, VM'd post-walk consumer, staging cap 768). **S1 COMPLETE (2026-10-02, 17 runs — evidence + S2 handoff in `docs/s1_camera_hunt.md`)**: the draw camera never surfaces in patchable plaintext data; the GPU-boundary SetVertexShaderConstantF rewrite is the per-eye channel — **verified visible (runs 16–17)**; ambient verification mode via `mc2vr.conf` (`gpu_boundary_rewrite=off|on|pulse`). **S2 = the current phase** (camera-row identification → per-eye rewrite → `RenderCmd_ExecuteStream` stream replay for the second draw pass → Present compositor) — design in `stereo_design.md` §S2. All S1 probing code was removed at closeout (windows A–O, brackets, pose captures, classification, exfil, SetTransform/SetViewport slots); the carrier now carries only the S2 channel + ambient rewrite. Device already exists at injection (creation params unchangeable post-boot); device-lost path + `Reset` VmtHook or Present-hook interop blit for param changes (the latter needs neither).

## Motion-control / logic-mod track (long-term)

Same rules, plus:

- VM-virtualized functions are denser in logic code: hook plaintext thunks/callers, never VM stubs; calling stubs is fine (proven).
- Keep the carrier thin (inject + install + mod host); gameplay mod logic goes in a separate hot-swappable module the carrier hosts.
- Input injection point: `XInputGetState`/`XInputSetState` import stubs `0x00a64d56`/`0x00a64d5c` (plaintext thunks); no dedicated input-update call exists (state-stack flow — `main_game_loop.md`). Marshal motion poses to the main thread at a defined frame point.
- Resolve the idle-reset buffer pair (`0x017d30e8` count/array, `0x00f7fb90` 0x1000 buffer) before designing input injection.
- Gameplay object models (player/camera/weapon, G-engine classes) need mapping via the `vtables.md` recipe — workload, not risk.

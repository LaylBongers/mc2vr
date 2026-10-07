# Launcher + Carrier Reference

The M4 design now lives in `stereo_design.md` (incl. the distilled evidence for the camera channel); this doc remains the launcher/mechanism/hook reference. Per-address facts live in Ghidra; not repeated beyond the hook list below.

## Architecture

`launch.sh` → `mc2vr_launcher.exe` (win32 i386, in-prefix via Proton) → [S4: spawn `mc2vr_host.exe` (win64 OpenXR host), wait for its ready log line] → `CreateProcess` game SUSPENDED → inject `mc2vr_carrier.dll` (win32 i386) → carrier installs its plaintext SafetyHook hooks → launcher resumes the game. Hooks are live before the game's first instruction (**early attach, PROVEN under Proton 2026-10-04**); state-dependent hooks (device, render shell) install in a second carrier stage once the engine has built them.

- Carrier = delivery vehicle only; mechanism stays "patch the code itself". Rejected: external cross-process `WriteProcessMemory` patching (no prologue relocation, no MidHook, messier under Wine).
- Game side (launcher, carrier) is 32-bit; the planned HMD host is the one 64-bit binary (see below). Game image can't relocate, base fixed `0x00400000` — all hook VAs are literal runtime addresses.
- DXVK runtime in this prefix (D3D9 → Vulkan → RADV): all hooked objects/vtables are DXVK implementations; same COM contract, expect quirk-level behavior differences.

## Build / deploy / test

- `cmake -B build/win32 -DCMAKE_TOOLCHAIN_FILE=cmake/i686-w64-mingw32.cmake && cmake --build build/win32` → `build/win32/bin/{mc2vr_launcher.exe,mc2vr_carrier.dll}`. `launch.sh` deploys to `<GAME_DIR>/mc2vr/` and runs under Proton.
- SafetyHook v0.7.0 vendored in `vendor/safetyhook/` (amalgamated + Zydis, BSL-1.0, C++23).
- Build lock: carrier verifies exe size + SHA-256 on disk against `src/carrier/build_lock.h`, refuses to hook on mismatch. Regenerate after re-RE: `tools/gen-build-lock.sh <exe>`.
- `tools/selftest/run.sh`: full chain test under plain Wine using a sleeper stand-in (base 0x400000 + ticking counter at the literal VA). Also verifies build-lock refusal. Run after carrier changes.

## Live-run workflow & healthy signatures

Iteration loop: the agent implements/logs; the human runs `./launch.sh` into GAMEPLAY (SteamVR up) and
reports; the agent audits `<GAME_DIR>/mc2vr/mc2vr_{carrier,host,launcher}.log` (`GAME_DIR` from
`launch.conf`; `tools/analyze_dumps.py <log>` parses view/S2c/eye evidence). Win64 host build:
`cmake -B build/win64 -DCMAKE_TOOLCHAIN_FILE=cmake/x86_64-w64-mingw32.cmake` →
`build/win64/bin/mc2vr_host.exe`. Selftest needs an unsandboxed terminal (`mkdir -p /tmp/opencode`
first). Pure camera math: `tools/test/test_vp_camera.cpp` (native g++, one-line build in its header).
`launch.sh` deploys with `cp -u` (binaries) / `cp -n` (conf — the deployed `mc2vr.conf` is never
overwritten; edit it in place). Env: `MC2VR_NO_HOST` (skip host), `MC2VR_IPC_NAME` (section name
override). Deployed conf steady state: `frame_replay=on eye_pass=on eye_rt=on eye_monitor_pin=on
eye_share=on view_table_inject=on view_row_rewrite=hmd_delta vsync=off` (in-tree defaults stay
host-less + vsync=on). Camera channel 2026-10-07: view_table_inject (union HMD pose at
g_CameraTable) + view_row_rewrite=hmd_delta (per-eye FOV/IPD at the uploads) is the live pair —
the old `hmd`/`stereo`/`on`/`pulse` modes were removed
(docs/stereo_improvements_plan.md).

Healthy-run signatures: host `submit: blit shaders ready`, `openxr: using swapchain format 91`,
`submit: window fresh/reused/pattern=0`, `submit: pose ids miss=0`; carrier `share window:` ~1330
L/R per 10 s with `ringFull=0` (≈ game fps × 10), `view/hmd: split=0 decompFail=0`; game cadence with
`vsync=off`: `FrameTick: dt avg` ~7.4 ms (~135 Hz). Pulsing test pattern in the HMD = carrier pipeline
not talking (`eye_share=off` or host not receiving).

## HMD host process (S4)

`mc2vr_host.exe` (x86_64 mingw; `cmake -B build/win64 -DCMAKE_TOOLCHAIN_FILE=cmake/x86_64-w64-mingw32.cmake && cmake --build build/win64` → `build/win64/bin/`; a 64-bit configure builds ONLY the host, a 32-bit configure the game side) runs in the same Proton prefix with DXVK D3D11 (`proton run`; plain `wine` only works with `--mock`) and owns the OpenXR session, presentation and event pump. `launch.sh` deploys it beside the carrier; the launcher spawns it before the game and waits for its `mc2vr_host: ready` log line (non-fatal on early exit/timeout/missing exe; env `MC2VR_NO_HOST` skips). Rules: the host never blocks the game and the carrier never blocks on the host (no host ⇒ game runs as before, logged once); log `mc2vr_host.log` in the deploy dir; the host is our own process (not subject to the SecuROM/hooking rules). Host exits on carrier `Shutdown` or game exit. Selftest: phase A host lifecycle line, phase B `--mock` ↔ probe round trip. Design, IPC contract, Proton/OpenXR gotchas: `stereo_design.md` §S4; S4 status/record (events/pacing/HUD, all live-verified): `stereo_design.md` §S4-5.

## Launch sequence (launcher)

1. Resolve paths from deploy location (`<game dir>/mc2vr/`). (S4: spawn the host first and wait for `ready`.)
2. `CreateProcessW` game `CREATE_SUSPENDED`. Stale `mc2vr_carrier.log` deleted first. No exe-base check in the launcher (suspended process has no module list yet) — the carrier verifies base + build lock in-process.
3. Inject: `CreateRemoteThread` + `LoadLibraryW` into the suspended process. Works under Proton and plain Wine (selftest). Launcher waits for the carrier log line `early init done` (stage 1 finished), then `ResumeThread`; if it never appears the game is killed, not resumed.
4. Carrier stages:
   - Stage 1 (before the game runs): conf, base/build-lock gate, FrameTick + BeginSubmit hooks, `render::install_early` (opcode/view/upload-gate MidHooks, stub tracer, SubmitToGPU). Plaintext `.text` only.
   - Stage 2 (once the engine has a device): probes, device capture + VmtHook, `render::install_late` (g_RenderShell vtable claim, poller), vmdump. Gate = plain memory read of `g_LtiRenderer` (`0x01175288`) → `dx9State` (`+0x5bc`), i.e. what `GetD3DDevice_Impl` does, WITHOUT calling the `GetD3DDevice` thunk: its VM slot is patched by the loader at runtime, so calling it before startup finishes would enter the unpatched stub. Device non-NULL ⇒ engine is past startup ⇒ the thunk is safe afterwards.
   - Proven 2026-10-04 (real game, Proton Experimental, clean prefix): stage 1 completes ~250ms after start; game resumes; first frame ~80ms later; device capture ~150ms after that; full stereo pipeline (replay, eye RT, monitor pin, camera rewrite) identical to the old late-attach runs; zero SecuROM reaction. The old assumption that SecuROM would block pre-boot patching was never true — `.text` is plaintext on disk, nothing decrypts it, and the VM only patches thunk *slots*, not hooked code.
   - GOTCHA: the frame counter `0x011755bc` is NOT a device-ready signal (spins ~1400 Hz pre-D3D); an earlier build gated on it, captured NULL and silently skipped all device hooks. The old launcher-side boot gate and the `--late` flow are removed.
   - New capability, USED since S4-5 (`vsync=off`): because we run before `Direct3DCreate9`/`CreateDevice`, creation params are changeable pre-boot. Mechanism: stage-1 InlineHook of the game's `Direct3DCreate9` IAT thunk (`0x00a4e892`, single caller `RenderSystem_Init` `0x0074c7a0`) + VmtHook on the returned `IDirect3D9` (`CreateDevice` = slot 16) forcing `PresentationInterval=D3DPRESENT_INTERVAL_IMMEDIATE`; the device-level `Reset` hook re-patches on Reset. The engine's CreateDevice call site is VM-gated (`FUN_0074c9b0` -> ptr `0x024cd09c`, SecuROM region — never hooked), hence the D3D-boundary interception.
5. Carrier `DllMain` only spawns the init thread (loader lock). Logs: `<deploy dir>/mc2vr_{launcher,carrier}.log`.

## Mechanism (proven, do not re-litigate)

- SecuROM is inert under everything we do: live `.text` inline patches, in-process `.data` writes, direct VM-stub thunk calls, DLL injection, cross-process reads. Verified over multi-minute runs. Still: never attach a debugger/ptrace; carrier is the only sanctioned probe.
- Mod code may CALL VM-stub thunks (`GetD3DDevice` etc.); never HOOK them or anything at `0x01a48000+`.
- Diagnostic carrier modules live in `src/carrier/debug/` (all conf keys prefixed `debug_`, default off; see `conf/mc2vr.conf`): `debug_stub_trace=on` (callback tracer around the render stub, `reverse_engineering/render_path.md`); `debug_vm_dump=on` (`src/carrier/debug/vm_dump.cpp`, read-only): at attach and again +15s it dumps the live VM chain behind thunk `0x0046ab80` diffed against the on-disk image, then censuses every `jmp [slot]` thunk (slot in `0x01a48000..0x03771f0f`) and writes `<GAME_DIR>/mc2vr/vm_thunks.csv` (file target vs runtime target per thunk; a copy of the 2026-10-03 run is `docs/reverse_engineering/data/vm_thunks_runtime.csv`). Findings: `reverse_engineering/securom_vm.md` item 4. The deployed conf is never overwritten by `launch.sh` — add the key to the deployed copy.
- Hook install needs NO thread suspension: SafetyHook v0.7.0 install is trap-based (page guard + VEH IP fixup) — atomic w.r.t. execution. External suspension is FORBIDDEN (v0.7.0 heap-allocates during `create_inline`; suspended thread holding the CRT heap lock would deadlock).
- VmtHook (cloned-vtable vptr swap) works on DXVK's MinGW-built objects (`VMT_HEADER=2` matches DXVK's Itanium vtables); safe from a foreign thread (aligned pointer store; in-flight calls keep the old valid vtable). Survives device-lost + `Reset` cycles.
- MidHook = register-context probe at arbitrary instructions — the tool for thiscall/unknown-convention sites (read ECX/ESP from context, no dispatch semantics).
- Hook objects are leaked by design: destructors would restore vptrs/bytes during process teardown after the target objects may be freed.
- Hook handlers must match the original convention; zero-arg void functions are convention-agnostic on i386.

## Hook sites

| Site | VA | Type | Status |
|---|---|---|---|
| `GameShell_FrameTick` | `0x00630e10` | InlineHook | DONE (M1): frame counter, 10s timing reports, `hooks::frame_count()` |
| `GetD3DDevice` thunk | `0x0047f2f0` | direct call | DONE (M2): device capture in carrier stage 2, after the device exists (thunk is hot-path — don't hook it). At runtime its slot is patched to native `GetD3DDevice_Impl` `0x00403160` (`g_LtiRenderer->dx9State`) |
| device vtable | runtime | VmtHook | DONE (M2): Present 17 / BeginScene 41 / EndScene 42 / Reset 16 — slots pinned (SetRenderState@57 anchor + runtime 1:1 call-pattern confirmation); present params via swapchain `GetPresentParameters` (slot 9) on first Present; Reset logs new params. DONE: SetVertexShaderConstantF 94 = the camera channel (`view_rewrite.cpp`; SetTransform is never called — shader-driven); SetRenderTarget 37 observed (pass gate: view rewrite only while RT0 = backbuffer size). DONE (S2c-2): SetRenderTarget 37 + StretchRect 34 = the pass-2 eye-RT redirect (`eye_replay.cpp`; gameplay's final composite is a DRAW into RT0=backbuffer — counter-proven, observed-not-redirected); UpdateSurface 30 (pass-2 main-RT source redirect; never observed live) + UpdateTexture 31 (diagnostic counter only) hooked. Monitor pin = carrier StretchRect snapshot (backbuffer->snapshot RT before pass 2) / restore (after) via `device::blit_surfaces` (original-method trampoline) — backbuffer writes are NEVER suppressed (SwapEffect=DISCARD: stale driver page, live-observed). Backbuffer identity via GetBackBuffer slot 18, ref released on Reset |
| `Direct3DCreate9` IAT thunk + `IDirect3D9` vtable | thunk `0x00a4e892` (jmp `[0x00b05620]`); IDirect3D9 CreateDevice slot 16 (runtime vtable) | InlineHook (stage 1) + VmtHook | S4-5 pacing (`vsync=off` only; default off = not installed): force `D3DPRESENT_INTERVAL_IMMEDIATE` in CreateDevice params; device Reset hook (slot 16 device vtable) re-patches. Why here: the engine's CreateDevice call site is VM-gated (`FUN_0074c9b0` → ptr `0x024cd09c`, SecuROM region) |
| `LtiRenderer_BeginSubmit` entry | `0x0074aaa0` | MidHook probe | DONE (M2.5): answered driver + vtable questions; one-shot, currently dormant |
| `RenderCmd_ExecuteStream` | `0x008569d0` | MidHook at opcode cmp `0x008569f5` (EAX=opcode, 27 ops) | M3 histogram + S2c stream tap — installed |
| `RenderQueue_SubmitWorldPackets` | `0x0048e620` | MidHook at view-loop lea `0x0048e9ea` (ESI=idx, ECX=type, EAX=off; entry+0x7e4 = per-view object; 3rd table `0x014095e0` stride 0x20) | M3 view aggregation — installed. S0-mapped, uninstalled: staging `0x0048ef71` (superseded S3 clone site), iterator `0x0048f013` (re-emit fallback) |
| `g_RenderShell` slots 4/5 (`EndOfFrameHook`/`PostUpdateHook`) | live vtable = base `LtiRenderer_vtbl` `0x00bd38e8`; object `0x017ceaf0` (=`*g_RenderShellPtr` `0x00dfb2f8`) | cloned-vtable swap (VmtHook), counting no-op handlers | M3 claim PROVEN 1:1 with frames. S4-5: slot-5 handler is now the host-event drain point (`ipc::drain_events()` — session-state/recenter logs; `MC2VR_MSG_EXIT` → one-shot `WM_CLOSE` to the game's root window via HWND global `0x01175274`). Risk (unobserved): the `0x00a7d950` reinit fragment reinstalls the original vtable — re-claim on device-lost if counts stop |
| `g_RenderQueue` counters | `0x00ff3618` | poller thread (250ms), 10s window reports | M3 producer rhythm — installed (ambient telemetry). S0: halves resolved — producers do `countersB(+0x14).low += count`, `countersA(+0x10).high += count`, spin-wait until the consumer-derived position `(countersA.low + countersA.high) % capacity` matches the producer slot `(countersB.low + countersA.low) % capacity`. `countersA.low` is advanced only by the CONSUMER — which is SecuROM-VM'd (no plaintext writer), explaining M3's "ring position" wrap. Old packed-pair spin model: close, but the halves' roles were swapped. S4-5 HUD timing (`src/carrier/hud_timing.cpp`, ambient): samples queue1+queue2 counters at Present + `set_pass` boundaries, 10s per-phase consumer/producer deltas (`hud2:` lines; queue2 = 2D/overlay) |
| viewContextData upload gate | `0x00855a78` | MidHook | DONE: `cmp [edi+0xd8],0` in PgPrimitive_SubmitToGPU before the gated Dx9_SetVertexShaderConstantF call; EDI = CURRENT technique — publishes exact viewContextData reg/count (+0xd4/+0xd8) + ViewProj (+0xdc/+0xe0) to the device-level rewriter |

Rules: plaintext `.text` only; never `0x01a48000+` or VM-stub thunks; the device vtable is the one sanctioned vtable patch (via clone). NOTE (S0): the ring/packet interpreter is itself VM-protected (stub `0x0050f660` via call site `0x004c99f9`) — probe it by bracketing the plaintext call sites, never by hooking the stub.

## Milestones

- M0 (toolchain, launcher, injection, carrier attach) — DONE.
- M1 (FrameTick stability; probes: `.data` write+restore, VM-stub call) — DONE. SecuROM live-patching caveat DISCHARGED.
- M2 (device capture, VmtHook, Present/EndScene/Reset pinning, present params) + M2.5 (BeginSubmit probe: frame driver + live vtable) — DONE. Per-frame submit chain is fully plaintext; see `reverse_engineering/render_path.md` "Frame driver chain".
- M3 (**COMPLETE**): view-table dump + command histogram + slot claim + queue poll — verified in-mission (runs incl. cutscene, boat/crouch cameras, PDA, satellite designation, alt-tabs, mission load).
  - Slot 4/5 claim (VmtHook clone on `g_RenderShell`): **PROVEN** — 1:1 with frames through everything; no vtable reinstalls.
  - View entry field map and view-loop facts moved to `reverse_engineering/view_and_camera.md`.
  - Carrier keeps the M3 instrumentation as ambient telemetry for future runs.
- M4: stereo rendering (design and status: `stereo_design.md`). **S0/S1/S2 (incl. S2c second draw pass) COMPLETE and live-verified** (the draw camera never surfaces in patchable plaintext data — the GPU-boundary `SetVertexShaderConstantF` rewrite is the per-eye channel). **S4 (S4-0..S4-5, HMD presentation via the separate 64-bit OpenXR host) COMPLETE and live-verified 2026-10-06** — events, pacing (`vsync=off`, free-run ~135 Hz) and HUD all resolved; record: `stereo_design.md` §S4-5. OpenVR is not used; OpenXR is not possible inside the 32-bit DX9 game process, hence the host. The carrier loads before the game creates its device (early attach), so creation params are changed at `Direct3DCreate9`/`CreateDevice` (the vsync-unlock mechanism) instead of via device-lost + `Reset`; the shared-texture copy needs no swapchain changes.

## Motion-control / logic-mod track (long-term)

Same rules, plus:

- VM-virtualized functions are denser in logic code: hook plaintext thunks/callers, never VM stubs; calling stubs is fine (proven).
- Keep the carrier thin (inject + install + mod host); gameplay mod logic goes in a separate hot-swappable module the carrier hosts.
- Controller/HMD input source: the host's OpenXR actions, delivered over the S4 IPC and applied at the slot-5 hook.
- Input injection point: `XInputGetState`/`XInputSetState` import stubs `0x00a64d56`/`0x00a64d5c` (plaintext thunks); no dedicated input-update call exists (state-stack flow — `reverse_engineering/main_game_loop.md`). Marshal motion poses to the main thread at a defined frame point.
- Resolve the idle-reset buffer pair (`0x017d30e8` count/array, `0x00f7fb90` 0x1000 buffer) before designing input injection.
- Gameplay object models (player/camera/weapon, G-engine classes) need mapping via the `reverse_engineering/vtables.md` recipe — workload, not risk.

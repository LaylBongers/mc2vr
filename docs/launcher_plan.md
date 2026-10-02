# Launcher + Carrier Plan (TEMPORARY)

Keep until M3/M4 planning is done, then fold into `initial_analysis.md`/`render_path.md` and delete. Per-address facts live in Ghidra; not repeated beyond the hook list below.

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
| device vtable | runtime | VmtHook | DONE (M2): Present 17 / BeginScene 41 / EndScene 42 / Reset 16 — slots pinned (SetRenderState@57 anchor + runtime 1:1 call-pattern confirmation); present params via swapchain `GetPresentParameters` (slot 9) on first Present; Reset logs new params |
| `LtiRenderer_BeginSubmit` entry | `0x0074aaa0` | MidHook probe | DONE (M2.5): answered driver + vtable questions; one-shot, currently dormant |
| `RenderCmd_ExecuteStream` | `0x008569d0` | InlineHook + MidHook at opcode switch | M3: command histogram |
| `RenderQueue_SubmitWorldPackets` | `0x0048e620` | MidHook in per-view loop | M3: dump view/portal table (`0x012865e0`, stride `0x810`) |
| `g_RenderShell` slots 4/5 (`EndOfFrameHook`/`PostUpdateHook`) | base vtable `0x00bd38e8` (LIVE; derived `0x00be84c0` never runs) | cloned-vtable swap on `g_RenderShell` (`0x017ceaf0`) | M3+: confirm slots are callable NoOps, then claim for VR frame hooks |
| `g_RenderQueue` counters | `0x00ff3618` | memory poll from carrier thread | M3: producer/consumer rhythm |

Rules: plaintext `.text` only; never `0x01a48000+` or VM-stub thunks; the device vtable is the one sanctioned vtable patch (via clone).

## Milestones

- M0 (toolchain, launcher, injection, carrier attach) — DONE.
- M1 (FrameTick stability; probes: `.data` write+restore, VM-stub call) — DONE. SecuROM live-patching caveat DISCHARGED.
- M2 (device capture, VmtHook, Present/EndScene/Reset pinning, present params) + M2.5 (BeginSubmit probe: frame driver + live vtable) — DONE. Per-frame submit chain is fully plaintext; see `render_path.md` "Frame driver chain".
- M3 (pending): view-table dump + command histogram → input for stereo submission design (separate plan). Note: view table is likely only populated in gameplay, not at menu — verify in-mission.
- M4 (future): first redirects. Device already exists at injection (creation params unchangeable post-boot); use device-lost path (`DAT_01174a94`) + `Reset` VmtHook for present-param changes, or Present-hook interop blit (needs neither).

## Motion-control / logic-mod track (long-term)

Same rules, plus:

- VM-virtualized functions are denser in logic code: hook plaintext thunks/callers, never VM stubs; calling stubs is fine (proven).
- Keep the carrier thin (inject + install + mod host); gameplay mod logic goes in a separate hot-swappable module the carrier hosts.
- Input injection point: `XInputGetState`/`XInputSetState` import stubs `0x00a64d56`/`0x00a64d5c` (plaintext thunks); no dedicated input-update call exists (state-stack flow — `main_game_loop.md`). Marshal motion poses to the main thread at a defined frame point.
- Resolve the idle-reset buffer pair (`0x017d30e8` count/array, `0x00f7fb90` 0x1000 buffer) before designing input injection.
- Gameplay object models (player/camera/weapon, G-engine classes) need mapping via the `vtables.md` recipe — workload, not risk.

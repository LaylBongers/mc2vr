# Launcher + Carrier Plan (TEMPORARY)

Temporary plan — delete once the injector works and lessons are folded back into `initial_analysis.md`/`render_path.md`. Per-address facts live in Ghidra; not repeated beyond the hook list below.

## Architecture

`launch.sh` (exists) → `mc2vr_launcher.exe` (win32 i386, runs inside the Wine prefix via Proton) → `CreateProcess` game → inject `mc2vr_carrier.dll` (win32 i386) → carrier installs SafetyHook hooks in-process.

- Chosen: launcher + carrier module with inline (detour) hooking — the carrier is only a delivery vehicle; the mechanism stays "patch the code itself".
- Rejected: pure external `WriteProcessMemory` patching from the launcher — hand-rolled rel32 detours lose SafetyHook's prologue relocation and MidHook (register-context at arbitrary instructions), which most of our hook points need; cross-process under Wine is messier than in-process.
- Everything 32-bit (game is i386). Image can't relocate, base fixed `0x00400000` — all hook VAs are literal runtime addresses (`initial_analysis.md`).

## Build setup

- Cross toolchain `i686-w64-mingw32-g++` — NOT yet installed (setup item; host GCC 16 + CMake 4.4 are fine).
- SafetyHook: FetchContent or the amalgamated `safetyhook.{hpp,cpp}` pair; needs a modern C++ compiler; MIT.
- CMake project, two targets: launcher (C is fine) + carrier (C++, links SafetyHook).

## Launch sequence (launcher)

1. Read `launch.conf` (exists: `GAME_DIR`, `GAME_EXE`, prefix, Proton).
2. `CreateProcessW` the game, normal start (suspension optional, not required).
3. Wait for boot-complete marker: poll a per-frame counter — `_DAT_017bad00` or `_DAT_011755bc` (both bumped every `GameTimeAccumulate_Update`, i.e. main loop alive). Counter moving ⇒ SecuROM stub has finished by construction. Rationale: no decryption gate exists (verified plaintext on disk — `initial_analysis.md`), so early `.text` patches would persist; the poll instead (a) lets the stub run on a pristine image so the unverified startup-tamper-check question never arises, (b) guarantees VM-init + hook-target globals are live. Coarse poll suffices — no suspension, OEP choreography, or debugger needed.
   NOTE: the counter bumps *before* `GameStateStack_Update` each tick, so "counter moving" ⇒ main loop alive but NOT necessarily rendering initialized (D3D init lands a few frames later in the boot state). Harmless — installed hooks idle until first call — and leaves a pre-device-creation install window open for M4 if creation-time interception is ever needed. If a strict "render ready" gate is ever required, poll `g_D3D9` (`0x01175284`) ≠ 0 instead.
4. Inject carrier: `CreateRemoteThread` + `LoadLibraryW`. Fallback if flaky under Proton: manual mapping or a boot-time import stub (decide only if needed).
5. Carrier `DllMain`: spawn an init thread; never install hooks in `DllMain` (loader lock).

## Carrier hook list (instrumentation first, redirect later)

| Site | VA | Hook type | Purpose |
|---|---|---|---|
| `GameShell_FrameTick` | `0x00630e10` | InlineHook | frame counter, timing sanity, hook-install ack |
| `GetD3DDevice` thunk | `0x0047f2f0` | InlineHook | capture `IDirect3DDevice9*` |
| device vtable | runtime | VmtHook (on captured device) | log Present/EndScene/Reset — pins the offsets without touching encrypted call sites |
| `RenderCmd_ExecuteStream` | `0x008569d0` | InlineHook + MidHook at opcode switch | command histogram; closes "what executes" open item |
| `RenderQueue_SubmitWorldPackets` | `0x0048e620` | MidHook in per-view loop | dump view/portal table entries (`0x012865e0`, stride `0x810`) — feeds stereo design |
| `RenderShell_vtable` slots `+0x10`/`+0x14` | `0x00be84c0` | direct pointer store (no lib) | confirm NoOp slots at runtime; claim for VR frame hooks |
| `g_RenderQueue` counters | `0x00ff3618` | memory poll from carrier thread | producer/consumer rhythm, spin-wait behavior |

- Device-vtable pinning: do NOT trust header-derived indices blindly — identify Present/EndScene by observed call patterns at runtime (args, call order around frame counters) before hooking. Known-good observed offsets: `+0xe4` SetRenderState, `+0x10c` SetSamplerState, `+0x114` SetTexture.
- Never patch inside `0x01a48000+` (S*/`.securom`) or at `thunk_FUN_02xxxxxx` targets — hook plaintext `.text` callers only.

## Milestones

- **M0** — toolchain builds; launcher starts game, injects carrier; carrier logs "attached". No hooks.
- **M1** — FrameTick hook logs stable for minutes; confirms SecuROM inert under live patching (AGENTS caveat discharged or escalated). Extend with two motion-control pre-probes in the same run: (a) write+restore a `.data` byte from the carrier (logic modding writes game structures constantly), (b) direct-call a benign VM-stub thunk (e.g. `GetD3DDevice` at `0x0047f2f0`) with correct convention — verifies mod code may *call* SecuROM-virtualized functions even though they can never be *hooked*.
- **M2** — device captured + VmtHook: Present/EndScene/Reset pinned, present params logged. Closes render-path open items 2–3.
- **M3** — view-table dump + command histogram. Input for stereo submission design (separate plan after this).
- **M4** — first redirects (eye duplication etc.) — out of scope here. Known caveat for that phase: post-boot hooks cannot alter device/swapchain *creation* parameters (device already exists); use the game's own device-lost path (`DAT_01174a94`) + `Reset` VmtHook to modify present params, or the pre-render-init install window noted in step 3. A `Present`-hook interop-blit approach needs neither.

## Risks / checks

- 32-bit everywhere; a 64-bit carrier silently fails to inject.
- `CreateRemoteThread`+`LoadLibrary` under Proton: verify early (M0); don't debug it later in the stack.
- Hook install races: render path is main-thread-only,, soso the carriercarrier installs hooks deterministically — `SuspendThread` main thread (or all-but-self)installs hooks deterministically — `SuspendThread` main thread (or all-but-self), install, `ResumeThread``ResumeThread` —— atomicatomic w.r.t. every hooked function. No loader-lock operations during install (SafetyHook only VirtualAllocs and writes memory; no DLL loads). No debugger, no ptrace — invisible to the anti-debug kit (`initial_analysis.md`w.r.t. every hooked function. No loader-lock operations during install (SafetyHook only VirtualAllocs and writes memory; no DLL loads). No debugger, no ptrace — invisible to the anti-debug kit (`initial_analysis.md`).
- VAs are build-locked: record the exe hash in the carrier and refuse to patch on mismatch (update path: re-RE via Ghidra project).
- No vtable/IAT hooking needed for M0–M3 except the device VmtHook — keep mechanism discipline (inline patches in `.text` + the one device vtable).
- Anti-tamper machinery is present in the binary (see `initial_analysis.md`); whether it re-checks `.text` post-boot is unknown — M1 is the empirical discharge. Never attach a debugger to the live game; the carrier (plain memory writes, no ptrace) is the only sanctioned probe.

## Motion-control / logic-mod track (long-term)

No mechanism changes — the same rules hold. Specifics:

- VM-virtualized functions are more frequent in logic code than in the render path. Rule as before: hook plaintext thunks/callers, never trampoline VM stubs; calling VM stubs from mod code is expected to work (M1 pre-probe (b) verifies).
- Architecture: keep the carrier thin (inject + install + mod host); gameplay mod logic lives in a separate, hot-swappable module the carrier hosts — the injection mechanism stays frozen while mod code iterates.
- Input injection: `XInputGetState`/`XInputSetState` import stubs at `0x00a64d56`/`0x00a64d5c` (plaintext `.text` thunks) are the clean synthetic-controller hook point; state-stack input flow means no dedicated update call to intercept (see `main_game_loop.md`). Marshal motion poses to the main thread at a defined frame point (FrameTick/pipeline hook).
- Resolve the idle-reset buffer pair (`0x017d30e8` count/array, `0x00f7fb90` 0x1000 buffer) before designing input injection — suspected input event buffer (`main_game_loop.md` open item).
- Static RE backlog grows: gameplay object models (player/camera/weapon, G-engine classes) need mapping via the `vtables.md` recipe — workload, not mechanism risk.

## Consumes open items

`render_path.md` open items: Present/EndScene pinning, `thunk_FUN_0256b6f0` runtime confirmation, `vt[4]`/`vt[5]` NoOp confirmation, view/portal table field map.

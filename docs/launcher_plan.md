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
| `RenderCmd_ExecuteStream` | `0x008569d0` | MidHook at opcode cmp `0x008569f5` (EAX=opcode, 27 ops) | M3: command histogram — **IMPLEMENTED, pending run** |
| `RenderQueue_SubmitWorldPackets` | `0x0048e620` | MidHook at view-loop lea `0x0048e9ea` (ESI=idx, ECX=type, EAX=off; entry+0x7e4 = per-view object; 3rd table `0x014095e0` stride 0x20) | M3: view aggregation + one-shot entry dumps — **IMPLEMENTED, pending run**; needs gameplay (menu may not submit world views) |
| `g_RenderShell` slots 4/5 (`EndOfFrameHook`/`PostUpdateHook`) | live vtable = base `LtiRenderer_vtbl` `0x00bd38e8`; object `0x017ceaf0` (=`*g_RenderShellPtr` `0x00dfb2f8`) | cloned-vtable swap (VmtHook), counting no-op handlers | M3 claim test — **IMPLEMENTED, pending run**. Risk: the `0x00a7d950` reinit fragment reinstalls the original vtable — if slot call counts stop after device-lost, re-claim (or re-install the swap on device-lost) |
| `g_RenderQueue` counters | `0x00ff3618` | poller thread (250ms), 10s window reports | M3 producer rhythm — **IMPLEMENTED, pending run** |

Rules: plaintext `.text` only; never `0x01a48000+` or VM-stub thunks; the device vtable is the one sanctioned vtable patch (via clone).

## Milestones

- M0 (toolchain, launcher, injection, carrier attach) — DONE.
- M1 (FrameTick stability; probes: `.data` write+restore, VM-stub call) — DONE. SecuROM live-patching caveat DISCHARGED.
- M2 (device capture, VmtHook, Present/EndScene/Reset pinning, present params) + M2.5 (BeginSubmit probe: frame driver + live vtable) — DONE. Per-frame submit chain is fully plaintext; see `render_path.md` "Frame driver chain".
- M3 (verified in-mission; one refinement pass pending): view-table dump + command histogram → input for stereo submission design (separate plan).
  - Slots 4/5 claim (VmtHook clone on `g_RenderShell`): **PROVEN** — called exactly 1:1 with frames through alt-tabs, cutscene, mission load; the `0x00a7d950` vtable-reinstall risk did not materialize.
  - View loop (`0x0048e9ea` MidHook): visits ACTIVE views only (per-frame submissions 1..608 in heavy scenes); indices observed to 239; **`DAT_00d29e60` is DYNAMIC** (0 at menu → 14 cutscene → 128 deep gameplay — live registered-view count, NOT a static capacity; the loop clearly iterates a larger table, ~256 slots × 0x810). The `g_RenderShell+0x2b90` WORD is a different constant-2 field — not the loop bound.
  - Type-4 views: **never observed** in normal play — including cutscene, boat/crouch cameras, PDA, and satellite designation (all t2, or rendered via the 2D overlay path). Deprioritized; instrument reports any sighting automatically.
  - Entry field map, first pass (dumps fired during load — entries pre-populated; steady-state re-dumps added for the next run): entry is mostly template/zero; varying fields: `+0x000..0x014` flags, `+0x770/+0x778/+0x784/+0x7a8` per-view state, **`+0x7c4..+0x7d0` 3D world position (floats, e.g. -1839, -1503, -1499)**, `+0x7e4..+0x7f0` three pointers (per-view objects = `ViewRef` instances, vtable `0x00bac1c8` — annotated in Ghidra as `ViewRef_vtbl`, ctor `0x00491670`, handle-id at obj+4, magic `0x5608bd5a` at obj+8), `+0x7f4` flag, `+0x800..0x808` misc. Camera view/proj matrices are NOT in the entry — they live in the `g_RenderShellPtr+idx*0x3a0` sub-objects and are copied into packets at submit.
  - Command histogram: gameplay ~21 active opcodes (menu: 9), 1.5k–3.4k commands/frame in gameplay; dominant bins 00/01/02/03.
  - Queue counters REVISED: `+0x10` is a ring POSITION (wraps within 0..capacity-1, non-monotonic — not a cumulative producer counter); `+0x14` stayed 0 during gameplay. Poller now logs raw window stats, no delta arithmetic.
  - Instrument fixes from this run: view aggregation cap 24→96 (distinct count exceeded it), tableCount read from `DAT_00d29e60`, steady-state entry re-dumps (poller-requested, MidHook-executed, ~1/min, ≤8/run). Second run found + fixed a re-dump ordering bug (pick read after window reset — never fired); cadence now ~30s, ≤12/run. First-wave dumps always consume their budget on idx 0–11 during boot/load (early slots activate first) — steady-state coverage relies on the re-dumps.
- M4 (future): first redirects. Device already exists at injection (creation params unchangeable post-boot); use device-lost path (`DAT_01174a94`) + `Reset` VmtHook for present-param changes, or Present-hook interop blit (needs neither).

## Motion-control / logic-mod track (long-term)

Same rules, plus:

- VM-virtualized functions are denser in logic code: hook plaintext thunks/callers, never VM stubs; calling stubs is fine (proven).
- Keep the carrier thin (inject + install + mod host); gameplay mod logic goes in a separate hot-swappable module the carrier hosts.
- Input injection point: `XInputGetState`/`XInputSetState` import stubs `0x00a64d56`/`0x00a64d5c` (plaintext thunks); no dedicated input-update call exists (state-stack flow — `main_game_loop.md`). Marshal motion poses to the main thread at a defined frame point.
- Resolve the idle-reset buffer pair (`0x017d30e8` count/array, `0x00f7fb90` 0x1000 buffer) before designing input injection.
- Gameplay object models (player/camera/weapon, G-engine classes) need mapping via the `vtables.md` recipe — workload, not risk.

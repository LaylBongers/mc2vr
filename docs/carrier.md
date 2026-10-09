# Carrier

`mc2vr_carrier.dll` (win32 i386) — injected into the suspended game by the [launcher](launcher.md). Delivery vehicle only: the mechanism is "patch the code itself" (rejected: external cross-process `WriteProcessMemory` patching — no prologue relocation, no MidHook, messier under Wine). All hooking goes through [hooks.md](hooks.md).

## Init stages

`DllMain` only spawns the init thread (loader lock).

- **Stage 1** (before the game runs): conf, base/build-lock gate, FrameTick + BeginSubmit hooks, `render::install_early` (opcode/view/upload-gate MidHooks, stub tracer, SubmitToGPU). Plaintext `.text` only. Logs `early init done` — the launcher's resume signal.
- **Stage 2** (once the engine has a device): probes, device capture + VmtHook, `render::install_late` (g_RenderShell vtable claim, poller), vmdump. Gate = plain memory read of `g_LtiRenderer` (`0x01175288`) → `dx9State` (`+0x5bc`), i.e. what `GetD3DDevice_Impl` does, WITHOUT calling the `GetD3DDevice` thunk (its VM slot is patched by the loader at runtime; calling it before startup finishes would enter the unpatched stub). Device non-NULL ⇒ engine past startup ⇒ the thunk is safe afterwards.

GOTCHA: the frame counter `0x011755bc` is NOT a device-ready signal (spins ~1400 Hz pre-D3D); an earlier build gated on it, captured NULL and silently skipped all device hooks.

## Design rules

- The carrier stays thin (inject + install + mod host); gameplay mod logic would go in a separate hot-swappable module the carrier hosts.
- State-dependent hooks install in stage 2 once the engine has built the objects.
- The carrier never blocks on the [host](host.md); no host ⇒ the game runs as before, logged once.

## Debug modules

`src/carrier/debug/`, all conf keys prefixed `debug_`, default off (see `conf/mc2vr.conf`): `debug_stub_trace` (VM stub callback tracer), `debug_vm_dump` (live thunk slot census → `vm_thunks.csv`), `debug_watch` (hardware watchpoints), `debug_camtable_probe` (camera transfer-function probe, [camera.md](camera.md)), `debug_eye_dump_frames` (per-eye BMPs). Full list + usage: [debugging.md](debugging.md). The deployed conf is never overwritten by `launch.sh` — add keys to the deployed copy.

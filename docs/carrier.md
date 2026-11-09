# Carrier

`mc2vr_carrier.dll` (win32 i386) — injected into the suspended game by the [launcher](launcher.md). Delivery vehicle only: the mechanism is "patch the code itself" (rejected: external cross-process `WriteProcessMemory` patching — no prologue relocation, no MidHook, messier under Wine). All hooking goes through [hooks.md](hooks.md).

## Init: attach + engine events (no stages)

`DllMain` only spawns the init thread (loader lock). There is no polling phase: everything installs at attach, and hooks on objects that don't exist yet are chained off engine events, on the main thread.

- **Attach** (game suspended; `init.cpp` `init`): conf, base/build-lock gate, IPC connect, FrameTick + BeginSubmit hooks, `render::install_early` (opcode/view/upload-gate MidHooks, stub tracer, SubmitToGPU, camera/culling), `rt_probe::install_early`, and the `Direct3DCreate9` thunk hook (always installed). Logs `early init done` — the launcher's resume signal. The init thread then exits.
- **Device creation** (`hooks/device.cpp` `createdevice_hook`): `Direct3DCreate9` → IDirect3D9 VmtHook → `CreateDevice` → the device VmtHook is installed before the engine receives the pointer. Device creation happens inside the first `SetupRenderTargets` ([reverse_engineering/render_target_table.md](reverse_engineering/render_target_table.md)), so this is the only race-free point. A re-created device is re-hooked.
- **First Present** (`init.cpp` `on_engine_running`): SecuROM pre-probes (one CALLS the `GetD3DDevice` VM-slot thunk — safe only after the loader patched it), `render::install_late` (queue poller thread), vmdump. (The g_RenderShell slot 4/5 hooks used to be a vtable claim here; they are now attach-time MidHooks at the FrameTick call sites — [hooks.md](hooks.md).) Logs `init complete`.

Hooking the device from another thread (polling `g_LtiRenderer->dx9State`) races the main thread's setup-tail device calls. The frame counter `0x011755bc` is not a device-ready signal (it spins ~1400 Hz pre-D3D).

## Design rules

- The carrier stays thin (inject + install + mod host); gameplay mod logic would go in a separate hot-swappable module the carrier hosts.
- Install everything at attach; hooks on objects that don't exist yet chain off the object's creation (or the first Present), on the engine's own thread — never poll from a side thread.
- The carrier never blocks on the [host](host.md); no host ⇒ the game runs as before, logged once.
- Reimplemented game systems live in `src/carrier/reimpl/` and replace their originals (`hooks/reimpl.cpp`; [reimpl.md](reimpl.md)). `debug_reimpl=shadow` verifies them instead, and `debug_reimpl=off` keeps the originals for A/B runs. Directory layout: [code_structure.md](code_structure.md).

## Debug modules

`src/carrier/debug/`, all conf keys prefixed `debug_`, default off (see `conf/mc2vr.conf`): `debug_stub_trace` (VM stub callback tracer), `debug_vm_dump` (live thunk slot census → `vm_thunks.csv`), `debug_watch` (hardware watchpoints), `debug_camtable_probe` (camera transfer-function probe, [camera.md](camera.md)), `debug_eye_dump_frames` (per-eye BMPs). Full list + usage: [debugging.md](debugging.md). The deployed conf is never overwritten by `launch.sh` — add keys to the deployed copy.

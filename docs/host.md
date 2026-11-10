# Host (OpenXR presentation process)

`mc2vr_host.exe` (x86_64 mingw, statically linked, vendored OpenXR SDK 1.1.54) — owns the OpenXR session, presentation and event pump. **Why a separate process** (2026-10-04): the game is 32-bit + D3D9, and OpenXR has no D3D9 graphics binding. OPEN, reconsidered 2026-11-10: a Vulkan binding on DXVK's own device may make OpenXR possible inside the game process — see [OPEN: in-process OpenXR](#open-in-process-openxr). OpenVR is not used anywhere.

Runs in the same Proton prefix with DXVK D3D11 (`proton run`; plain `wine` only works with `--mock`). Spawned by the [launcher](launcher.md) before the game. In [lockstep](pacing.md) the two sides wait on each other only with bounded timeouts. Log: `mc2vr_host.log` in the deploy dir. The host is our own process — not subject to the SecuROM/hooking rules ([hooks.md](hooks.md)). Wine/registry gotchas: [openxr_proton.md](openxr_proton.md). Selftest: host lifecycle (phase A) + `--mock` probe round trip (phase B) — [build.md](build.md).

## Source map (`src/host/`)

`main.cpp` (args `--mock --frames N --xr-debug`), `xr_session.cpp` (instance/session/swapchains/event pump/frame loop, pose history, Wine registry fixups), `mock.cpp` (no runtime: synthetic pose + blit smoke test), `d3d.cpp` (D3D11 device on the runtime LUID), `eyes.cpp` (test pattern fallback), `shared_eyes.cpp` (handle open cache, mirror window, `latest()` seam), `submit.cpp` (swapchain blit), `ipc.cpp`, `log.cpp`, `pose.hpp`. Carrier counterparts: `src/carrier/vr/eye_share.cpp` (capture ring), `src/carrier/ipc.cpp`.

## Frame loop

Lockstep ([pacing.md](pacing.md)): `xrWaitFrame` → `xrBeginFrame` → locate this frame's views at `predictedDisplayTime` and publish them (releases the carrier's frame gate) → wait for the complete pair carrying that pose id (`Sleep(0)` polling the command ring; skipped while no pairs are arriving) → blit + `xrEndFrame` with the pair's own pose. The event pump and the mirror run between frames.

Pose consistency: the carrier tags frames with `poseId = hostFrame+1`; the host keeps a 256-entry published-view history and submits the projection layer with the pose+FOV of that id (in lockstep this frame's own views; the history covers the timeout fallback). Only complete pairs (both eyes of one carrier frameId) are submitted (`seyes::latest`). The desktop mirror is throttled and maps its staging copy with `DO_NOT_WAIT`, so it never stalls the HMD loop. Focus lost: the host gates layer submission on `shouldRender` (zero layers while not VISIBLE/FOCUSED, Begin/End keeps cadence); the carrier gate is off then and the game free-runs with camera pass-through (`view/hmd: valid=0`).

## Submission

Runtime swapchains are sRGB-only for 8-bit (format 91 `B8G8R8A8_SRGB` preferred, else 29), and `CopyResource` UNORM↔sRGB is illegal, so `submit.cpp` draws a fullscreen triangle sampling the shared texture through a UNORM-cast SRV and writing through a UNORM-cast RTV (91→87, 29→28): sRGB-encoded LDR bytes pass through raw and the compositor decodes them (no double gamma). Shaders compiled at startup via dynamic `d3dcompiler_47.dll`. Fallback on any failure: the test pattern (`eyes.cpp`). `stretch=false` = aspect-fit letterbox (frames without a pose); `stretch=true` = fill (HMD-pose frames). While the HMD is in use the carrier renders at the eye swapchain's own size ([render_resolution.md](render_resolution.md)), so the fill is 1:1. Window-size images (the HMD not in use) are not shown in the headset. 10 s `submit:` lines: `fresh` (pair of this frame's pose), `late` (wait timed out, newest pair sent), `stale` (no new pair), wait times.

## OPEN: in-process OpenXR

Why it can work: DXVK's 32-bit `d3d9.dll` exposes its D3D9 interop interfaces (`ID3D9VkInteropInterface`/`Interface1`, `ID3D9VkInteropDevice`, `ID3D9VkInteropTexture`; DXVK's `src/d3d9/d3d9_interfaces.h`): the Vulkan instance, device, queue and images behind the game's D3D9 objects. DXVK also prepares its instance and device for OpenXR itself, enabling the Vulkan extensions wineopenxr reports (`__wineopenxr_GetVulkanInstanceExtensions` / `DeviceExtensions`). Proton ships a 32-bit `wineopenxr`, and SteamVR (since 2.17) a 32-bit OpenXR runtime.

Probe: `debug_xr_probe=on`, run with the host skipped: `MC2VR_NO_HOST=1 ./launch.sh` (`src/carrier/debug/xr_probe.hpp`; `xrprobe:` log lines, `RESULT` marks the milestones).

**Result (2026-11-10): in-process OpenXR works on Linux.** DXVK's handles via `ID3D9VkInteropDevice`; negotiation with the 32-bit `wineopenxr.dll`; an instance with `XR_KHR_vulkan_enable` and the HMD system; every required Vulkan instance extension already enabled by DXVK; the runtime's physical device = DXVK's; a session on DXVK's device + queue reached READY at once; 180/180 frames with no layers at the display period; `xrLocateViews` valid; clean exit. The game kept rendering into gameplay meanwhile.

What it needs:
- A 32-bit OpenXR runtime registered for 32-bit processes (`active_runtime.i686.json`; the launcher checks and logs the fix, [openxr_proton.md](openxr_proton.md) item 4). DXVK's own OpenXR extension query at device creation goes through the same loader.
- The Wine VR registry published before DXVK creates its instance (the launcher, [openxr_proton.md](openxr_proton.md) item 2).

Still OPEN:
- Not exercised yet: swapchains, copying the eye images into them on DXVK's queue (`ID3D9VkInteropTexture`, queue lock, ordering against DXVK's own submissions), frame pacing at the render gate, and memory pressure from the 32-bit runtime in the game's address space.
- Windows: whether SteamVR's (or other runtimes') 32-bit OpenXR covers Windows is unchecked. If it does, in-process could cover Windows + DXVK too and the host could retire; otherwise the host stays the Windows path.
- The probe's `GetInstanceExtensions` reported D3DERR_MOREDATA with a short list despite a large buffer; the required extensions were all present, but the contract reading may be wrong.

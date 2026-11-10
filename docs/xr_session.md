# OpenXR session (in the game process)

The carrier runs OpenXR inside the game, on DXVK's own Vulkan device and queue: the game is 32-bit D3D9, OpenXR has no D3D9 binding, and DXVK's D3D9 interop (`ID3D9VkInteropDevice`/`Texture`, `src/carrier/vr/dxvk_interop.hpp`) exposes the Vulkan objects behind the game's device and textures. Requires DXVK ([platforms.md](platforms.md)). OpenVR is not used anywhere. Decided 2026-11-10; until then a separate 64-bit host process presented through D3D11 shared textures (git history: `src/host/`).

Source (`src/carrier/vr/`): `xr_session.cpp` (session thread: setup, events, frame loop, published views), `xr_present.cpp` (Vulkan copy into the swapchains), `xr_runtime.cpp` (runtime DLL, no loader), `eye_share.cpp` (the eye ring and the hand-off, [eye_images.md](eye_images.md)).

## Runtime and setup

No OpenXR loader: `xr_runtime.cpp` negotiates with the runtime DLL directly (`xrNegotiateLoaderRuntimeInterface`) and resolves everything through its `xrGetInstanceProcAddr`. Under Wine that is Proton's 32-bit `wineopenxr.dll`; it needs the 32-bit runtime registered for 32-bit processes and the Wine VR registry published before DXVK creates its instance (both the launcher's, [openxr_proton.md](openxr_proton.md) items 2 and 4). On Windows: `XR_RUNTIME_JSON`, else the 32-bit `ActiveRuntime` registry value, and the manifest's `library_path` (never run).

Started at the engine's first Present (`init.cpp` `on_engine_running`), on its own thread (above-normal priority): the D3D9 device's interop → DXVK's instance, physical device, device, queue (family + index) → instance with `XR_KHR_vulkan_enable` → HMD system → Vulkan graphics requirements (required before the session) → session on DXVK's device + queue → LOCAL space → the recommended per-eye size (to `render_size` at the next update tick, [render_resolution.md](render_resolution.md)) → swapchain formats → the copy command pool. Any failure: `xr: no OpenXR session — VR off for this run`; the game runs with camera pass-through.

Proven before the conversion (2026-11-10 probe, since removed): negotiation, an instance and session on DXVK's device, READY at once, 180/180 frames at the display period, valid `xrLocateViews`.

## Frame loop

Lockstep ([pacing.md](pacing.md)): `xrWaitFrame` → `xrBeginFrame` → locate this frame's views at `predictedDisplayTime` and publish them (releases the render thread's pass-1 gate) → wait for the pair rendered from them (`eye_share::take_pair`, 100 ms, only while pairs arrived in the last 500 ms) → copy + `xrEndFrame` with the pair's own pose. Events are polled between frames.

- Published views: one `ViewState` under an SRW lock with a condition variable (pose/FOV per eye as OpenXR half-angles in radians, tracked flag, pose id, display time/period, session state). Readers: the render gate, the camera-table union pose ([camera.md](camera.md)), `render_size`'s HMD-in-use test.
- Pose consistency: the render thread samples the views once per frame at pass 1 and tags the pair with their pose id. A 256-entry history maps the id back to the views; the layer is submitted with them. Pose-less pairs use this frame's views if valid, else the previous layer's (a zero quaternion fails `xrEndFrame`). A layer `xrEndFrame` rejects is dropped (`layerRejected`) instead of being re-submitted.
- No new pair in time (loading, Reset, a hitch): the previous layer is re-submitted without acquiring, so the runtime shows the swapchains' last released images (`stale`). Before the first pair: zero layers (`empty`).
- Session state: READY → `xrBeginSession`; STOPPING → `xrEndSession`; EXITING / LOSS_PENDING / instance loss → one `WM_CLOSE` to the game window (the engine's quit path is VM-protected, `MC2_G_RENDER_HWND`) and the session is torn down. Captures run only while VISIBLE/FOCUSED. Recenter (reference space change) needs nothing: the camera follows the published views.
- 10 s log: `submit: frames … fresh late stale empty copyFail layerRejected | pair wait | pose ids hit/miss/none` and the LEFT view. Setup logs the Vulkan instance/device extensions the runtime needs (DXVK enables them under Proton).

## Presentation

`xr_present.cpp`: one command buffer per copy (3, each with a fence), recorded on the session thread and submitted on DXVK's queue under `LockSubmissionQueue` (which first waits until DXVK has handed every queued submission to Vulkan).

- Source: the VkImages behind the eye ring's D3D9 textures (`GetVulkanImageInfo`: handle + resting layout). Only taken once the pair's event query signalled, i.e. DXVK has submitted and finished its writes, so our later submission on the same queue reads complete images. The barriers make them visible and return them to their resting layout, ordered before DXVK's later writes.
- `vkCmdCopyImage`, raw bytes. Swapchains are created at the eye images' own size (re-created when it changes), so the copy is 1:1 and the image rect is the full swapchain.
- Format: the sRGB sibling of the eye images' format (`B8G8R8A8_UNORM` → `B8G8R8A8_SRGB`, R8G8B8A8 likewise). The LDR finals are already sRGB-encoded; the copy hands those bytes over unchanged and the compositor decodes them, so no double gamma. Without a sibling the UNORM format itself (logged: gamma off); without either, nothing is shown.
- Swapchain images: `UNDEFINED` → transfer → `COLOR_ATTACHMENT_OPTIMAL` before release (the `XR_KHR_vulkan_enable` contract for color-attachment swapchains; usage `COLOR_ATTACHMENT | TRANSFER_DST`). Both eyes' swapchains exist or neither. An image is released only after a successful wait (timeouts are retried); one that can't be released gets its swapchains re-created.
- Runtime calls that may use the queue (create/begin/end session, swapchain create/destroy, acquire/release, begin/end frame) run under DXVK's queue lock; `xrWaitFrame`, `xrWaitSwapchainImage`, `xrLocateViews` and `xrPollEvent` don't.
- Device Reset: `eye_share::on_reset` (before DXVK's Reset, outside its device lock) waits until the session thread holds no pair, then for our copies' fences, before the textures go (DXVK doesn't track our use of its images). Neither wait gives up on a timeout, only on device loss: freeing images still in use would be worse than a long hitch.

## Live result

2026-11-10, Index at 90 Hz, 2016×2240, msaa 4x: works. SteamVR/OpenXR 2.18.4 through 32-bit wineopenxr; session VISIBLE ~4 s after the first Present, FOCUSED ~1.5 s later. Swapchain formats offered (VkFormat) `43 50 109 106 97 64 …`; the eye images are `B8G8R8A8_UNORM` (44, resting layout `GENERAL`) → swapchains `B8G8R8A8_SRGB` (50), 3 images. Gameplay windows: 899–901 frames per 10 s, every one `fresh`, `stale=0 empty=0 copyFail=0 layerRejected=0`, `pose ids miss=0`, `gpuTimeouts=0`, render gate 0 timeouts — steadier than the host path's occasional single missed frames ([pacing.md](pacing.md#frame-time-headroom)). Per frame: render gate wait ~3–5 ms (the render thread's slack), pair wait ~8 ms (passes + GPU tail), fence → taken ~5 ms (the GPU tail). The HMD-in-use Reset to the render size went through cleanly; the first window (boot + load) had the expected `gpuTimeouts`, `stale` and gate timeouts around it. No memory trouble seen from the 32-bit runtime in a ~2 min run.

Cosmetic: the Reset to the render size took one last window-size pair first, so the swapchains were made at 2560×1440 and re-made at 2016×2240 half a second later.

## OPEN

- Latency: the session thread waits for the pair's GPU completion before submitting the copy (the same fence the old completion thread polled). Submitting right after DXVK has *submitted* the pair would save the GPU tail, but needs `FlushRenderingCommands`, which DXVK doesn't make safe against the D3D9 thread.
- `xrEndFrame` runs under DXVK's queue lock; if SteamVR blocks in it, DXVK's submissions wait meanwhile. Check with `debug_gpu_census` / `debug_wait_census` if pass timing changes.
- Game exit: the process ends with the session still running (no clean `xrDestroySession`); the runtime sees the client vanish.
- Windows: whether a 32-bit OpenXR runtime exists there is unchecked (SteamVR ships `steamxr_linux32` on Linux; its Windows counterpart is unconfirmed). Without one, VR is off on Windows.
- A re-created D3D9 device (not seen so far) would leave the session on the old VkDevice: logged, nothing more is shown.

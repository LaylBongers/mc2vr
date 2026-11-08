# Host (OpenXR presentation process)

`mc2vr_host.exe` (x86_64 mingw, statically linked, vendored OpenXR SDK 1.1.54) — owns the OpenXR session, presentation and event pump. **Why a separate process**: the game is 32-bit + D3D9 (DXVK); Valve's OpenXR driver has no 32-bit+DX9 support, and OpenXR is not possible inside the game process. OpenVR is not used anywhere.

Runs in the same Proton prefix with DXVK D3D11 (`proton run`; plain `wine` only works with `--mock`). Spawned by the [launcher](launcher.md) before the game. Rules: the host never blocks the game and the carrier never blocks on the host. Log: `mc2vr_host.log` in the deploy dir. The host is our own process — not subject to the SecuROM/hooking rules ([hooks.md](hooks.md)). Wine/registry gotchas: [openxr_proton.md](openxr_proton.md). Selftest: host lifecycle (phase A) + `--mock` probe round trip (phase B) — [build.md](build.md).

## Source map (`src/host/`)

`main.cpp` (args `--mock --frames N --xr-debug`), `xr_session.cpp` (instance/session/swapchains/event pump/frame loop, pose history, Wine registry fixups), `mock.cpp` (no runtime: synthetic pose + blit smoke test), `d3d.cpp` (D3D11 device on the runtime LUID), `eyes.cpp` (test pattern fallback), `shared_eyes.cpp` (handle open cache, mirror window, `latest()` seam), `submit.cpp` (swapchain blit), `ipc.cpp`, `log.cpp`, `pose.hpp`. Carrier counterparts: `src/carrier/eye_share.cpp` (capture ring), `src/carrier/ipc.cpp`.

## Frame loop

Per host frame: `xrWaitFrame` (HMD cadence ~120 Hz) → `xrBeginFrame` → **late latch** (drain carrier commands only now, so a pair finished during the WaitFrame block goes out this frame, not next) → locate this frame's views (layer fallback) → locate + **publish the render pose for `predictedDisplayTime + 1 period`** (the display time a pair rendered from it actually reaches; published before the blits so the next pass-1 sample sees it) → blit + `xrEndFrame`. The event pump and the mirror run between frames.

Pose consistency: the carrier tags frames with `poseId = hostFrame+1`; the host keeps a 256-entry published-view history and submits the projection layer with the pose+FOV of that id, so the compositor reprojects from what the image actually contains. Only **complete pairs** (both eyes of one carrier frameId) are submitted (`seyes::latest`). `submit:` lines report `warp avg/max` = submit display time − the pair's pose prediction time (the error the compositor must warp; ~0 ideal). The host mirror is throttled (≤5 Hz) and maps its staging copy with `DO_NOT_WAIT` — it must never stall the HMD loop (`seyes:` line `mirrorMax`). Focus lost: the host gates layer submission on `shouldRender` (zero layers while not VISIBLE/FOCUSED, Begin/End keeps cadence); the game keeps running with camera pass-through (`view/hmd: valid=0`).

## Submission

Runtime swapchains are sRGB-only for 8-bit (format 91 `B8G8R8A8_SRGB` preferred, else 29), and `CopyResource` UNORM↔sRGB is illegal, so `submit.cpp` draws a fullscreen triangle sampling the shared texture through a UNORM-cast SRV and writing through a UNORM-cast RTV (91→87, 29→28): sRGB-encoded LDR bytes pass through raw and the compositor decodes them (no double gamma). Shaders compiled at startup via dynamic `d3dcompiler_47.dll`. Fallback on any failure: the test pattern (`eyes.cpp`). `stretch=false` = aspect-fit letterbox (frames without a pose); `stretch=true` = fill (HMD-pose frames). While the HMD is in use the carrier renders at the eye swapchain's own size ([render_resolution.md](render_resolution.md)), so the fill is 1:1. Window-size images (the HMD not in use) are not shown in the headset. 10 s `submit:` stats lines are the acceptance evidence (fresh = per-eye blits of new carrier frames, reused = re-submits of the newest pair).

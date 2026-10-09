# OpenXR under Proton/Wine

Handled in `xr_session.cpp::init_wine_vr_registry`. Debug: `--xr-debug` writes loader tracing to `mc2vr_host_xrloader.log`; stdout is lost under proton, read `mc2vr_host.log` instead.

1. Must run through `proton run` (plain `wine` ⇒ wined3d: "doesn't support IDXGIVkInteropDevice").
2. wineopenxr negotiation fails (-6, runtime "lacks" every extension) unless `HKCU\Software\Wine\VR` exists, `wineopenxr_init_registry()` ran and DWORD `state`=1 (normally published by vrclient_x64 for an OpenVR app; state=2 fails) — the host creates these.
3. "Stuck in SYNCHRONIZED" has been SteamVR itself crashed — restart it first.

Session: LOCAL space, per-eye swapchains 2016×2240 ×3 images, runtime `SteamVR/OpenXR 2.17.10`, `ActiveRuntime` → `C:\openxr\wineopenxr64.json`. Swapchain format choice and sRGB blit: [host.md](host.md) § Submission.

# Platforms

The supported configuration is the game on **DXVK** (D3D9 → Vulkan): OpenXR runs on DXVK's own Vulkan device, inside the game ([xr_session.md](xr_session.md)).

- **Linux (Proton):** DXVK comes with Proton. The only configuration ever run.
- **Windows:** supported as **Windows + DXVK**: DXVK's `d3d9.dll` next to `Mercenaries2.exe`, plus a 32-bit OpenXR runtime (whether one exists is unchecked, [xr_session.md](xr_session.md#open)). Decided 2026-11-10. **Never run:** there is no Windows test machine.

Why DXVK and not stock Windows D3D9: OpenXR has no D3D9 binding, and DXVK's D3D9 interop hands over the Vulkan device, queue and images behind the game's D3D9 objects, which the session and the swapchain copies run on ([xr_session.md](xr_session.md)). The eye ring's textures also rely on DXVK-only behavior: legacy `pSharedHandle` textures on a plain (non-Ex) device keep their VkImage fixed ([eye_images.md](eye_images.md)). Without DXVK, the carrier logs `share: CreateTexture(… shared RT) FAILED … mc2vr needs DXVK's d3d9.dll` / `xr: the D3D9 device has no ID3D9VkInteropDevice` and VR stays inactive.

DXVK assumptions a Windows run would exercise for the first time:
- The runtime manifest lookup (`XR_RUNTIME_JSON`, else the 32-bit `ActiveRuntime` registry value, `src/carrier/vr/xr_runtime.cpp`).
- Device VmtHooks on DXVK objects ([hooks.md](hooks.md)): the same DXVK build on both platforms, so expected to hold.
- `launch.sh` is Linux-only: on Windows `mc2vr_launcher.exe` would be started directly, with the deploy and `mc2vr.conf` done by hand ([launcher.md](launcher.md)).
- The launcher's Wine VR registry init is skipped outside Wine.

OPEN: a first Windows + DXVK run, by anyone with a Windows machine and a headset.

# Platforms

The supported configuration is the game on **DXVK**: D3D9 → Vulkan for the game (carrier side), DXVK D3D11 for the [host](host.md).

- **Linux (Proton):** DXVK comes with Proton. The only configuration ever run.
- **Windows:** supported as **Windows + DXVK**: DXVK's `d3d9.dll` next to `Mercenaries2.exe`, DXVK's `d3d11.dll`/`dxgi.dll` next to `mc2vr_host.exe`. Decided 2026-11-10. **Never run:** there is no Windows test machine.

Why DXVK and not stock Windows D3D9: the eye images reach the host through `pSharedHandle` textures on the game's plain D3D9 device ([shared_textures.md](shared_textures.md)). Stock D3D9 shares only from a D3D9Ex device, and promoting the game's device to Ex would break any `D3DPOOL_MANAGED` resources (Ex rejects that pool; that the game uses it is likely for a 2008 D3D9 title but unverified), so each would need remapping. Running on DXVK on both platforms keeps the proven behavior, and keeps DXVK-only features (its Vulkan interop) available everywhere. Without DXVK, the carrier logs `share: CreateTexture(shared RT) FAILED … mc2vr needs DXVK's d3d9.dll` and VR stays inactive.

DXVK assumptions a Windows run would exercise for the first time:
- DXVK D3D9 shared handles opening in DXVK D3D11 cross-process on Windows (proven only under Proton).
- Device VmtHooks on DXVK objects ([hooks.md](hooks.md)): the same DXVK build on both platforms, so expected to hold.
- `launch.sh` is Linux-only: on Windows `mc2vr_launcher.exe` would be started directly, with the deploy and `mc2vr.conf` done by hand ([launcher.md](launcher.md)).
- The host's Wine fixups (`Software\Wine\VR`, `wineopenxr.dll`) are no-ops on Windows: the key is created if absent, the DLL load fails and is logged.

OPEN: a first Windows + DXVK run, by anyone with a Windows machine and a headset.

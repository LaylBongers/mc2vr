# Launcher

`mc2vr_launcher.exe` — win32 i386, runs in-prefix via Proton. `./launch.sh` builds if needed, deploys to `<GAME_DIR>/mc2vr/` (`cp -u` binaries, `cp -n` conf — the deployed `mc2vr.conf` is never overwritten, edit it in place) and runs under Proton. Logs land in the deploy dir: `mc2vr_{launcher,carrier}.log`. Build: [build.md](build.md). Debugging a run: [debugging.md](debugging.md).

## Architecture

```
launch.sh → mc2vr_launcher.exe (i386) → CreateProcess game SUSPENDED
  → inject mc2vr_carrier.dll ([carrier.md](carrier.md))
  → carrier installs its hooks ([hooks.md](hooks.md)) → resume game
```

- Everything is 32-bit; OpenXR runs inside the game ([xr_session.md](xr_session.md)).
- Game image can't relocate, base fixed `0x00400000` — all hook VAs are literal runtime addresses.
- DXVK runtime in this prefix (D3D9 → Vulkan → RADV): all hooked objects/vtables are DXVK implementations; same COM contract, expect quirk-level behavior differences.

## Launch sequence

1. Resolve paths from deploy location (`<game dir>/mc2vr/`).
2. Under Wine: check that a 32-bit process finds a 32-bit OpenXR runtime (logs the one-time fix if not), then publish the Wine VR registry (`vr_registry.c`, a direct call) ([openxr_proton.md](openxr_proton.md) items 2 and 4).
3. Delete stale `mc2vr_carrier.log`, then `CreateProcessW` the game `CREATE_SUSPENDED`. No exe-base check here (a suspended process has no module list yet) — the carrier verifies base + build lock in-process.
4. Inject the carrier: `CreateRemoteThread` + `LoadLibraryW` into the suspended process. Works under Proton and plain Wine.
5. Wait for the carrier log line `early init done` (attach finished), then `ResumeThread`. If it never appears, the game is killed, not resumed.

Hooks are live before the game's first instruction (**early attach**, proven under Proton 2026-10-04: attach completes ~250 ms after start, game resumes, first frame ~80 ms later; the device is hooked at its creation ([carrier.md](carrier.md)), zero SecuROM reaction). Because we run before `Direct3DCreate9`/`CreateDevice`, creation params are changeable pre-boot — the [pacing](pacing.md) vsync unlock uses this.

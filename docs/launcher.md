# Launcher

`mc2vr_launcher.exe` — win32 i386, runs in-prefix via Proton. `./launch.sh` builds if needed, deploys to `<GAME_DIR>/mc2vr/` (`cp -u` binaries, `cp -n` conf — the deployed `mc2vr.conf` is never overwritten, edit it in place) and runs under Proton. Logs land in the deploy dir: `mc2vr_{launcher,carrier,host}.log`. Build: [build.md](build.md). Debugging a run: [debugging.md](debugging.md).

## Architecture

```
launch.sh → mc2vr_launcher.exe (i386) → spawn mc2vr_host.exe (x86_64, [host.md](host.md))
  → CreateProcess game SUSPENDED → inject mc2vr_carrier.dll ([carrier.md](carrier.md))
  → carrier installs its hooks ([hooks.md](hooks.md)) → resume game
```

- Game side (launcher, carrier) is 32-bit; the host is the one 64-bit binary.
- Game image can't relocate, base fixed `0x00400000` — all hook VAs are literal runtime addresses.
- DXVK runtime in this prefix (D3D9 → Vulkan → RADV): all hooked objects/vtables are DXVK implementations; same COM contract, expect quirk-level behavior differences.

## Launch sequence

1. Resolve paths from deploy location (`<game dir>/mc2vr/`).
2. Spawn the [host](host.md) and wait for its `mc2vr_host: ready` log line (30 s; non-fatal on timeout/early exit/missing exe — no host ⇒ the game runs mono as before, logged once). `MC2VR_NO_HOST` skips the host; `MC2VR_IPC_NAME` overrides the [IPC](ipc.md) section name.
3. Delete stale `mc2vr_carrier.log`, then `CreateProcessW` the game `CREATE_SUSPENDED`. No exe-base check here (a suspended process has no module list yet) — the carrier verifies base + build lock in-process.
4. Inject the carrier: `CreateRemoteThread` + `LoadLibraryW` into the suspended process. Works under Proton and plain Wine (selftest, [build.md](build.md)).
5. Wait for the carrier log line `early init done` (stage 1 finished), then `ResumeThread`. If it never appears, the game is killed, not resumed.

Hooks are live before the game's first instruction (**early attach**, proven under Proton 2026-10-04: stage 1 completes ~250 ms after start, game resumes, first frame ~80 ms later, device capture ~150 ms after that, zero SecuROM reaction). Because we run before `Direct3DCreate9`/`CreateDevice`, creation params are changeable pre-boot — the [pacing](pacing.md) vsync unlock uses this.

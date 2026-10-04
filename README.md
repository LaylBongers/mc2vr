# Mercenaries 2 VR Project

Adding full-featured VR support to the game "Mercenaries 2: World in Flames", through reverse
engineering.

> This project is almost entirely implemented by AI agents, and in its current state extremely
> fragile. I don't recommend even attempting to use it just yet. Write-up, coming soon!

## Setup

For VR support, the launch script in this project is set up to launch the game under Proton.
You need to configure the location of your game install, and the location of Steam.

Copy "launch.conf.example" to "launch.conf", and fill in the missing details.

## Launcher / carrier

The launcher (`mc2vr_launcher.exe`) starts the game, waits for the main loop to
come alive (per-frame counter moving), and injects `mc2vr_carrier.dll`, which
installs SafetyHook hooks in-process. Plan and hook list:
`docs/launcher_plan.md`.

Build (32-bit MinGW cross toolchain, outputs to `build/win32/bin/`):

```sh
cmake -B build/win32 -DCMAKE_TOOLCHAIN_FILE=cmake/i686-w64-mingw32.cmake
cmake --build build/win32
```

`./launch.sh` builds if needed, deploys both binaries to `<GAME_DIR>/mc2vr/`,
and runs them under Proton. Logs land next to the deployed binaries
(`mc2vr_launcher.log`, `mc2vr_carrier.log`).

A self-test of the whole launcher → carrier chain (without the game, under plain
Wine) can be run with `tools/selftest/run.sh` — it uses a stand-in exe and also
verifies that the carrier's build lock refuses to hook a mismatched binary.

The carrier refuses to patch anything unless the game exe matches the hash in
`src/carrier/build_lock.h`. After re-RE'ing a different game build,
regenerate it with:

```sh
tools/gen-build-lock.sh /path/to/Mercenaries2.exe
```

SafetyHook (v0.7.0, BSL-1.0) is vendored under `vendor/safetyhook/`.

## Ghidra


The majority of the reverse engineering effort in this project is done through ghidra.
Ghidra files are impractical to check in to git, so instead we provide symbols in CSV format, and
data types in C header format.

Note: These are not yet available.

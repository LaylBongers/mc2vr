# Build / test

## Game side (win32)

```
cmake -B build/win32 -DCMAKE_TOOLCHAIN_FILE=cmake/i686-w64-mingw32.cmake
cmake --build build/win32
```

→ `build/win32/bin/{mc2vr_launcher.exe,mc2vr_carrier.dll}`. A 32-bit configure builds ONLY the game side.

## Host (win64)

```
cmake -B build/win64 -DCMAKE_TOOLCHAIN_FILE=cmake/x86_64-w64-mingw32.cmake
cmake --build build/win64
```

→ `build/win64/bin/mc2vr_host.exe` ([host.md](host.md)). A 64-bit configure builds ONLY the host.

## Build lock

The carrier refuses to hook unless the game exe size + SHA-256 on disk match `src/carrier/build_lock.h`. Regenerate after re-RE'ing a different game build: `tools/gen-build-lock.sh <exe>`.

## Selftest

`tools/selftest/run.sh` — full chain under plain Wine using a sleeper stand-in exe (base 0x400000 + ticking counter at the literal VA): injection, early hooks, build-lock refusal, host lifecycle (phase A) and `--mock` IPC round trip (phase B). Run after carrier changes. Needs an unsandboxed terminal (wineserver needs Unix sockets) and `mkdir -p /tmp/opencode` first.

## Dependencies / tools

- SafetyHook v0.7.0 vendored in `vendor/safetyhook/` (amalgamated + Zydis, BSL-1.0, C++23).
- OpenXR SDK 1.1.54 vendored, statically linked into the host.
- `tools/analyze_dumps.py <log>` — parses carrier view-dump / eye-evidence blocks.
- `tools/test/test_vp_camera.cpp` — native g++ unit test for the camera math ([camera.md](camera.md)); build line in its header.
- `tools/probe/run_shared_handle.sh` — shared-handle interop probe ([shared_textures.md](shared_textures.md)).

# Build / test

## Build (win32)

```
cmake -B build/win32 -DCMAKE_TOOLCHAIN_FILE=cmake/i686-w64-mingw32.cmake
cmake --build build/win32
```

→ `build/win32/bin/{mc2vr_launcher.exe,mc2vr_carrier.dll}`. Everything is 32-bit; a 64-bit configure is refused.

## Editor / clangd

- The build exports `compile_commands.json`, with include paths inline instead of in the MinGW toolchain's response files; `.clangd` points at `build/win32`.
- clangd takes the cross target from the compiler name. `--query-driver=/usr/bin/*-w64-mingw32-*` in the editor's clangd arguments is optional.
- `clangd --check=<file>` reports `tweak: … FAIL` lines from its own refactoring self-tests. Those aren't diagnostics; real ones show as `[code] Line N:`.

## Build lock

The carrier refuses to hook unless the game exe size + SHA-256 on disk match `src/carrier/build_lock.h`. Regenerate after re-RE'ing a different game build: `tools/gen-build-lock.sh <exe>`.

## Dependencies / tools

- SafetyHook v0.7.0 vendored in `vendor/safetyhook/` (amalgamated + Zydis, BSL-1.0, C++23).
- OpenXR SDK 1.1.54 vendored; the carrier uses its headers only and has no loader ([xr_session.md](xr_session.md)).
- Vulkan-Headers 1.4.363 vendored (`vendor/vulkan-headers/`), headers only: the carrier takes Vulkan from the library DXVK loaded.
- `tools/analyze_dumps.py [dir]` — measures eye-pair dumps (`debug_eye_dump_frames=N`).
- `tools/test/test_vp_camera.cpp` — native g++ unit test for the camera math ([camera.md](camera.md)); build line in its header.
- `tools/test/test_call_shim.cpp` — the game call shim under Wine ([reimpl.md](reimpl.md#tests)).
- `tools/test/test_game_entry.cpp` — the entry for replacements under Wine ([reimpl.md](reimpl.md#tests)).
- `tools/diagrams/render_pipeline.py` → `docs/render_pipeline.svg`: one frame end to end, for people (publishing), not for agents. It's ~70 KB of layout; the facts it shows are in the notes. Re-run it after notes change what it shows.

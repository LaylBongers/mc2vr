# Code structure

How the source is organized, and the reasoning behind the split. It describes the current layout. It isn't a checklist: where something doesn't fit cleanly, the reasoning matters more than the boxes.

## Top level

- **`src/launcher/`**: starts the game suspended, injects the carrier, spawns the host ([launcher.md](launcher.md)).
- **`src/host/`**: the 64-bit OpenXR host, in its own process ([host.md](host.md)).
- **`src/common/`**: the shared-memory contract between carrier and host (`mc2vr_ipc.h`) and small shared math.
- **`src/game/`** (`mc2vr_game` library): facts about `Mercenaries2.exe`, without mod behavior, mirroring the Ghidra project and keyed to the exe hash.
  - Typed data: structs with offset `static_assert`s, and `Global<T, VA>` globals under their Ghidra names.
  - Original functions as typed callables (`function.hpp`).
  - `addresses.h` holds hook/probe sites and older offset macros; the typed headers are the newer form.
- **`src/carrier/`**: the injected DLL; layout below.
- **`tools/`**: offline tools (`securom_trace.py`, `analyze_dumps.py`) and the tests in `tools/test/` ([reimpl.md](reimpl.md#tests)).

## `src/carrier/`

- **(root):** infrastructure: `dllmain`, `init` (conf parsing, install order), `log`, `ipc` (host connection), `build_lock`.
- **`hooks/`:** the attachments to game code: hook objects, the callbacks that decode registers and stack, code patches, and the D3D9 vtable hooks.
  - `install.cpp` lists them in install order.
  - `site.hpp` creates hooks and refuses sites inside replaced originals, where a hook would silently never fire.
  - `reimpl.cpp` lists the reimplementations that replace originals.
  - Keeping this in one place makes the full set of patches to the game visible, and keeps VR code free of register-level detail.
- **`reimpl/`:** reimplemented game systems ([reimpl.md](reimpl.md)), written to read like the original through `game::` types and functions. They call `vr/` directly where they implement VR behavior, which is the reason for reimplementing something at all.
- **`vr/`:** the VR implementation as plain C++: camera, view rewrite, eye images, render outputs and sizing, pacing, device parameters, shader constant tables.
  - Entry points are named for the game event they handle (`on_<event>`).
  - It uses a few services the hooks provide (`hooks::frame_count()`, `device::blit_surfaces`).
- **`debug/`:** debugging tools, each behind a `debug_*` conf key (off by default): censuses, dumps, probes, tracers. They install their own hooks, and the 10 s report poller lives here.
  - Other code calls into them only through instrumentation taps that do nothing while the tool is off.
  - So switching a tool off never changes what the mod does.

## Dependencies as they stand

- `game/` depends on nothing in the carrier.
- `hooks/` uses `vr/`, `reimpl/`, `game/` and the root.
- `reimpl/` uses `vr/`, `game/` and the root.
- `vr/` uses `game/`, the root and those hook services.
- `debug/` uses anything.
- Only `hooks/` and `debug/` use SafetyHook. `grep -rl 'safetyhook\|SafetyHook' src/carrier | grep -v 'hooks/\|debug/'` lists only `CMakeLists.txt`.
- Debug taps appear in `hooks/`, `reimpl/` and `vr/`.

A new VR feature usually has its logic in `vr/`, its hook in `hooks/<feature>.cpp` and an entry in `hooks/install.cpp`. A reimplementation goes in `reimpl/<subsystem>/` with an entry in `hooks/reimpl.cpp`. Hooks that sat inside the replaced original become direct calls in it; the guard logs a REFUSED line for any left over.

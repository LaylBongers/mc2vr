# Code structure: what goes where

Source layout and the dependency rules between directories. When adding code, put it where this says; when a file starts doing two of these jobs, split it.

## Top level

- **`src/launcher/`**: starts the game suspended, injects the carrier, spawns the host ([launcher.md](launcher.md)).
- **`src/host/`**: the 64-bit OpenXR host, in its own process ([host.md](host.md)).
- **`src/common/`**: the shared-memory contract between carrier and host (`mc2vr_ipc.h`) and small shared math.
- **`src/game/`** (`mc2vr_game` library): facts about `Mercenaries2.exe`, no mod behavior. Ghidra is the source of truth, and it is all keyed to the exe hash.
  - Typed data: structs with offset `static_assert`s, `Global<T, VA>` globals under their Ghidra names.
  - Original functions as typed callables (`function.hpp`).
  - `addresses.h`: hook/probe sites and older offset macros. New game data goes in the typed headers.
- **`src/carrier/`**: the injected DLL. Layout below.
- **`tools/`**: offline tools (`securom_trace.py`, `analyze_dumps.py`) and the two tests in `tools/test/` ([reimpl.md](reimpl.md#tests)).

## `src/carrier/`

| Directory | What goes there | Must not |
|---|---|---|
| (root) | Infrastructure: `dllmain`, `init` (conf parsing, install order), `log`, `ipc` (host connection), `build_lock`. | Contain VR logic or hooks. |
| `hooks/` | Every attachment to game code: hook objects, register/stack-decoding callbacks, code patches, the D3D9 vtable hooks, and the install list (`install.cpp`). A callback decodes the context, calls `vr/` or `reimpl/`, and writes results back. `site.hpp` is the only way to create a hook; it refuses sites inside replaced originals. `reimpl.cpp` lists the reimplementations that replace originals. | Hold VR decisions (policy, math, state machines); those go in `vr/`. |
| `reimpl/` | Reimplemented game systems, written to read like the original (handler by handler, through `game::` types and functions). A reimplementation calls `vr/` directly where it implements VR behavior; that is the point of reimplementing. See [reimpl.md](reimpl.md). | Create hooks, or call other reimplemented code through `game::` (call it directly). |
| `vr/` | The VR implementation as plain C++: camera, view rewrite, stereo replay, eye images, render outputs and sizing, pacing, device parameters. Entry points are named for the game event they handle (`on_<event>`). | Include `<safetyhook.hpp>` or take a `safetyhook::Context`. May use hook-provided services (`hooks::frame_count()`, `device::blit_surfaces`). |
| `debug/` | Debugging tools, self-contained, each behind a `debug_*` conf key (default off): censuses, dumps, probes, tracers, and `shadow/` (reimplementation verification). They install their own hooks, through `hooks/site.hpp`. The 10 s report poller is here. | Be needed for anything to work. Code outside `debug/` may call a tool's instrumentation taps (no-ops when it is off), but never depends on `debug/` for functionality. |

## Dependency rules

- `game/` depends on nothing.
- `hooks/` → `vr/`, `reimpl/`, `game/`, root. Also debug taps.
- `reimpl/` → `vr/`, `game/`, root.
- `vr/` → `game/`, root, and hook-provided services. Also debug taps.
- `debug/` → anything.
- Only `hooks/` and `debug/` use SafetyHook.

Check: `grep -rl 'safetyhook\|SafetyHook' src/carrier | grep -v 'hooks/\|debug/'` lists only `CMakeLists.txt`.

## Adding things

- **A new VR feature on an existing game event:** logic goes in `vr/`, the hook in `hooks/<feature>.cpp`, and one line in `hooks/install.cpp`.
- **Reimplementing a function:** the code goes in `reimpl/<subsystem>/` and the entry in `hooks/reimpl.cpp`. Any existing hook inside the original's body becomes a direct call from the reimplementation (the guard logs a REFUSED line if one is left). Verify the transcription against the original first ([reimpl.md](reimpl.md#verifying-a-new-reimplementation)).
- **A debugging tool:** everything goes in `debug/<tool>.cpp`, with a `debug_<tool>` conf key in `init.cpp` and its install in the debug section of `init.cpp`.

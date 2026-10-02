# Main Game Loop

Per-address facts (names, prototypes, loop addresses, vtable layouts, globals, startup chain) are stored in the Ghidra project as labels, plate comments, and the `Analysis/GameLoop` bookmark at `0x0063184c` — this file intentionally does not repeat them. SecuROM/OEP context: see `initial_analysis.md`.

## How the loop was located (anchors)

- `PeekMessageA` import has exactly one calling function in `.text` → the Win32 message pump, registered as a task by its single caller → the loop owner is one level up (`GameShell_Run`).
- Confirmed via debug strings: `"Mercenaries2"` mutex, `"g_HWND = 0x%08x"` print, `"vz.wad"` check — all inside the same function.
- The loop back-edge was verified byte-exact (`CMP`/`JNZ`/`CALL`/`JZ`) rather than trusting the decompiler.
- `WinMain`'s missing static xrefs explained: the CRT calls it through a SecuROM import-wrapper (`Thunk_to_WinMain`, indirect pointer) — resolved only at runtime. Same pattern hides several init calls and the task dispatcher.
- Engine identified as Pandemic's Pangea layer from debug source paths (anchor comment at the `PgCloudsWin32.cpp` string). RTTI only covers Havok classes, so Pangea classes must be identified by strings/fields/vtables.

## VR hook plan (strategy)

- Preferred frame hook: `GameShell_FrameTick` — **DONE (M1, live)**: carrier InlineHook, per-frame counting + 10s timing reports. Alternative frame-level slots: `g_RenderShell` vtable slots 4/5 (`EndOfFrameHook`/`PostUpdateHook`, +0x10/+0x14, NoOp on the live base vtable — see `render_path.md`).
- Reuse existing timing: `g_FrameDeltaSec` (raw QPC dt) and `g_Dt` (managed, post-framerate-policy dt) are already computed per frame — don't re-derive.
- Frame pacing for VR should bypass/neutralize the adaptive framerate path (`g_FrameratePolicy` / `AdaptiveFramerate_Govern`, ini `[framerate]` presets) so the HMD drives the cadence.
- Input injection has no dedicated input-update call: input flows through the state stack (`GameStateStack_Update`) and buffers cleared on the idle-reset path.
- Mechanism (proven, see `launcher_plan.md`): carrier (in-process SafetyHook) installs hooks statically by literal VA — image base fixed (no ASLR, relocs stripped, `initial_analysis.md`). Never hook via the SecuROM wrapper pointers (runtime-only) or inside `0x01a48000+`.
- Boot gate: carrier is injected when the per-frame counter at `0x011755bc` moves (main loop alive, SecuROM startup stub finished by construction). The pre-D3D loop spins uncapped (~1400 Hz); the D3D device already exists by carrier-init time.

## Open items

- True class name of the shell singleton (`g_RenderShell` = `0x017ceaf0`, holds base `LtiRenderer_vtbl` at frame time) unknown — no Pangea RTTI.
- Purpose of the pointer array at `0x017d30e8` (count `0x017d30dc`) and the `0x1000`-byte buffer at `0x00f7fb90`, both cleared on idle reset — suspected input event buffer; resolve before designing input injection.
- Roles of the two task tables (`g_TaskTable1`/`g_TaskTable2`) — one may be a shutdown table.
- Full mapping of ini `[framerate]` preset strings ("Strict Adaptive", "Hybrid", ...) to `AdaptiveFramerate_Govern` branches.
- `GameState3_Update` / `GameState2_Frontend_Update` internals — named by position, semantics unexplored.

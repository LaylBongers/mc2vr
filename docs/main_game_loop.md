# Main Game Loop

Per-address facts (names, prototypes, loop addresses, vtable layouts, globals, startup chain) are stored in the Ghidra project as labels, plate comments, and the `Analysis/GameLoop` bookmark at `0x0063184c` — this file intentionally does not repeat them. SecuROM/OEP context: see `initial_analysis.md`.

## How the loop was located (anchors)

- `PeekMessageA` import has exactly one calling function in `.text` → the Win32 message pump, registered as a task by its single caller → the loop owner is one level up (`GameShell_Run`).
- Confirmed via debug strings: `"Mercenaries2"` mutex, `"g_HWND = 0x%08x"` print, `"vz.wad"` check — all inside the same function.
- The loop back-edge was verified byte-exact (`CMP`/`JNZ`/`CALL`/`JZ`) rather than trusting the decompiler.
- `WinMain`'s missing static xrefs explained: the CRT calls it through a SecuROM import-wrapper (`Thunk_to_WinMain`, indirect pointer) — resolved only at runtime. Same pattern hides several init calls and the task dispatcher.
- Engine identified as Pandemic's Pangea layer from debug source paths (anchor comment at the `PgCloudsWin32.cpp` string). RTTI only covers Havok classes, so Pangea classes must be identified by strings/fields/vtables.

## VR hook plan (strategy)

- Preferred frame hook: `GameShell_FrameTick` — single call site, covers update → render → present ordering every frame. Alternative injection slots: the two empty `RenderShell_vtable` hooks (`vt[4]`/`vt[5]`), which nothing else uses; confirm at runtime no subclass overrides them.
- Reuse existing timing: `g_FrameDeltaSec` (raw QPC dt) and `g_Dt` (managed, post-framerate-policy dt) are already computed per frame — don't re-derive.
- Frame pacing for VR should bypass/neutralize the adaptive framerate path (`g_FrameratePolicy` / `AdaptiveFramerate_Govern`, ini `[framerate]` presets) so the HMD drives the cadence.
- Input injection has no dedicated input-update call: input flows through the state stack (`GameStateStack_Update`) and buffers cleared on the idle-reset path.
- Hook statically by VA, never via the SecuROM wrapper pointers — they are runtime-only. Launcher-style inline patching (external process + hooking lib writing trampolines) is viable as-is: image base is fixed (no ASLR, relocs stripped — see `initial_analysis.md`), so Ghidra VAs are literal runtime addresses. Patch `.text` callers, never the encrypted regions.
- Attaching a debugger: break at `WinMain` or `GameShell_Run`, not at the PE entry (SecuROM stub — see `initial_analysis.md`). Same timing rule for a launcher: if patching a suspended process before the SecuROM stub runs misbehaves, defer patching until after the stub (e.g., first `GameShell_FrameTick`).

## Open items

- True class name of the shell singleton (`g_RenderShell`) unknown — no Pangea RTTI.
- ~~Actual render call site inside a state's `update(dt)` not yet traced~~ RESOLVED:RESOLVED: toptop state `g_InGameShellState``g_InGameShellState` →→ frame pipeline →frame pipeline → render packet submit; full chainpacket submit; full chain in the `GameShell_FrameTick``GameShell_FrameTick` plate comment, see `render_path.md`plate comment, see `render_path.md`.
- ~~`DAT_01175288` singleton unidentified~~unidentified~~ RESOLVED: LTI `RenderSystem` singleton (created in `RenderSystem_Init`, owns the D3D9 state layer;RESOLVED: LTI `RenderSystem` singleton (created in `RenderSystem_Init`, owns the D3D9 state layer; `+0x5bc` sub-objectsub-object receivesreceives initialinitial state callsstate calls).
- Runtime confirmation that `vt[4]`/`vt[5]` remain no-ops (static analysis says the base+derived vtables both install `VirtHook_NoOp`).
- Purpose of the pointer array at `0x017d30e8` (count `0x017d30dc`) and the `0x1000`-byte buffer at `0x00f7fb90`, both cleared on idle reset — unknown.
- Roles of the two task tables (`g_TaskTable1`/`g_TaskTable2`) — one may be a shutdown table.
- Full mapping of ini `[framerate]` preset strings ("Strict Adaptive", "Hybrid", ...) to `AdaptiveFramerate_Govern` branches.

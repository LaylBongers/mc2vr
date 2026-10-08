# reverse_engineering/ index

Findings about the game itself (Mercenaries 2, Pandemic "G" engine), independent of our mod. Per-address
facts live in the Ghidra project (plates, labels, structs); these docs hold methodology, cross-cutting facts
and open items. Check here before researching anything. Mod implementation notes are in `../`.

| Doc | Read it for |
|---|---|
| `target_binary.md` | PE layout, sections, header facts, SecuROM on-disk state, symbols |
| `securom_vm.md` | SecuROM/VM boundary: call gates vs virtualized functions vs mutated plaintext vs runtime-patched thunks |
| `pandemic_engine.md` | Engine naming layers, identification oracles, name hashing, render data model, decompiler idioms |
| `main_game_loop.md` | How the game loop was located, timing/input facts, open items |
| `render_path.md` | Threading, frame driver chain, VM stub callbacks, D3D device slots, Dx9 state wrapper, queue counters, **§ draw-camera constant chain (E1/E1b watch-proven): VM orchestrates, plaintext builds the VP (ViewContext_BuildCameraConstants 0x008591ac) and fills the g_ViewContextTable records once per frame — incl. the probe-proven § matrix & handedness conventions (LH pipeline, entry rows = rendered axes, builder inverse)** |
| `view_and_camera.md` | Where the draw camera lives, `viewContextData` layout, technique constant map, view table/queue, **§ camera-data accessors: the watch-proven culling data flow (all plaintext, change-gated, `ViewEntry_DeriveCullTask 0x00876a90`; pose values originate in the VM via the staged round-trip)**, **§ view frustum: the builder's frustum products and their consumers (incl. the third-person camera clearance)**, the `debug_watch` tooling lessons, open RE items |
| `shader_ctab_map.md` | Generated VS constant-register map (`tools/shader_ctab.py`) |
| `vtables.md` | Recipe for annotating vtables in Ghidra |
| `ghidra-reva.md` | ReVa MCP usage notes and tool-call gotchas |
| `data/` | Captured runtime data (`vm_thunks_runtime.csv`) |

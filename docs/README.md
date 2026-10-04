# docs/ index

Per-address facts live in the Ghidra project (plates, labels, structs); these docs hold methodology,
cross-cutting facts and open items. Check here before researching anything.

| Doc | Read it for |
|---|---|
| `launcher_plan.md` | Launcher/carrier architecture, build/test, proven mechanism rules, hook inventory, milestone history (M0–M4) |
| `stereo_design.md` | Stereo design + status (S0–S5), the view channel, S2c second pass, open RE items |
| `s4_handover.md` | Active brief: HMD presentation (64-bit OpenXR host process, shared-handle images, IPC) + pose/event feedback |
| `render_path.md` | Frame driver chain, threading, VM stub callbacks, D3D device slots (`render_diagram.svg` = overview) |
| `pandemic_engine.md` | Engine naming/data model, SecuROM/VM boundary (gates vs VM vs runtime-patched thunks) |
| `initial_analysis.md` | PE layout, SecuROM v7 on-disk state, symbols |
| `main_game_loop.md` | How the game loop was located, frame/timing/input notes, open items |
| `shader_ctab_map.md` | Generated VS constant-register map (`tools/shader_ctab.py`) |
| `vtables.md` | Recipe for annotating vtables in Ghidra |
| `ghidra-reva.md` | ReVa MCP usage notes and tool-call gotchas |
| `data/` | Captured runtime data (`vm_thunks_runtime.csv`) |

Config keys are documented in `conf/mc2vr.conf` (debug tools: `debug_*`, code in `src/carrier/debug/`).

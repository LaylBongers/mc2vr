# AGENTS.md - MC2VR Project

This is a reverse engineering and modding project for the game "Mercenaries 2".
The goal is to add full-featured VR support, including motion controls.

## Quick Reference

- Target: `Mercenaries2.exe` (PE32 i386, ~52 MB), path from `launch.conf`. Ghidra program: `/Mercenaries2.exe` in `ghidra/mc2re.gpr` (gitignored). ReVa MCP tools: `ghidra-reva` namespace.
- SecuROM v7's enforcement layer is bypassed and inert (no license/anti-tamper/anti-debug response — proven M0–M3) — treat it as a non-issue for hooking safety. But the VM itself still EXECUTES: selected game functions are VM-virtualized (e.g. the `GetD3DDevice` thunk `0x0047f2f0`, and — per S0 — the render-packet interpreter, stub `0x0050f660`, called at `0x004c99f9`). Ignore the `Stext`/`Sitext`/`Srdata`/`Sdata`/`Sidata`/`.securom` sections (VM bytecode region: opaque, out of bounds for RE — hook the plaintext callers/neighbors instead). Only if live debugging/hooking misbehaves, suspect leftover anti-tamper there (see the doc).
- The binary has rich embedded MSVC symbols (Pandemic "G" engine).
- `/Mercenaries2.exe.0` in the Ghidra project is just the DOS stub; ignore it.
- When you encounter a vtable, annotate it pre-emptively (vtable struct + typed object/pointer globals) so virtual calls decompile as `obj->vftable->Method()` instead of raw pointer arithmetic. No MSVC RTTI in this binary — derive class names from symbols/ctors. See `docs/vtables.md` for the recipe and tooling gotchas.
- Before researching anything, check `docs/` for prior findings — they record methodology, cross-cutting facts, and open items not repeated in the Ghidra project (per-address facts live in Ghidra; docs point to them).
- Launcher/carrier (proven, M0–M2 complete): build `cmake -B build/win32 -DCMAKE_TOOLCHAIN_FILE=cmake/i686-w64-mingw32.cmake`; test chain headless via `tools/selftest/run.sh`; live-run via `./launch.sh`, logs in `<GAME_DIR>/mc2vr/mc2vr_*.log`. Mechanism rules and hook list: `docs/launcher_plan.md`.
- Iteration loop: the agent implements/logs; the human runs `./launch.sh` (gameplay, not menu, for render instrumentation) and reports; the agent audits the logs. `tools/analyze_dumps.py` parses carrier view-dump blocks and S1 evidence.
- **CURRENT MILESTONE STATE lives in `docs/stereo_design.md` (design + phase status) and `docs/s1_camera_hunt.md` (S1 evidence + S2 handoff data) — read BOTH FIRST when continuing, and update THEM (never this file) after each run.** Do not duplicate state here (this file is session-injected; keep it stable). Orientation in one line: M0–M3 + M4/S0 + M4/S1 complete — S1 proved the draw camera never surfaces in patchable plaintext data, so per-eye injection is GPU-boundary (SetVertexShaderConstantF rewrite — mechanism verified visible); S2 (camera-row identification → per-eye rewrite → stream replay) is the current phase.

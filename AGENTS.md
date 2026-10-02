# AGENTS.md - MC2VR Project

This is a reverse engineering and modding project for the game "Mercenaries 2".
The goal is to add full-featured VR support, including motion controls.

## Quick Reference

- Target: `Mercenaries2.exe` (PE32 i386, ~52 MB), path from `launch.conf`. Ghidra program: `/Mercenaries2.exe` in `ghidra/mc2re.gpr` (gitignored). ReVa MCP tools: `ghidra-reva` namespace.
- SecuROM v7 is present but already bypassed and inert — treat it as a non-issue. Ignore the `Stext`/`Sitext`/`Srdata`/`Sdata`/`Sidata`/`.securom` sections (protection VM, not game code). Only if live debugging/hooking misbehaves, suspect leftover anti-tamper there (see the doc).
- The binary has rich embedded MSVC symbols (Pandemic "G" engine).
- `/Mercenaries2.exe.0` in the Ghidra project is just the DOS stub; ignore it.
- When you encounter a vtable, annotate it pre-emptively (vtable struct + typed object/pointer globals) so virtual calls decompile as `obj->vftable->Method()` instead of raw pointer arithmetic. No MSVC RTTI in this binary — derive class names from symbols/ctors. See `docs/vtables.md` for the recipe and tooling gotchas.
- Before researching anything, check `docs/` for prior findings — they record methodology, cross-cutting facts, and open items not repeated in the Ghidra project (per-address facts live in Ghidra; docs point to them).
- Launcher/carrier (proven, M0–M2 complete): build `cmake -B build/win32 -DCMAKE_TOOLCHAIN_FILE=cmake/i686-w64-mingw32.cmake`; test chain headless via `tools/selftest/run.sh`; live-run via `./launch.sh`, logs in `<GAME_DIR>/mc2vr/mc2vr_*.log`. Mechanism rules and hook list: `docs/launcher_plan.md`.
- Iteration loop: the agent implements/logs; the human runs `./launch.sh` (gameplay, not menu, for render instrumentation) and reports; the agent audits the logs. `tools/analyze_dumps.py` parses carrier view-dump blocks. Milestone state: M0–M3 complete; M4 design at `docs/stereo_design.md` — next step is its S0 (pure Ghidra RE, no run needed).

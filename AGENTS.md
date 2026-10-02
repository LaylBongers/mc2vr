# AGENTS.md - MC2VR Project

This is a reverse engineering and modding project for the game "Mercenaries 2".
The goal is to add full-featured VR support, including motion controls.

## Quick Reference

- Target: `Mercenaries2.exe` (PE32 i386, ~52 MB), path from `launch.conf`. Ghidra program: `/Mercenaries2.exe` in `ghidra/mc2re.gpr` (gitignored). ReVa MCP tools: `ghidra-reva` namespace.
- SecuROM v7 is present but already bypassed and inert — treat it as a non-issue. Ignore the `Stext`/`Sitext`/`Srdata`/`Sdata`/`Sidata`/`.securom` sections (protection VM, not game code). Only if live debugging/hooking misbehaves, suspect leftover anti-tamper there (see the doc).
- The binary has rich embedded MSVC symbols (Pandemic "G" engine).
- `/Mercenaries2.exe.0` in the Ghidra project is just the DOS stub; ignore it.

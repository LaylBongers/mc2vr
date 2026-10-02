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

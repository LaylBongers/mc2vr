# AGENTS.md - MC2VR Project

This is a reverse engineering and modding project for the game "Mercenaries 2".
The goal is to add full-featured VR support. (Motion controls were cancelled — facts kept in `docs/input_injection.md`.)

## Quick Reference

- Read `docs/reverse_engineering/ghidra-reva.md` on how to use reva MCP correctly.
- Target: `Mercenaries2.exe` (PE32 i386, ~52 MB, rich embedded MSVC symbols — Pandemic "G" engine), path from `launch.conf`. Ghidra program: `/Mercenaries2.exe` in `ghidra/mc2re.gpr` (gitignored); `/Mercenaries2.exe.0` is just the DOS stub, ignore it. ReVa MCP tools: `ghidra-reva` namespace.
- SecuROM v7 is bypassed and inert — a non-issue for hooking safety (proven in live runs). What looks like a SecuROM thunk is typically one of: a forwarder you can follow (call gate, runtime-patched slot, or SecuROM-mutated native code — in `.text` or relocated into `.securom`, statically analyzable), VM-virtualized residue (read it in the live game: `debug_vm_dump`), or not SecuROM at all (a Ghidra coverage gap or a misread address). Classify before calling anything opaque; hook plaintext call sites, not thunks or protected code. Full rules: `docs/reverse_engineering/securom_vm.md`.
- No MSVC RTTI in this binary (except Havok) — derive class names from symbols/ctors, and annotate vtables pre-emptively so virtual calls decompile as `obj->vftable->Method()` instead of raw pointer arithmetic (`docs/reverse_engineering/vtables.md`).
- Code layout (`hooks/`, `reimpl/`, `vr/`, `debug/`, `src/game/`) and the reasoning behind it: `docs/code_structure.md`.
- Before researching anything, check `docs/` (mod implementation) and `docs/reverse_engineering/` (game-side RE) for prior findings — methodology, cross-cutting facts, and open items live in the notes; per-address facts live in the Ghidra project.
- Milestone status and handover briefs are deliberately NOT tracked in this file (they churn between milestones) — open tracks live in the `docs/` note they concern, closed history in git. Keep this file to stable standing rules; do not edit it as part of routine milestone work.
- Launcher/carrier (proven): build `cmake -B build/win32 -DCMAKE_TOOLCHAIN_FILE=cmake/i686-w64-mingw32.cmake`; live-run via `./launch.sh`, logs in `<GAME_DIR>/mc2vr/mc2vr_*.log`. Mechanism rules and hook list: `docs/hooks.md`.
- Iteration loop: the agent implements/logs; the human runs `./launch.sh` (gameplay, not menu, for render instrumentation) and reports; the agent audits the logs. `tools/analyze_dumps.py` measures the carrier's eye-pair dumps.

## Documentation

`docs/` is a zettelkasten: many small, terse, atomic notes linked with relative markdown links — both the mod notes and the game-side RE notes in `docs/reverse_engineering/`. There is no index — enter through `docs/launcher.md` (the system pipeline) or grep.

- One topic per note; link to related notes instead of repeating their content. When a note grows past one topic, split it.
- Per-address facts live in the Ghidra project, not docs — docs record methodology, cross-cutting facts, and open items, and point to them.
- Open tracks / known issues live in the note they concern, marked OPEN (e.g. `docs/hud.md`). When a track closes, record the verdict in the note; history lives in git — no changelogs.

## Context Budget

Every tool result stays in context and is re-sent on every later turn, so bulky output costs far more than its one-time size.

- Delegate broad sweeps to a subagent (`Explore` for read-only search): wide Ghidra digging (e.g. `search-comments` on a generic term, surveying many decompilations/xrefs), multi-doc surveys, or grepping across many files when only the conclusion is needed. Ask it for a compact answer (addresses, names, one-line findings) — not raw dumps. Do targeted single lookups (one function, one known address, one doc section) inline.
- Keep ReVa queries narrow: use specific search terms over generic ones (`camera`, `wrapper`), always pass an explicit small `maxResults` (≤15; tool defaults like `search-comments`' 100 are too large — raise only if the first page proves insufficient); `get-decompilation` one function at a time, paging with `offset`/`limit` (default 50 lines) rather than one huge `limit`, `signatureOnly=true` when only the prototype matters, and `includeReferenceContext=false` unless caller snippets are needed (on by default); avoid re-dumping large structures/vtables already annotated (`get-structure-info` has no field filter — it always returns the whole layout).

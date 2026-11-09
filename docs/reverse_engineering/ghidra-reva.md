# Ghidra / ReVa notes

How to work with the Ghidra project safely. Locating/identifying code: [re_methodology.md](re_methodology.md).

## Project state

- `/Mercenaries2.exe` is under Ghidra version control; check-ins are restore points. `checkin-program` reports "Attempted to release domain object with unknown consumer" yet succeeds — confirm via `getDomainFile().getVersionHistory()`. Old versions open read-only with `getDomainFile().getReadOnlyDomainObject(consumer, version, monitor)` (release them afterwards) — useful to diff or recover lost work.
- The "Non-Returning Functions – Discovered" analyzer is disabled for this program: it misreads thunk-shaped code and flags returning functions as no-return (it once caught `EnterCriticalSection` through a one-instruction `CALL [EnterCriticalSection]`), then "repairs" every caller by cutting it off at the call.
  - Such damage outlives the flag: per-instruction `CALL_RETURN` flow overrides stay on the call sites, and the code after them stays undefined or becomes a separate `FUN_`.
  - **Symptom:** a function body that ends at a call to a returning function. Notes that call code "undefined" or "without static callers" may stem from this.
  - **Reading overrides:** `CALL_RETURN` on a `JMP` is a tail call, which is normal (about 2,000 in `.text`). On a `CALL` it means the call never returns, which in `.text` is only `exit` / `longjmp`. A jump with the override also reports a call-type reference, so a count by reference type mixes the two.
- Undefined code or a missing caller is not by itself evidence of protection: check the raw bytes for a direct `E8 rel32` call before concluding the code is VM-only or mutated.
- Auto-analysis runs after each commit and can undo or re-derive a change within seconds (it clears fall-through overrides on CALL-overridden `jmp`s, [vm_thunk_patches.md](vm_thunk_patches.md)). Check results once it is idle (`AutoAnalysisManager.getAnalysisManager(prog).isAnalyzing()`).

## Editing

- Precise or bulk edits: `run-script` with its own transaction (`prog.startTransaction(msg)` … `prog.endTransaction(tx, ok)`; `ok=False` rolls back).
- Rename a function without touching its signature: `run-script` → `f.setName(name, SourceType.USER_DEFINED)`. (`create-label` + `setAsPrimary` can leave the label secondary on an already-named function; `set-function-prototype` rewrites the prototype, mangling register-arg `unaff_` decompiles, and rejects `__thiscall`.)
- `set-comment` replaces the whole comment of that type — read it first, resend the full text.
- `parse-c-structure` on an existing name replaces the struct in place; dependents keep working.

## Naming and provenance

- **Names:** the C++ name `TypeName::Member` becomes `TypeName_Member`. Name by observed role, never by a guessed source class. Never encode confidence in the name: docs and `src/game/` cite Ghidra names, so upgrading one would mean a rename everywhere. If the role can't be described, the function stays `FUN_` with a plate note.
- **Provenance tag** (Ghidra function tag, exactly one):
  - `name:anchored`: the name comes from the binary itself, i.e. a `"Class::Method"` debug/profiling/assert string, a Lua binding-table entry, or an embedded symbol;
  - `name:inferred`: the role was read from the decompile and is consistent with the function's callers and data;
  - `name:guess`: plausible from context, not read closely.
  - Add `verified-live` on top when a live run confirmed the behaviour.
  - Tags are stored and versioned with the program and readable by scripts (`Function.getTags()`, ReVa `function-tags`). The decompiler doesn't show them, so the plate carries the basis too.
- **Plate basis line:** `Name: <anchored|inferred|guess> <date> — <basis>`. For anchored names, quote the anchor string and its address. This replaces free-form markers like `[static sweep …, not deep-verified]`.
- **Older names:** the 226 user-named functions from before 2026-11-09 are untagged, so "no tag" means "named before the convention", not "no basis". The only ones backfilled are those strictly provable: an exact `Class::Method` string match (`PgPrimitive_Reset`, `PgPrimitive_SubmitToGPU`). Tag the others when they're next touched.
- Names are always `SourceType.USER_DEFINED`. Ghidra's own analysis can overwrite `ANALYSIS` names, and they look identical to its auto-names.

## Scripting (`run-script`)

- Needs Ghidra started through PyGhidra (`pyghidra /opt/ghidra`). Script-object field names (e.g. `state`) cannot be reused as Python globals.
- `listing.getDataAt` returns an `undefined` placeholder for undefined bytes — use `getDefinedDataAt`. `FunctionManager` has no `getFunctionBefore`: `fm.getFunctions(addr, False).next()`.
- Byte scans: `mem.getBytes(addr, buf, 0, n)` with `buf = JArray(JByte)(int(n))`; `AddressFactory.getAddress` takes a string. Assert a canary (`MZ` at `0x00400000`) first — a `try/except`-wrapped read silently turns a scan into "0 hits".
- Struct-offset uses (`[reg+0x3ea4]`): `find-constant-uses` silently truncates; walk `listing.getInstructions(True)` matching `'0x3ea4]' in str(ins)` (~50 s, disassembled code only).
- `write-script` saves to `~/ghidra_scripts`. Earlier project scripts are in git history (`git show 90f5205^:tools/ghidra/<name>`).

## Host disassembly

For code Ghidra lacks, or to cross-check it: `objdump -D -b binary -m i386 -M intel --adjust-vma=<va>` on bytes from `read-memory` or read straight from the exe (fixed base, no relocations: file offset = section raw offset + VA − section VA). capstone/`cstool` are installed on the host too (not inside PyGhidra). For SecuROM-mutated routines, `tools/securom_trace.py <exe> <va> [--listing]` does recursive descent through junk detours and `push ret; jmp f` / `push ret; push f; ret` calls, and prints the reach, the calls and the absolute refs ([securom_vm.md](securom_vm.md)).

## Tool-call gotchas

- Argument names differ: `get-decompilation` `functionNameOrAddress` (+ `offset`/`limit`); `find-cross-references` `location`; `read-memory` `addressOrSymbol` + `length` (default 16); `apply-data-type` `dataTypeString`. `get-functions`/`get-symbols` have no query parameter.
- `search-decompilation` refuses on more than 1000 functions — use cross-references / constant uses.
- `get-data` is for data items; on code it returns "No data found".
- Xref `fromAddress` = start of the referencing instruction.
- Carrier logs print `module+0xoffset`: add `0x00400000` for the Ghidra VA.

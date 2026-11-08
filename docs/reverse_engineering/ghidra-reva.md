# Ghidra / ReVa notes

How to work with the Ghidra project safely. Locating/identifying code: [re_methodology.md](re_methodology.md).

## Project state

- `/Mercenaries2.exe` is under Ghidra version control; check-ins are restore points. `checkin-program` reports "Attempted to release domain object with unknown consumer" yet succeeds — confirm via `getDomainFile().getVersionHistory()`. Old versions open read-only with `getDomainFile().getReadOnlyDomainObject(consumer, version, monitor)` (release them afterwards) — useful to diff or recover lost work.
- The "Non-Returning Functions – Discovered" analyzer is disabled for this program. It once took a one-instruction `CALL [EnterCriticalSection]` at `0x0084af34` for a thunk, flagged it no-return (propagating to the API), and its "Repair Flow Damage" cleared the code after every call site; `Float_Sqrt` followed the same way. The listing was rebuilt from version history, but older notes may still call some code "undefined" or "without static callers" because of it.
- Undefined code or a missing caller is not by itself evidence of protection: check the raw bytes for a direct `E8 rel32` call before concluding the code is VM-only or mutated.
- Auto-analysis runs after each commit and can undo or re-derive a change within seconds (it clears fall-through overrides on CALL-overridden `jmp`s, [vm_thunk_patches.md](vm_thunk_patches.md)). Check results once it is idle (`AutoAnalysisManager.getAnalysisManager(prog).isAnalyzing()`).

## Editing

- Precise or bulk edits: `run-script` with its own transaction (`prog.startTransaction(msg)` … `prog.endTransaction(tx, ok)`; `ok=False` rolls back).
- Rename a function without touching its signature: `run-script` → `f.setName(name, SourceType.USER_DEFINED)`. (`create-label` + `setAsPrimary` can leave the label secondary on an already-named function; `set-function-prototype` rewrites the prototype, mangling register-arg `unaff_` decompiles, and rejects `__thiscall`.)
- `set-comment` replaces the whole comment of that type — read it first, resend the full text.
- `parse-c-structure` on an existing name replaces the struct in place; dependents keep working.

## Scripting (`run-script`)

- Needs Ghidra started through PyGhidra (`pyghidra /opt/ghidra`). Script-object field names (e.g. `state`) cannot be reused as Python globals.
- `listing.getDataAt` returns an `undefined` placeholder for undefined bytes — use `getDefinedDataAt`. `FunctionManager` has no `getFunctionBefore`: `fm.getFunctions(addr, False).next()`.
- Byte scans: `mem.getBytes(addr, buf, 0, n)` with `buf = JArray(JByte)(int(n))`; `AddressFactory.getAddress` takes a string. Assert a canary (`MZ` at `0x00400000`) first — a `try/except`-wrapped read silently turns a scan into "0 hits".
- Struct-offset uses (`[reg+0x3ea4]`): `find-constant-uses` silently truncates; walk `listing.getInstructions(True)` matching `'0x3ea4]' in str(ins)` (~50 s, disassembled code only).
- `write-script` saves to `~/ghidra_scripts`. Earlier project scripts are in git history (`git show 90f5205^:tools/ghidra/<name>`).

## Host disassembly

For code Ghidra lacks, or to cross-check it: `objdump -D -b binary -m i386 -M intel --adjust-vma=<va>` on bytes from `read-memory` or read straight from the exe (fixed base, no relocations: file offset = section raw offset + VA − section VA). capstone/`cstool` are installed on the host too (not inside PyGhidra).

## Tool-call gotchas

- Argument names differ: `get-decompilation` `functionNameOrAddress` (+ `offset`/`limit`); `find-cross-references` `location`; `read-memory` `addressOrSymbol` + `length` (default 16); `apply-data-type` `dataTypeString`. `get-functions`/`get-symbols` have no query parameter.
- `search-decompilation` refuses on more than 1000 functions — use cross-references / constant uses.
- `get-data` is for data items; on code it returns "No data found".
- Xref `fromAddress` = start of the referencing instruction.
- Carrier logs print `module+0xoffset`: add `0x00400000` for the Ghidra VA.

# Ghidra / ReVa MCP notes

- Renaming a function that already has a custom primary name: use `set-function-prototype` with the new name in the signature (`createIfNotExists: false`) as the FIRST call — `create-label` + `setAsPrimary: true` can silently leave the label secondary, and that leftover secondary label then blocks `set-function-prototype` with "symbol already exists at this address" (no MCP tool deletes labels; recovery requires deleting the label in the GUI and re-running `set-function-prototype`).
- `parse-c-structure` on an existing name replaces the struct in place (fields rebuilt, dependent structs/typed globals keep working) — the way to correct a wrong layout. `set-comment` replaces the whole comment of that type at the address, so re-read it first and resend the full text.
- `run-script` (Python) only works if Ghidra was launched through PyGhidra (`pyghidra /opt/ghidra`; plain `ghidra`/`ghidraRun` gives "Ghidra was not started with PyGhidra"). `write-script` saves into `~/ghidra_scripts`; keep project scripts in `tools/ghidra/` and delete the home copy. Scripts must open their own transaction (`currentProgram.startTransaction`). Whole-block reads: `getBytes` has no (Address,long) overload — use jpype `JArray(JByte)([0]*size)` as the buffer (2026-10-06, `~/ghidra_scripts/find_fov_region_callers.py`: scans .text for E8 rel32 calls into a VA range; it found ViewEntry_DeriveCullTask's entry — move to `tools/ghidra/` when convenient).
- Flow overrides: verify AFTER commit, not inside the transaction. Ghidra's post-commit analysis clears the fall-through override on a CALL-overridden `jmp` within ~1 s (a RET with CALL override keeps it) — see `securom_vm.md` item 4 and `tools/ghidra/VmThunkFlowFix.py`. Polling `Instruction.isFallThroughOverridden()` for a few seconds after commit is the check.
- `read-memory` takes `addressOrSymbol` (not `address`); `get-functions`/`get-symbols` have no `query` param; `get-undefined-function-candidates` takes no address range. Host disassembly of dumped bytes: `objdump -D -b binary -m i386 -M intel --adjust-vma=<va>` (see `vtables.md`).

## Tool-call gotchas (arg names differ between tools)

- `get-decompilation`: `functionNameOrAddress` (not `addressOrSymbol`); supports `offset`/`maxLines` against `totalLines`.
- `find-cross-references`: `location` (not `addressOrSymbol`).
- `create-label`: `addressOrSymbol` + `labelName` (+ `setAsPrimary: true` to rename a function entry without touching its prototype — preferred over `set-function-prototype`, which can mangle register-arg decompilation).
- `read-memory`: `length` param (16 bytes if omitted) — fetch chunks, then disassemble on the host: `objdump -D -b binary -m i386 -M intel --adjust-vma=<va>`.
- `apply-data-type`: `dataTypeString` (not `typeName`).
- `search-decompilation` over the whole program refuses when >1000 functions — use the cross-ref / constant-use tools instead.
- Reference-site `fromAddress` from xref tools = start of the referencing instruction; align host disassembly there.
- Ghidra VAs vs carrier-log caller offsets: the log prints `module+0xoffset` — add `0x00400000` before querying Ghidra (M2 audit initially queried the raw offset and got "not in any memory block").

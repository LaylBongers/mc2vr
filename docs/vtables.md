# Vtable Annotation

Goal: virtual calls decompile as `obj->vftable->Method()` instead of `(**(code **)(*(int *)ptr + 0xNN))()`. Worked example: `RenderShell_vtbl` (see Ghidra comments at `0x00be84c0`).

## Recipe

1. **Address-less vtables**: if the vtable is only known through an object pointer held in a typed global (e.g. runtime-installed, ctor unfound — see `Dx9StateWrapper`), you do NOT need the vtable address. Define the `_vtbl` struct with named members anyway (unknowns = `slotNN`), define the object struct `{ X_vtbl *vftable; }`, and put the object pointer at the right offset in the parent struct (e.g. `LtiRenderer` + 0x5bc). Type the parent global — decompiles then read `g_LtiRenderer->dx9State->vftable->SetRenderState(...)`. To MODIFY an existing minimal struct: `delete-structure` (force — references re-resolve by name) then re-create via `parse-c-header` with pad members. Finding the vtable address later is a bonus (applies the struct at the data, xrefs remaining slots).
2. **Extent**: scan slots until a non-pointer entry (garbage/string data). Check `vtable[-1]` for RTTI — always 0 in this binary (no Pangea RTTI; only Havok classes have it). Class names come from symbols/ctors (`RenderShell_Init` installs `RenderShell_vtable`).
3. **Define slots**: slots that read as "not a function" are undefined code — `create-function` them. Create functions for all slots before structuring.
4. **Structs** via one `parse-c-header` (category `/ClassName`):
   - `typedef void (*ClassName_hook)(void);` — parameterless, always. The parser rejects `__thiscall` and any fn-ptr params; params cause bogus `unaff_XX` register vars in decompiles.
   - `struct ClassName_vtbl { ClassName_hook slot00; ... };` — one member per slot, exact order. Semantic names (`PostUpdateHook`) where known, else `slotNN`.
   - `struct ClassName { ClassName_vtbl *vftable; };`
   - One struct per call — a header with multiple structs reports success but silently creates only one; re-call per struct. `validate-c-structure` is a free dry-run for parser syntax (accepted fn-ptr form: `typedef void __stdcall *name(args);`).
5. **Apply**:
   - vtable address → `apply-data-type` with `ClassName_vtbl` (`apply-structure` fails on pre-existing pointer data in the range).
   - object global → `ClassName`; pointer global → `ClassName *`.
6. **Verify**: re-decompile a consumer; ECX/`this` disappears from output by design — document the calling convention (`this` in ECX) in slot EOL comments instead.

Renaming functions: `set-function-prototype` also rejects `__thiscall` and changing the prototype can mangle decompilation of register-arg (`unaff_`) functions — prefer `create-label` with `setAsPrimary: true` at the entry to rename without touching the signature.

## Comments to leave

- Plate on the vtable: install chain (which ctor), slot count, no-RTTI note.
- EOL per interesting slot: semantics, callers, default target.

## ReVa tool-call gotchas (arg names differ between tools)

- `get-decompilation`: `functionNameOrAddress` (not `addressOrSymbol`); supports `offset`/`maxLines` against `totalLines`.
- `find-cross-references`: `location` (not `addressOrSymbol`).
- `create-label`: `addressOrSymbol` + `labelName` (+ `setAsPrimary: true` to rename a function entry without touching its prototype — preferred over `set-function-prototype`, which can mangle register-arg decompilation).
- `read-memory`: `length` param (16 bytes if omitted) — fetch chunks, then disassemble on the host: `objdump -D -b binary -m i386 -M intel --adjust-vma=<va>`.
- `apply-data-type`: `dataTypeString` (not `typeName`).
- `search-decompilation` over the whole program refuses when >1000 functions — use the cross-ref / constant-use tools instead.
- Reference-site `fromAddress` from xref tools = start of the referencing instruction; align host disassembly there.
- Ghidra VAs vs carrier-log caller offsets: the log prints `module+0xoffset` — add `0x00400000` before querying Ghidra (M2 audit initially queried the raw offset and got "not in any memory block").

# Vtable Annotation

Goal: virtual calls decompile as `obj->vftable->Method()` instead of `(**(code **)(*(int *)ptr + 0xNN))()`. Worked example: `RenderShell_vtbl` (see Ghidra comments at `0x00be84c0`).

## Recipe

1. **Extent**: scan slots until a non-pointer entry (garbage/string data). Check `vtable[-1]` for RTTI — always 0 in this binary (no Pangea RTTI; only Havok classes have it). Class names come from symbols/ctors (`RenderShell_Init` installs `RenderShell_vtable`).
2. **Define slots**: slots that read as "not a function" are undefined code — `create-function` them. Create functions for all slots before structuring.
3. **Structs** via one `parse-c-header` (category `/ClassName`):
   - `typedef void (*ClassName_hook)(void);` — parameterless, always. The parser rejects `__thiscall` and any fn-ptr params; params cause bogus `unaff_XX` register vars in decompiles.
   - `struct ClassName_vtbl { ClassName_hook slot00; ... };` — one member per slot, exact order. Semantic names (`PostUpdateHook`) where known, else `slotNN`.
   - `struct ClassName { ClassName_vtbl *vftable; };`
   - One struct per call — a header with multiple structs reports success but silently creates only one; re-call per struct. `validate-c-structure` is a free dry-run for parser syntax (accepted fn-ptr form: `typedef void __stdcall *name(args);`).
4. **Apply**:
   - vtable address → `apply-data-type` with `ClassName_vtbl` (`apply-structure` fails on pre-existing pointer data in the range).
   - object global → `ClassName`; pointer global → `ClassName *`.
5. **Verify**: re-decompile a consumer; ECX/`this` disappears from output by design — document the calling convention (`this` in ECX) in slot EOL comments instead.

Renaming functions: `set-function-prototype` also rejects `__thiscall` and changing the prototype can mangle decompilation of register-arg (`unaff_`) functions — prefer `create-label` with `setAsPrimary: true` at the entry to rename without touching the signature.

## Comments to leave

- Plate on the vtable: install chain (which ctor), slot count, no-RTTI note.
- EOL per interesting slot: semantics, callers, default target.

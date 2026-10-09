# Decompiler idioms to expect

Patterns when reading this binary's decompilation. Naming: [engine_naming.md](engine_naming.md).

- Custom register-arg conventions: `this` in ESI/ECX (ctors leak as `unaff_*`), packed EAX pairs (`in_EAX = {startReg, count}` in the Dx9 constant helpers), `unaff_EDI` record pointers.
- 10-byte thunk chains (jmp wrapper → SecuROM call gate → real body — [securom_vm.md](securom_vm.md)); pool allocators `FUN_0084ae70(size, n)` / `FUN_0084d9d0`.
- Global-ctor-built `.bss` statics: instance memory is zero in the file image (no static vtables) — find ctors by xref to the `.bss` address.
- Watch int* pointer arithmetic in reads: `*(ushort *)(p + 0x10)` on `int *p` is +0x40 bytes, not +0x10.

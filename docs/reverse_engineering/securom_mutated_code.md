# SecuROM-mutated plaintext & VM→plaintext callbacks

Found 2026-10-03 with the stub tracer ([vm_stub_callbacks.md](vm_stub_callbacks.md)); more examples from the 2026-10-06 E1/E1b watch runs. Boundary classification: [securom_vm.md](securom_vm.md).

The VM is not a closed box: native glue in `Stext` (e.g. `0x024f22b6`) calls plaintext functions directly during the render stub's call window, and ordinary-looking `.text` addresses can be SecuROM-MUTATED code:

- functions split into blocks that share one stack frame (`0x0050c0f0` prologue → `0x00504a95` thunk → body `0x0050c106`),
- `push ret; jmp target` call sequences (`0x0058f010`) that auto-analysis does not disassemble,
- junk bytes, jmp/call used as jumps.

Mutated plaintext IS statically analyzable (it is not bytecode): merge the blocks into one function, fix the push/jmp sites, re-decompile. A `.text` return address therefore does not prove "ordinary game code"; classify by behavior. Protected routines also call plaintext helpers all the time outside the stub (`.securom` callers of `PoseStore_GetPoseByHandle`, `Pose_Copy`, `PgMaterial_ctor`).

More examples (2026-10-06, E1/E1b watch runs):

- (a) The viewContext-record fill lives in a MUTATED, UNDEFINED `.text` block `~0x004671xx-0x004674xx` with no static callers — a static decompiler-text hunt for its writer returned nothing; `debug_watch=addr:` on the target found it in one run (decode the hit EIP's bytes by hand). **Check such regions by WATCHING, not by text search.**
- (b) A no-xref 11-byte thunk (`0x00506a26`) is the VM's only entry into `ViewContext_BuildCameraConstants` (`0x008591ac`) — plain thunks with zero static callers are the VM's call-gate signature ([draw_camera_chain.md](draw_camera_chain.md)).

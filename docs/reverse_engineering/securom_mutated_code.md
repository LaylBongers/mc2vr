# SecuROM-mutated plaintext & VM→plaintext callbacks

Found 2026-10-03 with the stub tracer ([vm_stub_callbacks.md](vm_stub_callbacks.md)); more examples from the 2026-10-06 E1/E1b watch runs. Boundary classification: [securom_vm.md](securom_vm.md).

Protected code is not a closed box: native code in `.securom` (e.g. `0x024f22b6`, inside the packet-processing routine `0x024F2170`) calls plaintext functions directly, and ordinary-looking `.text` addresses can be SecuROM-MUTATED code:

- functions split into blocks that share one stack frame (`0x0050c0f0` prologue → `0x00504a95` thunk → body `0x0050c106`),
- `push ret; jmp target` call sequences (`0x0058f010`) that auto-analysis does not disassemble,
- junk bytes, jmp/call used as jumps.

Mutated plaintext IS statically analyzable (it is not bytecode): merge the blocks into one function, fix the push/jmp sites, re-decompile. A `.text` return address therefore does not prove "ordinary game code"; classify by behavior. Protected routines also call plaintext helpers all the time outside the stub (`.securom` callers of `PoseStore_GetPoseByHandle`, `Pose_Copy`, `PgMaterial_ctor`).

Another example:

- A no-xref 11-byte thunk (`0x00506a26`) is the VM's only entry into `ViewContext_BuildCameraConstants` (`0x008591ac`) — plain thunks with zero static callers are the VM's call-gate signature ([draw_camera_chain.md](draw_camera_chain.md)).

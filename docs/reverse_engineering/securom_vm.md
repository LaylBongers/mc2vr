# SecuROM thunks and protected code

How to classify code near the SecuROM layer. On-disk state + loader stub: [securom_on_disk.md](securom_on_disk.md). Live tracer results: [vm_stub_callbacks.md](vm_stub_callbacks.md). Mod-side hooking rules: [../hooks.md](../hooks.md).

What looks like a SecuROM thunk is usually one of three things — classify it before treating anything as opaque:

1. **A forwarder you can follow.** Several shapes, all recoverable statically:
   - *Call gates*: a plaintext stub JMPs into gate code that continues at a plaintext body, usually adjacent. Example: `Dx9_SetPixelShaderConstantF` `0x0084f150` → gate `0x004f56e6` → core `0x0084f15a`.
   - *Runtime-patched slots*: a `jmp [slot]` whose file target is a `.securom` stub, rewritten by the loader to another address — native `.text` ([vm_thunk_patches.md](vm_thunk_patches.md)) or native code inside `.securom`.
   - *Mutated native code*, in `.text` or relocated into `.securom`: junk instructions, `push ret; jmp target` calls, split blocks ([securom_mutated_code.md](securom_mutated_code.md)), and small obfuscated value fetches (`0x01aa03a0`: `pusha`, call through a pointer, adds 0x28 if the trap flag is set — anti-single-step).
2. **VM-virtualized residue** — flow genuinely dissolves into bytecode dispatch. Read it in the live game rather than statically: the carrier's `debug_vm_dump` reads a thunk chain's runtime slots and targets. Candidate: the pose getter's mid-function `jmp [0x024cdae4]` at `0x0048bf06`, which stays unpatched at runtime.
3. **Not SecuROM at all** — a Ghidra coverage gap (code the listing lacks, e.g. after the no-return damage, [ghidra-reva.md](ghidra-reva.md)) or a misread address. Check the raw bytes for direct calls before assuming protection.

## Worked example: the render-packet processing

`InGameShellState_FramePipeline` calls `0x0050c660` at `0x004c99f9`, right after `RenderQueue_SubmitWorldPackets`. `0x0050c660` is `jmp [0x024d6940]`; the loader patches that slot to `0x024F2170` in `.securom` ([data/vm_thunks_runtime.csv](data/vm_thunks_runtime.csv)). Its entry is native x86, SecuROM-mutated: a normal prologue reading game globals, junk padding, `push ret; jmp` calls into `.text` (`FUN_0050c0f0` → `FUN_0050c106`) and value fetches through `0x01aa03a0`. Only the entry (~0x140 bytes) has been read; whether later parts enter real VM bytecode is unmapped. This is the step that turns the staged packets into draw records, materials and command streams ([frame_chain.md](frame_chain.md)).

`0x0050f660`, long recorded as this "interpreter stub", is unrelated: a mutated copy of Lua's `lua_insert` (calls `index2adr` `0x0085d760`, recognizable by the `-10000/-10001/-10002` pseudo-indices), and the runtime target of thunk `0x005a0360`.

## Rules of thumb

- **Code opacity ≠ data opacity**: whatever the producer is, its inputs (staged elements) and outputs (typed record/table structures) are plaintext data — type/name them and work from the plaintext side.
- A protected-code-filled structure that "looks like" scene data must be verified via its ctor/string trail before naming (the table at `0x00ff36f4` — now `g_MaterialTable`, [render_data_model.md](render_data_model.md) — was first mislabeled `g_CameraTable`/`CameraEntry`).

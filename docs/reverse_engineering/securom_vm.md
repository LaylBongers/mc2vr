# SecuROM thunks and protected code

How to classify code near the SecuROM layer. On-disk state + loader stub: [securom_on_disk.md](securom_on_disk.md). Live tracer results: [vm_stub_callbacks.md](vm_stub_callbacks.md). Mod-side hooking rules: [../hooks.md](../hooks.md).

What looks like a SecuROM thunk is usually one of three things — classify it before treating anything as opaque:

1. **A forwarder you can follow.** Several shapes, all recoverable statically:
   - *Call gates*: a plaintext stub JMPs into gate code that continues at a plaintext body, usually adjacent. Example: `Dx9_SetPixelShaderConstantF` `0x0084f150` → gate `0x004f56e6` → core `0x0084f15a`.
   - *Runtime-patched slots*: a `jmp [slot]` whose file target is a `.securom` stub, rewritten by the loader to another address — native `.text` ([vm_thunk_patches.md](vm_thunk_patches.md)) or native code inside `.securom`.
   - *Mutated native code*, in `.text` or relocated into `.securom`: junk instructions, `push ret; jmp target` calls, split blocks ([securom_mutated_code.md](securom_mutated_code.md)), and small obfuscated value fetches (`0x01aa03a0`: `pusha`, call through a pointer, adds 0x28 if the trap flag is set — anti-single-step).
2. **VM-virtualized residue** — flow genuinely dissolves into bytecode dispatch. Read it in the live game rather than statically: the carrier's `debug_vm_dump` reads a thunk chain's runtime slots and targets. Candidate: the pose getter's mid-function `jmp [0x024cdae4]` at `0x0048bf06`, which stays unpatched at runtime.
3. **Not SecuROM at all** — a Ghidra coverage gap (code the listing lacks, e.g. after the no-return damage, [ghidra-reva.md](ghidra-reva.md)) or a misread address. Check the raw bytes for direct calls before assuming protection.

## Worked example: thunk `0x0050c660` (not the render-packet processing)

`InGameShellState_FramePipeline` calls `0x0050c660` at `0x004c99f9`, right after `RenderQueue_SubmitWorldPackets`. `0x0050c660` is `jmp [0x024d6940]`; the loader patches that slot to `0x024F2170` in `.securom` ([data/vm_thunks_runtime.csv](data/vm_thunks_runtime.csv)). It was long assumed to be the step that turns the staged packets into draw records. **Verdict (2026-11-08, fully mapped): it isn't.** It is `HandleEvents_DispatchHiNibble`, the deobfuscated pseudocode is on its Ghidra plate.

- **Size:** one native routine, `0x024f2170`–`0x024f22e9`, 119 real instructions. It has no VM bytecode; the only out-of-line block is a junk counter detour at `0x0059f323`. Recursive descent with `tools/securom_trace.py` covers it completely.
- **What it does:** it loops over `g_HandleEventQueue` (`0x00dd4770`: a `CRITICAL_SECTION` + count + 16-byte `{handle, state bytes}` entries) and pops each event via `HandleEventQueue_PopNext` (`0x00518890`). When the high nibbles of the two state bytes differ, it calls one of three handle-tree walkers on the object `0x00d6bb24`: `FUN_0050bcb0`, `FUN_0050c0f0` (→ body `FUN_0050c106`) or `FUN_0050b8b0`.
- **Two twins listen to the same queue on the low nibbles:** plaintext `FUN_004f8650` (FramePipeline `0x004c993f`) and mutated `FUN_0064a150`. Diffing against them gave the obfuscated loop step (= 1).
- **Render state:** it touches none of `g_RenderQueue`/`g_RenderQueue2`, the frame-ctx block, `PgPrimitive` records, `g_MaterialTable` or `g_RenderShell`. Neither does any function in the 76-function `.text` callee closure. It runs on the update thread, so it can't be the render worker's ~2.6 ms of prep ([../pacing.md](../pacing.md)).
- **Junk shapes:** `pushf … popf` brackets around a constant computed from a `SecuROM_ValueFetch_AntiTrace` result; `shl/shr r,0`, `mov esi,esi`, `lea ebp,[ebp]`, `nop`; and `dec [ctr]; je detour`, where the detour re-arms the counter and jumps back. Calls appear as `push ret; jmp f` and as `push ret; push f; ret`. Ret-target landing pads are a single `jmp`.
- The real scene → draw-record step is plaintext task code: [scene_record_build.md](scene_record_build.md). The `g_RenderQueue`/`g_RenderQueue2` consumer is still OPEN ([view_table.md](view_table.md)).

`0x0050f660`, long recorded as this "interpreter stub", is unrelated: a mutated copy of Lua's `lua_insert` (calls `index2adr` `0x0085d760`, recognizable by the `-10000/-10001/-10002` pseudo-indices), and the runtime target of thunk `0x005a0360`.

## Rules of thumb

- **Code opacity ≠ data opacity**: whatever the producer is, its inputs (staged elements) and outputs (typed record/table structures) are plaintext data — type/name them and work from the plaintext side.
- A protected-code-filled structure that "looks like" scene data must be verified via its ctor/string trail before naming (the table at `0x00ff36f4` — now `g_MaterialTable`, [render_data_model.md](render_data_model.md) — was first mislabeled `g_CameraTable`/`CameraEntry`).

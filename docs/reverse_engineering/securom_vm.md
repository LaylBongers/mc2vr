# SecuROM/VM boundary

How to classify code near the SecuROM layer. On-disk state + loader stub: [securom_on_disk.md](securom_on_disk.md). Live tracer results: [vm_stub_callbacks.md](vm_stub_callbacks.md). Mod-side hooking rules: [../hooks.md](../hooks.md).

Two mechanisms, do not conflate:

1. **SecuROM call gates** — NOT opaque. A plaintext stub JMPs into injected gate code, which continues at a PLAINTEXT body (usually adjacent to the stub). Fully recoverable statically; recover the convention from the two plaintext ends. Never analyze or hook the gate itself. Proven example: `Dx9_SetPixelShaderConstantF` `0x0084f150` → gate `0x004f56e6` → core `0x0084f15a` (full calling convention recovered; Ghidra follows the flow through the gate).
2. **VM-virtualized functions** — genuinely opaque: flow dissolves into bytecode dispatch with no plaintext successor. Examples: the render-packet interpreter (stub `0x0050f660`, called at `0x004c99f9`; fills the `PgPrimitive` records, `PgMaterial` rows, technique/pass tables, command streams) and its hidden callees, the pose getter `0x0048bf00` (its mid-function `jmp [0x024cdae4]` at `0x0048bf06` stays unpatched at runtime). NOT `GetD3DDevice` — that was wrong, see [vm_thunk_patches.md](vm_thunk_patches.md).

Two further phenomena blur the boundary:

3. **VM → plaintext callbacks and SecuROM-mutated plaintext** — the VM is not a closed box, and ordinary-looking `.text` can be mutated: [securom_mutated_code.md](securom_mutated_code.md).
4. **Runtime-patched thunk slots** — many `jmp [slot]` thunks whose file target is a `.securom` stub are native `.text` at runtime: [vm_thunk_patches.md](vm_thunk_patches.md).

(The old "Begin/EndScene + Present live below this in SecuROM-encrypted thunks" note is FALSIFIED — they are plaintext `LtiRenderer_*` functions: [frame_chain.md](frame_chain.md).)

## Rules of thumb

- **Follow-the-flow test** before writing a call off: Ghidra resolves a successor (especially a plaintext continuation adjacent to the stub) = gate; stub into bytecode with no readable successor = virtualized.
- **Code opacity ≠ data opacity**: even for virtualized producers, their inputs (staged elements) and outputs (typed record/table structures) are plaintext data — type/name the consumers, hook plaintext neighbors, never the gate/VM region.
- A VM-filled structure that "looks like" scene data must be verified via its ctor/string trail before naming (the table at `0x00ff36f4` — now `g_MaterialTable`, [render_data_model.md](render_data_model.md) — was first mislabeled `g_CameraTable`/`CameraEntry`).

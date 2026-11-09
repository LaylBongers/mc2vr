# Draw-record submission (`PgPrimitive_SubmitToGPU`)

`PgPrimitive_SubmitToGPU` (`0x00855690`) walks the frame's sorted draw-record list and issues every record's state and draw. The structure and per-address facts are on the Ghidra plate. The executable form is the reimplementation, `src/carrier/reimpl/render/submit_to_gpu.cpp`, which follows the original block by block and cites VA ranges ([../reimpl.md](../reimpl.md)). Context: [frame_chain.md](frame_chain.md), [render_cmd.md](render_cmd.md), [view_context_records.md](view_context_records.md).

- **Not SecuROM-mutated.** The whole range `0x00855690..0x0085647e` is plain compiler output. Mutation only shows up in callees: `Lti_LazyNameHash`, the split blocks of `Dx9_SetPixelShaderConstantF`, and `Lti_DebugPrintf`'s patched slot.
- **Re-entrant between frames.** Each call resets its own draw-state caches (`RenderShell_ResetDrawStateCache`, block `0x01169788`), so a second call re-submits the identical frame. The stereo replay relies on that ([../stereo.md](../stereo.md)).
- **The dirty mask is computed after `RenderShell_SetViewType`,** which can reset the caches it compares against. Pass selection keys off the raw mask (screen | material), not the 0x7f that a technique or pass change forces.
- **Callee conventions:** most callees take register arguments: EAX/ECX/EDX/EDI/ESI, AL for `RenderShell_PickPass_1_3_7`. They are declared in `src/game/render_shell.hpp` / `dx9.hpp` / `lti.hpp`. A read-before-write scan of each callee found no hidden register inputs.
- **Float math** is scalar SSE (`mulss`/`addss`). `reimpl/` is compiled with `-mfpmath=sse`, because the i686 default x87 would round `a*b+c` differently.
- **Hooking inside it:** five call instructions return onto a branch target (`0x855bb7`, `0x855bf6`, `0x855e15`, `0x856302`, `0x85634e`), so a 5-byte patch there corrupts the function. An earlier instruction with the arguments still in registers works instead.

## Verification

Verified against the original by paired passes ([../reimpl.md](../reimpl.md#verifying-a-new-reimplementation)): two original passes are identical in gameplay, and ours matched the original's call sequence in every pass. A dry run can't check this function, because its callees change state it reads straight afterwards.

## OPEN

- The roles of `DAT_01163764`, `DAT_011697c8`, `DAT_0116376c`, `DAT_011697c0`, `DAT_00ff36fc` (zeroed per submit) and of the two named textures (hash keys `0x5e84ea6d` / `0x16085a8d`, high dword `0xf011157a`).
- `RenderShell_SetViewType` internals (two jump tables, `0x856984` / `0x8569a4`).

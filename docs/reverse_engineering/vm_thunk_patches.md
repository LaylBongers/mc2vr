# Runtime-patched VM thunk slots

Found 2026-10-03 (carrier `debug_vm_dump=on`, `src/carrier/debug/vm_dump.cpp`). Boundary: [securom_vm.md](securom_vm.md).

The file image of a `jmp [slot]` thunk names a `.securom` VM stub, but the SecuROM loader rewrites the slot at startup. Static analysis of the file therefore shows the DEFAULT target; the live target can differ.

## Census

(~15 s after attach; 2448 thunks = every `FF 25 <slot in 0x01a48000..0x03771f0f>` in `.text`; per-thunk data in [data/vm_thunks_runtime.csv](data/vm_thunks_runtime.csv)):

- 1887 unpatched (still VM stubs),
- 155 patched to another address still in the SecuROM range (target role unknown),
- **406 patched into `.text`** (distinct, native, SecuROM-mutated plaintext: junk-counter prologue `lea ebp/eax,[X]; dec [..]; je`, `push ret; jmp` call sites).

Proven cases: `GetD3DDevice` `0x0047f2f0` → `GetD3DDevice_Impl` `0x00403160` (`return g_LtiRenderer ? g_LtiRenderer->dx9State : NULL`); `RenderTask_RenderFrame`'s follow-up thunk `0x0046ab80` → `NodeArray_DecrementChildRefs` `0x00518fa0`. Not covered by the census: non-slot stubs like `0x0050f660` (`push esi; ...; push 0x50f677; jmp 0x0085d760`, target is `.text`, untraced). Unaligned thunks are real too (mid-function VM calls, e.g. `0x0048bf06`).

## Rules

(a) A slot's runtime value is not in the file — read it live (carrier `debug_vm_dump`), do not conclude "opaque" from the `.securom` target.

(b) The 406 targets are mostly NOT disassembled in Ghidra: create the function at the target; mutated prologues decompile with a junk `DAT_0256xxxx` counter, `push X; jmp [IAT]` / `push X; push Y; ret` call sites stop the decompiler — resolve those by hand and check with `objdump`. DONE 2026-10-03: functions created at 396 of the 406 targets (9 already existed, 1 lands mid-instruction at `0x0040e586`), default `FUN_` names, bookmark category `Analysis`/`VMThunkTarget` on each (lists the thunk(s) that resolve there). Median body is only ~21 bytes: many targets are short mutated trampolines whose real body is a jump/call away (e.g. `0x004cc7a6` → `FUN_007f321b`), i.e. call-gate-like; 105 have multi-range bodies; four bodies exceed 2 KB because of far side blocks. Unaligned-target ones may be split-function blocks rather than whole functions — check before trusting a body as complete.

(c) The junk counters (`dec [ebp+off]; je` that reloads a constant and jumps back) are harmless noise.

## Flow modeling (`tools/ghidra/VmThunkFlowFix.py`, 2026-10-03)

The obfuscated call idioms hide the callee and return point from Ghidra, so decompiles stop at the first one.

- `push ret; push callee; RET` (P2) CAN be modeled: RET flow override CALL + fall-through override to `ret` + a CALL ref to `callee` persists and gives a correct decompile (93 sites, ~65 functions; `RET` + CALL override).
- `push ret; jmp X` (P1) CANNOT: the fall-through override on a CALL-overridden `jmp` is cleared by Ghidra within ~1 s of commit, leaving the function falling into junk (garbage decompile) — the first apply run did this and was reverted; always health-check AFTER commit, never only inside the transaction ([ghidra-reva.md](ghidra-reva.md)). P1 functions keep a truncated decompile; read them from the listing / `objdump`.
- Ghidra pre-marks many `jmp <function>` as `CALL_RETURN` (tail call, 294 here), which also drops the return point of a `push ret; jmp` idiom.

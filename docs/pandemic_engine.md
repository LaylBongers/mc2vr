# Pandemic Engine — Cross-Cutting Notes

Terse engine-wide knowledge for working with the decompilation. Per-address facts (layouts,
addresses, slot maps) live in Ghidra plates; render specifics in `render_path.md`, stereo/S2 in
`stereo_design.md`, vtable recipe in `vtables.md`.

## Naming layers

- `Pg*` = engine/render core (PgPrimitive, PgMaterial, Pg*.sho shaders). `Lti*` = platform layer
  (LtiRenderer, Lti_DebugPrintf, Dx9_* helpers). Source roots in debug strings:
  `D:\projects\Mercs2_PC\LTI\Src\...`, `D:\projects\Mercs2_PC\Odin\Win32\...` ("Odin" = this game's project).

## Fastest identification oracles (use in this order)

- **String search first.** `"Class::Method"` strings + source `.cpp` paths identify classes outright
  (e.g. the `PgPrimitive::*` method strings + `PgPrimitiveWin32.cpp` named the whole primitive
  system). A method string is usually referenced by the very function it names, often only lazily
  hashed at entry.
- Debug/assert strings name things (`RenderSystem_Init` via its thread assert, BeginSubmit/EndSubmit
  via their error prints).
- No MSVC RTTI — derive classes from ctor vtable writes + strings; annotate vtables pre-emptively.

## Name hashing (two distinct mechanisms — don't confuse them)

- `Lti_LazyNameHash` (0x008244a0, ~139 callers): per-site FNV-1a hash of `"Class::Method"` cached in
  `.bss`, read only by VM'd code — inert telemetry, not feature/device checks.
- Inline FNV-1a for **object names** (e.g. `PgMaterial::nameHash64`): basis `0x811c9dc5`, prime
  `0x1000193`, case-insensitive (`c | 0x20`), NUL terminator hashed as `0x2a`.

## Render data model (consume side, all plaintext; layouts on Ghidra plates)

- **`PgPrimitive`** (0x58): per-draw submit record — draw params, VS, technique, stencil, and table
  INDICES (`materialIdx/viewIdx/envIdx/viewContextIdx/screenIdx`). Singly-linked submit list
  (base/head/next-table in the `g_PrimitiveBase` plate; next entries are {6-byte sort key,
  ushort next}, `0xffff` terminates). Lifecycle: Reset → VM'd build/AssignKeys →
  `PgPrimitive_SortList` → SubmitToGPU (`0x00855690`, formerly mislabeled RenderShell_RenderFrame).
- **`PgMaterial`** (0x190): NAMED material ("OcclusionMaterial", "PgPrimitiveSubmitToGPU") —
  texture-projection transform `rows[6][4]` × view scale/offset goes to **pixel**-shader constants
  (texgen). Rule of thumb: PS-constant transform + textures + blend flags = material, not camera.
- Shaders: `.sho` files via `PgShader_Register(name, file, variant)`; `_pl/_sl/_pl_sl` screen-effect
  variants + `_li` fallbacks, gated by `g_ScreenEffectsEnabled`.
- Per-draw state is dirty-check cached with `0xffff/0xff` sentinel invalidation; caches are
  caller-side (Dx9_* layer), not inside the device wrapper.

## SecuROM/VM boundary — two mechanisms, do not conflate

1. **SecuROM call gates** — NOT opaque. A plaintext stub JMPs into injected gate code, which
   continues at a PLAINTEXT body (usually adjacent to the stub). Fully recoverable statically;
   recover the convention from the two plaintext ends. Never analyze or hook the gate itself.
   Proven example: `Dx9_SetPixelShaderConstantF` `0x0084f150` → gate `0x004f56e6` → core
   `0x0084f15a` (full calling convention recovered; Ghidra follows the flow through the gate).
   Same pattern as the earlier falsification of "Begin/EndScene live in SecuROM-encrypted thunks"
   (they were plaintext `LtiRenderer_*` functions).
2. **VM-virtualized functions** — genuinely opaque: flow dissolves into bytecode dispatch with no
   plaintext successor. Examples: the packet interpreter (stub `0x0050f660`, fills the `PgPrimitive`
   records, `PgMaterial` rows, technique/pass tables, command streams) and its hidden callees,
   the pose-getter `0x0048bf00` (its mid-function `jmp [0x024cdae4]` at `0x0048bf06` stays unpatched
   at runtime). NOT `GetD3DDevice` — that was wrong, see item 4.

3. **VM -> plaintext callbacks and SecuROM-mutated plaintext** (found 2026-10-03 with the stub
   tracer, `docs/render_path.md` § VM stub callbacks). The VM is not a closed box: native glue in
   `Stext` (e.g. `0x024f22b6`) calls plaintext functions directly during the render stub's call
   window, and ordinary-looking `.text` addresses can be SecuROM-MUTATED code — functions split
   into blocks that share one stack frame (`0x0050c0f0` prologue -> `0x00504a95` thunk -> body
   `0x0050c106`), `push ret; jmp target` call sequences (`0x0058f010`) that auto-analysis does not
   disassemble, junk bytes, jmp/call used as jumps. Mutated plaintext IS statically analyzable
   (it is not bytecode): merge the blocks into one function, fix the push/jmp sites, re-decompile.
   A `.text` return address therefore does not prove "ordinary game code"; classify by behavior.
   Protected routines also call plaintext helpers all the time outside the stub (`.securom`
   callers of `PoseStore_GetPoseByHandle`, `Pose_Copy`, `PgMaterial_ctor`).

4. **Runtime-patched thunk slots — many "VM" thunks are native at runtime** (found 2026-10-03,
   carrier `debug_vm_dump=on`, `src/carrier/debug/vm_dump.cpp`). The file image of a `jmp [slot]` thunk names a
   `.securom` VM stub, but the SecuROM loader rewrites the slot at startup. Static analysis of the file
   therefore shows the DEFAULT target; the live target can differ. Census (~15s after attach, 2448
   thunks = every `FF 25 <slot in 0x01a48000..0x03771f0f>` in `.text`; per-thunk data in
   `docs/data/vm_thunks_runtime.csv`): 1887 unpatched (still VM stubs), 155 patched to another
   address still in the SecuROM range (target role unknown), **406 patched into `.text`** (distinct,
   native, SecuROM-mutated plaintext: junk-counter prologue `lea ebp/eax,[X]; dec [..]; je`, `push ret;
   jmp` call sites). Proven cases: `GetD3DDevice` `0x0047f2f0` -> `GetD3DDevice_Impl` `0x00403160`
   (`return g_LtiRenderer ? g_LtiRenderer->dx9State : NULL`); `RenderTask_RenderFrame`'s follow-up
   thunk `0x0046ab80` -> `NodeArray_DecrementChildRefs` `0x00518fa0`. Not covered by the census:
   non-slot stubs like `0x0050f660` (`push esi; ...; push 0x50f677; jmp 0x0085d760`, target is `.text`,
   untraced). Unaligned thunks are real too (mid-function VM calls, e.g. `0x0048bf06`).
   Rules: (a) a slot's runtime value is not in the file — read it live (carrier `debug_vm_dump`), do not
   conclude "opaque" from the `.securom` target; (b) the 406 targets are mostly NOT disassembled in
   Ghidra yet (create the function at the target; mutated prologues decompile with a junk
   `DAT_0256xxxx` counter, `push X; jmp [IAT]` / `push X; push Y; ret` call sites stop the decompiler,
   resolve those by hand and check with `objdump`) — DONE 2026-10-03: functions created at 396 of the
   406 targets (9 already existed, 1 lands mid-instruction at `0x0040e586`), default `FUN_` names,
   bookmark category `Analysis`/`VMThunkTarget` on each (lists the thunk(s) that resolve there). Median
   body is only ~21 bytes: many targets are short mutated trampolines whose real body is a jump/call
   away (e.g. `0x004cc7a6` -> `FUN_007f321b`), i.e. call-gate-like; 105 have multi-range bodies;
   four bodies exceed 2 KB because of far side blocks.
   Flow modeling (`tools/ghidra/VmThunkFlowFix.py`, 2026-10-03): the obfuscated call idioms hide the
   callee and return point from Ghidra, so decompiles stop at the first one. `push ret; push callee;
   RET` (P2) CAN be modeled: RET flow override CALL + fall-through override to `ret` + a CALL ref to
   `callee` persists and gives a correct decompile (93 sites, ~65 functions; `RET` + CALL override).
   `push ret; jmp X` (P1) CANNOT: the fall-through override on a CALL-overridden `jmp` is cleared by
   Ghidra within ~1 s of commit, leaving the function falling into junk (garbage decompile) — the
   first apply run did this and was reverted; always health-check AFTER commit, never only inside
   the transaction. P1 functions keep a truncated decompile; read them from the listing / `objdump`.
   Also: Ghidra pre-marks many `jmp <function>` as `CALL_RETURN` (tail call, 294 here), which also
   drops the return point of a `push ret; jmp` idiom Unaligned-target ones may be split-function
   blocks rather than whole functions — check before trusting a body as complete; (c) the junk counters (`dec [ebp+off]; je` that
   reloads a constant and jumps back) are harmless noise.

Rules of thumb:
- **Follow-the-flow test** before writing a call off: Ghidra resolves a successor (especially a
  plaintext continuation adjacent to the stub) = gate; stub into bytecode with no readable
  successor = virtualized.
- **Code opacity ≠ data opacity**: even for virtualized producers, their inputs (staged elements)
  and outputs (typed record/table structures) are plaintext data — type/name the consumers, hook
  plaintext neighbors, never the gate/VM region.
- A VM-filled structure that "looks like" scene data must be verified via its ctor/string trail
  before naming (`g_CameraTable` was really the material table).

## Code patterns to expect when reading the decompilation

- Custom register-arg conventions: `this` in ESI/ECX (ctors leak as `unaff_*`), packed EAX pairs
  (`in_EAX = {startReg, count}` in the Dx9 constant helpers), `unaff_EDI` record pointers.
- 10-byte thunk chains (jmp wrapper → SecuROM call gate → real body — see § SecuROM/VM boundary);
  pool allocators `FUN_0084ae70(size, n)` / `FUN_0084d9d0`.
- Global-ctor-built `.bss` statics: instance memory is zero in the file image (no static vtables) —
  find ctors by xref to the `.bss` address.
- Watch int* pointer arithmetic in reads: `*(ushort *)(p + 0x10)` on `int *p` is +0x40 bytes, not
  +0x10.

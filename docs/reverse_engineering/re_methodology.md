# RE methodology: locating & identifying code

How functions/systems in this binary were found and named. Identification oracles first; then the render-path location anchors as a worked example. Tooling gotchas: [ghidra-reva.md](ghidra-reva.md).

## Identification oracles (use in this order)

- **String search first.** `"Class::Method"` strings + source `.cpp` paths identify classes outright (e.g. the `PgPrimitive::*` method strings + `PgPrimitiveWin32.cpp` named the whole primitive system). A method string is usually referenced by the very function it names, often only lazily hashed at entry.
- Debug/assert strings name things (`RenderSystem_Init` via its wrong-thread assert, `LtiRenderer_BeginSubmit`/`EndSubmit` via "BeginSubmit() called when already in a scene!").
- No MSVC RTTI — derive classes from ctor vtable writes + strings; annotate vtables pre-emptively ([vtables.md](vtables.md)).

## Render-path location anchors (worked example)

- No `EndScene`/`Present` imports; only `Direct3DCreate9` — D3D9 fully dynamic, every device call vtable-indirect. Import-xref tracing useless; worked from the state stack down and `Direct3DCreate9`'s single caller up.
- Debug-string anchors (source paths like `"D:\projects\Mercs2_PC\LTI\Src\..."`) remain the fastest way to name LTI/Pangea functions.
- Stack registration found by xrefs writing `g_GameStateStack`; top state `[4]`'s update slot leads to the frame pipeline.
- Render consumer found from xrefs to the `GetD3DDevice` thunk (`0x0047f2f0`, 12 refs); one caller sits in the RenderShell frame path.
- Device-lost branch (`DAT_01174a94 == 1`) gave the RenderShell slot semantics: `slot01` invalidate → `slot03` timed render → `slot02` restore.

## Runtime caller-attribution trick (carrier)

`__builtin_return_address(0)` + module lookup in hook bursts, or a MidHook at entry reading `ECX`/`[ESP]` for thiscall/virtual sites.

**Diagram:** [../render_diagram.svg](../render_diagram.svg) — frame flow, the opaque VM stub, and the mod's hook points.

## When static search fails

SecuROM-mutated regions have no defined functions and no static callers — decompiler-text hunts return nothing. Probe with `debug_watch=addr:` on the target address, watch the EIP + surrounding bytes, then decode by hand ([securom_mutated_code.md](securom_mutated_code.md); proven: the record-fill writer, [draw_camera_chain.md](draw_camera_chain.md)).

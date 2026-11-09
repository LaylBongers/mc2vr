# Incremental reimplementation (`src/reimpl/`)

Parts of the engine are rewritten as our own C++ and replace the originals, so we can change them. We never decompile everything: we reimplement what we need to change, or where a hook around it has become fragile, and that can be a single function or a whole subsystem. The decompilation is LLM-assisted, from Ghidra output plus disassembly. Mechanism rules: [hooks.md](hooks.md).

## Where the pieces live

Directory rules: [code_structure.md](code_structure.md).

- **`src/game/`:** what the game *is*. Ghidra is the source of truth; change both together.
  - Typed data: structs with `static_assert`s on their offsets (`pg_primitive.hpp`), stream formats (`render_cmd.hpp`'s `rcmd::` commands), and fixed-address globals as `Global<T, VA>` under their Ghidra names.
  - Original functions as typed callables, `Function<R(Args...)>` / `Method<...>` (`function.hpp`): the Ghidra name, the VA, and each parameter's location (`EAX`, `ESI`, `XMM0`, `STK`, ...). A call reads like C++: `Dx9_SetRenderState(D3DRS_CLIPPLANEENABLE, mask)`. One i386 shim (`function.cpp`) loads the registers, pushes the stack arguments and restores ESP afterwards, so callee-pop works too. Only the inputs a callee actually reads are declared.
- **`src/carrier/reimpl/<subsystem>/`:** the reimplementations. Each reads like the original, handler by handler, through `game::` types and functions. It calls `vr/` code directly where it implements VR behavior. Each header records the original VA, the status, what it calls, and which globals it writes. The Ghidra plate notes the reimplementation.
- **`src/carrier/hooks/reimpl.cpp`:** the replacement list, one line per reimplementation (name, the original's code range, our function). `hooks/site.hpp` refuses any other hook inside a replaced original's code. Such a hook would silently stop firing; it has to become a direct call in the reimplementation.
- **`src/carrier/debug/shadow/`:** optional verification. `harness.hpp` is generic. There is one spec per function (`render_cmd.cpp`): callee captures, the globals it writes, and how to call it.

## Replacement and shadow verification

Reimplementations are simply installed: an inline hook on the original entry (`hooks/reimpl.cpp`). There are no per-function switches. When a replacement misbehaves, `git` bisects it like any other change.

Inside reimplemented code, calls to other reimplemented functions are plain C++ calls. `game::` callables are only for code that is still original. A cdecl original whose caller relies only on callee-saved registers is replaced by our function directly; `ExecuteStream` is one. Any other convention needs an entry thunk generated from its `Function` declaration (not written yet).

Shadow verification (`debug_reimpl_shadow=on`) is a debugging aid for small, tricky functions where a mistake is invisible on screen. It doesn't scale to subsystems, and it is optional. With it on, the originals run, and ours are dry-run alongside and compared:
- A per-thread `CallInterceptor` records our `game::` calls without executing them.
- The globals we write are snapshotted before the dry run and restored after it.
- MidHooks capture the original's callee calls. The two call sequences and end states are compared at the caller's return site. Opaque arguments (an HRESULT, the device pointer) are not compared.
- Divergences are logged, along with a summary every 10 s.

Larger subsystems are verified at their outer boundary instead, e.g. by diffing the D3D9 call stream.

## Status

- `RenderCmd_ExecuteStream`: replaces the original (2026-11-08). The shadow check in gameplay is pending ([reverse_engineering/render_cmd.md](reverse_engineering/render_cmd.md)).
- Next: `PgPrimitive_SubmitToGPU` ([reverse_engineering/frame_chain.md](reverse_engineering/frame_chain.md)). The aim is to fold the stereo replay, the per-eye camera upload gate and the replay drain skip into explicit extension points, and to make a HUD render-target split possible ([reverse_engineering/gui_renderer.md](reverse_engineering/gui_renderer.md)).

## Tests

We only test what nothing else verifies. A test whose expected values come from the same reading of the disassembly as the code just repeats it. Faithfulness is checked against the original itself, in the game.

- `tools/test/test_call_shim.cpp` (`mc2vr_call_shim_test`, i386, run under Wine) covers the hand-written asm call shim. A shim bug in the game would show up as a crash far from its cause. Build and run: `cmake --build build/win32 --target mc2vr_call_shim_test && wine build/win32/bin/mc2vr_call_shim_test.exe`.

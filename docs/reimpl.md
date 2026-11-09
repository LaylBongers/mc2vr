# Incremental reimplementation (`src/carrier/reimpl/`)

Parts of the engine are rewritten as our own C++ and replace the originals, so they can be changed directly. The engine isn't decompiled wholesale. What gets reimplemented is what needs changing, or what a hook has become fragile around: a single function or a whole subsystem. The decompilation is LLM-assisted, from Ghidra output plus disassembly. Mechanism rules: [hooks.md](hooks.md).

## Where the pieces live

Directory rules: [code_structure.md](code_structure.md).

- **`src/game/`:** what the game *is*. Ghidra is the source of truth; change both together.
  - Typed data: structs with `static_assert`s on their offsets (`pg_primitive.hpp`), stream formats (`render_cmd.hpp`'s `rcmd::` commands), and fixed-address globals as `Global<T, VA>` under their Ghidra names.
  - Original functions as typed callables, `Function<R(Args...)>` / `Method<...>` (`function.hpp`): the Ghidra name, the VA, and each parameter's location (`EAX`, `ESI`, `XMM0`, `STK`, ...). A call reads like C++: `Dx9_SetRenderState(D3DRS_CLIPPLANEENABLE, mask)`. One i386 shim (`function.cpp`) loads the registers, pushes the stack arguments and restores ESP afterwards, so callee-pop works too. Only the inputs a callee actually reads are declared.
- **`src/carrier/reimpl/<subsystem>/`:** the reimplementations. Each reads like the original, handler by handler, through `game::` types and functions. It calls `vr/` code directly where it implements VR behavior. Each header records the original VA, the status, what it calls, and which globals it writes. The Ghidra plate notes the reimplementation.
- **`src/carrier/hooks/reimpl.cpp`:** the replacement list, one line per reimplementation (name, the original's code range, our function). `hooks/site.hpp` refuses any other hook inside a replaced original's code. Such a hook would silently stop firing; it has to become a direct call in the reimplementation.

## Replacement

A reimplementation is mandatory once it's in: an inline hook on the original's entry (`hooks/reimpl.cpp`). There are no switches back to the original; regressions are bisected with `git`. Once VR behavior is folded in, a reimplementation is no longer equivalent to the original anyway.

Inside reimplemented code, calls to other reimplemented functions are plain C++ calls, and `game::` callables are only for code that is still original. A cdecl/void original whose caller relies only on callee-saved registers is replaced by our function directly; both current ones are. Any other convention needs an entry thunk generated from its `Function` declaration (not written yet).

## Verifying a new reimplementation

Both current reimplementations were compared against their originals before any VR behavior went into them; that's the stage where a comparison is still meaningful. Two methods were used; the harness for both is in git history (`src/carrier/debug/shadow/`). Other checks may suit other functions better:
- **Dry run:** for a function whose callees don't feed back into its own logic (`ExecuteStream`). At each original call, run ours with its `game::` calls recorded instead of executed, then compare with the original's callee calls (captured by MidHooks) and the globals both write.
- **Paired passes:** for a function whose callees change state it reads (`SubmitToGPU`). The stereo replay's two identical passes run the original and ours for real, and their call sequences are compared. Comparing original against original first showed the comparison was sound.

Things that produced false differences:
- Arguments that live only in part of a register (AL).
- Pointers to each side's own stack temporaries.
- Globals that callees also write.
- Call sites whose 5-byte patch window covers a branch target.

## Status

- `RenderCmd_ExecuteStream`: replaces the original. Verified against it (dry run: identical call sequences in gameplay), no frame cost ([reverse_engineering/render_cmd.md](reverse_engineering/render_cmd.md)).
- `PgPrimitive_SubmitToGPU`: replaces the original and renders the stereo frame, a LEFT and a RIGHT pass ([stereo.md](stereo.md)). The `view_rewrite` upload gate is a direct call in it. Verified against the original (paired passes: identical call sequences in gameplay) ([reverse_engineering/submit_to_gpu.md](reverse_engineering/submit_to_gpu.md)).
- **Call-path cost:** a `game::Function` call costs about 5 ns under Wine, against 1 ns for a direct call. Frame-building is inlined per declaration; the rest is the generic asm shim. Native calls (`__cdecl`/`__stdcall`/`__fastcall`/`__thiscall`) where the convention fits would get closer to 1 ns, but each declaration would then need the callee's pop count.
- OPEN, candidates for the next reimplementations:
  - `LtiRenderer_BeginSubmit` (`0x0074aaa0`): Present prev, BeginScene, GPU sync, clear. That would turn the right-eye drain skip, now a MidHook at `0x0074ab5f` ([pacing.md](pacing.md)), into a plain condition.
  - The HUD: the Scaleform records (2-row `Mvp`/`TexGen` shaders, about 130 draws per frame) pass through our `SubmitToGPU`, which is where a HUD render-target split could go ([hud.md](hud.md), [reverse_engineering/gui_renderer.md](reverse_engineering/gui_renderer.md)).

## Tests

We only test what nothing else verifies. A test whose expected values come from the same reading of the disassembly as the code just repeats it. Faithfulness is checked against the original itself, in the game.

- `tools/test/test_call_shim.cpp` (`mc2vr_call_shim_test`, i386, run under Wine) covers the hand-written asm call shim. A shim bug in the game would show up as a crash far from its cause. Build and run: `cmake --build build/win32 --target mc2vr_call_shim_test && wine build/win32/bin/mc2vr_call_shim_test.exe`.

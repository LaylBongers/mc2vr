# Render command streams (`RenderCmd_ExecuteStream`)

Each draw record's `cmdStream` (`PgPrimitive+0x2c`) is a dword stream: an opcode, its operands, and so on, terminated by op 0. `PgPrimitive_SubmitToGPU` runs it per record through `RenderCmd_ExecuteStream` (`0x008569d0`) before that record's draw. The opcode table is on the Ghidra plate. The command format (`rcmd::` structs), the globals and the callees with their register conventions are typed in `src/game/render_cmd.hpp`, `dx9.hpp` and `render_shell.hpp`. The executable form is the reimplementation, `src/reimpl/render/render_cmd.cpp` ([../reimpl.md](../reimpl.md)). Frame context: [frame_chain.md](frame_chain.md).

- **Signature:** `cdecl void(stream, vsCtx, psCtx)`, called only at `0x008562be` (vsCtx = EDI, psCtx = EBX there). It saves ESI/EDI/EBP.
- **Dedupe:** a stream equal to the last executed one (`[0x011697b8]`) is skipped. That's why the stereo replay's pass 2 can skip a stream ([stereo.md](../stereo.md)).
- **What streams do:** set state (render states, shader constants, textures, render targets, clip planes, stream sources), clear, `StretchRect`, and run render-target pass helpers (surface passes, half-res, view-type-9 blit). No opcode draws.
- **Callees:** 24, all plaintext except op 23, which is real VM bytecode (`thunk_FUN_0256af90`, `push marker; call` into `Stext`; the slot isn't patched at runtime). A reimplementation calls the original thunk with the same registers.
- **Register conventions:** most callees take arguments in registers set by the handler (EAX/ECX/EDX/EDI/ESI, XMM0 for op 8). Leftover registers from earlier handlers only reach failure-logging paths.
- **Op 4 / op 24:** op 4 binds at sampler `D3DVERTEXTEXTURESAMPLER0 + slot`. Op 24 unbinds sampler `stage + 0xff` and sets render state 154 (`D3DRS_POINTSIZE`) to `0x71ff0050 | stage`. That value pattern looks like ATI's R2VB (render-to-vertex-buffer) sampler mapping; unverified.
- **Op 22:** device `StretchRect` (vtable `+0x88`), not "SetStreamSource" as earlier notes and `stream_capture` labeled it. Op 18 ignores its own payload.
- **Bad opcode:** an opcode > 26 makes the original spin forever. The reimplementation stops and reports it.

## Verified

- **Shadow run, 2026-11-09:** 66.6 M streams in gameplay; every call sequence matched the original. The only end-state differences were in `g_RenderCmdPassObject`. `RenderCmd_BeginSurfacePass` / `EndSurfacePass` also write it (1 / 2), and the dry run doesn't execute callees, so the harness no longer compares it.
- **Live, 2026-11-09:** replaces the original, with no visual issues and no measurable frame cost (A/B: [../reimpl.md](../reimpl.md)).

## OPEN

- With the reimplementation installed, `debug_stream_capture`'s opcode MidHook (`0x008569f5`, inside the original) no longer fires; capture needs `debug_reimpl=shadow`, which leaves the original running.
- The roles of the pass helpers behind ops 16/17/21/25 (`RenderCmd_{Begin,End}SurfacePass`, `FUN_0074af30`, `RenderShell_BeginHalfResPass`).

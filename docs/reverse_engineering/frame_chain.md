# Frame driver chain

Runtime-confirmed via carrier hooks (static additions 2026-10-02). Per-address facts in Ghidra plates (`GameShell_FrameTick` `0x00630e10`, `PgPrimitive_SubmitToGPU` `0x00855690`, `LtiRenderer_vtbl` `0x00bd38e8`, `RenderQueue_SubmitWorldPackets` `0x0048e620`, and the `g_LtiRenderer`/`g_MaterialTable`/`GameStateBase`/`RenderQueue`/`LtiRenderer`/`Dx9StateWrapper`/`PgMaterial` plates + structs). Threading: [render_threading.md](render_threading.md); staging detail: [view_table.md](view_table.md).

```
InGameShellState_FramePipeline (producer side, no D3D):
  ... RenderPackets_ResetViewFlags, RenderQueue_SubmitWorldPackets (0x0048e620: walks the
  ACTIVE-view linked list, stages one 96-byte {size,ptr} element per type-2/4 view, bulk-
  publishes to g_RenderQueue) ...
  0x004c99f9 -> SecuROM VM stub 0x0050f660: SUSPECTED packet interpreter (static analysis: no plaintext
  code reads the ring / frame-ctx 0xd29xx fields; it builds the draw-records + command
  streams consumed below)
PgPrimitive_SubmitToGPU (0x00855690, entered via vtable slot03 = RenderFrameTimed 0x0085abd0)
  RenderShell_InitDrawRecordTables (0x00853ee0, called from LtiRenderer-side FUN_007494e0)
  zeroes/allocates the draw-record tables + pools. SubmitToGPU walks the 0x58 PgPrimitive
  records (head g_PrimitiveHead, base g_PrimitiveBase, link g_PrimitiveNext), applying
  per-record state (order: view type/RT, technique + VS/PS constants, indices, pass object,
  stencil/RT mask, command stream, stream binding, draw — see the plate on the function):
  +0xC2 (0x00855752): virtual dispatch on g_RenderShell (0x017ceaf0, holds BASE LtiRenderer_vtbl 0x00bd38e8)
    slot 15 LtiRenderer_BeginSubmit (0x0074aaa0): [if g_SuppressPresent==0] Present(prev) via
        LtiRenderer_Dx9_Present (0x00748fb0: device vtable slot 17, all-NULL args); BeginScene (slot 41);
        sets in-scene flag DAT_0117526c=1; then a GPU SYNC: an event query (raw device slot 118
        CreateQuery, type 8) is issued and spun on with Sleep(0) until the GPU drains, then
        SetRenderTarget(0)/depth + Clear — every frame waits for all prior GPU work
    ... per record: RenderCmd_ExecuteStream (0x008569d0) runs the record's dword command
        stream (record+0x2c) — 27-opcode interpreter, device calls via Dx9_State layer ...
    slot 22 LtiRenderer_EndSubmit (0x0074ac30): clears state-cache flags, EndScene (slot 42), clears flag
```

- `g_RenderShell` holds the **base** `LtiRenderer_vtbl` at frame time; the derived `RenderShell_vtbl` (`0x00be84c0`) overrides (incl. slot 15 `Flush`) never run. Type as `LtiRenderer`/`LtiRenderer_vtbl` (done in Ghidra).
- Render submit: `RenderShell_RenderFrameTimed` is normally called from the **registered task `RenderTask_RenderFrame` (`0x0046a290`, return address `0x0046a2a6`)** — dispatched by the task queue, not from a direct call; the device-lost path in `InGameShellState_Update` (`0x004c0b50`) is the only other caller ([vm_stub_callbacks.md](vm_stub_callbacks.md)).

## Open

- `0x0117560c` — precache gate flag (if 0 while precache requested, frame bails early); semantics unexplored.
- `DAT_017d1818` / `DAT_017d2a94` — vtable'd singletons used by the type-8/9 passes (surface/rect providers); unidentified.
- The two frame-preamble hash keys (`0x5e84ea6d`, `0x16085a8d`, both with low dword `0xf011157a`) — presumed the two named world views; reverse the name strings if a registry-writer is found.

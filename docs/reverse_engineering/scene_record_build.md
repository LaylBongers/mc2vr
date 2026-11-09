# Scene → draw-record build

Found statically on 2026-11-08; per-address facts are on the Ghidra plates. It is all plaintext: the only SecuROM elements are two call gates, which are plain forwarders ([securom_vm.md](securom_vm.md)). Record layout: [render_data_model.md](render_data_model.md). Submit side: [frame_chain.md](frame_chain.md).

## Pipeline (task-system nodes, registered by `RenderTask_RegisterAll`)

```
node 0  RenderTask_BeginFrame      0x0046a270  scene = taskctx+0x104 -> g_RenderShell+0x2b94; shell->vtbl[1]()
node 3  RenderTask_BuildScene      0x0046a2c0  -> RenderShell_BuildSceneRecords 0x0085ac50 (QPC-timed -> g_SceneBuildMs 0x01175624)
          per sub-view v (count shell+0x2b90, stride 0xe80 at shell+0x10, iterated high -> low):
            ViewContext_BuildCameraConstants   gate 0x008591a0 -> 0x00506a26 -> 0x008591ac
            listener(shell+0x2ba0)->vtbl[1](phase 0/1/2, v)
            if view enabled (+0x3c): Scene_EmitViewPrimitives 0x00466da0 (scene)
              -> ~50 emitters (direct calls + vtable methods) -> PgPrimitiveScratchRecord_Submit 0x00854910
                 (appends a 0x58 record to the WRITE buffer; count 0x00ff36f0, cap 0x2000)
node 4  RenderTask_PublishRecords  0x0046a300  -> shell->vtbl[2] LtiRenderer_PublishIfNewFrame 0x0074ada0
          -> PgPrimitive_PublishFrame 0x00854da0 (gate -> body 0x00854da8): repoint the read globals
             (g_PrimitiveBase, g_MaterialTable, g_ViewContextTable, view/screen/instance tables) at
             buffer g_PrimBufferIndex, transpose its matrices, PgPrimitive_SortList -> g_PrimitiveHead,
             PgPrimitive_Reset flips g_PrimBufferIndex (0x00ff364c, ping-pong 0/1)
node 2  RenderTask_RenderFrame     0x0046a290  -> shell->vtbl[3] RenderFrameTimed -> PgPrimitive_SubmitToGPU (walks g_PrimitiveHead)
```

- Each node body ends with `thunk 0x0046ab80(EAX = node)` = `NodeArray_DecrementChildRefs`, the dependency signal ([vm_stub_callbacks.md](vm_stub_callbacks.md)).

## Measured (live, 2026-11-08, `debug_build_census`, gameplay at 90 Hz)

- **Thread:** all four nodes run on the render-task thread, the same thread as `RenderTask_RenderFrame` ([render_threading.md](render_threading.md)), never on the update thread.
- **Order:** strictly 0 → 3 → 4 → 2 every frame, each node once per frame, back to back.
- **Cost:** `RenderShell_BuildSceneRecords` takes 2.0–4.4 ms per 10 s window (typically ~3.1 ms, max ~5.8 ms), and it scales with the record count. The engine's `g_SceneBuildMs` agrees to 0.01 ms. This is the render-thread "prep" in [../pacing.md](../pacing.md). These numbers include the probe's per-record hook. `PgPrimitive_PublishFrame` takes 0.06 ms.
- **Volume:** 2.6k–3.9k records per frame, all through `PgPrimitiveScratchRecord_Submit` (records published = submits). There are ~73 distinct (emitter, type) combinations. Biggest emitters are the vtable methods `FUN_00478c90` / `FUN_004787f0` / `FUN_00478fd0` (`0x00bab1c4..` vtables).
- **Sub-views:** `shell+0x2b90` = 2, but every record comes from sub-view 1; sub-view 0 emits nothing.
- The draw records are double-buffered. The emitters write buffer `b` while `SubmitToGPU` reads the buffer published last. `PgPrimitive_PublishFrame` runs only when the shell's frame id changes (`+0x2be4 != +0x2be8`); it is also called by `LtiRenderer_OnScreenResize`.
- `g_RenderQueue`/`g_RenderQueue2` are not read by the build, neither directly nor anywhere in its 78-function `.text` closure; 8 VM thunks in that closure are unread. The ring's consumer is still OPEN ([view_table.md](view_table.md)). The build takes the scene from `shell+0x2b94` instead.

## Hook points (for the mod)

- `RenderShell_BuildSceneRecords` entry/exit brackets one frame's whole build. Its per-view loop is the place to emit extra views or skip some.
- `Scene_EmitViewPrimitives`: per view, plaintext and normally structured, so a clean hook target.
- `PgPrimitiveScratchRecord_Submit`: per record, cheap to filter (record type byte at `0x0116374c`: 4 = occlusion box, etc. — see its plate).
- `PgPrimitive_PublishFrame` exit: the finished, sorted frame before any pass walks it.
- HUD/2D: ordinary records from the Scaleform renderer `GuiRenderer` ([gui_renderer.md](gui_renderer.md)), a few dozen per frame, emitted inside this build. The first census missed them because they're few and share sub-view 1 and type 8 with the scene. `RenderShell_EmitScreenQuad` (×4 from the post chain `PgLtiBloom_EmitPasses`) is a full-screen-pass emitter, not HUD.

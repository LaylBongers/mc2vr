# IPC (protocol v1)

Carrier ↔ host link. Header: `src/common/mc2vr_ipc.h`; implementations `src/host/ipc.cpp`, `src/carrier/ipc.cpp`.

- One section created by the HOST (refuses if one exists), name `mc2vr_ipc_v1` (env `MC2VR_IPC_NAME`).
- `Mc2IpcState` seqlock (host writer): pose/FOV per eye (OpenXR **angles in radians, not tangents**), IPD, session state, recenter counter, `hostFrame` (`poseId = hostFrame+1`).
- SPSC rings: **events** host→carrier (session state, exit, recenter); **commands** carrier→host (`CONFIG`, `FRAME_READY{x=frameId, y=handle, a=slot, b=eye, c=w, d=h, e=poseId}`, `SHUTDOWN`).
- Both sides watch each other's process (carrier pid in the section header; `hostExiting` flag). The host exits on carrier `Shutdown` or game exit.
- Carrier connects in stage 1 (non-fatal); a 250 ms monitor thread logs state transitions; the render thread only does lock-free seqlock reads.
- Events are drained at the slot-5 `PostUpdateHook` on the main thread, once per frame (`ipc::drain_events()`); the monitor thread never pops (SPSC, one consumer). `SESSION_STATE` transitions are logged; `RECENTER` is logged only — nothing to apply (the camera consumes live poses, so a reference-space change propagates at the next pass-1 sample by construction).
- `MC2VR_MSG_EXIT` = one-shot `WM_CLOSE` to the game's root window (HWND global `0x01175274`). The pump/WndProc quit path is VM-protected (`PostQuitMessage`/`GetMessageA` are only SecuROM-region references), so the window message is the clean-quit signal: engine pump exits → carrier pid dies → host follows via its death watch. The host pushes EXIT only for runtime-initiated ends (session EXITING/LOSS_PENDING or instance loss without a prior self-initiated `xrRequestExitSession` — `State::selfExit` flag at the Shutdown-cmd/carrier-death/frame-limit sites).

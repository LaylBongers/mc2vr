# Input injection (track CANCELLED — facts kept)

Motion controls are not planned anymore (decision 2026-10-09). The facts below were gathered for that track and kept in case it is ever revived.

- The same hooking rules apply ([hooks.md](hooks.md)); protected code is denser in logic code — hook plaintext call sites, never thunks or protected code (calling thunks is fine, proven).
- Input injection point: `XInputGetState`/`XInputSetState` import stubs `0x00a64d56`/`0x00a64d5c` (plaintext thunks). No dedicated input-update call exists (state-stack flow — [main_game_loop.md](reverse_engineering/main_game_loop.md)). Marshal motion poses to the main thread at a defined frame point.
- Controller/HMD input source would be OpenXR actions on the game's session ([xr_session.md](xr_session.md)), synced at the stereo frame's start on the render thread and applied at the slot-5 hook.
- Resolve the idle-reset buffer pair (`0x017d30e8` count/array, `0x00f7fb90` 0x1000 buffer) before designing input injection.
- Gameplay object models (player/camera/weapon, G-engine classes) need mapping via the [vtables recipe](reverse_engineering/vtables.md) — workload, not risk.

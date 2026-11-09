# Shared textures (D3D9 → D3D11 interop)

Eye images cross carrier→host as shared-handle textures (proven by a probe on 2026-10-04, all-PASS; the probe was removed afterwards, see git `b6a05a1`). Code: `src/carrier/vr/eye_share.cpp` (capture ring), `src/host/shared_eyes.cpp` (handle open cache, `latest()` seam).

- DXVK D3D9 legacy `pSharedHandle` textures open in DXVK D3D11 (`OpenSharedResource`) cross-process (D3D9Ex/plain × A8R8G8B8/X8R8G8B8; → DXGI 87 / 88 with alpha 0xFF).
- Ring = 4 RT-usage slots per eye (StretchRect needs RT surfaces) at the current render size; handles travel as plain integers; recreated lazily after device Reset, and the new size reaches the host through `CONFIG`.
- Cross-process sync = an event query per pair (D3D9's fence; no fence on legacy handles): issued after the RIGHT capture with one `GetData(D3DGETDATA_FLUSH)` (DXVK never submits without it), then polled by a carrier completion thread that sends both FRAME_READYs once it signals — the render thread never waits for the GPU. Polling from another thread needs a `D3DCREATE_MULTITHREADED` device — the game already creates it that way (behavior flags `0x44`, logged); the CreateDevice hook adds the flag if it is ever missing. A slot is only rewritten once its pair was sent. `debug_sync_frame_ready=on` = the old synchronous wait (`share window: … sync stall`).
- Raw-vtable slots that bit us: texture GetSurfaceLevel=18, query Issue=6 / GetData=7, device CreateTexture=23.
- The host mirror must `CopyResource` into staging before `Map`; the handle cache is reserved (64) so `Entry*` stay stable.

Portability: legacy `pSharedHandle` textures on a plain (non-Ex) D3D9 device are DXVK behavior — native Windows only shares resources from a D3D9Ex device, so a native port would have to promote the game's device (`Direct3DCreate9Ex`/`CreateDeviceEx` in the existing creation hooks). The rest of the hand-off (event-query fence + `D3DGETDATA_FLUSH`, `D3DCREATE_MULTITHREADED` polling) is standard D3D9.

Capture points and the eye pass structure: [stereo.md](stereo.md).

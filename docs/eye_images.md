# Eye images (render thread → OpenXR session thread)

`src/carrier/vr/eye_share.cpp`. At the pass boundaries the carrier front holds each eye's LDR final ([render_outputs.md](render_outputs.md)); they are blitted into a ring and handed to the session thread, which copies them into the OpenXR swapchains ([xr_session.md](xr_session.md#presentation)).

- Ring: 3 pairs of D3D9 RT textures (StretchRect needs RT surfaces) at the render size, created lazily, dropped at every device Reset. Captures run only while the session is VISIBLE/FOCUSED.
- Textures are created with a legacy `pSharedHandle` that nothing opens: DXVK never relocates a shared image (`DxvkImage::canRelocate`), so the VkImage behind each texture is fixed for its lifetime. A plain RT texture may be moved by DXVK's memory defragmentation, which changes its VkImage.
- Fence: after the RIGHT blit, an event query per pair, issued with one `GetData(D3DGETDATA_FLUSH)` (DXVK never submits without it). The render thread never waits for the GPU.
- Hand-off (SRW lock + condition variable, slot states Free / Writing / Issued / InUse): the session thread waits for the pair rendered from this frame's views (or takes the newest one when its wait runs out, `late`), polls its fence (50 ms; the device is `D3DCREATE_MULTITHREADED`, so polling from another thread is safe — the game creates it that way, the CreateDevice hook adds the flag if missing), copies it, then frees the slot. Older unclaimed pairs are dropped. The render thread waits (50 ms) only if the slot it is about to write is still being copied.
- Raw-vtable slots that bit us: texture GetSurfaceLevel=18, query Issue=6 / GetData=7, device CreateTexture=23.
- 10 s log: `share window: capturesL/R pairs taken fresh/late dropped | slot waits/timeouts | fence->taken gpuTimeouts`.

Without DXVK (no shared RT textures on a plain D3D9 device, no interop) the carrier logs `share: CreateTexture(… shared RT) FAILED … mc2vr needs DXVK's d3d9.dll` and VR stays inactive ([platforms.md](platforms.md)).

Capture points and the eye pass structure: [stereo.md](stereo.md).

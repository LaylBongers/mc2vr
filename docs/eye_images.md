# Eye images (the stereo frame → OpenXR)

`src/carrier/vr/eye_share.cpp`, render thread only. At the pass boundaries the carrier front holds each eye's LDR final ([render_outputs.md](render_outputs.md)); they are blitted into one pair of textures, which the frame's end copies into the OpenXR swapchains ([xr_session.md](xr_session.md#presentation)).

- Textures: one pair of D3D9 RT textures (StretchRect needs RT surfaces) at the render size, created lazily, dropped at every device Reset (after our copies' fences). Captured only on frames the runtime shows (`xr_session::Frame::show`).
- One pair is enough: the frame's copy is submitted before the next frame writes the textures again, so the GPU orders the two (the copy's closing barrier).
- Textures are created with a legacy `pSharedHandle` that nothing opens: DXVK never relocates a shared image (`DxvkImage::canRelocate`), so the VkImage behind each texture is fixed for its lifetime. A plain RT texture may be moved by DXVK's memory defragmentation, which changes its VkImage.
- Flush: after the RIGHT blit `eye_share::finish` calls `FlushRenderingCommands` under DXVK's device lock (`LockDevice`; the call takes none itself; the game's device is `D3DCREATE_MULTITHREADED`, without which the lock is a no-op). It submits DXVK's pending work and waits until DXVK's CS thread has handed everything recorded so far to its submission queue, which DXVK requires before our own submissions use a D3D9 resource's image. The cost is the CS thread's lag (`share window: flush`; the ~1 ms avg / 4 ms max of the first runs was measured at single x87 precision, so 4 ms-quantized: [x87_precision.md](x87_precision.md)), not the GPU. No thread waits for the GPU.
- Captures run before `render_outputs` sees the boundary (its 2→0 handling ends the post-Reset transition frame; capturing after that published a white RIGHT eye, 2026-11-08). The transition frame itself has no capture source: no pair, the frame ends with no layers.
- History: until 2026-11-10 a session thread took the pairs from a 3-slot ring (slot states, condition variable) and, before that A/B, waited for each pair's event query (GPU-done) before copying.
- Raw-vtable slots that bit us: texture GetSurfaceLevel=18, query Issue=6 / GetData=7, device CreateTexture=23.
- 10 s log: `share window: capturesL/R pairs | flush avg/max`.

Without DXVK (no shared RT textures on a plain D3D9 device, no interop) the carrier logs `share: CreateTexture(… shared RT) FAILED … mc2vr needs DXVK's d3d9.dll` and VR stays inactive ([platforms.md](platforms.md)).

Capture points and the eye pass structure: [stereo.md](stereo.md).

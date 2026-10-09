# Shared textures (D3D9 → D3D11 interop)

Eye images cross carrier→host as shared-handle textures (probe `tools/probe/run_shared_handle.sh`, all-PASS). Code: `src/carrier/eye_share.cpp` (capture ring), `src/host/shared_eyes.cpp` (handle open cache, `latest()` seam).

- DXVK D3D9 legacy `pSharedHandle` textures open in DXVK D3D11 (`OpenSharedResource`) cross-process (D3D9Ex/plain × A8R8G8B8/X8R8G8B8; → DXGI 87 / 88 with alpha 0xFF).
- Ring = 4 RT-usage slots per eye (StretchRect needs RT surfaces); handles travel as plain integers; recreated lazily after device Reset.
- Cross-process sync = producer event-query flush only (no fence on legacy handles): **`GetData` must pass `D3DGETDATA_FLUSH`**; bounded 8 ms (`syncTimeouts` few/run, benign).
- Raw-vtable slots that bit us: texture GetSurfaceLevel=18, query Issue=6 / GetData=7, device CreateTexture=23.
- The host mirror must `CopyResource` into staging before `Map`; the handle cache is reserved (64) so `Entry*` stay stable.

Capture points and the eye pass structure: [stereo.md](stereo.md).

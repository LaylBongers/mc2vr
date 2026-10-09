# D3D9 device vtable slots

Header layout validated by the anchor: the game applies render state via `+0xe4` = slot 57 = SetRenderState per d3d9.h (SetDialogBoxMode quirk at slot 20 included).

- Corrected labels: `+0x10c` = slot 67 SetTextureStageState (36-state per-stage loops, stride-`0x24` cache), `+0x114` = slot 69 SetSamplerState (4-arg `(0, MAGFILTER, LINEAR)`), SetTexture = slot 65.
- Pinned hook slots: Reset 16, Present 17, BeginScene 41, EndScene 42; swapchain GetSwapChain 14, GetPresentParameters 9.

## Runtime call facts (carrier-observed)

- Calls per frame: Present/BeginScene/EndScene exactly 1:1 with `GameShell_FrameTick` count ([frame_chain.md](frame_chain.md)).
- `Reset` (slot 16) only on present-param change (resolution etc.), carries the new `D3DPRESENT_PARAMETERS`.
- Present args always all-NULL. Menu present params: 2560x1440, fullscreen, DISCARD, 60 Hz, interval DEFAULT, `hDeviceWindow=0x100b6`.

All render-path device calls actually go through the Dx9 wrapper: [dx9_state_wrapper.md](dx9_state_wrapper.md).

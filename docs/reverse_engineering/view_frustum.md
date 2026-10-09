# View frustum: builder products & consumers (2026-10-08)

Raw-decoded and watch-proven during the culling work ([../culling.md](../culling.md); plates on `ViewContext_BuildCameraConstants`, `CamCtrl_NearPlaneClearance`, `Fov_TanIndexHelper`). Upstream data flow: [camera_data_flow.md](camera_data_flow.md); builder chain: [draw_camera_chain.md](draw_camera_chain.md).

- **One source.** `ViewContext_BuildCameraConstants` (`0x008591ac`; EBX = the view render-ctx, `[ctx+0x28]` = camera object, entry = cam + [cam]*0x70) derives every frustum product of a view from two values:
  - `tanH = tan(fov·0.375·aspect)` and `tanV = tanH/aspect`, with fov = entry+0x58 (radians, 4:3-normalized; stock 0.95975 → 36.65°/22.71° at 16:9).
  - At `0x0085943B` they are in XMM2/XMM0 (just stored to ctx+0x30/+0x34). Everything after is derived from those registers: the projection (ctx+0xb20) → VP (ctx+0xb60) → `g_ViewContextTable` records ([view_context_records.md](view_context_records.md)), the frustum structs, and the shadow cascades.
- **Products** (main view: slot-1 ctx = `0x017CF980` = `g_RenderShell`+0x10+1·0xe80; slots are `g_RenderShellPtr + idx·0xe80 + 0x10`):
  - ctx+0x38 and ctx+0x250 — frustum structs built by `FUN_00857140`. Layout: `{near, far, 8 world corners at +0x0c (near 0..3 = (+H,+V)(+H,−V)(−H,−V)(−H,+V), then far), side-plane normals at +0x6c, plane D's at +0xb4, ...}`, 0x218 bytes. ctx+0x250 is the same view with far = near + Σ cascade splits.
  - ctx+0x954/0xa70/0xa80 — whole-view output of `FUN_0085a3f0`.
  - ctx+0x46c/0x8c0/0x900 — four shadow cascades; split distances at ctx+0x940.. (powers of 3).
- **Consumers** (watch-proven, main ctx):
  - ctx tans: `FUN_0047ded0` (`0x0047E35F`/`ED49`) — **render constants (fog/atmosphere-style), not a cull**; and `ViewCtx_BuildSnapshot` (`0x0061B973`). The snapshot is a stack temporary that `FUN_0061b7e0` uses only for a viewport rect.
  - frustum struct ctx+0x38 is block-copied (`rep movsd`) by: `0x00472E2F` (render thread, whole struct) and `0x005314C7` (first 0x114 bytes) — cull-side, unnamed; `0x005046C3` (`FUN_00504040`, per-player focus points); **`CamCtrl_NearPlaneClearance` (`0x007107d0`, game thread)** — the third-person camera's obstacle clearance: running maxima of the near-plane diagonal |c2−c0| and vertical edge |c1−c0| in the controller +0x4d0/+0x4d4. A bigger near plane makes the camera pull in near obstacles. This is the consumer that must stay stock when the frustum is widened.
- **The camera controller's own fov** (`EffectiveFov_ZoomCompose 0x0071BBC6` → [ctrl+0x5E8] → `Fov_TanIndexHelper`) is boom/offset math, separate from the projection's fov.
- **Not the projection frustum**: the ViewEntry fov triple (+0x2ec, from gcam +0x17c/+0x180, v/h = 0.3) feeding `ViewEntry_DeriveCullTask`'s per-view box ([camera_data_flow.md](camera_data_flow.md)).

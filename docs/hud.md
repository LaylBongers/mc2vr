# HUD depth

Opened 2026-10-07. The near-field focus discomfort that spawned it turned out to be two `hmd_delta` eye-offset bugs ([camera.md](camera.md) § Eye-offset frame), both fixed; the HUD itself was the remaining defect. The flat HUD is now its own layer (below); the world markers are still open.

## Finding

User-perceived, live-confirmed: the HUD/crosshair reads as placed way too close ("in your face"), making fixating near objects uncomfortable. Evidence numbers: 2026-10-07 eye-dump run (frames 11414…11566, runtime ipd 0.0647, `fovL` (−0.994 0.812 0.953 −0.954) rad).

## Root cause

The HUD is composited into the per-eye pass images (screen-space drawing into the carrier-owned front before the `eye_share` captures). It does NOT go through the per-eye camera rewrite (`hmd_delta` only rewrites `viewContextData` camera rows — [view_rewrite.md](view_rewrite.md)), so it lands at **identical NDC coordinates in both eyes**. But the world is not: the per-eye OpenXR frusta are asymmetric with different centers — left `cL = (tan(−0.994)+tan(0.812))/2 = −0.233` tangent units, right `cR ≈ +0.234` (measured world L/R offset −451 px, predicted −460; 1.7% span scale). So:

1. **Crossed disparity**: the left eye places the HUD at world direction −0.233, the right at +0.234 — inverted relative to real objects (real points always have the left eye seeing them right of the right eye). Crossed by ~0.467 tangent units ⇒ perceived at `z = ipd/0.467 ≈ 0.14 m` — a floating HUD ~14 cm in front of the face. Invisible pre-VR (mono output).
2. ~~Aspect distortion~~ resolved by HMD-native rendering ([render_resolution.md](render_resolution.md)). The image now has the eye's own aspect, the engine lays out the HUD at that aspect, and it renders correctly.

## HUD layer (live 2026-11-09, `vr/hud.cpp`)

Head-locked, carrier-side, drawn once. What is HUD comes from the engine itself ([reverse_engineering/gui_renderer.md](reverse_engineering/gui_renderer.md)):
- **Classification at build time:** our `RenderShell_EndSubView` (reimplemented, [reimpl.md](reimpl.md)) records the record-index ranges written by `Hud_DrawWorldMarkers` (world markers) and by the steps after it (the HUD and the GUI). The ranges are published with the record list at `PgPrimitive_PublishFrame`'s return (`hooks/hud.cpp`).
- **Laid out at 16:9:** while the layer is in use, the GUI sees one fixed 1920×1080 screen (`hud::kLayoutW/H`) on every thread. Nothing is swapped temporarily; each value is set where the engine produces it. RE: [reverse_engineering/gui_widgets.md](reverse_engineering/gui_widgets.md) (layout, re-layout triggers) and [reverse_engineering/view_blocks.md](reverse_engineering/view_blocks.md) (who reads the screen geometry, on which thread).
  - **UI scale** (view block `+0x34`, read by GUI code only): 2.25 in every block. It's written by our `RenderShell_SetupViewports` (its only writer, reimplemented: [reimpl.md](reimpl.md)) and at the mode switch. The stock H/480 values are saved there first, and the two GUI readers that draw into the eye image get them back at their load: world markers `0x0060f659`, pinned markers `0x00611809`. This sizes the minimap border (`MinimapWidget_UpdateBorderRect`, every frame on the update thread), text (`TextWidget_OnScreenChange`'s cache), blips and indicators.
  - **Aspect mode** `+0x2bc4`: WIDESCREEN. OnScreenResize snaps it from the render size (4:3 for the HMD's 0.9) before SetupViewports, and SetupRenderTargets' mutated stretch and the device restore re-snap it. So it's set at the end of our `SetupViewports`, after setup / restore (`vr/render_outputs`), and at the switch. In split-screen (not used on PC as far as known), mode 0 picks top/bottom halves, so WIDESCREEN would pick left/right. Readers: Lua `GetScreenRatio`, `GuiMgr_CheckAspectMode` (re-lays out), BeginDisplay's mode-3 pillarbox, split-screen viewports; the camera uses `+0x2bc8/+0x2bcc`. Script-set modes 2/3 are left alone.
  - **W/H, GUI readers only** (the scene and camera keep the real size):
    - widget layout `Widget_CorrectForResolution` (`0x00627d19`, XMM0/XMM5);
    - the GUI screen info and the scripts' `NewScreenWidth/Height` (`GuiMgr_OnScreenResize_Body` `0x00609f1d`/`0x00609f2d`);
    - HUD step 4 (`Hud_DrawOffscreenIndicators` `0x0061007d`, `Hud_IndicatorNdcToPixels` `0x00612b91`/`0x00612c7a`);
    - Scaleform's display rect (`GuiRenderer_BeginDisplay` `0x004af05a`, renderer `+0x6c048..`).
  - Hook handlers write integers / float bit patterns only, because several sites have live x87 state.
  - **Mode switch** (update tick): when switching on, the policy is applied before the mode flag is published; when switching off, the reverse. A build therefore never sees a mismatched scale. Then `GuiMgr_OnScreenResize` runs once, and a bogus view count (`[g_GuiMgr+0x60]+0`) forces a full re-layout on the manager's next update. The re-layout also queues the GUI's view-count event for Lua; harmless so far.
  - Mode = HMD in use (`render_size::hmd_in_use`: session VISIBLE/FOCUSED, already debounced by the runtime) and the layer target exists. There's no debounce of our own, as for the render-size switch ([render_resolution.md](render_resolution.md)). A build records which layout it ran with; only 16:9 builds are drawn as the layer, so the frame or two around a switch keep the stock path. A tracking drop (no pose for the frame) keeps the layer and composites with the last eye FOVs/IPD: the panel is head-locked.
- **Drawn once:** on headset frames, our `SubmitToGPU` takes the HUD records out of the walk. Pass 1 draws them afterwards into a carrier-owned 1920×1080 HUD target: transparent, its own depth-stencil for the minimap's stencil mask, alpha accumulated so the result is premultiplied. Pass 2 skips them. Before the draw, the engine's op-8 handler `RenderCmd_SetScreenConstants` sets the viewport and pixel-to-clip constants for 1920×1080: the engine-2D records carry no op 8 and inherit the view context's 2016×2240 one. The last-technique/pass caches are dropped so the constants upload. Everything is restored afterwards.
- **Composite:** after each pass (after `EndSubmit`, which StretchRects main into the front in menus), the HUD texture is drawn into that eye's image as a plane `hud_distance` metres ahead (default 1.5), `hud_size` degrees wide (default 55), centered on the head. Its position per eye comes from that eye's FOV and the IPD (the frame's pose sample). Fixed-function draw; the device state is restored with a state block, the targets from the engine's bind caches.
- The world markers stay in the eye images. Monitor mode (no headset) keeps the stock HUD.
- Log: `hud: frames layered=… of …, composites=…, failures=…, layout=16:9|stock, relayouts=…` every 10 s; `hud: layout -> …` at each switch.

Live: the HUD renders correctly on the panel at a comfortable distance; every headset frame is layered (2 composites per frame), no failures, 90 Hz unchanged at 4× MSAA. The crosshair is on the panel; in this third-person game it lines up somewhat better than before.

16:9 layout: Live 2026-11-09 with the earlier build-time swap: the main menu, the text, the minimap content (1528.5, 756, 225×225) and most HUD elements were correct. The minimap border and the edge-clamped objective icon were about 2× too large, with a rare flicker to the right size. Cause: `MinimapWidget_UpdateBorderRect` reads the live UI scale on the update thread, outside the swap. The swap raced it, and it would also race the camera's W/H reads ([reverse_engineering/view_blocks.md](reverse_engineering/view_blocks.md)). The swap and the TextWidget scale-cache hook are removed; the screen policy above replaces them. **Live 2026-11-09: fixed.** Border quad 346.5 px (154 × 2.25), view-1 UI scale 2.25 in HUD mode and stock 3.0 on the monitor, every headset frame layered. The border, the objective icon and the HUD elements line up, with no flicker.

OPEN:
- `Reticle_UpdateState` reads view 1's W/H. Whether that should be the HUD or the eye screen depends on where the reticle lands. Four readers are unclassified ([reverse_engineering/view_blocks.md](reverse_engineering/view_blocks.md)).
- Bink overlay rects (step 5, `Bink_DrawOverlays`) are cached at movie open from the render fields, so they still follow the eye-image size.
- Destroyable-crate and helicopter-pickup icons fall in the HUD ranges and are on the panel: no longer uncomfortable, but they don't line up with their objects.

## World markers per eye (live 2026-11-09, `vr/world_markers.cpp`)

Per-address facts are on the Ghidra plates (`Hud_DrawWorldMarkers` and its callees).
- **Why they were wrong:** each marker is projected with the view's single camera (`World_ProjectToNdc`/`World_ProjectToPixels`), drawn by `Hud_DrawQuad` at a fixed pixel size scaled by `s` = H/480 (≈ 4.7 at 2240 px; one marker kind applies `s` twice), at that projection's depth. Both eyes replay the same records: no stereo, and far too large.
- **Build:** a MidHook in `Hud_DrawQuad` just before its renderer call (`0x00617d91`; every caller draws markers) notes each emitted quad's record with its centre (XMM5/XMM6), half height (from the vertices) and depth. `Hud_DrawQuad` is not reimplemented: its material check is a SecuROM VM thunk (`0x006188f0` → `0x0255e420`), and calling it from our code crashed the game at the first gameplay frame, presumably because the protected code reads its caller's frame. Consecutive quads at the same depth are one marker (background + icon) and share the first quad's centre as anchor. Depth-0 quads (`Hud_DrawPinnedMarkers`, pinned to the screen edge) are left alone.
- **Draw:** per eye, the anchor goes through the pass's mono → eye clip map (`view_rewrite::mono_clip_to_eye`, the same delta the scene gets), and the quad is moved to the eye's anchor and scaled around it to `marker_size` degrees tall (default 3), through the technique's pixel-to-clip screen constant (VS c2 = (2/W, −2/H, −1, 1), `Technique.screenConsts`), read back from the device first and restored after the draw. (The material's VS c0–c1 is the texture-coordinate transform; changing it scrolled the icon textures — first attempt.)
- Log: `markers: placed=… no-delta=… not-pixel-map=… dropped=…` every 10 s.

Live: the icons render correctly, fuse in stereo at their objects' distance, and look about the right size at 3°. ~1,000–2,700 quads placed per second, no misses, 90 Hz unchanged.

OPEN:
- Depth-tested with the single camera's depth, not per eye (no visible problem so far).
- Pinned markers (`Hud_DrawPinnedMarkers`, depth 0, stuck to the screen edge) are unchanged: mono, original size.


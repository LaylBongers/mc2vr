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
- **Drawn once:** on headset frames, our `SubmitToGPU` takes the HUD records out of the walk. Pass 1 draws them afterwards into a carrier-owned HUD target: render size, transparent, its own depth-stencil for the minimap's stencil mask, alpha accumulated so the result is premultiplied. Pass 2 skips them.
- **Composite:** after each pass (after `EndSubmit`, which StretchRects main into the front in menus), the HUD texture is drawn into that eye's image as a plane `hud_distance` metres ahead (default 1.5), `hud_size` degrees wide (default 55), centered on the head. Its position per eye comes from that eye's FOV and the IPD (the frame's pose sample). Fixed-function draw; the device state is restored with a state block, the targets from the engine's bind caches.
- The world markers stay in the eye images. Monitor mode (no headset) keeps the stock HUD.
- Log: `hud: frames layered=… of …, composites=…, failures=…` every 10 s.

Live: the HUD renders correctly on the panel at a comfortable distance; every headset frame is layered (2 composites per frame), no failures, 90 Hz unchanged at 4× MSAA. The crosshair is on the panel; in this third-person game it lines up somewhat better than before.

OPEN:
- The HUD's layout follows the eye's tall aspect, so the panel looks horizontally compressed; a 16:9 layout needs the HUD code to see a different screen size.
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


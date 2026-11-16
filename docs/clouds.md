# Cloud layer depth window

Fix for the cloud layer covering distant mountains, and for it appearing and disappearing with view direction. It is an original-game bug. The game side, including why it happens: [reverse_engineering/sky_clouds.md](reverse_engineering/sky_clouds.md).

**Fix** (`reimpl/render/sky_emit.cpp`, the `Sky_EmitRecords` reimplementation): the sky computes its depth window once (the view rect, MinZ = MaxZ = the depth at 0.8 × far), and every record drawn in it carries the window in its own stream (op 8): the dome, the sun, and cloud render 2. The sky calls render 2 directly with it (`PgClouds_EmitCloudRender2InWindow`) when the clouds object has the original vtable, else through its slot 2 as the original does. The window record and the restore record are unchanged, so whatever sorts around the sky sees the original state. The fix makes the sky's records independent of the sort, which in the original can put the water's depth pass, with its viewport-resetting target binds, between the window and render 2. The per-eye sky matrices are unaffected; the window's depth comes from the projection's A and B, which every eye keeps.

Log per 10 s window: `sky: window: emits=… clouds in window=… clouds virtual=… sky flag set=…`. Expected: `emits` = one per frame per view drawing a sky, `clouds in window` the same (unless the sky flag is set), `clouds virtual=0`.

History: a first fix (2026-11-16) re-issued the window from cloud render 2 alone, recomputing it with a copy of the sky's math, gated on the sky's sort-bucket override. It was live-verified, then replaced by the sky reimplementation so that the window has one source and the dome and sun are covered too. That first fix's verification: with the water's depth pass sorted in between (`BETWEEN` in `debug_gui_census=clouds`, viewport left at z 0..1), render 2's first draw read the window (`z[0.9999791, 1.000979]`, the same as the dome). In the unmodified game, the obscured distant mountains pop into view when the camera is moved so that the water stops drawing (user, 2026-11-16).

Verified live (2026-11-16, HMD off, `debug_gui_census=clouds`, 2.5 min of gameplay): `emits` = `clouds in window` = ~120 per s (one per frame), `clouds virtual=0`. The dome, sun and render 2 records each start with op 8 and draw with the window. With the water's depth pass sorted in between (`BETWEEN`, which leaves the viewport at z 0..1), render 2's first draw reads the window (`z[0.9998959, 1.000896]`, the same as the dome). The rest of each stream is the original's: the dome's `scale` and `projectParams` uploads, op 12 and op 13; the sun's stream has no upload (its constant has no register); the sort keys and order are unchanged.

## Sky at infinity (VR)

The dome and the sun quad are placed around the camera (16 units away), and their records are replayed in both eye passes. Placed around the mono camera, they had the stereo depth of 16 units in the headset, not infinity. `SubmitToGPU` now uploads their instance matrix (`objectData`) per eye: on an eye record, `view_rewrite::eye_anchored_instance` shifts its translation by the eye's offset from the mono camera (this eye's `cameraPos` − the game's), so each eye sees them around itself. Which records: those drawn with PgSkyVP or PgSunVP (`reimpl::anchored_at_camera`); only `Sky_EmitRecords` uses those techniques. The clouds need nothing: their sky matrices are rebuilt per eye without translation ([view_rewrite.md](view_rewrite.md#clouds)), and the haze is per pixel.

Log per window: `view/hmd: camera-anchored instances (sky dome, sun): eye=… passthrough=…`. Expected in the headset: `eye` = 4 per frame (dome and sun, 2 passes), `passthrough` only without a pose; with the HMD off everything is `passthrough`.

OPEN:
- Live verification in the headset: the sky and the sun read as infinitely far (no stereo depth, behind the mountains); `eye` as expected. With the HMD off: unchanged.

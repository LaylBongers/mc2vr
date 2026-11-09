#pragma once

// HDR eye-adaptation continuity across carrier-driven device resets
// (render-size switches, docs/render_resolution.md;
// RE: docs/reverse_engineering/hdr_adaptation.md).
//
// The engine's auto-exposure state lives in GPU surfaces of the bloom object
// (PgLtiBloom_CreateSurfaces): _AdaptiveLuminanceSurface /
// _pPrevAdaptiveLuminanceSurface (1x1 fp16 RT textures, ping-pong) and
// g_pAdaptiveDest (1x1 RT, 0x0117529c). A device Reset re-creates them at
// their initial value, so after every switch the image flashes bright and
// slowly adapts back down. The carrier saves their contents right before it
// triggers the reset and writes them into the re-created surfaces before the
// next frame renders.
namespace mc2vr::adaptation {

// Attach, before the hooks: records g_pAdaptiveDest's slot.
void init();

// The bloom object's surface creation (hooks/adaptation.cpp): where it keeps
// the adaptive-luminance pair.
void on_adaptive_slot(void **holder);
void on_prev_adaptive_slot(void **holder);

// Main thread, right before a forced device reset (surfaces still alive).
void save();

// Device Reset completed (hooks/device.cpp, after the original Reset returns).
void on_reset_done();

// Main thread, at the start of each frame's pass 1 (before any rendering):
// after a completed Reset, restores a pending save into the re-created
// surfaces.
void on_frame_begin();

} // namespace mc2vr::adaptation

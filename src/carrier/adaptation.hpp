#pragma once

// HDR eye-adaptation continuity across carrier-driven device resets
// (render-size switches, docs/render_resolution.md track 4).
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

// Attach: MidHooks at the two creation log sites that hold the slot
// addresses (ESI / EDI) — records where the bloom object keeps the pair.
void install();

// Main thread, right before a forced device reset (surfaces still alive).
void save();

// Device Reset completed (device.cpp, after the original Reset returns).
void on_reset_done();

// Main thread, at the start of each frame's pass 1 (before any rendering):
// after a completed Reset, restores a pending save into the re-created
// surfaces.
void on_frame_begin();

} // namespace mc2vr::adaptation

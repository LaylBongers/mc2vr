// Sky_EmitRecords (0x0047f890) — reimplementation.
//
// STATUS: done. Transcribed from the disassembly (0x0047f890-0x00480163) and
// reviewed against it instruction by instruction (2026-11-16); live-verified
// outside the HMD (2026-11-16, debug_gui_census=clouds: the streams are the
// original's plus the window). Not compared against the original call for call.
// Ghidra plate: Sky_EmitRecords. Notes: docs/reverse_engineering/sky_clouds.md
// (the game side), docs/clouds.md (the fix).
// Original: thiscall(sky, camera world matrix), ret 4; a vtable slot
// (0x00bab634) on the producer side (the scene build), once per frame per view.
// Entry: game/sky.hpp's declaration (hooks/reimpl.cpp).
//
// With the scratch Submit's bucket override on (bucket 2) and the depth key
// 1, it emits:
//   A. a technique-less record: the view's viewport with MinZ = MaxZ = the
//      projected depth at 0.8 * far (the "depth window"), clip planes off;
//   B. the sky dome (PgSkyVP + the dome material; a 16-unit sphere at the
//      camera, placed at 0.8 * far by the window). With the sky flag clear
//      (*(g_GlobalCamCtx + 0x104) == 0) its stream also copies main into
//      ColorBufferCopy and uploads PgSkyFP's projectParams: the haze;
//   C. the sun (PgSunVP, a quad 16 units along the sun direction, x 0.8);
//   then, with the sky flag clear, cloud render 2 (the clouds object, +0x7e0);
//   D. a technique-less record restoring the viewport to z 0..1.
//
// Calls (original): ViewCtx_GetViewport, thunk_FUN_02537cb0 x4,
// PgPrimitiveScratchRecord_Submit x4, RenderShell vtable slot 17 x2,
// Matrix_OrientFromDirection, D3DXMatrixScaling, D3DXMatrixMultiply, the
// clouds object's vtable slot 2.
// Writes directly: the scratch constant and command buffers and their next
// counters, two instance matrices and g_InstanceMatrixCount, the scratch
// record's fields, g_ScratchBucketOverride, g_PrimSortSequence.
// Deviations: the fld/fstp float copies are plain copies (bit-identical
// except that x87 would quiet a signaling NaN).
//
// Fix (docs/clouds.md): the window is computed once and carried by every
// record drawn in it. Records B and C start with it (op 8), and cloud render
// 2 gets it as a parameter, called directly (reimpl::
// PgClouds_EmitCloudRender2InWindow) when the clouds object's slot 6 is the
// original's. The original sets it only in record A, and the sort can
// separate A from the others: cloud render 2 is type 8 while A-C are type 6,
// and the water's depth pass, which binds render targets and so resets the
// viewport, sorts in between. Records A and D are unchanged, so what sorts
// around the sky sees the original state.
#pragma once

#include <cstdint>

namespace mc2vr::game {
struct Technique;
struct ViewContext;
}

namespace mc2vr::reimpl {

// The sky's depth window: the view's viewport rect and the depth both MinZ
// and MaxZ get.
struct SkyDepthWindow {
    int32_t x, y, w, h;
    float z;
};

void Sky_EmitRecords(uint8_t *sky, const float *camera);

// True for the techniques whose instance matrix Sky_EmitRecords anchors at the
// camera (the dome, PgSkyVP, and the sun, PgSunVP; nothing else draws with
// them). VR: SubmitToGPU anchors those at each eye instead
// (view_rewrite::eye_anchored_instance, docs/clouds.md).
bool anchored_at_camera(const game::Technique *tech);

// "sky:" log line per 10 s window.
void sky_report_window();

} // namespace mc2vr::reimpl

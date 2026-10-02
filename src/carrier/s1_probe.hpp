// S1c — third revision of the S1 instrumentation. Run history:
//   S1  (run 1): consumer bracket + SetTransform/SetViewport logging + m[1]
//        residency patch + satellite classification. ANSWERS: consumer runs
//        at pipeline time; elements are {size,ptr} descriptors; SetTransform
//        NEVER called (shader-driven); satellite frames confirmed; the S0
//        counter decode was wrong.
//   S1b (run 2): SetVertexShaderConstantF (slot 94) classification + live-head
//        patch windows. ANSWERS: world-view element {0x30,0x810,0x680} VERIFIED
//        in the ring (staging/entry/ctx pointers all correct); ring position =
//        A.low16 % cap (advancing 624/frame in the boat scene; mid-frame reads
//        are VM scratch — high16 mutates transiently); GPU-bound matrices do
//        NOT exactly match any ViewEntry matrix (derived, not copied); patch
//        windows A(m[1])/B(staging slot) on the only rendered view produced
//        NO nudge -> neither channel feeds the draw camera. REGRESSION: the
//        per-group classification (up to 3.6M/10s x ~800 memcmps) halved the
//        frame rate -> S1c memoizes and budgets it.
//   S1c (run 3): performance fix (content-hash memo + per-frame
//        classification/region budgets); matched-tag logging (run 2's single
//        real match — 4x transposed on c12 — had no tag logged); patch
//        windows A-E across ALL walked views and ALL candidate camera
//        channels: A entry m[1][3], B camera staging slot pos (ctx+0xc2110),
//        C camera-record ring records (ctx+0xcb110, {pos,serial,rot,entry*,lod}
//        0x28-stride — the walk-time snapshot the VM consumer most plausibly
//        reads), D entry pos7c4[0], E entry m[0][12]; a full-ring SCAN for
//        {0x30,0x810,0x680} world elements (no more position guessing); and a
//        bounded one-shot matrix EXFIL (unique GPU matrices + walked-view
//        m[0]/m[1] hex + FOV dwords) for offline derivation analysis — the
//        GPU matrices are derived, so exact-match classification can only ever
//        attribute a subset; the exfil lets the analyzer search the
//        relationships offline.
//   S1g (run 7, this): run 6 was decisive — the GPU proof fired zero times
//        (no patched matrix ever reached the GPU) while unpatched entry m[0]
//        uploads do occur, so the VM consumer does NOT read the walked views'
//        camera data at all; the main camera was located on the GPU (c21
//        position + c23-c26 view matrix, dynamic per frame); static RE found
//        the upstream chain: ViewEntry.camData (entry+0x7ec) -> upstream
//        camera objects, FUN_0048f9d0 converts the entry quaternion
//        (+0x7d4) + pos7c4 into the matrices (entries are DERIVED state).
//        S1g adds window I (entry quaternion patch) and window J (camData
//        object pos-match patch), and exfils the camData objects + the
//        global camera chain for offline layout RE.
//   S1h (run 8, this): run 7 — no nudge; J starved (camData = parameters,
//        not pose); frame hitches at window cadence = patches landing in
//        game-consumed state but not the draw camera; no exact exfil hits.
//        S1h adds the walk-entry MidHook (SubmitWorldPackets entry
//        0x0048e620): window K patches ALL camera fields of live views
//        BEFORE the walk's copies; the vsclock control patches a view
//        proven to upload its m[0] and times the snapshot (vspatched-ctrl
//        = consume-time read; vsclean = snapshot predates the walk); a
//        24-frame burst correlates the c21-c26 GPU camera registers with
//        every live view's position.
// New MidHooks live here; the M3 handlers (render_dump.cpp) and the device
// VmtHook (device.cpp) feed this module via the note_*/on_* taps. Handlers
// run on the main thread; report_window() runs on the queue poller thread.
#pragma once

#include <cstdint>

namespace mc2vr::s1 {

// Installs the S1 MidHooks (pre-VM 0x004c99f9 + RenderFrame entry 0x00855690).
// Best-effort: failures are logged and don't stop the rest.
void install();

// Tap from render_dump.cpp's view-loop MidHook (0x0048e9ea): one call per
// walked view. `flags` = the full ViewRef+0x14 dword (low16 = type);
// `frame_ctx` = EBX at that site = the frame-ctx object, validated
// structurally before use.
void note_view(uint32_t idx, uint32_t type, uint32_t flags, uintptr_t frame_ctx);

// Tap from render_dump.cpp's opcode MidHook (0x008569f5): first stream
// command of each frame (bracket point 3).
void note_stream_opcode();

// Tap from render_dump.cpp's slot-4 handler (EndOfFrameHook): bracket point 4.
void note_end_of_frame();

// Taps from device.cpp's VmtHook handlers.
void on_set_transform(uint32_t state, const float *m);
void on_set_viewport(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                     float minz, float maxz);
void on_set_vs_constant(uint32_t start_register, const float *data, uint32_t vec4_count);

// 10s window report (called from render_dump.cpp's poller); resets the
// window aggregates. Process-lifetime one-shots are NOT reset.
void report_window();

} // namespace mc2vr::s1

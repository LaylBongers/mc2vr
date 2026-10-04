// S4-1/S4-2: OpenVR bridge — SteamVR connectivity + compositor table
// (docs/s4_handover.md, docs/stereo_design.md §S4).
//
// The i386 OpenVR API in this Proton prefix is `openvr_api_dxvk.dll`
// (syswow64) — Valve's drop-in OpenVR API replacement for 32-bit apps under
// Proton+DXVK; it owns the D3D9/DXVK texture-sharing path `IVRCompositor::Submit`
// needs in S4-2. `C:\vrclient\vrclient.dll` is the raw vrclient bridge with
// no VR_Init-style API — NOT a load target (settled 2026-10-04, export table
// verified).
//
// Stage 1 (conf openvr=on): connectivity smoke test + the facts S4-2/3 need
// (HMD identity, recommended target size, projection, pose sample). Stage 2
// (S4-2): also acquires the IVRCompositor FnTable for hmd_submit.cpp. Every
// failure is logged and the carrier stays fully functional without it.
#pragma once

#include "openvr_fntables.hpp"

namespace mc2vr::ovr {

// mc2vr.conf openvr=off|on (default off).
bool set_enabled(const char *value);

// mc2vr.conf openvr_init_registry=off|on (default off). In-game
// vrclient_init_registry is a LIVE-PROVEN HAZARD: it spawns a Background
// vrclient session whose delayed VR_Shutdown tears down our own init
// mid-use — both the bootstrap thread and the render thread froze, full
// game hang (2026-10-04 run 5, docs/s4_handover.md). Off = skip the call
// (VR_InitInternal2 may then fail with err 105; the carrier-time registry
// window is the real fix).
bool set_init_registry(const char *value);

// Bootstrap, called once from init() after conf load. If enabled: load the
// bridge DLL, VR_InitInternal2(Scene), fetch the IVRSystem + IVRCompositor
// FnTables, and log everything S4 needs (HMD identity, recommended target
// size, D3D9 adapter, projection, one pose sample). Fails soft on every step.
void init();

// True once the IVRSystem FnTable is live (S4-2/3 gate on this).
bool ready();

// The IVRCompositor_022 FnTable once acquired, else nullptr. Entries are
// ECX-preset thiscall thunks — call WITHOUT self (see openvr_fntables.hpp).
// Render-thread use only (Submit/PostPresentHandoff contract).
VRCompositor_FnTable_022 *compositor();

} // namespace mc2vr::ovr

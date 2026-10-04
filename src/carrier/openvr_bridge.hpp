// S4-1: OpenVR bridge — SteamVR compositor connectivity (docs/s4_handover.md,
// docs/stereo_design.md §S4).
//
// The i386 OpenVR API in this Proton prefix is `openvr_api_dxvk.dll`
// (syswow64) — Valve's drop-in OpenVR API replacement for 32-bit apps under
// Proton+DXVK; it owns the D3D9/DXVK texture-sharing path `IVRCompositor::Submit`
// will need in S4-2. `C:\vrclient\vrclient.dll` is the raw vrclient bridge with
// no VR_Init-style API — NOT a load target (settled 2026-10-04, export table
// verified).
//
// This module (Stage 1) only proves connectivity and harvests the facts S4-2/3
// need; it does not touch the render path. Enabled via mc2vr.conf openvr=on;
// every failure is logged and the carrier stays fully functional without it.
#pragma once

namespace mc2vr::ovr {

// mc2vr.conf openvr=off|on (default off).
bool set_enabled(const char *value);

// Bootstrap, called once from init() after conf load. If enabled: load the
// bridge DLL, VR_InitInternal2(Scene), fetch the IVRSystem FnTable, and log
// everything S4 needs (HMD identity, recommended target size, D3D9 adapter,
// projection, one pose sample). Fails soft on every step.
void init();

// True once the IVRSystem FnTable is live (S4-2/3 gate on this).
bool ready();

} // namespace mc2vr::ovr

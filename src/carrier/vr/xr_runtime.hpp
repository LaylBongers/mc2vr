// The OpenXR runtime, loaded into the game process without a loader
// (docs/xr_session.md): the runtime DLL is negotiated with directly
// (xrNegotiateLoaderRuntimeInterface) and every call goes through the
// xrGetInstanceProcAddr it returns.
//
//   - Under Wine: Proton's 32-bit wineopenxr.dll (its Unix side needs the
//     32-bit runtime registered for 32-bit processes, docs/openxr_proton.md
//     item 4).
//   - On Windows: the 32-bit active runtime's manifest — XR_RUNTIME_JSON, else
//     HKLM\SOFTWARE\Khronos\OpenXR\1\ActiveRuntime as a 32-bit process reads
//     it — and its library_path. Never run (docs/platforms.md).
#pragma once

#include "vr/xr_api.hpp"

namespace mc2vr::xr_runtime {

// Loads and negotiates once; logs the outcome. Null when no runtime.
PFN_xrGetInstanceProcAddr load();

} // namespace mc2vr::xr_runtime

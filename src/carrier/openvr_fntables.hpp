// S4: OpenVR FnTable layouts served by this prefix's Proton vrclient
// (docs/s4_handover.md). The POD types and enums come straight from the
// VENDORED, PINNED header — vendor/openvr/openvr_1.16.8.h — which is the
// layout source of truth for what this runtime serves:
//   - IVRSystem: 1.16.8's IVRSystem class IS the 022 layout (its
//     IVRSystem_Version is "IVRSystem_022"); live-proven in run 6b.
//   - IVRCompositor: the runtime serves 022, which is 1.16.8's IVRCompositor
//     (027) class TRUNCATED at 46 methods — 2026-10-04 RE of the builtin
//     vrclient.dll create_winIVRCompositor_IVRCompositor_022_FnTable
//     (0x1000e8c0, thunk array = 0x2e0 bytes = 46 entries) confirmed the
//     method order matches the header for slots 0..45 and NOTHING beyond:
//     SetStageOverride_Async .. GetPosesForFrame are 024+ additions and are
//     NOT served. Never touch slot 46+ of the compositor table (OOB read).
// The newer SDK 2.15 openvr.h (026/029 layouts) was deleted from vendor/ —
// the runtime does not serve those and the mismatch caused the original
// slot-28 crash folklore.
//
// ABI (live-proven run 5, docs/s4_handover.md item 1c): the FnTable entries
// Proton serves are THISCALL thunks that load the interface object into ECX
// THEMSELVES ("mov ecx, obj; mov edx, wrapper; jmp edx"). The first STACK
// argument is the method's first parameter — call entries WITHOUT self.
// Wrong-ABI calls shift every argument one slot and can watchdog-abort
// vrserver; they are a SAFETY requirement, not a nicety.
#pragma once

#include "../../vendor/openvr/openvr_1.16.8.h"

namespace mc2vr::ovr {

// IVRSystem_022 — exact 1.16.8 IVRSystem virtual-method order (28 slots).
struct VRSystem_FnTable_022 {
    void (*GetRecommendedRenderTargetSize)(uint32_t *w, uint32_t *h);             // 0
    void *GetProjectionMatrix;                                                    // 1
    void (*GetProjectionRaw)(vr::EVREye eye, float *left, float *right,
                             float *top, float *bottom);                          // 2
    void *ComputeDistortion;                                                      // 3
    void *GetEyeToHeadTransform;                                                  // 4 (struct return — i386 sret, S4-3)
    void *GetTimeSinceLastVsync;                                                  // 5
    int32_t (*GetD3D9AdapterIndex)();                                             // 6
    void *GetDXGIOutputInfo;                                                      // 7
    void *GetOutputDevice;                                                        // 8
    void *IsDisplayOnDesktop;                                                     // 9
    void *SetDisplayVisibility;                                                   // 10
    void (*GetDeviceToAbsoluteTrackingPose)(vr::ETrackingUniverseOrigin origin,
                                            float predicted_seconds,
                                            vr::TrackedDevicePose_t *poses,
                                            uint32_t count);                     // 11
    void *GetSeatedZeroPoseToStandingAbsoluteTrackingPose;                        // 12
    void *GetRawZeroPoseToStandingAbsoluteTrackingPose;                           // 13
    void *GetSortedTrackedDeviceIndicesOfClass;                                   // 14
    void *GetTrackedDeviceActivityLevel;                                          // 15
    void *ApplyTransform;                                                         // 16
    void *GetTrackedDeviceIndexForControllerRole;                                 // 17
    void *GetControllerRoleForTrackedDeviceIndex;                                 // 18
    int32_t (*GetTrackedDeviceClass)(uint32_t index);                             // 19
    void *IsTrackedDeviceConnected;                                               // 20
    void *GetBoolTrackedDeviceProperty;                                           // 21
    float (*GetFloatTrackedDeviceProperty)(uint32_t index,
                                           int32_t prop, int32_t *error);        // 22
    int32_t (*GetInt32TrackedDeviceProperty)(uint32_t index,
                                             int32_t prop, int32_t *error);      // 23
    void *GetUint64TrackedDeviceProperty;                                         // 24
    void *GetMatrix34TrackedDeviceProperty;                                       // 25
    void *GetArrayTrackedDeviceProperty;                                          // 26
    uint32_t (*GetStringTrackedDeviceProperty)(uint32_t index,
                                               int32_t prop, char *value,
                                               uint32_t size,
                                               int32_t *error);                  // 27
};

static_assert(__builtin_offsetof(VRSystem_FnTable_022, GetProjectionRaw) ==
              2 * sizeof(void *), "IVRSystem_022 slot 2 (GetProjectionRaw)");
static_assert(__builtin_offsetof(VRSystem_FnTable_022, GetD3D9AdapterIndex) ==
              6 * sizeof(void *), "IVRSystem_022 slot 6 (GetD3D9AdapterIndex)");
static_assert(__builtin_offsetof(VRSystem_FnTable_022,
                                 GetDeviceToAbsoluteTrackingPose) ==
              11 * sizeof(void *),
              "IVRSystem_022 slot 11 (GetDeviceToAbsoluteTrackingPose)");
static_assert(__builtin_offsetof(VRSystem_FnTable_022,
                                 GetStringTrackedDeviceProperty) ==
              27 * sizeof(void *),
              "IVRSystem_022 slot 27 (GetStringTrackedDeviceProperty)");

// IVRCompositor_022 — 46 slots; exact 1.16.8 IVRCompositor method order
// truncated at IsCurrentSceneFocusAppLoading (RE-verified against the builtin
// builder 2026-10-04; see the file comment). Only the entries S4 calls are
// typed; the rest are placeholders so the used slots sit at the right
// indices.
struct VRCompositor_FnTable_022 {
    void *SetTrackingSpace;                                                       // 0
    void *GetTrackingSpace;                                                       // 1
    int32_t (*WaitGetPoses)(vr::TrackedDevicePose_t *render_poses,
                            uint32_t render_count,
                            vr::TrackedDevicePose_t *game_poses,
                            uint32_t game_count);                                 // 2
    void *GetLastPoses;                                                           // 3
    void *GetLastPoseForTrackedDeviceIndex;                                       // 4
    vr::EVRCompositorError (*Submit)(vr::EVREye eye, const vr::Texture_t *texture,
                                     const vr::VRTextureBounds_t *bounds,
                                     vr::EVRSubmitFlags flags);                   // 5
    void *ClearLastSubmittedFrame;                                                // 6
    void (*PostPresentHandoff)();                                                 // 7
    void *GetFrameTiming;                                                         // 8
    void *GetFrameTimings;                                                        // 9
    void *GetFrameTimeRemaining;                                                  // 10
    void *GetCumulativeStats;                                                     // 11
    void *FadeToColor;                                                            // 12
    void *GetCurrentFadeColor;                                                    // 13
    void *FadeGrid;                                                               // 14
    void *GetCurrentGridAlpha;                                                   // 15
    void *SetSkyboxOverride;                                                      // 16
    void *ClearSkyboxOverride;                                                    // 17
    void *CompositorBringToFront;                                                 // 18
    void *CompositorGoToBack;                                                     // 19
    void *CompositorQuit;                                                         // 20
    void *IsFullscreen;                                                           // 21
    void *GetCurrentSceneFocusProcess;                                            // 22
    void *GetLastFrameRenderer;                                                   // 23
    void *CanRenderScene;                                                         // 24
    void *ShowMirrorWindow;                                                       // 25
    void *HideMirrorWindow;                                                       // 26
    void *IsMirrorWindowVisible;                                                 // 27
    void *CompositorDumpImages;                                                   // 28
    void *ShouldAppRenderWithLowResources;                                        // 29
    void *ForceInterleavedReprojectionOn;                                         // 30
    void *ForceReconnectProcess;                                                  // 31
    void *SuspendRendering;                                                      // 32
    void *GetMirrorTextureD3D11;                                                  // 33
    void *ReleaseMirrorTextureD3D11;                                              // 34
    void *GetMirrorTextureGL;                                                     // 35
    void *ReleaseSharedGLTexture;                                                 // 36
    void *LockGLSharedTextureForAccess;                                           // 37
    void *UnlockGLSharedTextureForAccess;                                         // 38
    uint32_t (*GetVulkanInstanceExtensionsRequired)(char *value, uint32_t size);  // 39
    uint32_t (*GetVulkanDeviceExtensionsRequired)(void *physical_device,
                                                 char *value, uint32_t size);    // 40
    void *SetExplicitTimingMode;                                                  // 41
    void *SubmitExplicitTimingData;                                               // 42
    void *IsMotionSmoothingEnabled;                                              // 43
    void *IsMotionSmoothingSupported;                                             // 44
    void *IsCurrentSceneFocusAppLoading;                                         // 45
};

static_assert(__builtin_offsetof(VRCompositor_FnTable_022, Submit) ==
              5 * sizeof(void *), "IVRCompositor_022 slot 5 (Submit)");
static_assert(__builtin_offsetof(VRCompositor_FnTable_022, PostPresentHandoff) ==
              7 * sizeof(void *), "IVRCompositor_022 slot 7 (PostPresentHandoff)");
static_assert(__builtin_offsetof(VRCompositor_FnTable_022,
                                 GetVulkanInstanceExtensionsRequired) ==
              39 * sizeof(void *),
              "IVRCompositor_022 slot 39 (GetVulkanInstanceExtensionsRequired)");
static_assert(__builtin_offsetof(VRCompositor_FnTable_022,
                                 GetVulkanDeviceExtensionsRequired) ==
              40 * sizeof(void *),
              "IVRCompositor_022 slot 40 (GetVulkanDeviceExtensionsRequired)");
static_assert(sizeof(VRCompositor_FnTable_022) == 46 * sizeof(void *),
              "IVRCompositor_022 FnTable = exactly 46 served slots");

// FnTable version strings the layouts above match.
inline constexpr const char *kIVRSystem_Version = "IVRSystem_022";
inline constexpr const char *kIVRCompositor_Version = "IVRCompositor_022";

} // namespace mc2vr::ovr

// RenderShell (the render shell object and its per-view helpers): globals and
// the original functions the reimplemented code calls.
#pragma once

#include <cstdint>

#include <windows.h>

#include "game/dx9.hpp"
#include "game/function.hpp"
#include "game/global.hpp"
#include "game/pg_primitive.hpp"

namespace mc2vr::game {

struct RenderShell;  // layout: Ghidra /RenderPath (not needed here yet)

using g_RenderShellPtr = Global<RenderShell *, 0x00dfb2fcu>;  // -> g_RenderShell 0x017ceaf0
using g_CurViewType = Global<uint8_t, 0x01169792u>;
using g_HalfResPassAllowed = Global<uint8_t, 0x01175616u>;
using g_HalfResPassActive = Global<uint8_t, 0x01175617u>;

inline constexpr Function<void(RenderShell *shell, uint32_t slot, uint32_t view)> RenderShell_BindViewTexture{
    "RenderShell_BindViewTexture", 0x00749230u, {EAX, ECX, EDI}};
inline constexpr Function<void()> RenderShell_ViewType9_StretchBlit{"RenderShell_ViewType9_StretchBlit",
                                                                     0x0074adf0u};
inline constexpr Function<void()> RenderShell_BeginHalfResPass{"RenderShell_BeginHalfResPass", 0x0074b3e0u};

// ---- the render shell object and its submit-time vtable slots ----------------------
// At frame time g_RenderShell holds the BASE LtiRenderer_vtbl (0x00bd38e8).
constexpr uintptr_t kRenderShellObject = 0x017ceaf0u;  // g_RenderShell (g_RenderShellPtr points here)
inline RenderShell *render_shell() { return reinterpret_cast<RenderShell *>(kRenderShellObject); }

// Slot 15 LtiRenderer_BeginSubmit (Present prev, BeginScene, GPU sync, clear). AL = go.
// Reimplemented (reimpl/render/begin_submit.hpp); this declares its entry.
inline constexpr Function<bool(RenderShell *self)> LtiRenderer_BeginSubmit{"LtiRenderer_BeginSubmit", 0x0074aaa0u,
                                                                           {ECX}};
// Slot 22 LtiRenderer_EndSubmit 0x0074ac30 (EndScene).
inline constexpr Method<bool(RenderShell *)> LtiRenderer_EndSubmit{"LtiRenderer_EndSubmit", 0x58, {ECX}};

// One frame's GPU submission; called by RenderShell_RenderFrameTimed.
// Reimplemented (reimpl/render/submit_to_gpu.hpp); this declares its entry.
inline constexpr Function<void()> PgPrimitive_SubmitToGPU{"PgPrimitive_SubmitToGPU", 0x00855690u};

// ---- per-frame submit state -----------------------------------------------------------
using g_InRenderFrame = Global<uint8_t, 0x01175615u>;
using g_NameHash_PgPrimitiveSubmitToGPU = Global<uint32_t, 0x0198cf50u>;  // lazy profiler id
using g_PgPrimitiveScratchRecord = Global<uint32_t, 0x01163700u>;        // 0 = nothing built yet
using g_SuppressPresent = Global<uint8_t, 0x011755c3u>;
using g_CurPassObject = Global<uint32_t, 0x0117561cu>;
using g_LastRecViewFlags = Global<uint16_t, 0x00ce7d20u>;  // 0xff = none
using DAT_00ce7d24 = Global<uint16_t, 0x00ce7d24u>;        // reset together with g_LastRecViewFlags
using DAT_00dfb363 = Global<uint8_t, 0x00dfb363u>;         // EffectPass.stencilFlag.x = 1 when set
// Zeroed at each submit start; roles unknown.
using DAT_01163764 = Global<uint32_t, 0x01163764u>;
using DAT_011697c8 = Global<uint32_t, 0x011697c8u>;
using DAT_0116376c = Global<uint32_t, 0x0116376cu>;
using DAT_011697c0 = Global<uint32_t, 0x011697c0u>;
using DAT_00ff36fc = Global<uint32_t, 0x00ff36fcu>;

// Precache rendering (asset warm-up frames at load).
using g_PrecacheRequested = Global<uint8_t, 0x0117560fu>;
using DAT_0117560c = Global<uint8_t, 0x0117560cu>;         // precache allowed
using g_PrecacheStarted = Global<uint8_t, 0x0117560eu>;
using g_PrecacheComplete = Global<uint8_t, 0x0117560du>;
using g_PrecacheDisplayIndex = Global<uint32_t, 0x01175610u>;
inline constexpr Function<bool()> RenderShell_PrecacheLoadStep{"RenderShell_PrecacheLoadStep", 0x007519a0u};
inline constexpr Function<void()> RenderShell_PrecacheFinish{"RenderShell_PrecacheFinish", 0x00751980u};

// Named-texture registry (64-bit name hashes), guarded by a critical section.
using g_TextureRegistry = Global<void *, 0x01175630u>;
constexpr uintptr_t kTextureRegistryLock = 0x01173ffcu;  // g_TextureRegistryLock (CRITICAL_SECTION)
inline CRITICAL_SECTION *texture_registry_lock() { return reinterpret_cast<CRITICAL_SECTION *>(kTextureRegistryLock); }
// Returns the entry (+0x14 u16 kind, +0x1c the bindable texture) or NULL. Callee pops the key pointer.
inline constexpr Function<uint8_t *(void *table, const uint64_t *key)> HashTable_FindByHash64{
    "HashTable_FindByHash64", 0x00874370u, {EDI, STK}};

// ---- record walk helpers -----------------------------------------------------------
inline constexpr Function<void(void *cache_block)> RenderShell_ResetDrawStateCache{
    "RenderShell_ResetDrawStateCache", 0x00853e90u, {EAX}};
// Switches the view type (render targets, viewport, states); resets the
// draw-state caches on a change and writes g_CurViewType.
inline constexpr Function<void(uint32_t view_type)> RenderShell_SetViewType{"RenderShell_SetViewType", 0x00856820u,
                                                                            {STK}};
inline constexpr Function<EffectPass *(PgMaterial *material, uint32_t screen_offset)> RenderShell_PickPass_Material{
    "RenderShell_PickPass_Material", 0x00858590u, {STK, STK}};
inline constexpr Function<EffectPass *(PgMaterial *material)> RenderShell_PickPass_0_4{"RenderShell_PickPass_0_4",
                                                                                         0x008587a0u, {EDI}};
inline constexpr Function<EffectPass *(uint8_t view_type, PgMaterial *material)> RenderShell_PickPass_1_3_7{
    "RenderShell_PickPass_1_3_7", 0x00858870u, {AL, EDI}};
inline constexpr Function<void(PgPrimitive *record)> RenderShell_BindRecordStreams{"RenderShell_BindRecordStreams",
                                                                                   0x00856770u, {ESI}};
inline constexpr Function<void(PgPrimitive *record)> RenderShell_DrawRecordPrimitives{
    "RenderShell_DrawRecordPrimitives", 0x008566d0u, {EAX}};
inline constexpr Function<void()> RenderShell_EndHalfResPass{"RenderShell_EndHalfResPass", 0x0074b560u};

// The game's own strings PgPrimitive_SubmitToGPU passes (by address).
using s_PgPrimitive_SubmitToGPU = Global<const char, 0x00be85b0u>;      // "PgPrimitive::SubmitToGPU"
using s_FinishedPrecacheRender = Global<const char, 0x00be85ccu>;       // "- Finished PrecacheRender %i\n"
using s_PrecacheRenderingComplete = Global<const char, 0x00be85ecu>;    // "Precache Rendering Complete.\n"

// RenderShell fields (the view-type-7 frame end, BeginSubmit).
constexpr uintptr_t kRenderShellDevice = 0x2d28;          // IDirect3DDevice9 * (BeginSubmit's query device)
constexpr uintptr_t kRenderShellRT0Surface = 0x3e90;      // IDirect3DSurface9 *
constexpr uintptr_t kRenderShellDepthSurface = 0x3ea0;    // IDirect3DSurface9 *
constexpr uintptr_t kRenderShellSkipClear = 0x4448;       // u32; nonzero = BeginSubmit does not clear

// ---- BeginSubmit -------------------------------------------------------------------
using g_InD3DScene = Global<uint8_t, 0x0117526cu>;  // set after BeginScene succeeds

} // namespace mc2vr::game

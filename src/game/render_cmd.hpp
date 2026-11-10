// Render command streams: the dword command format each PgPrimitive carries
// (cmdStream, +0x2c), its globals, and the helper functions its executor
// RenderCmd_ExecuteStream (0x008569d0) calls.
// Note: docs/reverse_engineering/render_cmd.md; opcode table on the Ghidra
// plate of RenderCmd_ExecuteStream.
//
// A stream is a sequence of commands, each an opcode dword followed by its
// operands, ended by op 0. Field names marked aN have unknown roles.
#pragma once

#include <d3d9.h>

#include <cstdint>

#include "game/dx9.hpp"
#include "game/function.hpp"
#include "game/global.hpp"

namespace mc2vr::game {

// ---- command format ------------------------------------------------------------

enum class RenderCmdOp : uint32_t {
    End = 0,
    SetRenderState = 1,
    SetVSConstants = 2,
    SetPSConstants = 3,
    BindVertexTexture = 4,
    SetStreamSource = 5,
    SetRenderTargetAndDepth = 6,
    SetRenderTarget = 7,
    SetScreenConstants = 8,
    SetClipPlane = 9,
    ReapplyClipPlanes = 10,
    DisableClipPlanes = 11,
    ViewType9StretchBlit = 12,
    StretchRectFromGlobal = 13,
    SetCullMode = 14,
    Clear = 15,
    BeginSurfacePass = 16,
    EndSurfacePass = 17,
    BindTextureAndSamplers = 18,
    Nop = 19,
    BindViewTexture = 20,
    Op21 = 21,
    StretchRect = 22,
    VmOp23 = 23,
    UnbindTextureStage = 24,
    BeginHalfRes = 25,
    EndHalfRes = 26,
};
constexpr uint32_t kRenderCmdOpCount = 27;

namespace rcmd {

struct End { RenderCmdOp op; };
struct SetRenderState { RenderCmdOp op; D3DRENDERSTATETYPE state; uint32_t value; };
struct SetShaderConstants { RenderCmdOp op; uint32_t start_reg; const float *data; uint32_t vec4_count; };
// slot + 0x101 = D3DVERTEXTEXTURESAMPLER0 + slot.
struct BindVertexTexture { RenderCmdOp op; uint32_t slot; uint32_t view; };
struct SetStreamSource { RenderCmdOp op; uint32_t stream; IDirect3DVertexBuffer9 *buffer; };  // null = unbind
struct SetRenderTargetAndDepth { RenderCmdOp op; IDirect3DSurface9 *color0; IDirect3DSurface9 *depth; };
struct SetRenderTarget { RenderCmdOp op; uint32_t index; IDirect3DSurface9 *surface; };
struct SetScreenConstants { RenderCmdOp op; uint32_t a1, a2, a3, a4; float a5, a6; };
struct SetClipPlane { RenderCmdOp op; uint32_t index; const float *plane; };  // null plane = disable
struct ReapplyClipPlanes { RenderCmdOp op; };
struct DisableClipPlanes { RenderCmdOp op; };
struct ViewType9StretchBlit { RenderCmdOp op; };
struct StretchRectFromGlobal { RenderCmdOp op; uint32_t a1; };
struct SetCullMode { RenderCmdOp op; uint32_t cull_mode; };  // D3DCULL, applied by the pass pickers
struct Clear { RenderCmdOp op; D3DCOLOR color; DWORD flags; };
struct SurfacePass { RenderCmdOp op; uint32_t a1, a2, a3; uint32_t gate; };  // runs when gate is 1 or 2
struct BindTextureAndSamplers { RenderCmdOp op; uint32_t unused[2]; };    // payload not read
struct Nop { RenderCmdOp op; uint32_t unused; };
struct BindViewTexture { RenderCmdOp op; uint32_t slot; uint32_t view; };
struct Op21 { RenderCmdOp op; uint32_t a1; };
struct StretchRect { RenderCmdOp op; IDirect3DSurface9 *src; IDirect3DSurface9 *dst; };
struct VmOp23 { RenderCmdOp op; uint32_t a1; uint32_t a2; const uint32_t *obj; };  // obj[4] (+0x10) is passed
// stage + 0xff as the sampler; then render state 154 with
// 0x71ff0050 | stage (the pattern of ATI's R2VB sampler mapping — unverified).
struct UnbindTextureStage { RenderCmdOp op; uint32_t stage; };
struct BeginHalfRes { RenderCmdOp op; };
struct EndHalfRes { RenderCmdOp op; };

} // namespace rcmd

// Command size in dwords, opcode included.
constexpr uint8_t kRenderCmdSize[kRenderCmdOpCount] = {
    sizeof(rcmd::End) / 4, sizeof(rcmd::SetRenderState) / 4, sizeof(rcmd::SetShaderConstants) / 4,
    sizeof(rcmd::SetShaderConstants) / 4, sizeof(rcmd::BindVertexTexture) / 4, sizeof(rcmd::SetStreamSource) / 4,
    sizeof(rcmd::SetRenderTargetAndDepth) / 4, sizeof(rcmd::SetRenderTarget) / 4,
    sizeof(rcmd::SetScreenConstants) / 4, sizeof(rcmd::SetClipPlane) / 4, sizeof(rcmd::ReapplyClipPlanes) / 4,
    sizeof(rcmd::DisableClipPlanes) / 4, sizeof(rcmd::ViewType9StretchBlit) / 4,
    sizeof(rcmd::StretchRectFromGlobal) / 4, sizeof(rcmd::SetCullMode) / 4, sizeof(rcmd::Clear) / 4,
    sizeof(rcmd::SurfacePass) / 4, sizeof(rcmd::SurfacePass) / 4, sizeof(rcmd::BindTextureAndSamplers) / 4,
    sizeof(rcmd::Nop) / 4, sizeof(rcmd::BindViewTexture) / 4, sizeof(rcmd::Op21) / 4, sizeof(rcmd::StretchRect) / 4,
    sizeof(rcmd::VmOp23) / 4, sizeof(rcmd::UnbindTextureStage) / 4, sizeof(rcmd::BeginHalfRes) / 4,
    sizeof(rcmd::EndHalfRes) / 4,
};
// The original's per-handler stream advance (disassembly), as a layout check.
static_assert(kRenderCmdSize[1] == 3 && kRenderCmdSize[2] == 4 && kRenderCmdSize[5] == 3 &&
              kRenderCmdSize[8] == 7 && kRenderCmdSize[15] == 3 && kRenderCmdSize[16] == 5 &&
              kRenderCmdSize[18] == 3 && kRenderCmdSize[19] == 2 && kRenderCmdSize[23] == 4 &&
              kRenderCmdSize[24] == 2 && kRenderCmdSize[26] == 1);

// ---- globals --------------------------------------------------------------------

using g_LastExecuteStream = Global<const uint32_t *, 0x011697b8u>;  // dedupe
using g_ClipPlaneEnableMask = Global<uint32_t, 0x01169784u>;        // D3DRS_CLIPPLANEENABLE shadow
using g_PassCullMode = Global<uint32_t, 0x017d2ac0u>;        // RenderShell+0x3fd0: D3DRS_CULLMODE the pass pickers apply; op 14, surface passes
// Source path string Dx9_CheckHResult reports ("D:\projects\Mercs2_PC\Odin\Win32...").
using s_OdinWin32SourcePath = Global<const char, 0x00be860cu>;

// ---- functions --------------------------------------------------------------------

inline constexpr Function<void(const uint32_t *stream, void *vs_ctx, void *ps_ctx)> RenderCmd_ExecuteStream{
    "RenderCmd_ExecuteStream", 0x008569d0u, {STK, STK, STK}};

inline constexpr Function<void()> RenderCmd_ResetPassState{"RenderCmd_ResetPassState", 0x00858980u};
inline constexpr Function<void(uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, float a5, float a6,
                               void *vs_ctx, void *ps_ctx)>
    RenderCmd_SetScreenConstants{"RenderCmd_SetScreenConstants", 0x00852740u,
                                 {EAX, ECX, STK, EDI, XMM0, STK, STK, STK}};
inline constexpr Function<void(uint32_t a1)> RenderCmd_StretchRectFromGlobal{"RenderCmd_StretchRectFromGlobal",
                                                                             0x0074aec0u, {EDX}};
inline constexpr Function<void(HRESULT hr)> RenderCmd_DumpTargets_Debug{"RenderCmd_DumpTargets_Debug", 0x007533c0u,
                                                                        {ESI}};
inline constexpr Function<void(uint32_t a1, uint32_t a2, uint32_t a3)> RenderCmd_BeginSurfacePass{
    "RenderCmd_BeginSurfacePass", 0x00756420u, {STK, STK, EAX}};
inline constexpr Function<void(uint32_t a1, uint32_t a2, uint32_t a3)> RenderCmd_EndSurfacePass{
    "RenderCmd_EndSurfacePass", 0x00756590u, {ECX, EDX, STK}};
inline constexpr Function<void()> RenderCmd_BindTextureAndSamplers{"RenderCmd_BindTextureAndSamplers", 0x00756390u};
inline constexpr Function<void(uint32_t a1)> Dx9_ClearTexture{"Dx9_ClearTexture", 0x0074af30u, {EAX}};
// Op 23: real SecuROM VM bytecode behind the thunk (callable, not hookable).
inline constexpr Function<void(uint32_t a1, uint32_t a1_minus_1, uint32_t obj_10, uint32_t a2)> thunk_FUN_0256af90{
    "thunk_FUN_0256af90", 0x00482b00u, {EDI, ECX, EAX, STK}};

} // namespace mc2vr::game

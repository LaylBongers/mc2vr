// The engine's Dx9 state layer (Dx9_*) and the device getter, as typed
// original functions (function.hpp). Ghidra names; locations from the
// disassembly of each function's entry and of its callers.
#pragma once

#include <d3d9.h>

#include <cstdint>

#include "game/function.hpp"
#include "game/global.hpp"

namespace mc2vr::game {

struct LtiRenderer;  // layout: Ghidra (only the fields below are used here)

using g_LtiRenderer = Global<uint8_t *, 0x01175288u>;  // LtiRenderer*; NULL until the engine builds it
constexpr uintptr_t kLtiRendererDevice = 0x5bc;       // IDirect3DDevice9 * (dx9State)
constexpr uintptr_t kLtiRendererFlags = 0x5e4;        // u32; bit 4 = occlusion queries on

// The device as the engine reads it inline: g_LtiRenderer ? ->dx9State : NULL.
inline IDirect3DDevice9 *lti_device()
{
    uint8_t *const lti = g_LtiRenderer::get();
    return lti != nullptr ? *reinterpret_cast<IDirect3DDevice9 **>(lti + kLtiRendererDevice) : nullptr;
}

// VM-slot thunk (calling it is fine, hooking it is not — securom_vm.md).
inline constexpr Function<IDirect3DDevice9 *()> GetD3DDevice{"GetD3DDevice", 0x0047f2f0u};

inline constexpr Function<void(D3DRENDERSTATETYPE state, uint32_t value)> Dx9_SetRenderState{
    "Dx9_SetRenderState", 0x004843e0u, {STK, STK}};

inline constexpr Function<void(uint32_t start_reg, const float *data, uint32_t vec4_count)>
    Dx9_SetVertexShaderConstantF_Direct{"Dx9_SetVertexShaderConstantF_Direct", 0x00749010u, {STK, STK, EDX}};
inline constexpr Function<void(uint32_t start_reg, const float *data, uint32_t vec4_count)>
    Dx9_SetPixelShaderConstantF_Direct{"Dx9_SetPixelShaderConstantF_Direct", 0x007515d0u, {STK, STK, EDX}};

// The stream number is read both from ECX and from the stack.
inline constexpr Function<void(uint32_t stream_ecx, IDirect3DVertexBuffer9 *buffer, uint32_t stream, uint32_t offset)>
    Dx9_SetStreamSource{"Dx9_SetStreamSource", 0x00752c50u, {ECX, EAX, STK, STK}};
// Unbinding variant (thunk 0x004842b0 -> 0x00541166).
inline constexpr Function<void(uint32_t stream_ecx, uint32_t edx_zero, uint32_t stream, uint32_t a, uint32_t b)>
    Dx9_SetStreamSourceNull{"Dx9_SetStreamSourceNull", 0x004842b0u, {ECX, EDX, STK, STK, STK}};

inline constexpr Function<void(uint32_t index, IDirect3DSurface9 *surface)> Dx9_SetRenderTarget{
    "Dx9_SetRenderTarget", 0x00749160u, {STK, EDI}};
inline constexpr Function<void(IDirect3DSurface9 *surface, uint32_t eax_zero)> Dx9_SetDepthStencilSurface{
    "Dx9_SetDepthStencilSurface", 0x00749080u, {ECX, EAX}};
inline constexpr Function<void(uint32_t index, const float *plane)> Dx9_SetClipPlaneCached{
    "Dx9_SetClipPlaneCached", 0x00751740u, {EAX, STK}};
inline constexpr Function<void(uint32_t sampler, uint32_t stage, IDirect3DBaseTexture9 *texture)> Dx9_SetTexture{
    "Dx9_SetTexture", 0x00484350u, {ECX, ESI, STK}};

// IDirect3DDevice9::Clear argument order, Stencil in EDX.
inline constexpr Function<HRESULT(DWORD count, const D3DRECT *rects, DWORD flags, D3DCOLOR color, float z,
                                  DWORD stencil)>
    thunk_Dx9_Clear_Direct{"thunk_Dx9_Clear_Direct", 0x00748f10u, {STK, STK, STK, STK, STK, EDX}};

// Logs a failed HRESULT with its source location; AL = failed.
inline constexpr Function<bool(HRESULT hr, const char *file, uint32_t line)> Dx9_CheckHResult{
    "Dx9_CheckHResult", 0x0074d620u, {opaque(ESI), STK, STK}};

// A shader-constant binding inside a technique/pass object: register range.
// count == 0 = not used by this shader (every upload is gated on it).
struct ShaderConstParam {
    uint32_t start_reg;
    uint32_t count;
};
static_assert(sizeof(ShaderConstParam) == 8);

// dev->Set{Vertex,Pixel}ShaderConstant{F,B}(param.start_reg, data, param.count);
// EAX = &param, callee pops the data pointer.
inline constexpr Function<void(const ShaderConstParam *param, const void *data)> Dx9_SetVertexShaderConstantF{
    "Dx9_SetVertexShaderConstantF", 0x00749200u, {EAX, STK}};
inline constexpr Function<void(const ShaderConstParam *param, const void *data)> Dx9_SetPixelShaderConstantF{
    "Dx9_SetPixelShaderConstantF", 0x0084f150u, {EAX, STK}};
inline constexpr Function<void(const ShaderConstParam *param, const void *data)> Dx9_SetPixelShaderConstantB{
    "Dx9_SetPixelShaderConstantB", 0x00853e60u, {EAX, STK}};

// Device methods the engine calls directly (vtable offsets from d3d9.h order).
inline constexpr Method<HRESULT(IDirect3DDevice9 *, IDirect3DVertexShader9 *shader)> Device_SetVertexShader{
    "IDirect3DDevice9::SetVertexShader", 0x170};
inline constexpr Method<HRESULT(IDirect3DDevice9 *, UINT start_reg, const float *data, UINT vec4_count)>
    Device_SetVertexShaderConstantF{"IDirect3DDevice9::SetVertexShaderConstantF", 0x178};
inline constexpr Method<HRESULT(IDirect3DDevice9 *, IDirect3DIndexBuffer9 *indices)> Device_SetIndices{
    "IDirect3DDevice9::SetIndices", 0x1a0};
inline constexpr Method<HRESULT(IDirect3DDevice9 *, UINT start_reg, const float *data, UINT vec4_count)>
    Device_SetPixelShaderConstantF{"IDirect3DDevice9::SetPixelShaderConstantF", 0x1b4};
inline constexpr Method<HRESULT(IDirect3DQuery9 *, DWORD flags)> Query_Issue{"IDirect3DQuery9::Issue", 0x18};

inline constexpr Method<HRESULT(IDirect3DDevice9 *, IDirect3DSurface9 *src, const RECT *src_rect,
                                IDirect3DSurface9 *dst, const RECT *dst_rect, D3DTEXTUREFILTERTYPE filter)>
    Device_StretchRect{"IDirect3DDevice9::StretchRect", 0x88};

} // namespace mc2vr::game

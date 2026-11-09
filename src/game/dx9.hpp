// The engine's Dx9 state layer (Dx9_*) and the device getter, as typed
// original functions (function.hpp). Ghidra names; locations from the
// disassembly of each function's entry and of its callers.
#pragma once

#include <d3d9.h>

#include <cstdint>

#include "game/function.hpp"

namespace mc2vr::game {

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

inline constexpr Method<HRESULT(IDirect3DDevice9 *, IDirect3DSurface9 *src, const RECT *src_rect,
                                IDirect3DSurface9 *dst, const RECT *dst_rect, D3DTEXTUREFILTERTYPE filter)>
    Device_StretchRect{"IDirect3DDevice9::StretchRect", 0x88};

} // namespace mc2vr::game

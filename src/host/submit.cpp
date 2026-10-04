#include "submit.hpp"

#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>

#include <cstring>

#include "log.hpp"

namespace sub {

namespace {

ID3D11Device* g_dev = nullptr;
ID3D11VertexShader* g_vs = nullptr;
ID3D11PixelShader* g_ps = nullptr;
ID3D11SamplerState* g_sam = nullptr;
bool g_ok = false;

// Fullscreen triangle (SV_VertexID, no vertex buffer). UV (0,0) lands at the
// viewport's top-left so the source's row 0 is the top row (D3D convention).
const char* HLSL = R"(
struct VSOut {
    float2 uv : TEXCOORD0;
    float4 pos : SV_Position;
};
VSOut vs(uint id : SV_VertexID) {
    VSOut o;
    o.uv = float2((id << 1) & 2, id & 2);          // (0,0) (2,0) (0,2)
    o.pos = float4(o.uv.x * 2 - 1, 1 - o.uv.y * 2, 0, 1);
    return o;
}
Texture2D tex : register(t0);
SamplerState sam : register(s0);
float4 ps(VSOut i) : SV_Target {
    // Alpha forced opaque: the shared X8R8G8B8 source opens as
    // B8G8R8X8 (alpha reads 0xFF, probe-proven) and the projection
    // layer blends OPAQUE anyway — never trust the source's X channel.
    return float4(tex.Sample(sam, i.uv).rgb, 1.0);
}
)";

HRESULT compile(const char* entry, const char* target, ID3DBlob** out) {
    typedef HRESULT(WINAPI * D3DCompileFn)(LPCVOID, SIZE_T, LPCSTR,
                                           const D3D10_SHADER_MACRO*,
                                           ID3DInclude*, LPCSTR, LPCSTR,
                                           UINT, UINT, ID3DBlob**, ID3DBlob**);
    static D3DCompileFn fn = nullptr;
    if (fn == nullptr) {
        HMODULE m = LoadLibraryW(L"d3dcompiler_47.dll");
        if (m == nullptr) {
            hostlog::write("submit: d3dcompiler_47.dll not found (%lu) — "
                           "shared-eye submission off", GetLastError());
            return E_FAIL;
        }
        fn = (D3DCompileFn)GetProcAddress(m, "D3DCompile");
        if (fn == nullptr) {
            hostlog::write("submit: D3DCompile not exported — submission off");
            return E_FAIL;
        }
    }
    ID3DBlob* err = nullptr;
    HRESULT hr = fn(HLSL, strlen(HLSL), nullptr, nullptr, nullptr, entry, target,
                    0, 0, out, &err);
    if (FAILED(hr)) {
        hostlog::write("submit: compile %s failed hr=0x%08lx: %.*s", entry,
                       (unsigned long)hr,
                       err ? (int)err->GetBufferSize() : 0,
                       err ? (const char*)err->GetBufferPointer() : "");
    }
    if (err) err->Release();
    return hr;
}

}  // namespace

bool init(ID3D11Device* dev) {
    if (g_ok) return true;
    if (dev == nullptr) return false;
    g_dev = dev;

    ID3DBlob* vb = nullptr;
    ID3DBlob* pb = nullptr;
    if (FAILED(compile("vs", "vs_4_0", &vb)) || FAILED(compile("ps", "ps_4_0", &pb))) {
        if (vb) vb->Release();
        if (pb) pb->Release();
        return false;  // reason already logged
    }
    HRESULT hr = g_dev->CreateVertexShader(vb->GetBufferPointer(),
                                           vb->GetBufferSize(), nullptr, &g_vs);
    if (SUCCEEDED(hr)) {
        hr = g_dev->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(),
                                      nullptr, &g_ps);
    }
    vb->Release();
    pb->Release();
    if (FAILED(hr)) {
        hostlog::write("submit: shader creation failed hr=0x%08lx",
                       (unsigned long)hr);
        return false;
    }

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(g_dev->CreateSamplerState(&sd, &g_sam))) {
        hostlog::write("submit: sampler creation failed");
        return false;
    }

    g_ok = true;
    hostlog::write("submit: blit shaders ready");
    return true;
}

bool ready() { return g_ok; }

void draw(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* srv,
          ID3D11RenderTargetView* rtv, uint32_t srcW, uint32_t srcH,
          uint32_t dstW, uint32_t dstH) {
    if (!g_ok || ctx == nullptr || srv == nullptr || rtv == nullptr) return;
    if (srcW == 0 || srcH == 0 || dstW == 0 || dstH == 0) return;

    const float black[4] = {0.f, 0.f, 0.f, 1.f};
    ctx->ClearRenderTargetView(rtv, black);

    // Aspect-preserving fit, centered (2560x1440 source in a ~2016x2240 eye
    // leaves letterbox bars top/bottom).
    const float scale =
        ((float)dstW / srcW < (float)dstH / srcH) ? (float)dstW / srcW
                                                  : (float)dstH / srcH;
    const float fitW = srcW * scale, fitH = srcH * scale;
    D3D11_VIEWPORT vp = {(dstW - fitW) * 0.5f, (dstH - fitH) * 0.5f, fitW, fitH,
                         0.f, 1.f};

    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ctx->RSSetViewports(1, &vp);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(g_vs, nullptr, 0);
    ctx->PSSetShader(g_ps, nullptr, 0);
    ctx->PSSetShaderResources(0, 1, &srv);
    ctx->PSSetSamplers(0, 1, &g_sam);
    ctx->Draw(3, 0);

    // Unbind everything we touched — the mirror's staging reads and any
    // future consumer must not inherit this pass's bindings.
    ID3D11ShaderResourceView* nullSrv = nullptr;
    ctx->PSSetShaderResources(0, 1, &nullSrv);
    ID3D11SamplerState* nullSam = nullptr;
    ctx->PSSetSamplers(0, 1, &nullSam);
    ID3D11RenderTargetView* nullRtv = nullptr;
    ctx->OMSetRenderTargets(1, &nullRtv, nullptr);
}

}  // namespace sub

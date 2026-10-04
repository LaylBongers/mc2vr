#include "mock.hpp"

#include <cmath>

#include "d3d.hpp"
#include "eyes.hpp"
#include "log.hpp"
#include "pose.hpp"

#include <windows.h>

namespace mock {

namespace {

constexpr UINT kW = 512, kH = 512;
constexpr float kIpd = 0.064f;

HmdFrame synth(unsigned n) {
    const float t = n / 90.0f;
    HmdFrame f;
    f.tracked = true;
    f.displayTime = (long long)(t * 1e9);
    const float yaw = 0.3f * std::sin(t * 0.5f);
    Quat q{0, std::sin(yaw / 2), 0, std::cos(yaw / 2)};
    for (int e = 0; e < 2; ++e) {
        f.eye[e].pos = {(e ? 0.5f : -0.5f) * kIpd, 1.7f + 0.02f * std::sin(t), 0};
        f.eye[e].rot = q;
        f.eye[e].fov = {-0.8f, 0.8f, 0.8f, -0.8f};
    }
    return f;
}

}  // namespace

int run(int frames) {
    d3d::Device d;
    if (!d3d::create(nullptr, D3D_FEATURE_LEVEL_11_0, d)) return 1;

    ID3D11Texture2D* tex[2] = {};
    ID3D11RenderTargetView* rtv[2] = {};
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = kW;
    td.Height = kH;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    for (int e = 0; e < 2; ++e) {
        if (FAILED(d.dev->CreateTexture2D(&td, nullptr, &tex[e])) ||
            FAILED(d.dev->CreateRenderTargetView(tex[e], nullptr, &rtv[e]))) {
            hostlog::write("mock: eye texture %d creation failed", e);
            return 1;
        }
    }

    hostlog::write("mock: ready (no OpenXR runtime; synthetic pose, %ux%u eye textures)", kW, kH);

    for (unsigned n = 0; frames == 0 || n < (unsigned)frames; ++n) {
        const HmdFrame f = synth(n);
        for (int e = 0; e < 2; ++e) eyes::draw_pattern(d.ctx, rtv[e], e, n);
        d.ctx->Flush();
        if (n % 90 == 0)
            hostlog::write("mock: frame %u head=(%.3f %.3f %.3f) qy=%.3f", n,
                           (f.eye[0].pos.x + f.eye[1].pos.x) / 2, f.eye[0].pos.y, f.eye[0].pos.z,
                           f.eye[0].rot.y);
        Sleep(11);
    }
    hostlog::write("mock: done");
    return 0;
}

}  // namespace mock

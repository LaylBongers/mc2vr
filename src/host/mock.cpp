#include "mock.hpp"

#include <cmath>

#include "d3d.hpp"
#include "eyes.hpp"
#include "ipc.hpp"
#include "log.hpp"
#include "pose.hpp"
#include "submit.hpp"

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

// True when the host should stop: Shutdown command from the carrier, or the
// registered carrier (game) process died.
bool stop_requested() {
    Mc2IpcMsg m;
    while (ipc::pop_command(&m)) {
        if (m.type == MC2VR_CMD_SHUTDOWN) {
            hostlog::write("mock: Shutdown command from carrier (pid %u)",
                           ipc::carrier_pid());
            return true;
        }
        if (m.type == MC2VR_CMD_CONFIG) {
            hostlog::write("mock: ring config %ux%u fmt %u noted (no mirror in "
                           "mock)", m.a, m.b, m.c);
        } else if (m.type == MC2VR_CMD_FRAME_READY) {
            // Mock has no D3D11 mirror; log the first one only (the selftest
            // probe sends handle 0 as a shape check).
            static uint64_t seen = 0;
            if (seen++ < 3)
                hostlog::write("mock: FRAME_READY frame=%llu handle=0x%llx eye=%u "
                               "slot=%u %ux%u (not opened in mock)",
                               (unsigned long long)m.x, (unsigned long long)m.y,
                               m.b, m.a, m.c, m.d);
        } else {
            hostlog::write("mock: unexpected command %u ignored", m.type);
        }
    }
    return ipc::carrier_died(1000);
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

    // S4-3 smoke check: compile the blit shaders + run one draw in the same
    // prefix the real host uses, so the SELFTEST — not the first HMD run —
    // catches a missing d3dcompiler_47 or a broken view cast.
    if (sub::init(d.dev)) {
        eyes::draw_pattern(d.ctx, rtv[1], 1, 0);
        ID3D11ShaderResourceView* srv = nullptr;
        if (SUCCEEDED(d.dev->CreateShaderResourceView(tex[1], nullptr, &srv))) {
            sub::draw(d.ctx, srv, rtv[0], kW, kH, kW, kH, false);
            srv->Release();
        }
        hostlog::write("mock: submit blit shaders ready (draw smoke ok)");
    }

    // Canonical ready marker (launcher/selftest wait for this line) + a
    // synthetic session ramp so the event ring has realistic content.
    hostlog::write("mc2vr_host: ready (mock mode; no OpenXR runtime — synthetic "
                   "pose, %ux%u eye textures)", kW, kH);
    ipc::push_event(MC2VR_MSG_SESSION_STATE, MC2VR_XR_SESSION_VISIBLE, 0);

    uint32_t sessionState = MC2VR_XR_SESSION_VISIBLE;
    unsigned visibleAfter = 45;  // ~0.5s of frames then FOCUSED

    for (unsigned n = 0; frames == 0 || n < (unsigned)frames; ++n) {
        if (stop_requested()) break;

        if (visibleAfter && n >= visibleAfter) {
            sessionState = MC2VR_XR_SESSION_FOCUSED;
            ipc::push_event(MC2VR_MSG_SESSION_STATE, MC2VR_XR_SESSION_FOCUSED, 0);
            visibleAfter = 0;
        }

        const HmdFrame f = synth(n);
        for (int e = 0; e < 2; ++e) eyes::draw_pattern(d.ctx, rtv[e], e, n);
        d.ctx->Flush();
        ipc::publish(f, kIpd, sessionState, 0, n);
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

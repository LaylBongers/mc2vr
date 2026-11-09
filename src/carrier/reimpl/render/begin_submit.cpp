// LtiRenderer_BeginSubmit — see begin_submit.hpp.
//
// Follows the original block by block (VA ranges in the comments).
// Deviations:
//   - Failures are logged by us, not through Lti_DebugPrintf.
//   - The query pointer starts as NULL; the original's slot starts as
//     whatever ECX held (`push ecx` reserves it). They differ only if
//     CreateQuery fails without writing it, which DXVK never does.

#include "reimpl/render/begin_submit.hpp"

#include <windows.h>
#include <d3d9.h>

#include <cstdint>

#include "game/dx9.hpp"
#include "game/render_shell.hpp"
#include "log.hpp"
#include "vr/pacing.hpp"

namespace mc2vr::reimpl {

using namespace game;

namespace {

template <class T>
T shell_field(RenderShell *self, uintptr_t offset)
{
    return *reinterpret_cast<T *>(reinterpret_cast<uint8_t *>(self) + offset);
}

// Waits until the GPU has finished everything submitted so far
// (0x74ab23-0x74ab9b).
void drain_gpu(RenderShell *self)
{
    IDirect3DDevice9 *const dev = shell_field<IDirect3DDevice9 *>(self, kRenderShellDevice);
    IDirect3DQuery9 *query = nullptr;
    dev->CreateQuery(D3DQUERYTYPE_EVENT, &query);
    if (query == nullptr) {
        return;
    }
    query->Issue(D3DISSUE_END);
    HRESULT hr = query->GetData(nullptr, 0, D3DGETDATA_FLUSH);
    // VR: the replay pass reports the GPU idle and skips the drain
    // (docs/pacing.md).
    if (hr == S_FALSE && pacing::on_drain_check_busy()) {
        hr = S_OK;
    }
    while (hr == S_FALSE) {
        Sleep(0);
        hr = query->GetData(nullptr, 0, D3DGETDATA_FLUSH);
    }
    query->Release();
}

} // namespace

bool LtiRenderer_BeginSubmit(RenderShell *self)
{
    // ---- already in a scene 0x74aaa1-0x74aabe ----
    if (g_InD3DScene::get() != 0) {
        MC2VR_LOG("BeginSubmit: called when already in a scene");
        return false;
    }

    // ---- present the previous frame 0x74aabf-0x74aac8 ----
    if (g_SuppressPresent::get() == 0) {
        lti_device()->Present(nullptr, nullptr, nullptr, nullptr);
    }

    // ---- BeginScene 0x74aacd-0x74ab1c ----
    HRESULT hr = lti_device()->BeginScene();
    if (FAILED(hr)) {
        MC2VR_LOG("BeginSubmit: BeginScene failed (hr %08lx)", (unsigned long)hr);
    } else {
        g_InD3DScene::set(1);
    }

    drain_gpu(self);

    // ---- targets and clear 0x74aba3-0x74ac21 ----
    Dx9_SetRenderTarget(0, shell_field<IDirect3DSurface9 *>(self, kRenderShellRT0Surface));
    Dx9_SetDepthStencilSurface(shell_field<IDirect3DSurface9 *>(self, kRenderShellDepthSurface), 0);
    if (shell_field<uint32_t>(self, kRenderShellSkipClear) == 0) {
        hr = lti_device()->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0, 1.0f, 0);
        if (FAILED(hr)) {
            MC2VR_LOG("BeginSubmit: Clear failed (hr %08lx)", (unsigned long)hr);
        }
    }
    return true;
}

} // namespace mc2vr::reimpl

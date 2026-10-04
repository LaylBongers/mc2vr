#include "d3d.hpp"

#include "log.hpp"

namespace d3d {

bool create(const LUID* luid, D3D_FEATURE_LEVEL minLevel, Device& out) {
    IDXGIAdapter1* adapter = nullptr;
    if (luid) {
        IDXGIFactory1* factory = nullptr;
        if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory))) {
            for (UINT i = 0; factory->EnumAdapters1(i, &adapter) == S_OK; ++i) {
                DXGI_ADAPTER_DESC1 d;
                adapter->GetDesc1(&d);
                if (d.AdapterLuid.LowPart == luid->LowPart &&
                    d.AdapterLuid.HighPart == luid->HighPart) {
                    hostlog::write("d3d11: adapter %u '%ls' (LUID match)", i, d.Description);
                    break;
                }
                adapter->Release();
                adapter = nullptr;
            }
            factory->Release();
        }
        if (!adapter) hostlog::write("d3d11: runtime LUID adapter not found, using default");
    }

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0,
                                        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL got{};
    HRESULT hr = D3D11CreateDevice(adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
                                   nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                                   sizeof levels / sizeof *levels, D3D11_SDK_VERSION, &out.dev, &got,
                                   &out.ctx);
    if (adapter) adapter->Release();
    if (FAILED(hr)) {
        hostlog::write("d3d11: D3D11CreateDevice failed hr=0x%08lx", (unsigned long)hr);
        return false;
    }
    if (got < minLevel) {
        hostlog::write("d3d11: feature level 0x%x below runtime minimum 0x%x", got, minLevel);
        return false;
    }
    hostlog::write("d3d11: device ok, feature level 0x%x", got);
    return true;
}

}  // namespace d3d

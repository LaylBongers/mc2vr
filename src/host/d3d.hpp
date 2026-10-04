#pragma once

#include <d3d11.h>
#include <dxgi.h>

namespace d3d {

struct Device {
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
};

// `luid` (optional) selects the adapter the OpenXR runtime requires;
// null = default adapter. Returns false and logs on failure.
bool create(const LUID* luid, D3D_FEATURE_LEVEL minLevel, Device& out);

}  // namespace d3d

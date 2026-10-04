#include "eyes.hpp"

#include <cmath>

namespace eyes {

void draw_pattern(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, int eye, unsigned n) {
    const float pulse = 0.5f + 0.5f * std::sin(n * 0.05f);
    const float c[4] = {eye == 0 ? 0.3f + 0.5f * pulse : 0.05f, 0.1f,
                        eye == 1 ? 0.3f + 0.5f * pulse : 0.05f, 1.0f};
    ctx->ClearRenderTargetView(rtv, c);
}

}  // namespace eyes

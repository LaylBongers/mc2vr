// Hooks for vr/render_size (render-size policy).
#pragma once

namespace mc2vr::hooks {

// The OnScreenResize entry/epilogue and SetupRenderTargets store/chain-desc
// MidHooks — installed when render_size is set
// (without render_size only the entry, forwarding to the probe). Call after
// the conf is parsed.
void install_render_size();

} // namespace mc2vr::hooks

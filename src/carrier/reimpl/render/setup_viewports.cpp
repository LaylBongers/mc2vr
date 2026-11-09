// RenderShell_SetupViewports — see setup_viewports.hpp. Follows the original
// (0x0085a9b0-0x0085ab46) branch by branch; deviations: none intended.

#include "reimpl/render/setup_viewports.hpp"

#include <cstring>

#include "game/render_shell.hpp"
#include "vr/hud.hpp"

namespace mc2vr::reimpl {

namespace {

// [0x00bea694]: the original's 1/480 (float). A float product of two 24-bit
// mantissas is exact in x87's extended precision, so `(float)h * kInv480`
// rounds once, to the same bits as the original's cvtsi2ss + mulss.
constexpr float kInv480 = 0.0020833334f;

void set_i32(uint8_t *shell, uintptr_t off, int32_t v)
{
    std::memcpy(shell + off, &v, 4);
}
void set_f32(uint8_t *shell, uintptr_t off, float v)
{
    std::memcpy(shell + off, &v, 4);
}
int32_t get_i32(const uint8_t *shell, uintptr_t off)
{
    int32_t v;
    std::memcpy(&v, shell + off, 4);
    return v;
}
void set_u16(uint8_t *shell, uintptr_t off, uint16_t v)
{
    std::memcpy(shell + off, &v, 2);
}

} // namespace

void RenderShell_SetupViewports(uint32_t mode, uint8_t *shell)
{
    const int32_t W = get_i32(shell, 0x2b98), H = get_i32(shell, 0x2b9c);

    if ((mode & 0xffff) != 2) {
        // ---- single view 0x85a9c7-0x85aa19 ----
        set_u16(shell, 0x2b90, 1);
        set_i32(shell, 0xea0, 0);
        set_i32(shell, 0xea4, 0);
        set_i32(shell, 0xea8, W);
        set_i32(shell, 0xeac, H);
        set_f32(shell, 0xeb4, (float)H * kInv480);
        set_i32(shell, 0x1d20, 0);
        set_i32(shell, 0x1d28, 0);
        set_i32(shell, 0x1d2c, 0);
        set_f32(shell, 0x1d34, 0.0f);
        set_i32(shell, 0x1d24, 0);
    } else {
        set_u16(shell, 0x2b90, 2);
        if (get_i32(shell, 0x2bc4) == 0) {
            // ---- split, top/bottom halves 0x85aa2d-0x85aa9f ----
            const int32_t h2 = H / 2;
            set_i32(shell, 0xeac, h2);
            set_i32(shell, 0xea8, W);
            set_i32(shell, 0xea0, 0);
            set_i32(shell, 0xea4, 0);
            set_f32(shell, 0xeb4, (float)h2 * kInv480);
            set_i32(shell, 0x1d20, 0);
            set_i32(shell, 0x1d24, h2);
            set_i32(shell, 0x1d28, W);
            set_i32(shell, 0x1d2c, h2);
            set_f32(shell, 0x1d34, (float)h2 * kInv480);
        } else {
            // ---- split, left/right halves 0x85aaa1-0x85ab0f ----
            const int32_t w2 = W / 2;
            set_i32(shell, 0xea8, w2);
            set_i32(shell, 0xeac, H);
            set_i32(shell, 0xea0, 0);
            set_i32(shell, 0xea4, 0);
            set_f32(shell, 0xeb4, (float)H * kInv480);
            set_i32(shell, 0x1d2c, H);
            set_i32(shell, 0x1d20, w2);
            set_i32(shell, 0x1d28, w2);
            set_f32(shell, 0x1d34, (float)H * kInv480);
            set_i32(shell, 0x1d24, 0);
        }
    }

    // ---- the main viewport, block 0 0x85ab15-0x85ab42 ----
    set_i32(shell, 0x20, 0);
    set_i32(shell, 0x24, 0);
    set_i32(shell, 0x28, W);
    set_i32(shell, 0x2c, H);
    set_f32(shell, 0x34, (float)H * kInv480);
    uint16_t n;
    std::memcpy(&n, shell + 0x2b90, 2);
    set_u16(shell, 0x2b90, (uint16_t)(n + 1));
    shell[0x3c] = 0;

    hud::on_viewports_set(shell);  // VR: the HUD layer's UI scale / aspect mode
}

} // namespace mc2vr::reimpl

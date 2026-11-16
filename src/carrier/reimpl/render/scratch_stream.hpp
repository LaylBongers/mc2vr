// The scratch command-buffer writer shared by the reimplemented emitters: one
// record's RenderCmd stream (opcodes: docs/reverse_engineering/render_cmd.md,
// game/render_cmd.hpp). The originals write the same dwords inline; the
// stream's commands are published to g_ScratchCmdNext only by end(), as the
// originals store the counter after the last command.
#pragma once

#include <bit>
#include <cstdint>

#include "game/dx9.hpp"
#include "game/view_context.hpp"

namespace mc2vr::reimpl {

class ScratchStream {
public:
    ScratchStream() : base_(game::g_ScratchCmdBase::get()), start_(game::g_ScratchCmdNext::get()), n_(start_) {}

    void op(uint32_t v) { base_[n_++] = v; }
    void ptr(const void *p) { op((uint32_t)(uintptr_t)p); }
    void f(float v) { op(std::bit_cast<uint32_t>(v)); }

    // op 6: RT0 and depth (null = none).
    void target(const void *color, const void *depth)
    {
        op(6);
        ptr(color);
        ptr(depth);
    }
    // op 8: the viewport (and the screen constants derived from it).
    void viewport(int32_t x, int32_t y, int32_t w, int32_t h, float min_z = 0.0f, float max_z = 1.0f)
    {
        op(8);
        op((uint32_t)x);
        op((uint32_t)y);
        op((uint32_t)w);
        op((uint32_t)h);
        f(min_z);
        f(max_z);
    }
    // op 2 (VS) / op 3 (PS): `p`'s registers from `data`, the count clamped
    // to `max_count` by a signed compare (the originals' cmp/cmovl). Nothing
    // when the shader lacks the constant (count 0).
    void upload(uint32_t opcode, const game::ShaderConstParam &p, const void *data, int32_t max_count)
    {
        if (p.count == 0) return;
        const int32_t c = (int32_t)p.count;
        op(opcode);
        op(p.start_reg);
        ptr(data);
        op((uint32_t)(c < max_count ? c : max_count));
    }
    // End of stream: publishes the used dwords, returns the stream.
    uint32_t *end()
    {
        op(0);
        game::g_ScratchCmdNext::set(n_);
        return base_ + start_;
    }
    uint32_t *base() const { return base_; }

private:
    uint32_t *base_;
    uint32_t start_, n_;
};

} // namespace mc2vr::reimpl

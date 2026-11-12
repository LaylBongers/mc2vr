// RenderCmd_ExecuteStream — see render_cmd.hpp.
//
// Handler by handler as in the original's jump table (0x856d40). Deviations:
//   - an opcode > 26 stops the stream and is logged, instead of spinning
//     forever;
//   - op 8 passes its floats as bits; the original moves a6 through x87,
//     which would quiet a signaling-NaN payload;
//   - leftover registers from earlier handlers are not passed on (they only
//     reach failure-logging paths: Dx9_CheckHResult,
//     RenderCmd_StretchRectFromGlobal).

#include "reimpl/render/render_cmd.hpp"

#include "game/dx9.hpp"
#include "game/render_cmd.hpp"
#include "game/render_shell.hpp"
#include "debug/render_dump.hpp"
#include "log.hpp"
#include "vr/view_rewrite.hpp"

namespace mc2vr::reimpl {

using namespace game;

namespace {

template <class Cmd>
const Cmd &as(const uint32_t *c)
{
    return *reinterpret_cast<const Cmd *>(c);
}

} // namespace

void RenderCmd_ExecuteStream(const uint32_t *stream, void *vs_ctx, void *ps_ctx)
{
    if (stream == nullptr || stream == g_LastExecuteStream::get()) {
        return;
    }
    g_LastExecuteStream::set(stream);

    for (const uint32_t *c = stream;; ) {
        render::on_render_command(c);  // debug tap
        if (c[0] >= kRenderCmdOpCount) {
            static int logged = 0;
            if (logged < 8) {
                logged++;
                MC2VR_LOG("reimpl: RenderCmd_ExecuteStream: bad opcode %u at %p (stream %p) — stream stopped "
                          "(the original would hang)",
                          (unsigned)c[0], (const void *)c, (const void *)stream);
            }
            return;
        }
        const auto op = static_cast<RenderCmdOp>(c[0]);
        switch (op) {
        case RenderCmdOp::End:
            return;

        case RenderCmdOp::SetRenderState: {
            const auto &cmd = as<rcmd::SetRenderState>(c);
            Dx9_SetRenderState(cmd.state, cmd.value);
            RenderCmd_ResetPassState();
            break;
        }
        case RenderCmdOp::SetVSConstants: {
            const auto &cmd = as<rcmd::SetShaderConstants>(c);
            Dx9_SetVertexShaderConstantF_Direct(cmd.start_reg, cmd.data, cmd.vec4_count);
            break;
        }
        case RenderCmdOp::SetPSConstants: {
            const auto &cmd = as<rcmd::SetShaderConstants>(c);
            // VR: the projected shadow's ViewToLocal per eye (docs/view_rewrite.md).
            const float *data = view_rewrite::on_record_ps_constant(cmd.start_reg, cmd.data, cmd.vec4_count);
            Dx9_SetPixelShaderConstantF_Direct(cmd.start_reg, data, cmd.vec4_count);
            break;
        }
        case RenderCmdOp::BindVertexTexture: {
            const auto &cmd = as<rcmd::BindVertexTexture>(c);
            RenderShell_BindViewTexture(g_RenderShellPtr::get(), D3DVERTEXTEXTURESAMPLER0 + cmd.slot, cmd.view);
            break;
        }
        case RenderCmdOp::SetStreamSource: {
            const auto &cmd = as<rcmd::SetStreamSource>(c);
            if (cmd.buffer != nullptr) {
                Dx9_SetStreamSource(cmd.stream, cmd.buffer, cmd.stream, 0);
            } else {
                Dx9_SetStreamSourceNull(cmd.stream, 0, cmd.stream, 0, 0);
            }
            break;
        }
        case RenderCmdOp::SetRenderTargetAndDepth: {
            const auto &cmd = as<rcmd::SetRenderTargetAndDepth>(c);
            Dx9_SetRenderTarget(0, cmd.color0);
            Dx9_SetDepthStencilSurface(cmd.depth, 0);
            break;
        }
        case RenderCmdOp::SetRenderTarget: {
            const auto &cmd = as<rcmd::SetRenderTarget>(c);
            Dx9_SetRenderTarget(cmd.index, cmd.surface);
            break;
        }
        case RenderCmdOp::SetScreenConstants: {
            const auto &cmd = as<rcmd::SetScreenConstants>(c);
            RenderCmd_SetScreenConstants(cmd.a1, cmd.a2, cmd.a3, cmd.a4, cmd.a5, cmd.a6, vs_ctx, ps_ctx);
            break;
        }
        case RenderCmdOp::SetClipPlane: {
            const auto &cmd = as<rcmd::SetClipPlane>(c);
            const uint32_t bit = 1u << (cmd.index & 31);
            uint32_t mask = g_ClipPlaneEnableMask::get();
            mask = cmd.plane != nullptr ? (mask | bit) : (mask & ~bit);
            g_ClipPlaneEnableMask::set(mask);
            Dx9_SetRenderState(D3DRS_CLIPPLANEENABLE, mask);
            if (cmd.plane != nullptr) {
                Dx9_SetClipPlaneCached(cmd.index, cmd.plane);
            }
            break;
        }
        case RenderCmdOp::ReapplyClipPlanes:
            Dx9_SetRenderState(D3DRS_CLIPPLANEENABLE, g_ClipPlaneEnableMask::get());
            break;
        case RenderCmdOp::DisableClipPlanes:  // the mask global is left as is
            Dx9_SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
            break;
        case RenderCmdOp::ViewType9StretchBlit:
            RenderShell_ViewType9_StretchBlit();
            break;
        case RenderCmdOp::StretchRectFromGlobal:
            RenderCmd_StretchRectFromGlobal(as<rcmd::StretchRectFromGlobal>(c).a1);
            break;
        case RenderCmdOp::SetCullMode:
            g_PassCullMode::set(as<rcmd::SetCullMode>(c).cull_mode);
            RenderCmd_ResetPassState();
            break;
        case RenderCmdOp::Clear: {
            const auto &cmd = as<rcmd::Clear>(c);
            const HRESULT hr = thunk_Dx9_Clear_Direct(0, nullptr, cmd.flags, cmd.color, 1.0f, 0);
            if (Dx9_CheckHResult(hr, s_OdinWin32SourcePath::ptr(), 0x9a7)) {
                RenderCmd_DumpTargets_Debug(hr);
            }
            break;
        }
        case RenderCmdOp::BeginSurfacePass: {
            const auto &cmd = as<rcmd::SurfacePass>(c);
            if (cmd.gate == 1 || cmd.gate == 2) {
                RenderCmd_BeginSurfacePass(cmd.a1, cmd.a2, cmd.a3);
            }
            break;
        }
        case RenderCmdOp::EndSurfacePass: {
            const auto &cmd = as<rcmd::SurfacePass>(c);
            if (cmd.gate == 1 || cmd.gate == 2) {
                RenderCmd_EndSurfacePass(cmd.a1, cmd.a2, cmd.a3);
            }
            break;
        }
        case RenderCmdOp::BindTextureAndSamplers:
            RenderCmd_BindTextureAndSamplers();
            break;
        case RenderCmdOp::Nop:
            break;
        case RenderCmdOp::BindViewTexture: {
            const auto &cmd = as<rcmd::BindViewTexture>(c);
            RenderShell_BindViewTexture(g_RenderShellPtr::get(), cmd.slot, cmd.view);
            break;
        }
        case RenderCmdOp::Op21:
            Dx9_ClearTexture(as<rcmd::Op21>(c).a1);
            break;
        case RenderCmdOp::StretchRect: {
            const auto &cmd = as<rcmd::StretchRect>(c);
            IDirect3DDevice9 *device = GetD3DDevice();
            const HRESULT hr = Device_StretchRect(device, cmd.src, nullptr, cmd.dst, nullptr, D3DTEXF_NONE);
            Dx9_CheckHResult(hr, s_OdinWin32SourcePath::ptr(), 0xa83);  // result unused
            break;
        }
        case RenderCmdOp::VmOp23: {
            const auto &cmd = as<rcmd::VmOp23>(c);
            thunk_FUN_0256af90(cmd.a1, cmd.a1 - 1, cmd.obj[4], cmd.a2);
            break;
        }
        case RenderCmdOp::UnbindTextureStage: {
            const uint32_t stage = as<rcmd::UnbindTextureStage>(c).stage;
            Dx9_SetTexture(stage + 0xff, stage, nullptr);
            Dx9_SetRenderState(D3DRS_POINTSIZE, 0x71ff0050u | (stage & 0xf));
            break;
        }
        case RenderCmdOp::BeginHalfRes: {
            const bool begin = g_CurViewType::get() == 8;
            g_HalfResPassAllowed::set(1);
            if (begin) {
                RenderShell_BeginHalfResPass();
                g_HalfResPassActive::set(1);
            }
            break;
        }
        case RenderCmdOp::EndHalfRes:
            g_HalfResPassAllowed::set(0);
            break;
        }
        c += kRenderCmdSize[c[0]];
    }
}

} // namespace mc2vr::reimpl

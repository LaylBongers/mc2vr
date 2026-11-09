// Shadow verification of the RenderCmd_ExecuteStream reimplementation
// (reimpl/render/render_cmd.cpp) against the original (debug_reimpl_shadow=on).

#include <array>
#include <cstdio>

#include "game/dx9.hpp"
#include "game/render_cmd.hpp"
#include "game/render_shell.hpp"
#include "debug/shadow/harness.hpp"
#include "debug/shadow/shadow.hpp"
#include "reimpl/render/render_cmd.hpp"

namespace mc2vr::shadow {

namespace {

using namespace game;

struct ExecuteStreamShadow {
    static constexpr const char *kName = "ExecuteStream";
    static constexpr uintptr_t kOriginal = 0x008569d0u;
    static constexpr uintptr_t kOriginalEnd = 0x00856d3fu;   // one past the final RET
    static constexpr uintptr_t kCallerReturn = 0x008562c3u;  // after the call in PgPrimitive_SubmitToGPU

    static constexpr std::array kCaptures = {
        at_entry(Dx9_SetRenderState),
        at_entry(RenderCmd_ResetPassState),
        at_entry(Dx9_SetVertexShaderConstantF_Direct),
        at_entry(Dx9_SetPixelShaderConstantF_Direct),
        at_entry(Dx9_SetStreamSource),
        at_entry(Dx9_SetStreamSourceNull),
        at_entry(RenderShell_BindViewTexture),
        at_entry(Dx9_SetRenderTarget),
        at_entry(Dx9_SetDepthStencilSurface),
        at_entry(RenderCmd_SetScreenConstants),
        at_entry(Dx9_SetClipPlaneCached),
        at_entry(RenderShell_ViewType9_StretchBlit),
        at_entry(RenderCmd_StretchRectFromGlobal),
        at_entry(thunk_Dx9_Clear_Direct),
        at_entry(Dx9_CheckHResult),
        at_entry(RenderCmd_DumpTargets_Debug),
        at_entry(RenderCmd_BeginSurfacePass),
        at_entry(RenderCmd_EndSurfacePass),
        at_entry(RenderCmd_BindTextureAndSamplers),
        at_entry(FUN_0074af30),
        at_entry(Dx9_SetTexture),
        at_entry(RenderShell_BeginHalfResPass),
        at_call(0x00856c7du, GetD3DDevice),        // op 22 (VM-slot thunk: no entry hook)
        at_call(0x00856c93u, Device_StretchRect),  // op 22 `call edx`
        at_call(0x00856cc1u, thunk_FUN_0256af90),  // op 23 (VM bytecode)
    };

    static constexpr std::array kState = {
        state<g_LastExecuteStream>("last"),
        state<g_ClipPlaneEnableMask>("clip"),
        // Also written by RenderCmd_BeginSurfacePass (1) and EndSurfacePass (2).
        state<g_RenderCmdPassObject>("pass", false),
        state<g_HalfResPassAllowed>("halfAllowed"),
        state<g_HalfResPassActive>("halfActive"),
    };

    static void run(const uint32_t *args)
    {
        reimpl::RenderCmd_ExecuteStream(reinterpret_cast<const uint32_t *>(args[0]), reinterpret_cast<void *>(args[1]),
                                        reinterpret_cast<void *>(args[2]));
    }

    static void describe(const uint32_t *args, char *out, size_t size)
    {
        const auto *stream = reinterpret_cast<const uint32_t *>(args[0]);
        int n = snprintf(out, size, "stream=%p head:", (const void *)stream);
        for (int i = 0; i < 10 && stream != nullptr && n > 0 && (size_t)n < size; i++) {
            n += snprintf(out + n, size - n, " %08x", (unsigned)stream[i]);
        }
    }
};

} // namespace

void install()
{
    const bool ok = Harness<ExecuteStreamShadow>::install();
    MC2VR_LOG("shadow: RenderCmd_ExecuteStream %s", ok ? "verifying" : "install FAILED");
}

} // namespace mc2vr::shadow

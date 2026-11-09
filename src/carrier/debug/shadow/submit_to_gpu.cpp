// Paired verification of the PgPrimitive_SubmitToGPU reimplementation
// (reimpl/render/submit_to_gpu.cpp), debug_reimpl=shadow:PgPrimitive_SubmitToGPU.
//
// The dry run of harness.hpp does not fit this function: its callees change
// state it reads right after (RenderShell_SetViewType and
// ResetDrawStateCache reset the draw-state caches the dirty mask is computed
// from). Instead the stereo replay's two passes — identical records, each
// pass resetting its own caches — run the original for pass 1 and ours for
// pass 2, both for real:
//   pass 1: capture hooks record the original's calls (at callee entries,
//           filtered by return address, or at sites inside the original);
//   pass 2: a pass-through interceptor records our game:: calls and executes
//           them; then the two sequences are compared.
// Until kBaselineFrames gameplay-sized passes were seen, pass 2 runs the
// original too: the check that two original passes are identical, i.e. that
// the comparison is sound. (Frame 0 differs by design: Lti_LazyNameHash runs
// only while the name hash is unset.)
// If ours is wrong, the RIGHT eye shows it.

#include <windows.h>

#include <array>
#include <cstdio>
#include <utility>
#include <vector>

#include "debug/shadow/call_trace.hpp"
#include "debug/shadow/capture.hpp"
#include "debug/shadow/shadow.hpp"
#include "game/dx9.hpp"
#include "game/lti.hpp"
#include "game/render_cmd.hpp"
#include "game/render_shell.hpp"
#include "hooks/site.hpp"
#include "hooks/submit_to_gpu.hpp"
#include "log.hpp"
#include "reimpl/render/submit_to_gpu.hpp"

namespace mc2vr::shadow {

namespace {

using namespace game;

constexpr uintptr_t kOriginal = 0x00855690u;
constexpr uintptr_t kOriginalEnd = 0x0085647fu;
constexpr uint32_t kBaselineFrames = 300;      // gameplay-sized passes compared original vs original
constexpr uint32_t kGameplayPassCalls = 5000;  // smaller passes (loading, menus) don't count toward it
constexpr int kLogMax = 8;

// Hand-written decoders for calls whose own site cannot be patched (a branch
// target inside the 5-byte window). Register roles from the listing.
void dec_atmosphere_vec(const safetyhook::Context &c, uint32_t *a)  // 0x855b9e
{
    a[0] = c.eax;                                                   // device
    a[1] = *reinterpret_cast<const uint32_t *>(c.edi + 0xec);       // tech->atmosphereVec.start_reg
    a[2] = c.esp + 0x110;                                           // the vec4 temporary
    a[3] = c.ecx;                                                   // count
}
void dec_record_consts(const safetyhook::Context &c, uint32_t *a)   // 0x855be4
{
    a[0] = c.eax;                                                   // device
    a[1] = c.edi;                                                   // tech->recordConsts.start_reg
    a[2] = *reinterpret_cast<const uint32_t *>(c.esi + 0x50);       // rec->vsConstData
    a[3] = c.ecx * 3;                                               // rec->vsConstCount * 3
}
void dec_stencil_flag(const safetyhook::Context &c, uint32_t *a)    // 0x855e02
{
    a[0] = c.eax;                                                   // device
    a[1] = c.edx;                                                   // pass->stencilFlag.start_reg
    a[2] = c.esp + 0xc0;                                            // the vec4 temporary
    a[3] = c.ecx;                                                   // count
}
void dec_set_indices(const safetyhook::Context &c, uint32_t *a)     // 0x8562f8
{
    a[0] = c.ecx;                                                   // device
    a[1] = c.eax;                                                   // geometry->indices
}
void dec_query_end(const safetyhook::Context &c, uint32_t *a)       // 0x856346
{
    a[0] = c.edi;                                                   // the query
    a[1] = D3DISSUE_END;
}

constexpr std::array kCaptures = {
    at_entry(Lti_LazyNameHash),
    at_entry(RenderShell_PrecacheLoadStep),
    at_entry(RenderShell_PrecacheFinish),
    at_entry(RenderShell_ResetDrawStateCache),
    at_entry(HashTable_FindByHash64),
    at_entry(RenderShell_BindViewTexture),
    at_entry(RenderShell_SetViewType),
    at_entry(Dx9_SetVertexShaderConstantF),
    at_entry(RenderShell_PickPass_Material),
    at_entry(RenderShell_PickPass_0_4),
    at_entry(RenderShell_PickPass_1_3_7),
    at_entry(Dx9_SetPixelShaderConstantF),
    at_entry(Dx9_SetPixelShaderConstantB),
    at_entry(Dx9_SetRenderState),
    at_entry(RenderShell_BindRecordStreams),
    at_entry(RenderShell_DrawRecordPrimitives),
    at_entry(Dx9_SetRenderTarget),
    at_entry(RenderShell_EndHalfResPass),
    at_call(0x00855705u, Lti_DebugPrintf_1),
    at_call(0x00855722u, Lti_DebugPrintf),
    at_call(0x00855750u, LtiRenderer_BeginSubmit),   // vtable call; entry has the pacing hook
    at_call(0x00855a46u, Device_SetVertexShader),
    at_site(0x00855b9eu, Device_SetVertexShaderConstantF, dec_atmosphere_vec),
    at_site(0x00855be4u, Device_SetVertexShaderConstantF, dec_record_consts),
    at_site(0x00855e02u, Device_SetPixelShaderConstantF, dec_stencil_flag),
    at_call(0x008562beu, game::RenderCmd_ExecuteStream),
    at_site(0x008562f8u, Device_SetIndices, dec_set_indices),
    at_call(0x0085633du, Query_Issue),
    at_site(0x00856346u, Query_Issue, dec_query_end),
    at_call(0x008563c3u, LtiRenderer_EndSubmit),
};

// Allocated at install: a pass makes tens of thousands of calls.
constexpr uint32_t kTraceCapacity = 1u << 17;
CallTrace *g_orig = nullptr;
CallTrace *g_ours = nullptr;
CallTrace *g_capture_into = nullptr;  // where the capture hooks record; nullptr = off
uint32_t g_baseline_done = 0;

struct Window {
    uint32_t passes = 0, identical = 0, diverged = 0, overflow = 0;
};
Window g_baseline, g_reimpl;
uint64_t g_total_reimpl = 0, g_total_reimpl_diverged = 0;
uint64_t g_last_report_ms = 0;
int g_logged = 0;
std::vector<SafetyHookMid> *g_hooks = nullptr;

// Records our game:: calls and executes them.
class PassThrough final : public CallInterceptor {
public:
    explicit PassThrough(CallTrace &trace) : trace_(trace) {}
    uint32_t on_call(const FnInfo &fn, const uint32_t *args) override
    {
        trace_.add(fn, args);
        return abi::real_call(fn, args);
    }

private:
    CallTrace &trace_;
};

template <size_t I>
void on_capture(safetyhook::Context &ctx)
{
    constexpr const Capture &cap = kCaptures[I];
    CallTrace *const into = g_capture_into;
    if (into == nullptr) {
        return;
    }
    if (cap.where == Capture::Where::Entry) {
        const uint32_t ret = *reinterpret_cast<const uint32_t *>(ctx.esp);
        if (ret <= kOriginal || ret >= kOriginalEnd) {
            return;  // not called by SubmitToGPU itself
        }
    }
    uint32_t args[kMaxParams];
    decode(ctx, cap, args);
    into->add(*cap.fn, args);
}

void report(uint64_t now)
{
    MC2VR_LOG("reimpl: SubmitToGPU paired window: baseline (original vs original) passes=%u identical=%u "
              "diverged=%u | reimpl passes=%u identical=%u diverged=%u overflow=%u | total reimpl %llu, %llu "
              "diverged | %u calls/pass",
              g_baseline.passes, g_baseline.identical, g_baseline.diverged, g_reimpl.passes, g_reimpl.identical,
              g_reimpl.diverged, g_baseline.overflow + g_reimpl.overflow, (unsigned long long)g_total_reimpl,
              (unsigned long long)g_total_reimpl_diverged, g_orig->size());
    g_baseline = {};
    g_reimpl = {};
    g_last_report_ms = now;
}

void log_divergence(bool baseline, const Divergence &d)
{
    if (g_logged >= kLogMax) {
        return;
    }
    g_logged++;
    char a[200], b[200], pa[200], pb[200];
    format_call(*g_orig, d.index, a, sizeof(a));
    format_call(*g_ours, d.index, b, sizeof(b));
    format_call(*g_orig, d.index ? d.index - 1 : 0, pa, sizeof(pa));
    format_call(*g_ours, d.index ? d.index - 1 : 0, pb, sizeof(pb));
    MC2VR_LOG("reimpl: SubmitToGPU PAIRED DIVERGENCE (%s) at call %u of %u/%u | pass 1 %s | pass 2 %s | "
              "previous: %s / %s",
              baseline ? "baseline: original vs original" : "original vs reimpl", (unsigned)d.index,
              (unsigned)g_orig->size(), (unsigned)g_ours->size(), a, b, pa, pb);
}

void pass1_original()
{
    g_orig->clear();
    g_capture_into = g_orig;
    hooks::call_submit_to_gpu_original();
    g_capture_into = nullptr;
}

void pass2_compared()
{
    // Baseline until enough gameplay passes were compared: the check that two
    // original passes are identical has to cover gameplay, not just loading.
    const bool baseline = g_baseline_done < kBaselineFrames;
    if (baseline && g_orig->size() >= kGameplayPassCalls) {
        g_baseline_done++;
    }
    g_ours->clear();
    if (baseline) {
        g_capture_into = g_ours;  // the original again, through the same captures
        hooks::call_submit_to_gpu_original();
        g_capture_into = nullptr;
    } else {
        PassThrough rec(*g_ours);
        ScopedInterceptor scope(rec);
        reimpl::PgPrimitive_SubmitToGPU();
    }

    Window &w = baseline ? g_baseline : g_reimpl;
    w.passes++;
    if (g_orig->overflowed() || g_ours->overflowed()) {
        w.overflow++;
    } else {
        // Thread stack bounds: pointers to each side's own temporaries match.
        const NT_TIB *tib = reinterpret_cast<const NT_TIB *>(NtCurrentTeb());
        const Divergence d = compare(*g_orig, *g_ours, (uintptr_t)tib->StackLimit, (uintptr_t)tib->StackBase);
        if (d.equal) {
            w.identical++;
        } else {
            w.diverged++;
            if (!baseline) {
                g_total_reimpl_diverged++;
            }
            log_divergence(baseline, d);
        }
    }
    if (!baseline) {
        g_total_reimpl++;
    }
    const uint64_t now = GetTickCount64();
    if (now - g_last_report_ms >= 10000) {
        report(now);
    }
}

template <size_t... Is>
bool add_captures(std::index_sequence<Is...>)
{
    bool ok = true;
    ((ok = ok && [] {
         SafetyHookMid hook;
         if (!hooks::create_mid(hook, kCaptures[Is].site, &on_capture<Is>, kCaptures[Is].fn->name)) {
             return false;
         }
         g_hooks->push_back(std::move(hook));
         return true;
     }()),
     ...);
    return ok;
}

} // namespace

bool install_submit_to_gpu()
{
    g_orig = new CallTrace(kTraceCapacity);
    g_ours = new CallTrace(kTraceCapacity);
    g_hooks = new std::vector<SafetyHookMid>();
    g_hooks->reserve(kCaptures.size());
    if (!add_captures(std::make_index_sequence<kCaptures.size()>{})) {
        g_hooks->clear();
        return false;
    }
    hooks::set_submit_passes(&pass1_original, &pass2_compared);
    MC2VR_LOG("shadow: PgPrimitive_SubmitToGPU paired verification: %u capture hooks; baseline (original in both "
              "passes) for %u gameplay passes (>= %u calls), then pass 2 = reimplementation",
              (unsigned)g_hooks->size(), kBaselineFrames, kGameplayPassCalls);
    return true;
}

} // namespace mc2vr::shadow

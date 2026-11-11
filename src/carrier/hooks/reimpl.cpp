// See reimpl.hpp. One line per reimplemented function.

#include "hooks/reimpl.hpp"

#include <safetyhook.hpp>

#include <array>
#include <utility>

#include "game/render_cmd.hpp"
#include "game/render_shell.hpp"
#include "hooks/entry.hpp"
#include "log.hpp"
#include "game/view_context.hpp"
#include "reimpl/render/begin_submit.hpp"
#include "reimpl/render/build_camera_constants.hpp"
#include "reimpl/render/end_sub_view.hpp"
#include "reimpl/render/render_cmd.hpp"
#include "reimpl/render/setup_viewports.hpp"
#include "reimpl/render/submit_to_gpu.hpp"

namespace mc2vr::hooks {

namespace {

// Ours (Fn) replaces the original declared by Decl (game/), whose code ends
// at hi. Game code reaches ours through the entry generated from Decl
// (hooks/entry.hpp), which adapts the original's convention.
template <const auto &Decl, auto Fn>
constexpr Replacement replace(uintptr_t hi)
{
    return Replacement{Decl.info.name, Decl.info.va, hi, &kEntry<Decl, Fn>};
}

const std::array kReplacements = {
    replace<game::RenderCmd_ExecuteStream, &reimpl::RenderCmd_ExecuteStream>(0x00856d3fu),
    replace<game::PgPrimitive_SubmitToGPU, &reimpl::PgPrimitive_SubmitToGPU>(0x0085647fu),
    replace<game::LtiRenderer_BeginSubmit, &reimpl::LtiRenderer_BeginSubmit>(0x0074ac2bu),
    replace<game::RenderShell_EndSubView, &reimpl::RenderShell_EndSubView>(0x0062eebfu),
    replace<game::RenderShell_SetupViewports, &reimpl::RenderShell_SetupViewports>(0x0085ab47u),
    replace<game::ViewContext_BuildCameraConstants, &reimpl::ViewContext_BuildCameraConstants>(0x00859921u),
};

std::array<SafetyHookInline, kReplacements.size()> g_hooks;

} // namespace

const Replacement *replaced_at(uintptr_t va)
{
    for (const Replacement &r : kReplacements) {
        if (va >= r.lo && va < r.hi) {
            return &r;
        }
    }
    return nullptr;
}

void install_reimpl()
{
    for (size_t i = 0; i < kReplacements.size(); i++) {
        const Replacement &r = kReplacements[i];
        void *const entry = make_entry(r.entry);
        if (entry == nullptr) {
            MC2VR_LOG("reimpl: %s @ %08x: entry stub FAILED — original runs", r.name, (unsigned)r.lo);
            continue;
        }
        // Created directly: the guard in site.hpp would refuse our own range.
        auto hook = SafetyHookInline::create(reinterpret_cast<uint8_t *>(r.lo), reinterpret_cast<uint8_t *>(entry));
        if (hook) {
            g_hooks[i] = std::move(*hook);
        }
        MC2VR_LOG("reimpl: %s @ %08x %s", r.name, (unsigned)r.lo, hook ? "replaced" : "FAILED — original runs");
    }
}

} // namespace mc2vr::hooks

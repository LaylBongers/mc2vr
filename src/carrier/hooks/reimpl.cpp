// See reimpl.hpp. One line per reimplemented function.

#include "hooks/reimpl.hpp"

#include <safetyhook.hpp>

#include <array>
#include <utility>

#include "log.hpp"
#include "reimpl/render/render_cmd.hpp"
#include "reimpl/render/submit_to_gpu.hpp"

namespace mc2vr::hooks {

namespace {

// Each entry's convention must match the original's exactly as its callers
// use it: plain cdecl/stdcall replacements go in directly; register
// conventions need an entry thunk (none yet).
const std::array kReplacements = {
    // cdecl; the caller (PgPrimitive_SubmitToGPU) relies only on callee-saved registers.
    Replacement{"RenderCmd_ExecuteStream", 0x008569d0u, 0x00856d3fu,
                reinterpret_cast<void *>(&reimpl::RenderCmd_ExecuteStream)},
    // void(void); the caller (RenderShell_RenderFrameTimed) relies only on
    // callee-saved registers. Ours renders the stereo frame (both eyes).
    Replacement{"PgPrimitive_SubmitToGPU", 0x00855690u, 0x0085647fu,
                reinterpret_cast<void *>(&reimpl::PgPrimitive_SubmitToGPU)},
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
        // Created directly: the guard in site.hpp would refuse our own range.
        auto hook = SafetyHookInline::create(reinterpret_cast<uint8_t *>(r.lo), reinterpret_cast<uint8_t *>(r.ours));
        if (hook) {
            g_hooks[i] = std::move(*hook);
        }
        MC2VR_LOG("reimpl: %s @ %08x %s", r.name, (unsigned)r.lo, hook ? "replaced" : "FAILED — original runs");
    }
}

} // namespace mc2vr::hooks

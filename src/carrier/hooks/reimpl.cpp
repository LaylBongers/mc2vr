// See reimpl.hpp. One line per reimplemented function.

#include "hooks/reimpl.hpp"

#include <safetyhook.hpp>

#include <array>
#include <utility>

#include "log.hpp"
#include "reimpl/render/render_cmd.hpp"

namespace mc2vr::hooks {

namespace {

// Each entry's convention must match the original's exactly as its callers
// use it: plain cdecl/stdcall replacements go in directly; register
// conventions need an entry thunk (none yet).
const std::array kReplacements = {
    // cdecl; the caller (PgPrimitive_SubmitToGPU) relies only on callee-saved registers.
    Replacement{"RenderCmd_ExecuteStream", 0x008569d0u, 0x00856d3fu,
                reinterpret_cast<void *>(&reimpl::RenderCmd_ExecuteStream)},
    // Its entry is the stereo replay hook, which runs ours (hooks/submit_to_gpu.cpp).
    Replacement{"PgPrimitive_SubmitToGPU", 0x00855690u, 0x0085647fu, nullptr},
};

ReimplMode g_mode = ReimplMode::Replace;
std::array<SafetyHookInline, kReplacements.size()> g_hooks;

} // namespace

void set_reimpl_mode(ReimplMode mode) { g_mode = mode; }
ReimplMode reimpl_mode() { return g_mode; }

const Replacement *replaced_at(uintptr_t va)
{
    if (g_mode != ReimplMode::Replace) {
        return nullptr;
    }
    for (const Replacement &r : kReplacements) {
        if (va >= r.lo && va < r.hi) {
            return &r;
        }
    }
    return nullptr;
}

void install_reimpl()
{
    if (g_mode != ReimplMode::Replace) {
        MC2VR_LOG("reimpl: debug_reimpl=%s — originals kept (%u reimplementations %s)",
                  g_mode == ReimplMode::Shadow ? "shadow" : "off", (unsigned)kReplacements.size(),
                  g_mode == ReimplMode::Shadow ? "verified instead" : "unused");
        return;
    }
    for (size_t i = 0; i < kReplacements.size(); i++) {
        const Replacement &r = kReplacements[i];
        if (r.ours == nullptr) {
            MC2VR_LOG("reimpl: %s @ %08x replaced (by its own hook)", r.name, (unsigned)r.lo);
            continue;
        }
        // Created directly: the guard in site.hpp would refuse our own range.
        auto hook = SafetyHookInline::create(reinterpret_cast<uint8_t *>(r.lo), reinterpret_cast<uint8_t *>(r.ours));
        if (hook) {
            g_hooks[i] = std::move(*hook);
        }
        MC2VR_LOG("reimpl: %s @ %08x %s", r.name, (unsigned)r.lo, hook ? "replaced" : "FAILED — original runs");
    }
}

} // namespace mc2vr::hooks

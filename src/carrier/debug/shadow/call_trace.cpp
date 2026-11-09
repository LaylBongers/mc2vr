// Shadow-mode call traces — see call_trace.hpp.

#include "debug/shadow/call_trace.hpp"

#include <cstdio>

namespace mc2vr::shadow {

namespace {

bool same_function(const game::FnInfo &a, const game::FnInfo &b)
{
    return a.kind == b.kind && a.va == b.va && a.vtbl_offset == b.vtbl_offset;
}

} // namespace

void CallTrace::add(const game::FnInfo &fn, const uint32_t *args)
{
    if (n_ >= capacity_) {
        overflow_ = true;
        return;
    }
    Call &call = calls_[n_++];
    call.fn = &fn;
    for (uint8_t i = 0; i < game::kMaxParams; i++) {
        call.args[i] = i < fn.argc ? args[i] : 0;
    }
}

Divergence compare(const CallTrace &original, const CallTrace &reimpl, uintptr_t stack_lo, uintptr_t stack_hi)
{
    const auto on_stack = [&](uint32_t v) { return v >= stack_lo && v < stack_hi; };
    const uint32_t n = original.size() < reimpl.size() ? original.size() : reimpl.size();
    for (uint32_t i = 0; i < n; i++) {
        const Call &a = original[i];
        const Call &b = reimpl[i];
        if (!same_function(*a.fn, *b.fn)) {
            return {false, i};
        }
        for (uint8_t k = 0; k < a.fn->argc; k++) {
            if (!a.fn->params[k].opaque && a.args[k] != b.args[k] && !(on_stack(a.args[k]) && on_stack(b.args[k]))) {
                return {false, i};
            }
        }
    }
    return {original.size() == reimpl.size(), n};
}

void format_call(const CallTrace &trace, uint32_t i, char *out, uint32_t out_size)
{
    if (i >= trace.size()) {
        snprintf(out, out_size, "<end>");
        return;
    }
    const Call &c = trace[i];
    int n = snprintf(out, out_size, "%s(", c.fn->name);
    for (uint8_t k = 0; k < c.fn->argc && n > 0 && (uint32_t)n < out_size; k++) {
        n += snprintf(out + n, out_size - n, "%s%s%08x", k ? "," : "", c.fn->params[k].opaque ? "?" : "",
                      (unsigned)c.args[k]);
    }
    if (n > 0 && (uint32_t)n < out_size) {
        snprintf(out + n, out_size - n, ")");
    }
}

} // namespace mc2vr::shadow

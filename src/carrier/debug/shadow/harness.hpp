// Generic shadow-mode harness: runs one reimplemented function alongside its
// original and compares them, per original call.
//
// A function's shadow glue is declarative — a Spec:
//
//   struct Spec {
//       static constexpr const char *kName;        // log prefix
//       static constexpr uintptr_t kOriginal;      // entry VA
//       static constexpr uintptr_t kOriginalEnd;   // one past its last byte
//       static constexpr uintptr_t kCallerReturn;  // return site in its (single) caller
//       static constexpr std::array<Capture, N> kCaptures;  // its callees (capture.hpp)
//       static constexpr std::array<StateVar, M> kState;    // every global it writes (state<G>())
//       static void run(const uint32_t *args);     // calls the reimplementation
//       static void describe(const uint32_t *args, char *out, size_t size);  // log context
//   };
//
// Per original call, on the calling thread:
//   1. Entry hook: snapshot kState, run the reimplementation with the
//      original's stack arguments while a CallInterceptor records its calls
//      (they are not executed), keep its end state, restore the snapshot.
//      The original then sees an untouched world.
//   2. The original runs. Capture hooks record its callee calls (capture.hpp).
//   3. Caller-return hook: compare the call sequences and kState. Logs the
//      first divergences in full and a summary line every 10 s.
// The original always runs for real: shadow mode changes nothing on screen.
#pragma once

#include <safetyhook.hpp>

#include <windows.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

#include "game/function.hpp"
#include "debug/shadow/call_trace.hpp"
#include "debug/shadow/capture.hpp"
#include "hooks/site.hpp"
#include "log.hpp"

namespace mc2vr::shadow {

// A global the reimplementation writes (u8/u16/u32). Always snapshotted and
// restored around the dry run. `compared` = false when a callee writes it
// too: callees do not run in the dry run, so its end value cannot match.
struct StateVar {
    const char *name;
    uintptr_t va;
    uint8_t size;
    bool compared;
};
template <class G>
constexpr StateVar state(const char *name, bool compared = true)
{
    static_assert(sizeof(typename G::type) == 1 || sizeof(typename G::type) == 2 || sizeof(typename G::type) == 4);
    return {name, G::address, (uint8_t)sizeof(typename G::type), compared};
}

inline uint32_t read_state(const StateVar &v)
{
    switch (v.size) {
    case 1: return *reinterpret_cast<volatile const uint8_t *>(v.va);
    case 2: return *reinterpret_cast<volatile const uint16_t *>(v.va);
    default: return *reinterpret_cast<volatile const uint32_t *>(v.va);
    }
}
inline void write_state(const StateVar &v, uint32_t x)
{
    switch (v.size) {
    case 1: *reinterpret_cast<volatile uint8_t *>(v.va) = (uint8_t)x; break;
    case 2: *reinterpret_cast<volatile uint16_t *>(v.va) = (uint16_t)x; break;
    default: *reinterpret_cast<volatile uint32_t *>(v.va) = x; break;
    }
}

// Records every intercepted call; returns 0 (S_OK, "not failed", null).
class Recorder final : public game::CallInterceptor {
public:
    explicit Recorder(CallTrace &trace) : trace_(trace) {}
    uint32_t on_call(const game::FnInfo &fn, const uint32_t *args) override
    {
        trace_.add(fn, args);
        return 0;
    }

private:
    CallTrace &trace_;
};

template <class Spec>
class Harness {
public:
    static bool install()
    {
        hooks_ = new std::vector<SafetyHookMid>();
        hooks_->reserve(Spec::kCaptures.size() + 2);
        // Captures first, entry last: nothing records until the entry hook arms a capture.
        const bool ok = add_captures(std::make_index_sequence<Spec::kCaptures.size()>{}) &&
                        add_hook(Spec::kCallerReturn, on_caller_return, "caller return site") &&
                        add_hook(Spec::kOriginal, on_entry, "entry");
        if (!ok) {
            hooks_->clear();  // unhooks everything installed so far
            return false;
        }
        MC2VR_LOG("reimpl: %s shadow: %u hooks installed", Spec::kName, (unsigned)hooks_->size());
        return true;
    }

private:
    // Summary line, at most every 10 s (from the caller-return hook).
    static void maybe_report()
    {
        const uint64_t now = GetTickCount64();
        if (now - last_report_ms_ < 10000) {
            return;
        }
        last_report_ms_ = now;
        MC2VR_LOG("reimpl: %s shadow window: calls=%u identical=%u diverged calls=%u state=%u | "
                 "overflow=%u unmatched=%u | total %llu, %llu diverged",
                 Spec::kName, stats_.runs.exchange(0), stats_.identical.exchange(0), stats_.call_diverged.exchange(0),
                 stats_.state_diverged.exchange(0), stats_.overflow.exchange(0),
                 stats_.unmatched.exchange(0), (unsigned long long)stats_.total.load(),
                 (unsigned long long)stats_.total_diverged.load());
    }

    static constexpr size_t kArgCopy = 4;
    static constexpr int kLogMax = 8;

    struct Stats {
        std::atomic<uint32_t> runs{0}, identical{0}, call_diverged{0}, state_diverged{0}, overflow{0}, unmatched{0};
        std::atomic<uint64_t> total{0}, total_diverged{0};
    };

    static inline CallTrace orig_, dry_;
    static inline bool capturing_ = false;
    static inline uint32_t args_[kArgCopy];
    static inline std::array<uint32_t, Spec::kState.size()> reimpl_end_;
    static inline Stats stats_;
    static inline int logged_ = 0;
    static inline uint64_t last_report_ms_ = 0;
    static inline std::vector<SafetyHookMid> *hooks_ = nullptr;

    template <size_t I>
    static void on_capture(safetyhook::Context &ctx)
    {
        constexpr const Capture &cap = Spec::kCaptures[I];
        if (!capturing_) {
            return;
        }
        if (cap.where == Capture::Where::Entry) {
            const uint32_t ret = *reinterpret_cast<const uint32_t *>(ctx.esp);
            if (ret <= Spec::kOriginal || ret >= Spec::kOriginalEnd) {
                return;  // called by someone else
            }
        }
        uint32_t args[game::kMaxParams];
        decode(ctx, cap, args);
        orig_.add(*cap.fn, args);
    }

    static void on_entry(safetyhook::Context &ctx)
    {
        const uint32_t *args = reinterpret_cast<const uint32_t *>(ctx.esp) + 1;
        if (capturing_) {
            stats_.unmatched++;  // the previous call's return site never fired
        }
        std::memcpy(args_, args, sizeof(args_));

        std::array<uint32_t, Spec::kState.size()> before;
        for (size_t i = 0; i < before.size(); i++) {
            before[i] = read_state(Spec::kState[i]);
        }
        dry_.clear();
        {
            Recorder rec(dry_);
            game::ScopedInterceptor scope(rec);
            Spec::run(args);
        }
        for (size_t i = 0; i < before.size(); i++) {
            reimpl_end_[i] = read_state(Spec::kState[i]);
            write_state(Spec::kState[i], before[i]);
        }
        orig_.clear();
        capturing_ = true;
    }

    static void on_caller_return(safetyhook::Context &)
    {
        if (!capturing_) {
            return;
        }
        capturing_ = false;
        maybe_report();
        stats_.runs++;
        stats_.total++;
        if (orig_.overflowed() || dry_.overflowed()) {
            stats_.overflow++;
            return;
        }
        const Divergence d = compare(orig_, dry_);
        bool state_ok = true;
        for (size_t i = 0; i < Spec::kState.size(); i++) {
            state_ok = state_ok && (!Spec::kState[i].compared || read_state(Spec::kState[i]) == reimpl_end_[i]);
        }
        if (d.equal && state_ok) {
            stats_.identical++;
            return;
        }
        stats_.total_diverged++;
        (d.equal ? stats_.state_diverged : stats_.call_diverged)++;
        if (logged_ < kLogMax) {
            logged_++;
            d.equal ? log_state_divergence() : log_call_divergence(d);
        }
    }

    static void log_call_divergence(const Divergence &d)
    {
        char a[200], b[200], ctx[160];
        format_call(orig_, d.index, a, sizeof(a));
        format_call(dry_, d.index, b, sizeof(b));
        Spec::describe(args_, ctx, sizeof(ctx));
        MC2VR_LOG("reimpl: %s SHADOW DIVERGENCE (calls) at call %u of %u/%u | original %s | reimpl %s | %s",
                 Spec::kName, (unsigned)d.index, (unsigned)orig_.size(), (unsigned)dry_.size(), a, b, ctx);
    }

    static void log_state_divergence()
    {
        char buf[400], ctx[160];
        int n = 0;
        for (size_t i = 0; i < Spec::kState.size() && n < (int)sizeof(buf) - 64; i++) {
            n += snprintf(buf + n, sizeof(buf) - n, " %s=%08x/%08x", Spec::kState[i].name,
                          (unsigned)read_state(Spec::kState[i]), (unsigned)reimpl_end_[i]);
        }
        Spec::describe(args_, ctx, sizeof(ctx));
        MC2VR_LOG("reimpl: %s SHADOW DIVERGENCE (end state, original/reimpl):%s | %s", Spec::kName, buf, ctx);
    }

    static bool add_hook(uintptr_t va, safetyhook::MidHookFn fn, const char *what)
    {
        SafetyHookMid hook;
        if (!hooks::create_mid(hook, va, fn, what)) {
            return false;
        }
        hooks_->push_back(std::move(hook));
        return true;
    }

    template <size_t... Is>
    static bool add_captures(std::index_sequence<Is...>)
    {
        bool ok = true;
        ((ok = ok && add_hook(Spec::kCaptures[Is].site, &on_capture<Is>, Spec::kCaptures[Is].fn->name)), ...);
        return ok;
    }
};

} // namespace mc2vr::shadow

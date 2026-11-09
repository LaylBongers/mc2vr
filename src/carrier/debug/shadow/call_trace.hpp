// Shadow-mode verification: two call sequences and where they first diverge.
//
// Both sides of a shadow comparison append one Call per original-function
// call (game::Function / game::Method): the original's calls decoded from its
// registers and stack (capture.hpp), the reimplementation's calls as
// intercepted during its dry run (harness.hpp). Opaque parameters (results of
// earlier calls a dry run cannot know) are kept for logging but not compared.
//
// Fixed capacity (allocated once), allocation-free afterwards, single-threaded per trace.
#pragma once

#include <cstdint>

#include "game/function.hpp"

namespace mc2vr::shadow {

struct Call {
    const game::FnInfo *fn;
    uint32_t args[game::kMaxParams];
};

class CallTrace {
public:
    explicit CallTrace(uint32_t capacity = 4096) : calls_(new Call[capacity]), capacity_(capacity) {}
    ~CallTrace() { delete[] calls_; }
    CallTrace(const CallTrace &) = delete;
    CallTrace &operator=(const CallTrace &) = delete;

    void clear() { n_ = 0; overflow_ = false; }
    void add(const game::FnInfo &fn, const uint32_t *args);  // fn.argc args

    uint32_t size() const { return n_; }
    bool overflowed() const { return overflow_; }
    const Call &operator[](uint32_t i) const { return calls_[i]; }

private:
    Call *calls_;
    uint32_t capacity_;
    uint32_t n_ = 0;
    bool overflow_ = false;
};

struct Divergence {
    bool equal;      // true: identical sequences
    uint32_t index;  // first differing position (== shorter length on a length mismatch)
};

// Call by call: the same function (by VA/vtable slot), then every non-opaque
// argument. With a stack range, two arguments that both point into it count
// as equal (pointers to each side's own stack temporaries).
Divergence compare(const CallTrace &original, const CallTrace &reimpl, uintptr_t stack_lo = 0, uintptr_t stack_hi = 0);

// "Name(args...)" for trace[i] ("<end>" past the end); opaque args print as ?value.
void format_call(const CallTrace &trace, uint32_t i, char *out, uint32_t out_size);

} // namespace mc2vr::shadow

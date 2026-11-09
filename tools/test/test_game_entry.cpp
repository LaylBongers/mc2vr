// The i386 entry for reimplemented originals (src/carrier/hooks/entry.cpp,
// hand-written asm + the adapters in entry.hpp): parameters from registers and the stack,
// AL-only bools, XMM0, the callee-pop return (`ret N`), the EAX result and
// every other general register restored. A caller in the game that gets its
// stack or registers back wrong crashes far from the cause; this pins it down.
#include <cstdint>
#include <cstdio>

#include "hooks/entry.hpp"

static int g_fail = 0;
#define CHECK(cond, ...)                                                          \
    do {                                                                          \
        if (!(cond)) {                                                            \
            g_fail++;                                                             \
            std::printf("FAIL %s:%d %s: ", __FILE__, __LINE__, #cond);            \
            std::printf(__VA_ARGS__);                                             \
            std::printf("\n");                                                    \
        }                                                                         \
    } while (0)

using namespace mc2vr::game;
using namespace mc2vr::hooks;

extern "C" {
// What the test caller loads before the call: eax ecx edx ebx esi edi xmm0,
// then the two stack arguments it pushes ([esp+4] = s0 at the callee).
uint32_t g_in[9];
// After the call: eax ecx edx ebx esi edi ebp, then ESP after minus ESP before.
uint32_t g_out[8];
uint32_t g_ebp_before;
void *g_target;
void test_caller(void *target);
}

// Calls `target` the way game code would: registers from g_in, two stack
// arguments, then records the registers and how far ESP moved.
asm(R"(
    .text
    .globl _test_caller
    .intel_syntax noprefix
_test_caller:
    push ebp
    mov ebp, esp
    push ebx
    push esi
    push edi
    mov [_g_ebp_before], ebp
    push dword ptr [_g_in + 32]
    push dword ptr [_g_in + 28]
    movd xmm0, dword ptr [_g_in + 24]
    mov ecx, [_g_in + 4]
    mov edx, [_g_in + 8]
    mov ebx, [_g_in + 12]
    mov esi, [_g_in + 16]
    mov edi, [_g_in + 20]
    mov eax, [ebp + 8]
    mov [_g_target], eax
    mov eax, [_g_in + 0]
    call dword ptr [_g_target]
    mov [_g_out + 0], eax
    mov [_g_out + 4], ecx
    mov [_g_out + 8], edx
    mov [_g_out + 12], ebx
    mov [_g_out + 16], esi
    mov [_g_out + 20], edi
    mov [_g_out + 24], ebp
    lea eax, [ebp - 20]             /* ESP before the call (after the two pushes) */
    sub eax, esp
    neg eax
    mov [_g_out + 28], eax
    lea esp, [ebp - 12]
    pop edi
    pop esi
    pop ebx
    pop ebp
    ret
    .att_syntax prefix
)");

namespace {

uint32_t g_got[8];

// Every register parameter plus both stack ones; callee pops 8.
uint32_t all_regs(uint32_t s0, uint32_t a, uint32_t c, uint32_t d, uint32_t b, uint32_t si, uint32_t di, uint32_t s1)
{
    const uint32_t v[8] = {s0, a, c, d, b, si, di, s1};
    for (int i = 0; i < 8; i++) g_got[i] = v[i];
    return 0x1234;
}
inline constexpr Function<uint32_t(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t)>
    kAllRegs{"all_regs", 0, {STK, EAX, ECX, EDX, EBX, ESI, EDI, STK}, Pops{8}};

// thiscall-like with a byte bool in AL and a float in XMM0; caller pops.
struct Obj {
    int x;
};
bool flags(Obj *self, bool on, float f)
{
    g_got[0] = (uint32_t)(uintptr_t)self;
    g_got[1] = on;
    g_got[2] = std::bit_cast<uint32_t>(f);
    return !on;
}
inline constexpr Function<bool(Obj *, bool, float)> kFlags{"flags", 0, {ECX, AL, XMM0}};

// No parameters, no result (the SubmitToGPU shape).
int g_void_calls = 0;
void no_args() { g_void_calls++; }
inline constexpr Function<void()> kNoArgs{"no_args", 0};

void set_in()
{
    const uint32_t in[9] = {0xa, 0xc, 0xd, 0xb, 0x51, 0xd1, 0x3fc00000u, 0x50, 0x60};
    for (int i = 0; i < 9; i++) g_in[i] = in[i];
}

void check_preserved(const char *what)
{
    CHECK(g_out[1] == g_in[1] && g_out[2] == g_in[2] && g_out[3] == g_in[3] && g_out[4] == g_in[4] &&
              g_out[5] == g_in[5],
          "%s: registers after ecx %x edx %x ebx %x esi %x edi %x", what, g_out[1], g_out[2], g_out[3], g_out[4],
          g_out[5]);
    CHECK(g_out[6] == g_ebp_before, "%s: ebp %x, was %x", what, g_out[6], g_ebp_before);
}

} // namespace

int main()
{
    void *const regs = make_entry(&kEntry<kAllRegs, &all_regs>);
    void *const fl = make_entry(&kEntry<kFlags, &flags>);
    void *const na = make_entry(&kEntry<kNoArgs, &no_args>);
    CHECK(regs != nullptr && fl != nullptr && na != nullptr, "make_entry failed");
    if (g_fail) return 1;

    set_in();
    test_caller(regs);
    CHECK(g_out[0] == 0x1234, "return %08x", g_out[0]);
    CHECK(g_got[1] == 0xa && g_got[2] == 0xc && g_got[3] == 0xd && g_got[4] == 0xb && g_got[5] == 0x51 &&
              g_got[6] == 0xd1,
          "register parameters %x %x %x %x %x %x", g_got[1], g_got[2], g_got[3], g_got[4], g_got[5], g_got[6]);
    CHECK(g_got[0] == 0x50 && g_got[7] == 0x60, "stack parameters %x %x", g_got[0], g_got[7]);
    CHECK(g_out[7] == 8, "popped %u bytes, declared 8", g_out[7]);
    check_preserved("all_regs");

    // AL = 1 with garbage above it: on = true; returns false in AL.
    Obj obj{};
    set_in();
    g_in[0] = 0x12345601;
    g_in[1] = (uint32_t)(uintptr_t)&obj;
    test_caller(fl);
    CHECK(g_got[0] == (uint32_t)(uintptr_t)&obj, "this %08x", g_got[0]);
    CHECK(g_got[1] == 1, "AL bool %u", g_got[1]);
    CHECK(g_got[2] == 0x3fc00000u, "xmm0 %08x", g_got[2]);
    CHECK((g_out[0] & 0xff) == 0, "bool return %08x", g_out[0]);
    CHECK(g_out[7] == 0, "popped %u bytes, declared 0", g_out[7]);
    check_preserved("flags");
    g_in[0] = 0x12345600;
    test_caller(fl);
    CHECK(g_got[1] == 0 && (g_out[0] & 0xff) == 1, "AL bool false: got %u, return %08x", g_got[1], g_out[0]);

    set_in();
    test_caller(na);
    CHECK(g_void_calls == 1, "no_args calls %d", g_void_calls);
    CHECK(g_out[7] == 0, "popped %u bytes, declared 0", g_out[7]);
    check_preserved("no_args");

    if (g_fail) {
        std::printf("%d FAILED\n", g_fail);
        return 1;
    }
    std::printf("game entry: all passed\n");
    return 0;
}

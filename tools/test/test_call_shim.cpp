// The i386 call shim behind game::Function / game::Method
// (src/game/function.cpp, hand-written asm): register loading, stack order,
// callee-pop (`ret N`), XMM0, vtable dispatch. A shim bug in the game shows
// up as a crash or corrupted state far from its cause; this pins it down.
#include <cstdint>
#include <cstdio>

#include "game/function.hpp"

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

extern "C" {
uint32_t g_seen[9];  // eax ecx edx ebx esi edi xmm0 [esp+4] [esp+8]
uint32_t g_seen_xmm[3];  // xmm1 xmm5 xmm6
void test_callee();
void test_callee_xmm();
}

// Records its registers and two stack arguments, clobbers the callee-saved
// registers (the shim must restore them), returns 0x1234 and pops 8 bytes.
asm(R"(
    .text
    .globl _test_callee
    .intel_syntax noprefix
_test_callee:
    mov [_g_seen + 0], eax
    mov [_g_seen + 4], ecx
    mov [_g_seen + 8], edx
    mov [_g_seen + 12], ebx
    mov [_g_seen + 16], esi
    mov [_g_seen + 20], edi
    movd [_g_seen + 24], xmm0
    mov eax, [esp + 4]
    mov [_g_seen + 28], eax
    mov eax, [esp + 8]
    mov [_g_seen + 32], eax
    mov ebx, 0xdead0001
    mov esi, 0xdead0002
    mov edi, 0xdead0003
    mov eax, 0x1234
    ret 8
    .att_syntax prefix
)");

// Records xmm1/xmm5/xmm6.
asm(R"(
    .text
    .globl _test_callee_xmm
    .intel_syntax noprefix
_test_callee_xmm:
    movd [_g_seen_xmm + 0], xmm1
    movd [_g_seen_xmm + 4], xmm5
    movd [_g_seen_xmm + 8], xmm6
    ret
    .att_syntax prefix
)");

namespace {

struct FakeDevice;
struct FakeVtbl {
    void *slots[40];
};
struct FakeDevice {
    FakeVtbl *vtbl;
    uint32_t got[3];
};

long __stdcall fake_method(FakeDevice *self, uint32_t a, uint32_t b)
{
    self->got[0] = 1;
    self->got[1] = a;
    self->got[2] = b;
    return 0x77;
}

} // namespace

int main()
{
    const Function<uint32_t(uint32_t s0, uint32_t a, uint32_t c, uint32_t d, uint32_t b, uint32_t si, uint32_t di,
                            uint32_t s1)>
        regs{"test_callee", (uintptr_t)&test_callee, {STK, EAX, ECX, EDX, EBX, ESI, EDI, STK}};
    const uint32_t r = regs(0x50, 0xa, 0xc, 0xd, 0xb, 0x51, 0xd1, 0x60);
    CHECK(r == 0x1234, "return %08x", r);
    CHECK(g_seen[0] == 0xa && g_seen[1] == 0xc && g_seen[2] == 0xd && g_seen[3] == 0xb && g_seen[4] == 0x51 &&
              g_seen[5] == 0xd1,
          "registers %x %x %x %x %x %x", g_seen[0], g_seen[1], g_seen[2], g_seen[3], g_seen[4], g_seen[5]);
    CHECK(g_seen[7] == 0x50 && g_seen[8] == 0x60, "stack order %x %x", g_seen[7], g_seen[8]);

    const Function<void(float x, uint32_t s0, uint32_t s1)> fl{"test_callee", (uintptr_t)&test_callee,
                                                                {XMM0, STK, STK}};
    fl(1.5f, 1, 2);
    CHECK(g_seen[6] == 0x3fc00000u, "xmm0 %08x", g_seen[6]);

    const Function<void(float, float, float)> xf{"test_callee_xmm", (uintptr_t)&test_callee_xmm, {XMM1, XMM5, XMM6}};
    xf(2.0f, 3.0f, 4.0f);
    CHECK(g_seen_xmm[0] == 0x40000000u && g_seen_xmm[1] == 0x40400000u && g_seen_xmm[2] == 0x40800000u,
          "xmm1/5/6 %08x %08x %08x", g_seen_xmm[0], g_seen_xmm[1], g_seen_xmm[2]);

    static FakeVtbl vtbl{};
    vtbl.slots[0x88 / 4] = (void *)&fake_method;
    FakeDevice dev{&vtbl, {}};
    const Method<long(FakeDevice *, uint32_t, uint32_t)> m{"FakeDevice::m", 0x88};
    const long hr = m(&dev, 7, 8);
    CHECK(hr == 0x77 && dev.got[0] == 1 && dev.got[1] == 7 && dev.got[2] == 8, "method %ld %u %u", hr, dev.got[1],
          dev.got[2]);

    if (g_fail) {
        std::printf("%d FAILED\n", g_fail);
        return 1;
    }
    std::printf("game call shim: all passed\n");
    return 0;
}

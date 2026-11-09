// Call shim for game::Function / game::Method (function.hpp).

#include "game/function.hpp"

#if !defined(__i386__)
#error "the game call shim is i386-only (the game is a 32-bit exe)"
#endif

namespace mc2vr::game {

namespace {

thread_local CallInterceptor *t_interceptor = nullptr;

// Register/stack image handed to the assembly shim. Offsets are hard-coded
// in the asm below.
struct CallFrame {
    uint32_t target;                       // +0
    uint32_t eax, ecx, edx, ebx, esi, edi; // +4 .. +24
    uint32_t xmm0;                         // +28 (low lane)
    const uint32_t *stack;                 // +32 (first = [esp+4] at the callee)
    uint32_t nstack;                       // +36
};
static_assert(sizeof(CallFrame) == 40);

} // namespace

} // namespace mc2vr::game

extern "C" uint32_t mc2vr_game_call(const mc2vr::game::CallFrame *frame);

// Loads the registers, pushes the stack arguments, calls, then restores ESP
// from EBP — so caller-pop and callee-pop (`ret N`) callees both work. EBX,
// ESI, EDI, EBP are restored for our C++ caller; EAX is the result. The
// callee must preserve EBP (every MSVC-compiled function does).
asm(R"(
    .text
    .globl _mc2vr_game_call
    .def _mc2vr_game_call; .scl 2; .type 32; .endef
    .intel_syntax noprefix
_mc2vr_game_call:
    push ebp
    mov ebp, esp
    push ebx
    push esi
    push edi
    mov eax, [ebp + 8]
    push dword ptr [eax]            /* target, at [ebp - 16] */
    mov ecx, [eax + 36]
    mov edx, [eax + 32]
1:
    test ecx, ecx
    jz 2f
    dec ecx
    push dword ptr [edx + ecx * 4]
    jmp 1b
2:
    movd xmm0, dword ptr [eax + 28]
    mov ecx, [eax + 8]
    mov edx, [eax + 12]
    mov ebx, [eax + 16]
    mov esi, [eax + 20]
    mov edi, [eax + 24]
    mov eax, [eax + 4]
    call dword ptr [ebp - 16]
    lea esp, [ebp - 12]
    pop edi
    pop esi
    pop ebx
    pop ebp
    ret
    .att_syntax prefix
)");

namespace mc2vr::game {

void set_thread_interceptor(CallInterceptor *ic) { t_interceptor = ic; }
CallInterceptor *thread_interceptor() { return t_interceptor; }

namespace abi {

uint32_t invoke(const FnInfo &fn, const uint32_t *args)
{
    if (t_interceptor != nullptr) {
        return t_interceptor->on_call(fn, args);
    }
    CallFrame f{};
    uint32_t stack[kMaxParams];
    for (uint8_t i = 0; i < fn.argc; i++) {
        const uint32_t v = args[i];
        switch (fn.params[i].loc) {
        case Loc::Stack: stack[f.nstack++] = v; break;
        case Loc::EAX: f.eax = v; break;
        case Loc::ECX: f.ecx = v; break;
        case Loc::EDX: f.edx = v; break;
        case Loc::EBX: f.ebx = v; break;
        case Loc::ESI: f.esi = v; break;
        case Loc::EDI: f.edi = v; break;
        case Loc::XMM0: f.xmm0 = v; break;
        }
    }
    f.stack = stack;
    if (fn.kind == FnInfo::Kind::Method) {
        const uint32_t vtbl = *reinterpret_cast<const uint32_t *>((uintptr_t)args[0]);
        f.target = *reinterpret_cast<const uint32_t *>((uintptr_t)(vtbl + fn.vtbl_offset));
    } else {
        f.target = (uint32_t)fn.va;
    }
    return mc2vr_game_call(&f);
}

} // namespace abi

} // namespace mc2vr::game

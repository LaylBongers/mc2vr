// Call shim for game::Function / game::Method (function.hpp).

#include "game/function.hpp"

#if !defined(__i386__)
#error "the game call shim is i386-only (the game is a 32-bit exe)"
#endif

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

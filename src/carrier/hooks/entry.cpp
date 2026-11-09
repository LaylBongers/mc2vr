// Entries for reimplemented originals (entry.hpp): the shared asm entry and
// the per-replacement stubs.

#include "hooks/entry.hpp"

#include <windows.h>

#include <cstring>

#if !defined(__i386__)
#error "the game entry is i386-only (the game is a 32-bit exe)"
#endif

// Reached from a stub with the EntryInfo pushed:
//   [ebp+4] info, [ebp+8] the game's return address, [ebp+12] stack arguments.
// Builds the EntryFrame below EBP, calls info->adapter(frame), then returns
// like the original: EAX = result, every other general register as the
// caller left it, info->pops bytes of arguments popped (the return address
// is moved up to where `ret N` would leave ESP).
asm(R"(
    .text
    .globl _mc2vr_game_entry
    .def _mc2vr_game_entry; .scl 2; .type 32; .endef
    .intel_syntax noprefix
_mc2vr_game_entry:
    push ebp
    mov ebp, esp
    sub esp, 32
    mov [ebp - 32], eax
    mov [ebp - 28], ecx
    mov [ebp - 24], edx
    mov [ebp - 20], ebx
    mov [ebp - 16], esi
    mov [ebp - 12], edi
    movd [ebp - 8], xmm0
    lea eax, [ebp + 12]
    mov [ebp - 4], eax
    lea eax, [ebp - 32]
    push eax
    mov eax, [ebp + 4]
    call dword ptr [eax]            /* adapter(frame), cdecl */
    mov ecx, [ebp + 4]
    movzx ecx, word ptr [ecx + 4]   /* pops */
    lea ecx, [ebp + ecx + 8]        /* where the return address goes */
    mov edx, [ebp + 8]
    mov [ecx], edx
    mov [ebp + 4], ecx              /* the final ESP, read after EBP is restored */
    mov ecx, [ebp - 28]
    mov edx, [ebp - 24]
    mov ebx, [ebp - 20]
    mov esi, [ebp - 16]
    mov edi, [ebp - 12]
    mov esp, ebp
    pop ebp
    mov esp, [esp]
    ret
    .att_syntax prefix
)");

namespace mc2vr::hooks {

namespace {

constexpr size_t kPageSize = 4096;
constexpr size_t kStubSize = 16;  // 10 used, 16-byte aligned

uint8_t *g_page = nullptr;
size_t g_used = 0;

} // namespace

void *make_entry(const entry_abi::EntryInfo *info)
{
    DWORD old;
    if (g_page == nullptr || g_used + kStubSize > kPageSize) {
        g_page = static_cast<uint8_t *>(VirtualAlloc(nullptr, kPageSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        g_used = 0;
        if (g_page == nullptr) {
            return nullptr;
        }
    } else if (!VirtualProtect(g_page, kPageSize, PAGE_READWRITE, &old)) {
        return nullptr;
    }

    uint8_t *const stub = g_page + g_used;
    const uint32_t imm = (uint32_t)(uintptr_t)info;
    const int32_t rel = (int32_t)((uintptr_t)&entry_abi::mc2vr_game_entry - (uintptr_t)(stub + 10));
    stub[0] = 0x68;  // push imm32
    std::memcpy(stub + 1, &imm, 4);
    stub[5] = 0xe9;  // jmp rel32
    std::memcpy(stub + 6, &rel, 4);
    g_used += kStubSize;

    if (!VirtualProtect(g_page, kPageSize, PAGE_EXECUTE_READ, &old)) {
        return nullptr;
    }
    FlushInstructionCache(GetCurrentProcess(), stub, kStubSize);
    return stub;
}

} // namespace mc2vr::hooks

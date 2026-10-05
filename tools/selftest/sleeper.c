// Test stand-in for Mercenaries2.exe (built as mc2vr_selftest_sleeper.exe).
// Mimics the two runtime contracts the launcher relies on, so the full
// launcher → carrier chain can be exercised under plain Wine without the
// game or Steam:
//   1. loads at the fixed image base 0x00400000 (default mingw base), and
//   2. maps the boot-counter page at its literal VA (game_addresses.h) and
//      increments it, like GameTimeAccumulate_Update does.
// The carrier is expected to log "attached" and then REFUSE to hook (build
// lock: this is not the real game) — proving the lock gate works too.
#include <windows.h>
#include <stdint.h>

#define FRAME_COUNTER_2 ((uintptr_t)0x011755bcu) // the real exe's frame counter VA (docs/reverse_engineering/main_game_loop.md)

static DWORD WINAPI ticker(LPVOID param)
{
    // VirtualAlloc returns the page base (0x01170000), not the requested
    // sub-page address — always increment the exact counter VA.
    (void)param;
    volatile uint32_t *counter = (volatile uint32_t *)FRAME_COUNTER_2;
    for (;;) {
        (*counter)++;
        Sleep(50);
    }
}

int main(void)
{
    // Small exes never reach 0x011755bc with their image, so the page is
    // free; if a future toolchain changes that, this call fails loudly.
    void *page = VirtualAlloc((void *)FRAME_COUNTER_2, 0x1000,
                              MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!page) {
        return 1;
    }

    HANDLE thread = CreateThread(nullptr, 0, ticker, page, 0, nullptr);
    if (!thread) {
        return 1;
    }

    Sleep(2 * 60 * 1000); // "main loop"; self-cleans if the script's cleanup misses us
    return 0;
}

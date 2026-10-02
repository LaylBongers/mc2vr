// Runtime addresses and other hard facts from the Ghidra project, shared by
// the launcher and the carrier. Per-address findings live in Ghidra; this
// header only records what the build needs. Update together with the exe
// hash in src/carrier/build_lock.h (same build-lock discipline).
//
// The exe image cannot relocate (fixed load base, no relocations); every VA
// below is a literal runtime address, not a base-relative RVA.
#pragma once

// Expected load base of Mercenaries2.exe. Both components refuse to operate
// if the real base differs — every VA below is absolute.
#define MC2_GAME_BASE_EXPECTED ((uintptr_t)0x00400000u)

// Per-frame counters, bumped every GameTimeAccumulate_Update (main loop
// alive). The launcher polls one of these to detect boot completion
// (docs/launcher_plan.md, launch sequence step 3). "Counter moving" implies
// the SecuROM startup stub has finished; it does NOT imply rendering is up
// (D3D init lands a few frames later — harmless, hooks idle until called).
#define MC2_FRAME_COUNTER_1 ((uintptr_t)0x017bad00u) // _DAT_017bad00
#define MC2_FRAME_COUNTER_2 ((uintptr_t)0x011755bcu) // _DAT_011755bc

// Optional strict "render ready" gate for later milestones: non-zero once
// g_D3D9 exists.
#define MC2_G_D3D9 ((uintptr_t)0x01175284u)

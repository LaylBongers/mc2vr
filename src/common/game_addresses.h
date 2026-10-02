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

// ---- M1 hook/probe sites (all plaintext .text/.data, well below the ----
// ---- SecuROM region at 0x01a48000; see docs/launcher_plan.md hook list) --

// GameShell_FrameTick — void (void), called once per main-loop iteration from
// GameShell_Run (single call site). The main M1 inline hook: frame counting,
// timing sanity, install ack.
#define MC2_GAMESHELL_FRAMETICK ((uintptr_t)0x00630e10u)

// LtiRenderer_BeginSubmit (0x0074aaa0, plaintext .text, vtable slot 15 of
// LtiRenderer_vtbl 0x00bd38e8) — thiscall (this in ECX). Every frame:
// Present(prev frame) then BeginScene (M2 runtime evidence). An M2.5 MidHook
// probe at its entry reads ECX (this) and [ESP] (return address) to answer
// two open questions: which vtable the live object holds (base vs the
// derived RenderShell override 'Flush') and who drives the frame (the
// encrypted thunk_FUN_0256b6f0 is the suspect).
#define MC2_LTI_BEGINSUBMIT ((uintptr_t)0x0074aaa0u)

// ---- M3 instrument sites (instruction-level, verified by disassembly) ----

// RenderCmd_ExecuteStream opcode dispatch: `cmp eax,0x1a` — 27 opcodes
// (0x00..0x1a), jump table at 0x856d40. At this instruction EAX = opcode,
// EBP = current stream pointer. MidHook fires once per command.
#define MC2_RENDERCMD_OPCODE_CMP ((uintptr_t)0x008569f5u)

// RenderQueue_SubmitWorldPackets per-view loop head: `lea eax,[eax+0x12865e0]`.
// At this instruction ESI = view index, ECX = view type (WORD, from
// per-view-object+0x14; types 2 and 4 get processed), EAX = byte offset
// (idx*0x810). Entry = 0x012865e0 + EAX. Per-view object pointer at
// entry+0x7e4. Companion table 0x014095e0 (stride 0x20, byte at +0x18).
// Loop count = *(WORD*)(*g_RenderShellPtr + 0x2b90).
#define MC2_SUBMITVIEW_LOOP_LEA ((uintptr_t)0x0048e9eau)
#define MC2_VIEW_TABLE ((uintptr_t)0x012865e0u)
#define MC2_VIEW_OBJ_PTR_OFF ((uintptr_t)0x7e4u)
#define MC2_VIEW_TABLE3 ((uintptr_t)0x014095e0u)

// g_RenderShell (object, holds live base LtiRenderer_vtbl at frame time) and
// its pointer global. VmtHook claim target for slots 4 (EndOfFrameHook) /
// 5 (PostUpdateHook), called by GameShell_FrameTick every frame.
#define MC2_G_RENDERSHELL ((uintptr_t)0x017ceaf0u)
#define MC2_G_RENDERSHELLPTR ((uintptr_t)0x00dfb2f8u)

// g_RenderQueue ring buffer fields (render_path.md): base is the struct;
// elementSize base+4, capacity base+8, buffer base+0xc, producer counters
// base+0x10/0x14 (packed 16-bit pairs semantics unresolved), CS base+0x18.
#define MC2_G_RENDERQUEUE ((uintptr_t)0x00ff3618u)

// GetD3DDevice — thunk (6 bytes, jmp into a SecuROM VM stub), void* (void),
// 12 call sites. Hooking VM stubs is forbidden; CALLING them is fine (probe).
#define MC2_GETD3DDEVICE_THUNK ((uintptr_t)0x0047f2f0u)

// Probe (a) target: the OTHER per-frame counter (.data, bumped in
// GameTimeAccumulate_Update). Deliberately not 0x011755bc — that one is what
// the launcher polls; keep the probe off it so its evidence stays clean.
// The counter is diagnostic-only, so a tick landing mid-probe corrupting it
// by one increment is acceptable (logged, never "fixed").
#define MC2_PROBE_DATA_BYTE ((uintptr_t)0x017bad00u)

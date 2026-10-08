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
#define MC2_VIEW_STRIDE ((uintptr_t)0x810u)
#define MC2_VIEW_OBJ_PTR_OFF ((uintptr_t)0x7e4u)
#define MC2_VIEW_TABLE3 ((uintptr_t)0x014095e0u)
// Companion-table liveness byte (Ghidra plate: flips 0->1 when the view's
// camera data goes live; the 12 loading-template views stay 00, live world
// views show 01 — live-verified 2026-10-06).
#define MC2_VIEW_T3_LIVE ((uint32_t)1u)
#define MC2_VIEW_TABLE3_STRIDE ((uintptr_t)0x20u)
#define MC2_VIEW_T3_OFF ((uintptr_t)0x18u)
// Camera staging block inside the frame-ctx object (RenderQueue_SubmitWorldPackets
// plate): per active view, a 0x30-byte record staged at this+0xc2110+idx*0x30 —
// {pos3, serial, rot16=quat @+0x10, lodByte@+0x20} — copied there by the PLAINTEXT
// staging code (watch-proven: 0x0048EC46/0x0048EC5E) and consumed by the VM'd
// packet interpreter post-walk. Watching it catches the culling consumer's reads.
#define MC2_VIEW_STAGING_OFF ((uintptr_t)0xc2110u)
#define MC2_VIEW_STAGING_STRIDE ((uintptr_t)0x30u)
#define MC2_VIEW_STAGING_QUAT_OFF ((uintptr_t)0x10u)
#define MC2_VIEW_STAGING_SERIAL_OFF ((uintptr_t)0xcu)
// Active-view intrusive list head = INDEX into g_ViewTable (negative terminates;
// link = ViewEntry+0x4). S0-corrected semantics; render_dump logs it per frame.
#define MC2_ACTIVE_VIEW_LIST_HEAD ((uintptr_t)0x00d29e60u)

// ViewEntry camera-field offsets (plate on g_ViewTable; the suspected culling
// inputs — docs/reverse_engineering/view_and_camera.md). All 4-byte aligned
// relative to the 0x810-stride table (required for DR LEN=4 watchpoints):
//   +0x20  slot[0].mtx[0] row0 (viewToWorld, camera pos at row 3)
//   +0x60  slot[0].mtx[1] row0 (worldToView, negated pos)
//   +0x8c  slot[0].dir[3]   +0xa8 slot[0].params  +0x188 near-plane-ish
//   +0x2ec fovH / +0x2f0 fovV / +0x2f4 = 0 (fov half-angle extents; static
//           decode 2026-10-08: the live (kindA4==0) write at 0x0048a955 stores
//           {h*scale*100, 0, v*scale*100} from the global-cam object — the old
//           "fovCos/fovSin" names are wrong; observed +0x2ec 0.957826 ~
//           tan(43.75deg), plausibly tan(half-angle))  +0x670 dir670[3]
//   +0x7ac pos prev / +0x7c4 pos cur / +0x7d0 pose serial / +0x7d4 quat[4]
#define MC2_VIEW_OFF_SLOT0      ((uintptr_t)0x20u)
#define MC2_VIEW_OFF_SLOT0V      ((uintptr_t)0x60u)
#define MC2_VIEW_OFF_SLOTDIR     ((uintptr_t)0x8cu)
#define MC2_VIEW_OFF_SLOTPARAMS  ((uintptr_t)0xa8u)
#define MC2_VIEW_OFF_NEAR        ((uintptr_t)0x188u)
#define MC2_VIEW_OFF_FOVCOS      ((uintptr_t)0x2ecu) // fovH extent
#define MC2_VIEW_OFF_FOVMID      ((uintptr_t)0x2f0u) // fovV extent
#define MC2_VIEW_OFF_FOVSIN      ((uintptr_t)0x2f4u) // always 0 (old name)
#define MC2_VIEW_OFF_DIR670      ((uintptr_t)0x670u)
#define MC2_VIEW_OFF_POS_PREV    ((uintptr_t)0x7acu)
#define MC2_VIEW_OFF_POS_CUR     ((uintptr_t)0x7c4u)
#define MC2_VIEW_OFF_SERIAL      ((uintptr_t)0x7d0u)
#define MC2_VIEW_OFF_QUAT        ((uintptr_t)0x7d4u)
#define MC2_VIEW_OFF_CAMDATA     ((uintptr_t)0x7ecu)
#define MC2_VIEW_OFF_FLAGS808    ((uintptr_t)0x808u)
#define MC2_VIEW_TYPE2 ((uint32_t)2u) // normal world view (ViewRef.type14)

// Global-cam object — the SOURCE of the entry fov extents. Chain (static
// decode 2026-10-08, ViewEntry_MatrixFromGlobalCam 0x0048a8f0 fallback
// path, which is the live one — live views take kindA4==0): owner =
// *(u32*)0x00e79dfc; gcam = *(u32*)(owner+0x104); then fovH = *(float*)
// (gcam+0x17c), fovV = *(float*)(gcam+0x180), scale = *(float*)(gcam+0x184),
// and the entry triple is written as {fovH*scale*100, 0, fovV*scale*100}
// (+0x2ec/+0x2f0/+0x2f4). The cull task (0x00876a90) scales its bound
// vectors by these — widening them widens the culling volume
// (frustum_cull_plan.md open item).
#define MC2_G_GLOBALCAM_OWNER      ((uintptr_t)0x00e79dfcu)
#define MC2_GLOBALCAM_OBJ_OFF     ((uintptr_t)0x104u)
#define MC2_GLOBALCAM_FOV_H_OFF   ((uintptr_t)0x17cu)
#define MC2_GLOBALCAM_FOV_V_OFF   ((uintptr_t)0x180u)
#define MC2_GLOBALCAM_FOV_SCALE_OFF ((uintptr_t)0x184u)

// End of the fov-triple write in ViewEntry_MatrixFromGlobalCam's fallback
// (kindA4==0 — the LIVE path; S5 watch-proven writer). 0x0048a966/6e/75 store
// {fovH*k, 0, fovV*k} at EDI = entry+0x2ec (k = gcamScale*100, near dist;
// {fovH,fovV} unit, v/h = tanV/tanH); 0x0048a97a re-reads [EDI] for the
// length sqrt. MidHook there = cull_fov_widen: scale the just-written [EDI]
// by tanHmdHalfH/fovH and [EDI+8] by tanHmdHalfV/fovV (frustum_cull_plan.md
// D0) — widens the culling cross-section to the HMD FOV before the cull
// task (0x00876a90) consumes it via Vector_Scale.
#define MC2_VIEWENTRY_FOV_WRITE_END ((uintptr_t)0x0048a97au)

// g_RenderShell (object, holds live base LtiRenderer_vtbl at frame time) and
// its pointer global. VmtHook claim target for slots 4 (EndOfFrameHook) /
// 5 (PostUpdateHook), called by GameShell_FrameTick every frame.
#define MC2_G_RENDERSHELL ((uintptr_t)0x017ceaf0u)

// g_RenderQueue ring buffer fields (docs/reverse_engineering/render_path.md): base is the struct;
// elementSize base+4, capacity base+8, buffer base+0xc, counters base+0x10/
// base+0x14 (S0 decode: countersA base+0x10 packs low16 = consumer-advanced,
// high16 = producer-advanced; countersB base+0x14 packs low16 = producer
// batch count), CS base+0x18.
// g_RenderQueue ring — S2c (stream replay) timing inputs; ring
// position = queue+0x10 low16 % cap (S1b, verified).
#define MC2_G_RENDERQUEUE ((uintptr_t)0x00ff3618u)
#define MC2_QUEUE_ELEM_SIZE ((uintptr_t)0x00ff361cu)  // u32 = 96
#define MC2_QUEUE_CAPACITY ((uintptr_t)0x00ff3620u)   // u32 = 4096
#define MC2_QUEUE_BUFFER ((uintptr_t)0x00ff3624u)     // u32 -> element array
#define MC2_QUEUE_COUNTERS_A ((uintptr_t)0x00ff3628u) // packed u16 consumer/producer
#define MC2_QUEUE_COUNTERS_B ((uintptr_t)0x00ff362cu) // packed u16 producer batch

// g_RenderQueue2 (0x00ff3650) — 2D/overlay submissions (docs/reverse_engineering/view_and_camera.md).
// No plaintext xref: the frame-ctx block hands &g_RenderQueue2 to the SecuROM-VM'd
// interpreter (frame-ctx +0x78/+0x90; queue1's counters are at +0x60/+0x6c/+0x9c/+0xa8).
// Same struct as g_RenderQueue (elem +0x04, cap +0x08, buffer +0x0c, countersA +0x10
// = low16 consumer-advanced / high16 producer elements, countersB +0x14). S4-5 HUD
// timing: the carrier samples these counters at Present + pass boundaries because
// the consumer itself is unhookable (VM).
#define MC2_G_RENDERQUEUE2 ((uintptr_t)0x00ff3650u)
#define MC2_QUEUE2_ELEM_SIZE ((uintptr_t)0x00ff3654u)
#define MC2_QUEUE2_CAPACITY ((uintptr_t)0x00ff3658u)
#define MC2_QUEUE2_BUFFER ((uintptr_t)0x00ff365cu)
#define MC2_QUEUE2_COUNTERS_A ((uintptr_t)0x00ff3660u)
#define MC2_QUEUE2_COUNTERS_B ((uintptr_t)0x00ff3664u)

// GetD3DDevice — thunk (6 bytes, jmp into a SecuROM VM stub), void* (void),
// 12 call sites. Hooking VM stubs is forbidden; CALLING them is fine (probe).
// g_LtiRenderer (LtiRenderer*; NULL until the engine builds it) and the
// offset of its dx9State (= the live IDirect3DDevice9*). Reading these is what
// GetD3DDevice_Impl does, without entering the VM-slot thunk.
#define MC2_G_LTIRENDERER ((uintptr_t)0x01175288u)
#define MC2_LTIRENDERER_DX9STATE_OFF ((uintptr_t)0x5bcu)
#define MC2_GETD3DDEVICE_THUNK ((uintptr_t)0x0047f2f0u)

// RenderSystem's HWND (stored by RenderSystem_Init 0x0074c7a0: DAT_01175274 =
// the window it was given; GetWindowThreadProcessId'd right after). Read at
// MC2VR_MSG_EXIT time to post WM_CLOSE to the game's root window — the message
// pump/WndProc quit path is VM-protected (PostQuitMessage/GetMessageA are only
// referenced from the SecuROM region), so WM_CLOSE is the standard, engine-
// expected clean-quit signal.
#define MC2_G_RENDER_HWND ((uintptr_t)0x01175274u)

// Direct3DCreate9 IAT thunk (6 bytes: JMP dword ptr [0x00b05620], the D3D9.DLL
// import slot), plaintext .text, single caller: RenderSystem_Init
// (0x0074c7a0, call at 0x0074c7e6, SDK version 0x20, result stored to g_D3D9
// 0x01175284). The IDirect3D9::CreateDevice call itself happens behind the
// VM/gate trampoline FUN_0074c9b0 (indirect through 0x024cd09c, inside the
// SecuROM region — never hooked), so the plaintext way to reach device
// creation params is: InlineHook this thunk, VmtHook the returned IDirect3D9
// (CreateDevice = slot 16) and patch the D3DPRESENT_PARAMETERS there.
// S4-5 pacing: vsync=off forces PresentationInterval=IMMEDIATE (two
// vsync-locked Presents per frame cap the game at ~30 Hz).
#define MC2_D3DCREATE9_THUNK ((uintptr_t)0x00a4e892u)

// PgPrimitive_SubmitToGPU (0x00855690, plate on the function) — per-frame D3D
// submission: BeginSubmit (Present prev + BeginScene + GPU-sync + RT set +
// Clear) then the SecuROM-mutated per-record walk (state + ExecuteStream +
// bind + draw per record) then EndSubmit. void(void), single caller
// (RenderShell_RenderFrameTimed 0x0085abd0). S2c-1 second draw pass = call
// this twice (re-entrant between frames: EndSubmit clears the in-scene flag).
#define MC2_PGPRIMITIVE_SUBMITTOGPU ((uintptr_t)0x00855690u)

// viewContextData upload gate in PgPrimitive_SubmitToGPU (2026-10-03):
// 0x00855a78 = `cmp dword ptr [edi+0xd8], 0` immediately before the gated
// call (JLE skips) — `mov edx,[esp+0x18]; push edx; lea eax,[edi+0xd4]; call
// Dx9_SetVertexShaderConstantF (0x00749200)` which dispatches to the device
// VmtHook slot 94 (= view::on_set_vs_constant). At this instruction EDI = the
// CURRENT technique object (g_LastTechnique is stale here — updated only at
// the end of the record loop). The technique's resolved constant map (plate
// on Technique_ResolveConstantRegisters 0x0085b260): +0xd4 viewContextData
// reg, +0xd8 count, +0xdc viewContextData.ViewProj reg, +0xe0 count. The
// carrier's MidHook here publishes the exact register map so the
// view rewrite targets registers, not row shapes (shape heuristics conflate
// techniques that share register numbers).
//
// [esp+0x18] AT THE GATE = the viewContext RECORD pointer for the pass being
// uploaded (the wrapper's data arg: `mov edx,[esp+0x18]; push edx`; the PS
// upload gate just below re-reads the same slot). The MidHook logs each
// distinct record VA + computed index so an E1 `debug_watch=addr:` run can
// target the main pass's record directly (docs/stereo_improvements_plan.md).
#define MC2_VCD_UPLOAD_CMP ((uintptr_t)0x00855a78u)
#define MC2_VCD_GATE_REC_SLOT ((uintptr_t)0x18u)  // [esp+0x18] = record ptr
#define MC2_TECH_VCD_REG_OFF ((uintptr_t)0xd4u)
#define MC2_TECH_VCD_COUNT_OFF ((uintptr_t)0xd8u)
#define MC2_TECH_VP_REG_OFF ((uintptr_t)0xdcu)
#define MC2_TECH_VP_COUNT_OFF ((uintptr_t)0xe0u)

// g_ViewContextTable (2026-10-06, E1 prep): holds a POINTER, not the array.
// Sole plaintext xref is the initializer (FUN_00854da8, write 0x00854e6d):
//   g_ViewContextTable = 0x018c45e0 + DAT_00ff364c * 0xe00
// — a double-buffered array of 32 records x 0x70 stride, buffer-selected by
// the same frame index that swings g_PrimitiveBase/g_MaterialTable/etc.
// (readers are all VM-side, hence no plaintext read xrefs). Record layout:
// +0x00 viewContextData (VP rows first), +0x40 PS view consts, +0x60
// atmosphereData*, +0x64 globalLightData* (docs/reverse_engineering/
// view_and_camera.md). A record VA rec decomposes as
//   base = *g_ViewContextTable;  idx = (rec - base) / 0x70  (idx < 32)
#define MC2_G_VIEWCONTEXTTABLE ((uintptr_t)0x01169774u)
#define MC2_VIEWCONTEXT_STRIDE ((uintptr_t)0x70u)
#define MC2_VIEWCONTEXT_RECORDS ((uintptr_t)0x20u)   // per buffer
#define MC2_VIEWCONTEXT_BUFFERSZ ((uintptr_t)0xe00u) // 32 * 0x70

// Record-fill epilogue in ViewContext_BuildCameraConstants (2026-10-08,
// frustum_cull_plan.md I3): the function writes the finished VP INLINE into
// the current g_ViewContextTable record — record = 0x018c45e0 + (bufsel*0x20
// + recidx)*0x70, VP rows at record+0x00..0x3c — then bumps recidx
// (0x00ed9d42) BEFORE the copies, so at the epilogue the just-filled record
// index is recidx-1. 0x00859912 = `MOV [0x01163734],EDI` (6 bytes, last
// store before the pops + RET 4 at 0x0085991e): a MidHook there runs after
// every record write — record_fov_widen rewrites the just-filled record's
// VP projection rows to the HMD FOV union (view_rewrite.cpp; matched against
// the latest main-pass decomposed camera so shadow/offscreen records skip).
#define MC2_VIEWCTX_ARRAY_BASE  ((uintptr_t)0x018c45e0u) // both-buffer array base
#define MC2_VIEWCTX_BUFSEL      ((uintptr_t)0x00ff364cu) // u32 0/1 buffer select
#define MC2_VIEWCTX_RECIDX      ((uintptr_t)0x00ed9d42u)  // u32 next record idx
#define MC2_VIEWCTX_BUILD_EPILOG ((uintptr_t)0x00859912u)

// Site B — the REAL full-VP record fill in the mutated 0x004671xx fill block
// (raw-decoded 2026-10-08; E1's live watch saw a main-record row0 fill here at
// 0x004673bf): inline fld/fstp copy of the COMPLETE 16-dword VP
// (record+0x00..0x3c) with EAX = record, idx byte bumped before. The last
// store FSTP [EAX+0x3C] ends at 0x0046742a — the carrier's PRIMARY record-fov
// hook point (site A, the Matrix_Copy3x4 call at 0x0046718c, copies only 12
// dwords = rows 0-2 and never writes row 3 — aux records; the gate's 4-row
// decompose only succeeds on site-B records).
#define MC2_VIEWCTX_FILLB_COMPLETE ((uintptr_t)0x0046742au)

// THE CULL FRUSTUM TERMS (round-15 watch, 2026-10-08): the frame-ctx
// (0x017CF980) holds the projection's tan extents at +0x30 (tanHalfH) and
// +0x34 (tanHalfV) — written by the builder at 0x00859425/0x00859436
// (traps fire at 0x0085942A/0x0085943B), then consumed by:
//   - THE CULL TESTS: 0x0047E35F (DIVSS XMM2,[ctx+0x30]) / 0x0047E36A
//     (DIVSS [ctx+0x34]) and 0x0047ED49 / 0x0047ED63 — per-object loops
//     (object structs in ESI/EDI) dividing bounds by the frustum extents;
//   - 0x0061B973/0x0061B97A (FLD ctx+0x30/0x34 -> copies into ctx+0x56/0x5A
//     — a downstream derivation).
// CRITICAL DETAIL: at 0x0085943B the builder's next instruction is
// `LEA EAX,[EBX+0xB20]` — the projection matrix is built from REGISTER
// copies, NOT the ctx slots — so overwriting the slots at 0x0085943B
// widens ONLY the slot consumers (the cull), leaving the projection stock.
// THE CLEAN FIX: hook 0x0085943B, verify the just-written terms are the
// MAIN view's (match 1/g_game_cam.a within tolerance — shadow builds write
// their own terms), then write tan(hmdHalfH+margin)/tan(hmdHalfV+margin).
#define MC2_VCTX_TAN_STORES_DONE ((uintptr_t)0x0085943bu)
#define MC2_FRAMECTX_TANH_OFF   ((uintptr_t)0x30u)
#define MC2_FRAMECTX_TANV_OFF   ((uintptr_t)0x34u)

// THE PER-VIEW SNAPSHOT (round-17/18, 2026-10-08): FUN_0061b930
// (called by FUN_0061b7e0 ~5.5/frame, once per view) clones the view's
// render-slot ctx into a per-view state object (param_1, in EBX) —
// INCLUDING the tan extents (param_1[0xc]/[0xd] = +0x30/+0x34), the VP
// rows (+0x10..0x1c), and the ctx+0x8c0/0x900 matrices (+0x230/+0x240).
// THIS snapshot is what the per-object cull reads (no static address —
// why every static-slot watch missed it). Round 16's static-ctx override
// (0x0085943b) wrote a DIFFERENT ctx object than the snapshot's source —
// hence the clean negative. THE FIX: hook the snapshot tail — 0x0061BB4E
// = `FSTP [EBX+0xE70]` (6 bytes, EBX = param_1 = the snapshot, still live;
// the function then does MOV EAX,EBX + more [EBX+0xE74] stores + RET 8) —
// and rewrite the snapshot's tan fields to the HMD-union values. The
// snapshot's source ctx/records stay stock (rendering stock); only the
// per-view cull state sees the HMD frustum.
#define MC2_VIEW_SNAPSHOT_TAN_SITE ((uintptr_t)0x0061bb4eu)

// Site C — the WATCH-PROVEN main-record VP fill completion (2026-10-08, I3 round 3:
// DR watchpoints on the main records' row0/row3 dwords hit 0x00859774 [FSTP [EAX],
// row0] and 0x00859807 [after FSTP [EAX+0x30], row3] ~1100x/window with
// EAX = MAIN records 018c4730/018c5530): the full 16-dword VP copy in
// ViewContext_BuildCameraConstants continues D9 58 34/38/3C, the last store
// FSTP [EAX+0x3C] at 0x0085982C ends at 0x0085982F — EAX = the record, VP
// complete, +0x40 PS writes not yet done. NOTE: the mutated function exits
// this path without reaching the 0x00859912 epilogue (the epi hook never saw
// a main record — identity/aux calls only), which is why the earlier hook
// never fired. This is the PRIMARY record-fov hook point.
#define MC2_VIEWCTX_FILLC_COMPLETE ((uintptr_t)0x0085982fu)

// Camera-entry template fill epilogue (raw-decoded 2026-10-08): the fill
// (CamPose_CopyGlobal_To_Entry 0x004665b0 — clear-from-zero-template +
// constants) writes via ESI = entry: near +0x50 (=[0x00B92B58]), far +0x54
// (=[0x00BEAC28]=100), fov +0x58 (=[0x00BEAB5C]=300.0 — the game's fov in
// engine units, feeds the projection tan-table AND the 0x0048067E cull-reader
// +0x44/+0x48/+0x4c/+0x5c/+0x60/+0x64 misc. Hook point
// 0x0046662F = the MOVSS [ESI+0x64] store (exactly 5 bytes; ESI = entry;
// +0x58 already written) — the carrier's entry_fov diagnostic + scale point
// (frustum_cull_plan.md: the ADS-narrowing clue implicates the entry fov
// chain as the operative cull input). CAREFUL: 0x0046662e is the LAST BYTE
// of the previous MOVSS [ESI+0x60] — hooking there decodes garbage and
// crashes the game at boot (live-proven 2026-10-08, round-5 staging).
#define MC2_CAMENTRY_FILL_EPILOG ((uintptr_t)0x0046662fu)

// THE game fov constant (decoded 2026-10-08, frustum_cull_plan.md I3 round 5):
// 300.0 in engine units (NOT a cos — the old "fovCos" names are wrong). Read
// by EIGHT sites — the whole game fov chain derives from this one dword:
// CamPose_CopyGlobal_To_Entry 0x004665b0 (+0x58 store) and FUN_004660a6
// (camera-entry fillers — the latter likely the STACK-LOCAL gameplay entry
// the fill hook missed), plus FUN_0070aa60 / FUN_0070a910 / FUN_0070aff6
// (the 0x0070axx camera/view fov derivations — the gcam cull-fov sources).
// Baseline (live 2026-10-08): value 300 -> rendered projection a=1.3440
// b=2.3894 (halfH=36.65° halfV=22.71°, aspect-locked 16:9; CONSTANT under
// ADS). Scale relation ≈ angle-linear (feeds a tan-table index ∝ value).
// The carrier's entry_fov_scale PATCHES THIS CONSTANT at init — every
// consumer (projection, cull derivations, entries) widens in the game's own
// parametrization. NOTE: one value can't shape the HMD's ~1:1 tan aspect —
// the widened frustum stays 16:9-shaped, over-covering horizontally
// (conservative for culling: halfV 60° ⇒ halfH ~97°).
#define MC2_CAMENTRY_FOV_CONST ((uintptr_t)0x00beab5cu)  // float 300.0

// ViewContext_BuildCameraConstants (E1b watch-proven 2026-10-06, plate there):
// the plaintext draw-camera VP builder, called ONLY from the VM via thunk
// 0x00506a26 (VMThunk_ViewContext_BuildCameraConstants). Entered with the
// CALLER'S EBP live (mutated convention — `mov ebx,[ebp+8]` is the first arg
// access, no prologue first): first arg = [ebp+8] = the view render-ctx
// struct. [arg+0x28] = the CAMERA OBJECT: a self-indexed 0x70-stride array
// base — active entry = base + *(u32*)base * 0x70; entry+0x50/0x54/0x58 =
// near/far/fov floats, +0x74 another param (self-indexed dword access
// obj[obj[0]*0x1c + 0x14/15/16/1d] in the decompiler). The view matrix is
// read from the camera object via Matrix_Copy3x4 inside; the built VP lands
// in scratch 0x017D04E0 -> the g_ViewContextTable records. E2b probe hooks
// this plaintext entry (neighbor, allowed) to log/dump the camera object —
// the remaining upstream injection candidate.
#define MC2_VCCAMERA_BUILDER ((uintptr_t)0x008591acu)
#define MC2_VCCAM_ARG_EBP_OFF ((uintptr_t)0x8u)      // arg = [ebp+8] at entry
#define MC2_VCCAM_CTX_CAMARRAY_OFF ((uintptr_t)0x28u) // camera object slot
#define MC2_VCCAM_ENTRY_STRIDE ((uintptr_t)0x70u)
// Camera object entry layout (E2b live-dumped 2026-10-06; convention
// PROBE-PROVEN 2026-10-07 — camprobe transfer run, see the plate on
// CameraTable_FillFromPose):
//   +0x00 status dword (1.0 = populated slot, 0 = empty)
//   +0x10..0x3c 3x3 rotation, row-major — the RENDERED camera's axes are
//                its ROWS: R = -row0, U = row1, F = row2. TRANSFER LAW for
//                rewrites: E' = E*M renders axes' = M^-1 * axes (the
//                builder's 4x4 inverse, FUN_008225c0, sits between the
//                entry and the view). The carrier therefore writes the
//                closed form E' = S_r*L^-1*S_r*E (LEFT multiply, S_r =
//                diag(measured row signs) = diag(-1,1,1); composite quat
//                (qx,-qy,+qz,qw) for the desired local rotation
//                L = (-qx,-qy,+qz,qw)).
//   +0x40 position (x,y,z) + w=1.0 — world position, positive
//   +0x50 near, +0x54 far, +0x58 fovCos (the game's widescreen projection
//                source — LEFT-HANDED pipeline, clip.w = +z_view; per-eye
//                OpenXR FOV is applied at the record level,
//                view_row_rewrite=hmd_delta)
//   +0x50 near, +0x54 far (2400), +0x58 fovCos (0.9597)
//   +0x60.. LOD-ish params (0.1, 100, 300, 0.3)
// The array base is a self-index (active idx = *(u32*)base). GAMEPLAY
// instances are STACK-LOCAL (arr ~0x072e9xxx/0x072eadd0, ctx 0x072e9d20/
// 0x072eafb0, built fresh per builder call in the VM's frames); the menu
// used the STATIC global g_CameraTable (0x014a2ee0, ctx 0x017cf980 — the
// same global frame-ctx seen in the E1b watch hits).
#define MC2_VCCAM_ENTRY_ROT_OFF ((uintptr_t)0x10u)
#define MC2_VCCAM_ENTRY_POS_OFF ((uintptr_t)0x40u)
#define MC2_VCCAM_ENTRY_NEAR_OFF ((uintptr_t)0x50u)
#define MC2_VCCAM_ENTRY_FAR_OFF ((uintptr_t)0x54u)
#define MC2_VCCAM_ENTRY_FOVCOS_OFF ((uintptr_t)0x58u)

// g_CameraPoseClearBlock (E2b step-3 CORRECTED 2026-10-06, plate there): a
// STATIC zero template (never written at runtime) — NOT a live pose source
// (the step-2 "canonical pose global" interpretation is retracted).
// CamPose_ClearEntryPose (0x004665b0) copies it into the per-call camera
// entries to clear their pose fields for refill (~6.5/frame). The 29 static
// ref sites read it as the engine default pose. The LIVE fill of the camera
// entries is the remaining unknown (E2b step 4).
#define MC2_G_CAMERA_POSE_CLEAR ((uintptr_t)0x00dfbbd0u)
#define MC2_CAMPOSE_CLEAR_ENTRY ((uintptr_t)0x004665b0u)

// ---- g_CameraTable: union HMD injection site (2026-10-07, docs/ ----
// ---- stereo_improvements_plan.md "Decided architecture")           ----
// g_CameraTable = 5 camera-entity slots x 0x620 (constructed by FUN_0070f020:
// FUN_00401890(&g_CameraTable,0x620,5,ctor)); camera-object ptr at slot+0x1e0,
// fov source at slot+0x614. Once per frame the CameraTable_FillFromPose loop
// (FUN_0070f430, tail do-while at 0x0070f62c, from InGameShellState_FramePipeline)
// fills every slot whose +0x1e0 camera object exists (~1.7/frame live) from the
// camera entity's quat+pos. Each slot is a self-indexed 0x70-stride camera
// entry array (active entry = slot + [slot+4]*0x70) with the MC2_VCCAM_ENTRY_*
// layout; the fill's Matrix_Copy3x4 (call 0x0070aef3 -> 0x00836120, dest
// `lea eax,[ecx+esi*1+0x10]` at 0x0070aee5) writes entry+0x10..0x4c: rotation
// rows +0x10/+0x20/+0x30, position +0x40.
#define MC2_G_CAMTABLE ((uintptr_t)0x014a2ee0u)
#define MC2_CAMTABLE_SLOT_STRIDE ((uintptr_t)0x620u)
#define MC2_CAMTABLE_SLOTS ((uintptr_t)5u)
// MidHook site: the first instruction AFTER the fill copy's call (ret addr
// 0x0070aef8, `fld [ebp+8]`). At this instruction EAX = the just-filled
// entry+0x10 (single caller = the fill loop; ESI = slot base, live across the
// call). The carrier rewrites EAX's rotation rows + position in place —
// after every fill, before every consumer (draw-camera builder path AND the
// culling/fov readers 0x0048067E family read this table).
#define MC2_CAMTABLE_FILL_COPY_END ((uintptr_t)0x0070aef8u)

// Probe (a) target: the OTHER per-frame counter (.data, bumped in
// GameTimeAccumulate_Update). Deliberately not 0x011755bc — that one is what
// the launcher polls; keep the probe off it so its evidence stays clean.
// The counter is diagnostic-only, so a tick landing mid-probe corrupting it
// by one increment is acceptable (logged, never "fixed").
#define MC2_PROBE_DATA_BYTE ((uintptr_t)0x017bad00u)

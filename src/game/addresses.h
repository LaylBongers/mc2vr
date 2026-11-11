// Hook/probe sites and other hard facts from the Ghidra project, as macros.
// Per-address findings live in Ghidra; this header only records what the
// build needs. Game data with a known shape (structs, typed globals) and
// original functions go in the typed headers next to it (pg_primitive.hpp,
// render_cmd.hpp, function.hpp, ...) — new entries there, not here; the
// offset macros below move over as their users are touched. All of src/game
// is keyed to the exe hash in src/carrier/build_lock.h (build-lock discipline).
//
// The exe image cannot relocate (fixed load base, no relocations); every VA
// below is a literal runtime address, not a base-relative RVA.
#pragma once

// Expected load base of Mercenaries2.exe. Both components refuse to operate
// if the real base differs — every VA below is absolute.
#define MC2_GAME_BASE_EXPECTED ((uintptr_t)0x00400000u)

// ---- hook/probe sites (all plaintext .text/.data, well below the ----
// ---- SecuROM region at 0x01a48000; see docs/hooks.md hook list) --

// GameShell_FrameTick — void (void), called once per main-loop iteration from
// GameShell_Run (single call site). The main inline hook: frame counting,
// timing sanity, install ack.
#define MC2_GAMESHELL_FRAMETICK ((uintptr_t)0x00630e10u)

// ---- instrument sites (instruction-level, verified by disassembly) ----

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
// staging code (watch-proven: 0x0048EC46/0x0048EC5E) and consumed post-walk by an
// unlocated consumer (not thunk 0x0050c660). Watching it catches the culling consumer's reads.
#define MC2_VIEW_STAGING_OFF ((uintptr_t)0xc2110u)
#define MC2_VIEW_STAGING_STRIDE ((uintptr_t)0x30u)
#define MC2_VIEW_STAGING_QUAT_OFF ((uintptr_t)0x10u)
#define MC2_VIEW_STAGING_SERIAL_OFF ((uintptr_t)0xcu)
// Active-view intrusive list head = INDEX into g_ViewTable (negative terminates;
// link = ViewEntry+0x4). Corrected semantics; render_dump logs it per frame.
#define MC2_ACTIVE_VIEW_LIST_HEAD ((uintptr_t)0x00d29e60u)

// ViewEntry camera-field offsets (plate on g_ViewTable; the suspected culling
// inputs — docs/reverse_engineering/camera_data_flow.md). All 4-byte aligned
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




// g_RenderShell (object, holds live base LtiRenderer_vtbl at frame time) and
// its pointer global. VmtHook claim target for slots 4 (EndOfFrameHook) /
// 5 (PostUpdateHook), called by GameShell_FrameTick every frame.
#define MC2_G_RENDERSHELL ((uintptr_t)0x017ceaf0u)

// g_RenderQueue ring buffer fields (docs/reverse_engineering/view_table.md): base is the struct;
// elementSize base+4, capacity base+8, buffer base+0xc, counters base+0x10/
// base+0x14 (static decode: countersA base+0x10 packs low16 = consumer-advanced,
// high16 = producer-advanced; countersB base+0x14 packs low16 = producer
// batch count), CS base+0x18.
// g_RenderQueue ring — stream-replay timing inputs; ring
// position = queue+0x10 low16 % cap (runtime-verified).
#define MC2_G_RENDERQUEUE ((uintptr_t)0x00ff3618u)
#define MC2_QUEUE_ELEM_SIZE ((uintptr_t)0x00ff361cu)  // u32 = 96
#define MC2_QUEUE_CAPACITY ((uintptr_t)0x00ff3620u)   // u32 = 4096
#define MC2_QUEUE_BUFFER ((uintptr_t)0x00ff3624u)     // u32 -> element array
#define MC2_QUEUE_COUNTERS_A ((uintptr_t)0x00ff3628u) // packed u16 consumer/producer
#define MC2_QUEUE_COUNTERS_B ((uintptr_t)0x00ff362cu) // packed u16 producer batch

// g_RenderQueue2 (0x00ff3650) — 2D/overlay submissions (docs/reverse_engineering/view_table.md).
// No plaintext xref: the frame-ctx block carries &g_RenderQueue2 to an unlocated
// render-worker consumer (frame-ctx +0x78/+0x90; queue1's counters are at +0x60/+0x6c/+0x9c/+0xa8).
// Same struct as g_RenderQueue (elem +0x04, cap +0x08, buffer +0x0c, countersA +0x10
// = low16 consumer-advanced / high16 producer elements, countersB +0x14). HUD
// timing: the carrier samples these counters at Present + pass boundaries because
// the consumer itself is unhookable (VM).
#define MC2_G_RENDERQUEUE2 ((uintptr_t)0x00ff3650u)
#define MC2_QUEUE2_ELEM_SIZE ((uintptr_t)0x00ff3654u)
#define MC2_QUEUE2_CAPACITY ((uintptr_t)0x00ff3658u)
#define MC2_QUEUE2_BUFFER ((uintptr_t)0x00ff365cu)
#define MC2_QUEUE2_COUNTERS_A ((uintptr_t)0x00ff3660u)
#define MC2_QUEUE2_COUNTERS_B ((uintptr_t)0x00ff3664u)

// g_LtiRenderer (LtiRenderer*; NULL until the engine builds it) and the
// offset of its dx9State (= the live IDirect3DDevice9*). Reading these is what
// GetD3DDevice_Impl does, without entering the VM-slot thunk.
#define MC2_G_LTIRENDERER ((uintptr_t)0x01175288u)
#define MC2_LTIRENDERER_DX9STATE_OFF ((uintptr_t)0x5bcu)

// RenderSystem's HWND (stored by RenderSystem_Init 0x0074c7a0: DAT_01175274 =
// the window it was given; GetWindowThreadProcessId'd right after). Read when
// the OpenXR runtime ends the session, to post WM_CLOSE to the game's root window — the message
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
// The carrier always forces PresentationInterval=IMMEDIATE (a vsync-locked
// Present stalls the HMD; docs/pacing.md).
#define MC2_D3DCREATE9_THUNK ((uintptr_t)0x00a4e892u)

// A technique's resolved constant map (plate on
// Technique_ResolveConstantRegisters 0x0085b260; Technique in
// game/pg_primitive.hpp): viewContextData register/count and its ViewProj
// register/count. view_rewrite targets these registers, not row shapes.
#define MC2_TECH_VCD_REG_OFF ((uintptr_t)0xd4u)
#define MC2_TECH_VCD_COUNT_OFF ((uintptr_t)0xd8u)
#define MC2_TECH_VP_REG_OFF ((uintptr_t)0xdcu)
#define MC2_TECH_VP_COUNT_OFF ((uintptr_t)0xe0u)

// g_ViewContextTable (2026-10-06, E1 prep): holds a POINTER, not the array.
// Sole plaintext xref is the initializer (PgPrimitive_PublishFrame_body, write 0x00854e6d):
//   g_ViewContextTable = 0x018c45e0 + DAT_00ff364c * 0xe00
// — a double-buffered array of 32 records x 0x70 stride, buffer-selected by
// the same frame index that swings g_PrimitiveBase/g_MaterialTable/etc.
// (readers are all VM-side, hence no plaintext read xrefs). Record layout:
// +0x00 viewContextData (VP rows first), +0x40 PS view consts, +0x60
// atmosphereData*, +0x64 globalLightData* (docs/reverse_engineering/
// view_context_records.md). A record VA rec decomposes as
//   base = *g_ViewContextTable;  idx = (rec - base) / 0x70  (idx < 32)
#define MC2_G_VIEWCONTEXTTABLE ((uintptr_t)0x01169774u)
#define MC2_VIEWCONTEXT_STRIDE ((uintptr_t)0x70u)
#define MC2_VIEWCONTEXT_RECORDS ((uintptr_t)0x20u)   // per buffer
#define MC2_VIEWCONTEXT_BUFFERSZ ((uintptr_t)0xe00u) // 32 * 0x70








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
#define MC2_VCCAM_CTX_CAMARRAY_OFF ((uintptr_t)0x28u) // camera object slot
#define MC2_VCCAM_ENTRY_STRIDE ((uintptr_t)0x70u)
// Camera object entry layout (E2b live-dumped 2026-10-06; convention
// PROBE-PROVEN 2026-10-07 — camprobe transfer run, see the plate on
// CameraTable_FillFromPose):
//   +0x00 status dword (1.0 = populated slot, 0 = empty)
//   +0x10..0x3c 3x3 rotation, row-major — the RENDERED camera's axes are
//                its ROWS: R = -row0, U = row1, F = row2. TRANSFER LAW for
//                rewrites: E' = E*M renders axes' = M^-1 * axes (the
//                builder's 4x4 inverse, Matrix_Inverse4x4, sits between the
//                entry and the view). The carrier therefore writes the
//                closed form E' = S_r*L^-1*S_r*E (LEFT multiply, S_r =
//                diag(measured row signs) = diag(-1,1,1); composite quat
//                (qx,-qy,+qz,qw) for the desired local rotation
//                L = (-qx,-qy,+qz,qw)).
//   +0x40 position (x,y,z) + w=1.0 — world position, positive
//   +0x50 near, +0x54 far (2400), +0x58 fov (0.9597 stock: the builder
//                takes halfH = fov * 0.375 * aspect (radians, 4:3-normalized)
//                and tanV = tanH / aspect — LEFT-HANDED pipeline, clip.w =
//                +z_view; per-eye OpenXR FOV is applied at the record level,
//                view_rewrite hmd_delta)
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

// The view ctx frustum half-extents (game/view_context.hpp ViewContext::tanH/tanV).
#define MC2_VCCAM_CTX_TANH_OFF ((uintptr_t)0x30u)
#define MC2_VCCAM_CTX_TANV_OFF ((uintptr_t)0x34u)

// The occlusion-query ring (docs/occluder_boxes.md, vr/occlusion_queries.hpp):
// RenderShell_InitDrawRecordTables' tail right after it points
// g_OcclusionQueryArray at the engine's ring (`mov [0x01163768],edx`; five
// one-byte pops follow, also the target of the skip branch);
// LtiRenderer_ReleaseRenderTargets' entry (EAX = the renderer); and the ring
// id mask in OcclusionBox_EmitPrimRecord (`and eax,0x800003ff`, the imm32's
// second byte = the id bits above 8).
#define MC2_RING_POINTER_SET ((uintptr_t)0x0085405fu)
#define MC2_RELEASE_RENDER_TARGETS ((uintptr_t)0x0074a850u)
#define MC2_RING_ID_MASK_AND ((uintptr_t)0x0046fa0du)
// After the id's mask and sign fix-up (also the `jns` target): EAX = the new
// ring id, about to go into the object's history, the counter and the record.
#define MC2_RING_ID_READY ((uintptr_t)0x0046fa22u)

// The clouds/fog pass (docs/view_rewrite.md "Camera-relative matrices"): the two emitters'
// extent [begin, end): PgClouds_EmitViewRecords
// 0x0047ded0..0x0047e8b0, FUN_0047e8c0 ..0x0047f088.
#define MC2_CLOUDS_EMIT_BEGIN ((uint32_t)0x0047ded0u)
#define MC2_CLOUDS_EMIT_END ((uint32_t)0x0047f088u)

// The builder's view frustum struct at ctx+0x38 (Frustum_Build output, 0x218 bytes):
// +0x00 near, +0x04 far, +0x0c 8 world-space corners (vec3): near-plane corners
// 0..3 = (+H,+V) (+H,-V) (-H,-V) (-H,+V) at distance `near`, then the 4 far-plane
// corners; +0x6c side-plane normals.
#define MC2_FRUSTUM_NEAR_OFF ((uintptr_t)0x0u)
#define MC2_FRUSTUM_CORNERS_OFF ((uintptr_t)0xcu)

// Third-person camera clearance from the main view's near plane (raw-decoded
// 2026-10-08, watch-found): CamCtrl_NearPlaneClearance (callers FUN_00715ad0 / FUN_00713b60,
// camera controller) block-copies the MAIN slot's frustum struct
// ([g_RenderShellPtr]+0xec8 = slot-1 ctx+0x38, `rep movsd` 0x86 dwords to
// [esp+0x18]) and keeps running maxima of the near-plane diagonal |c2-c0| and
// vertical edge |c1-c0| in the controller ([ebx+0x4d0]/[ebx+0x4d4]) — the
// camera's obstacle clearance. 0x007107F9 = `MOVSS XMM2,[ESP+0x44]` (6 bytes),
// the first instruction after the copy: the copy is at [ESP+0x18] there.
// mc2vr rescales that private copy's near corners back to the
// game's own extents (the HMD-wide quad made the camera pull in).
#define MC2_CAMCLEAR_FRUSTUM_COPIED ((uintptr_t)0x007107f9u)
#define MC2_CAMCLEAR_COPY_ESP_OFF ((uintptr_t)0x18u)

// Occluder boxes (docs/occluder_boxes.md): ViewObjects_DrawAndEmitOcclusionBoxes
// (0x00468ea0) calls OcclusionBox_EmitPrimRecord at two sites (`call rel32`,
// 5 bytes, no stack args) — per flagged object and for the ctx+0x1430 list.
// debug_occlusion_off NOPs both (occlusion culling off).
#define MC2_OCCLUSION_BOX_EMIT ((uintptr_t)0x0046edf0u)
#define MC2_OCCLUSION_BOX_CALL_A ((uintptr_t)0x00468eedu)
#define MC2_OCCLUSION_BOX_CALL_B ((uintptr_t)0x00468f12u)

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
// ---- docs/camera.md "Decided architecture")           ----
// g_CameraTable = 5 camera-entity slots x 0x620 (constructed by CameraTable_StaticInit:
// Array_ConstructElements(&g_CameraTable,0x620,5,ctor)); camera-object ptr at slot+0x1e0,
// fov source at slot+0x614. Once per frame the CameraTable_FillFromPose loop
// (CameraTable_FillLoop, tail do-while at 0x0070f62c, from InGameShellState_FramePipeline)
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
// after every fill, before every consumer (the view-context builder works on
// copies of these entries, so draw camera, culling and LOD all follow).
#define MC2_CAMTABLE_FILL_COPY_END ((uintptr_t)0x0070aef8u)

// ---- Render size + render target table (docs/reverse_engineering/
// rt_dims_source.md, render_target_table.md) ----------
// Display-size authority (INI [Render] ScrW/ScrH).
#define MC2_G_RENDERCFG_SCRW ((uintptr_t)0x00dfb328u)
#define MC2_G_RENDERCFG_SCRH ((uintptr_t)0x00dfb32cu)
// Mouse scale = cfg / client (written by OnScreenResize).
#define MC2_G_MOUSESCALE_X ((uintptr_t)0x00f7df40u)
#define MC2_G_MOUSESCALE_Y ((uintptr_t)0x00f7df44u)
// Render-size authority + aspect state on g_RenderShell (offsets).
#define MC2_RS_SCREEN_W_OFF ((uintptr_t)0x2b98u)
#define MC2_RS_SCREEN_H_OFF ((uintptr_t)0x2b9cu)
#define MC2_RS_FLAG169_OFF ((uintptr_t)0x2bc4u)
#define MC2_RS_ASPECT_X_OFF ((uintptr_t)0x2bc8u)
#define MC2_RS_ASPECT_Y_OFF ((uintptr_t)0x2bccu)
// Render target table: g_RenderShell + 0x3e90 .. + 0x3eb0 (9 slots).
#define MC2_RS_RT_TABLE_OFF ((uintptr_t)0x3e90u)
#define MC2_RS_RT_TABLE_SLOTS 9u
#define MC2_RS_RT_AUTODEPTH_OFF ((uintptr_t)0x3e94u)  // device auto depth (owns a ref)
#define MC2_RS_RT_FRONTREF_OFF ((uintptr_t)0x3e98u)   // front copy (owns a ref; op-6 source)
#define MC2_RS_RT_SCENEDEPTH_OFF ((uintptr_t)0x3ea0u) // own D24S8, or alias of +0x3e94
#define MC2_RS_RT_FRONT_OFF ((uintptr_t)0x3ea4u)      // GetRenderTarget(0) (owns a ref)
// Dx9State_SeedCachesFromDevice's auto-depth ref (released by RenderSystem_ReleaseDeviceResources).
#define MC2_G_DX9_AUTODEPTH_REF ((uintptr_t)0x01175318u)
// MidHook sites (instruction boundaries verified; no references into the
// patched bytes). OnScreenResize entry: [esp+4] = W, EAX = H, ESI = this.
// SetupRenderTargets entry: ECX = this.
#define MC2_ONSCREENRESIZE ((uintptr_t)0x007492f0u)
#define MC2_SETUPRENDERTARGETS ((uintptr_t)0x007494e0u)

// GameShell_FrameTick's per-frame virtual calls on g_RenderShell slots 5
// (PostUpdateHook) and 4 (EndOfFrameHook) — both slots point to the shared
// 1-byte VirtHook_NoOp 0x006b9360. MidHook sites = the 6-byte
// `mov ecx,[0x00dfb2f8]` that starts each call sequence (`mov edx,[ecx];
// mov eax,[edx+0x14|0x10]; call eax`); branches land only on the first byte.
// Live-proven call sites 2026-11-08 (return addresses 0x00630ef4 / 0x00630fa6).
#define MC2_FRAMETICK_POSTUPDATE_SITE ((uintptr_t)0x00630ee7u)
#define MC2_FRAMETICK_ENDOFFRAME_SITE ((uintptr_t)0x00630f99u)

// RenderSystem_CheckDevice device-restore tail: after OnScreenResize(cfg),
// RenderSystem_RestoreDeviceResources (restore: re-seeds the Dx9 caches via
// Dx9State_SeedCachesFromDevice and refills the render target table —
// SetupRenderTargets is NOT re-run on Reset, live 2026-11-08) and
// Dx9State_ApplyDefaultRenderStates. MidHook site = the 6-byte `mov edx,[0x01175288]` right after
// (no branch targets inside it).
#define MC2_CHECKDEVICE_RESTORED ((uintptr_t)0x0074d087u)

// g_SuppressPresent (bool): LtiRenderer_BeginSubmit skips its present-prev
// Present when set. PgPrimitive_SubmitToGPU sets it itself only in its
// precache branch (0x00855714) and clears it right after a successful
// BeginSubmit (0x00855766). The carrier sets it before pass 1 to retire the
// duplicate per-frame Present (render_outputs::skip_next_present).
#define MC2_G_SUPPRESS_PRESENT ((uintptr_t)0x011755c3u)

// Render-size policy sites (render_size.cpp; docs/render_resolution.md).
// OnScreenResize epilogue: `pop edi` after the last callee — single branch
// target (from 0x007494c2) on its first byte; the 5-byte patch spans
// pop edi / pop ebx / mov esp,ebp / pop ebp (ret 4 at 0x007494da).
#define MC2_ONSCREENRESIZE_EPILOGUE ((uintptr_t)0x007494d5u)
// SetupRenderTargets' cfg -> fields store (`mov [ebx+0x2b98],ecx`; ECX =
// cfg W, EAX = cfg H, loaded just before).
#define MC2_SETUP_FIELD_STORE ((uintptr_t)0x00749500u)

// SetupRenderTargets plaintext tail: the main render chain (main texture,
// depth copy, scene depth, half-size downsample targets) is sized from the
// FRONT surface's desc — GetDesc([edi] = +0x3ea4) at 0x0074a2b3 into a stack
// D3DSURFACE_DESC at [esp+0x18] (Width [esp+0x30], Height [esp+0x34]), NOT
// from the render fields. MidHook site = `mov ebx,eax` right after the call
// (EAX = HRESULT; no branch targets in the patched bytes).
#define MC2_SETUP_CHAIN_DESC ((uintptr_t)0x0074a2c1u)
#define MC2_SETUP_CHAIN_DESC_ESP_OFF ((uintptr_t)0x18u)

// RenderSystem_CheckDevice body range (true entry 0x0074ce90 — the render
// task's per-frame call at 0x0046a290; Ghidra's function starts at
// 0x0074ce9f). Its TestCooperativeLevel call is the only one render_size
// answers with D3DERR_DEVICENOTRESET to trigger the engine's own device-lost
// recovery (release -> Reset -> OnScreenResize -> restore) on a render-size
// switch.
#define MC2_CHECKDEVICE_BEGIN ((uintptr_t)0x0074ce90u)
#define MC2_CHECKDEVICE_END ((uintptr_t)0x0074d0ccu)

// HDR eye adaptation (adaptation.cpp). PgLtiBloom_CreateSurfaces logs each
// adaptive surface right after creating it; at the log pushes the holder
// address is in a register: 0x0074e83d `push 0xbd48e8` (ESI =
// &_AdaptiveLuminanceSurface, bloom +0x22e4), 0x0074e9c3 `push 0xbd4968`
// (EDI = &_pPrevAdaptiveLuminanceSurface, bloom +0x22c4). Both 5-byte pushes,
// no branch targets inside. g_pAdaptiveDest: 1x1 RT surface pointer global.
#define MC2_BLOOM_ADAPTIVE_LOG ((uintptr_t)0x0074e83du)
#define MC2_BLOOM_PREV_ADAPTIVE_LOG ((uintptr_t)0x0074e9c3u)
#define MC2_G_ADAPTIVE_DEST ((uintptr_t)0x0117529cu)

// ---- frame timing (docs/pacing.md; host-disassembled 2026-11-08) ----------
// g_FrameDeltaSec: float seconds, written at the END of GameShell_FrameTick
// (0x00630f79) as the FrameTick BODY time (QPC at entry -> after the update),
// read at the START of the next FrameTick by GameTime_ComputeDt 0x004c1690
// (0x004c169d) as the governor input, and by AdaptiveFramerate_Govern
// (0x004f5b9e). Lockstep overwrites it at FrameTick entry with the display
// time delta.
#define MC2_G_FRAMEDELTASEC ((uintptr_t)0x01175048u)
// g_Dt: float seconds, the governed simulation step (policy+0x1c copied by
// GameTime_ComputeDt at 0x004c170a); 17 readers across the game.
#define MC2_G_DT ((uintptr_t)0x01174a90u)
// dt governor GameTime_GovernDt 0x004f5720 mode: (byte [0x00dfad75] ? dword [0x01750870] :
// dword [0x0175086c]) — 0 = raw clamped [1e-4, 0.1] s; 1 = fixed substeps of
// 1/rate (count via GameTime_RecordDtSamples); 2 = one step quantized to the smallest
// multiple of 1/rate STRICTLY greater than the input. rate = float
// [0x01750874]. All runtime-set (.bss zero in the image).
#define MC2_FRAMERATE_MODE_SEL ((uintptr_t)0x00dfad75u)
#define MC2_FRAMERATE_MODE_A ((uintptr_t)0x01750870u)
#define MC2_FRAMERATE_MODE_B ((uintptr_t)0x0175086cu)
#define MC2_FRAMERATE_RATE ((uintptr_t)0x01750874u)

// Scene -> draw-record build (docs/reverse_engineering/scene_record_build.md;
// plates in Ghidra). All plaintext .text MidHook sites, no branch targets
// inside the patched bytes (checked 2026-11-08). Used by debug/build_census.
#define MC2_RENDERTASK_BEGINFRAME ((uintptr_t)0x0046a270u)   // task node 0 entry
#define MC2_RENDERTASK_RENDERFRAME ((uintptr_t)0x0046a290u)  // task node 2 entry (call CheckDevice)
#define MC2_RENDERTASK_BUILDSCENE ((uintptr_t)0x0046a2c0u)   // task node 3 entry
#define MC2_RENDERTASK_PUBLISH ((uintptr_t)0x0046a300u)      // task node 4 entry
// RenderShell_BuildSceneRecords (ESI = g_RenderShell): entry, and the loop-exit
// block `lea ecx,[esp+8]` before the closing QPC (x87 stack empty there).
#define MC2_BUILDSCENE_ENTRY ((uintptr_t)0x0085ac50u)
#define MC2_BUILDSCENE_LOOP_DONE ((uintptr_t)0x0085ad04u)
// Hud_DrawQuad (0x00617c70) just before its renderer slot-10 call: the 6-byte
// `mov ecx,[0x00dfb2f8]` after the vertex loop (not a branch target). There:
// XMM5/XMM6 = the quad's centre, [esp+0x10] = the vertices (stride 0x30; vertex 0
// y at +0x14 = centre y - half height), [ebp+8] = the depth (float).
#define MC2_HUD_DRAWQUAD_EMIT ((uintptr_t)0x00617d91u)
// Widget_CorrectForResolution (0x00627cc0, Ghidra plate) right after it loads
// the view's W/H: XMM0 = W, XMM5 = H (floats). 7 bytes (`xorps xmm7,xmm7;
// lea eax,[...]`); the flags of the `cmp` at 0x00627d01 are live for the `jz`
// at 0x00627d32 (the MidHook stub preserves EFLAGS).
#define MC2_WIDGET_LAYOUT_SIZE ((uintptr_t)0x00627d19u)
// GuiMgr_OnScreenResize (0x00609ef0; entry is mutated code 0x00544338 that
// loads EAX = [0x00dfb2f8] and jumps back into the plaintext body): the current
// view's W just loaded into ECX (`cvtsi2ss xmm0,ecx; mov ecx,[ebp+0x60]`,
// 7 bytes), and its H just loaded into EDI (`lahf; test ah,0x44; mov
// [esp+0x14],edx`, 8 bytes; flags of the ucomiss live). Both feed the GUI
// screen info [mgr+0x60]+4/+8 and the Lua NewScreenWidth/Height event.
#define MC2_GUI_RESIZE_W ((uintptr_t)0x00609f1du)
#define MC2_GUI_RESIZE_H ((uintptr_t)0x00609f2du)
// The HUD layer's screen policy (vr/hud.hpp; docs/reverse_engineering/view_blocks.md).
// The UI scale's source is our RenderShell_SetupViewports (reimpl); these are
// the GUI loads that need the eye image's scale or the layer's W/H.
// Hud_DrawWorldMarkers: the view's UI scale just loaded into XMM0 (`movzx ecx,
// word [eax+0x2b92]`, 7 bytes). ECX = view index * 0xe80.
#define MC2_WORLDMARKERS_SCALE ((uintptr_t)0x0060f659u)
// Hud_DrawPinnedMarkers: the view's UI scale just loaded into XMM0 (`mov eax,
// [ebp+0x10]; cmp dword [ebx+eax*4+0x18],0`, 8 bytes). EDI = view index * 0xe80.
#define MC2_PINNEDMARKERS_SCALE ((uintptr_t)0x00611809u)
// Hud_DrawOffscreenIndicators: the view's W (EAX) / H (ECX) just loaded from the
// view context (`movss xmm2,[ebx]; addss xmm0,xmm6`, 8 bytes).
#define MC2_OFFSCREEN_IND_WH ((uintptr_t)0x0061007du)
// Hud_IndicatorNdcToPixels: H (XMM5) / W (XMM2) just converted (`movss
// [esp+0x3c],xmm5`, 6 bytes), and later W (EDI) / H (EAX) just loaded (`mov esi,
// [ebp+0x10]; movaps xmm6,xmm4`, 6 bytes; the x87 stack holds 2 values there).
#define MC2_IND_NDC_WH_A ((uintptr_t)0x00612b91u)
#define MC2_IND_NDC_WH_B ((uintptr_t)0x00612c7au)
// GuiRenderer_BeginDisplay: the current view's x/y/W/H just copied into
// renderer (EDI) +0x6c048..+0x6c054 (`cmp dword [ecx+0x2bc4],3`, 7 bytes).
#define MC2_BEGINDISPLAY_VIEWRECT ((uintptr_t)0x004af05au)
// LtiRenderer_PublishIfNewFrame: `call PgPrimitive_PublishFrame` and its successor.
#define MC2_PUBLISH_CALL ((uintptr_t)0x0074add1u)
#define MC2_PUBLISH_RETURN ((uintptr_t)0x0074add6u)
// PgPrimitiveScratchRecord_Submit entry ([esp] = emitter return address).
#define MC2_PRIM_SCRATCH_SUBMIT ((uintptr_t)0x00854910u)
#define MC2_G_PRIM_SCRATCH_TYPE ((uintptr_t)0x0116374cu)     // scratch record type byte
#define MC2_G_SCENE_BUILD_MS ((uintptr_t)0x01175624u)        // engine's own build timer (float ms)
#define MC2_RS_SUBVIEW_COUNT_OFF ((uintptr_t)0x2b90u)        // g_RenderShell u16 sub-view count
#define MC2_RS_SUBVIEW_CUR_OFF ((uintptr_t)0x2b92u)          // g_RenderShell u16 current sub-view
// The published draw-record list and its write counter: typed in game/pg_primitive.hpp.
// Scaleform GFx 2.x GRenderer implementation (vtable 0x00bad178, ctor 0x004adfe0):
// BeginDisplay (slot 5) entry, and the class's code range (all its draw-record
// emitters lie inside it, every scene emitter outside).
#define MC2_GUI_BEGIN_DISPLAY ((uintptr_t)0x004aefd0u)
#define MC2_GUI_RENDERER_CODE_LO ((uintptr_t)0x004ae2d0u)
#define MC2_GUI_RENDERER_CODE_HI ((uintptr_t)0x004b051cu)

// ---- gameplay census (debug_gameplay_census; docs/first_person.md, ----------
// ---- docs/reverse_engineering/{camera_classes,character_pose,player_input}.md)
// Function entries (MidHooks; every entry starts with >= 5 bytes of whole
// instructions, checked 2026-11-11). Frame order in InGameShellState_FramePipeline:
// player update 0x004c9811 -> command drain 0x004c982b -> physics 0x004c984c ->
// animation 0x004c990c -> camera table 0x004c993a.
#define MC2_PLAYER_FRAME_UPDATE ((uintptr_t)0x006a0710u)     // Player_FrameUpdate, EAX = player
#define MC2_HUMAN_PHYSICS_FRAME_STEP ((uintptr_t)0x00429be0u) // HumanPhysics_FrameStep
#define MC2_ANIM_FRAME_UPDATE ((uintptr_t)0x00695a00u)       // AnimSystem_FrameUpdate
#define MC2_ANIM_POST_UPDATE ((uintptr_t)0x00694f10u)        // AnimSystem_PostUpdate (after the 3 task queues)
#define MC2_CAMTABLE_FILL_LOOP ((uintptr_t)0x0070f430u)      // CameraTable_FillLoop
#define MC2_BODYBONES_ON_POSE ((uintptr_t)0x006916b0u)       // BodyBoneTracker_OnPose (slot 5; ECX = ctrl)
#define MC2_BODYBONES_SLOT2 ((uintptr_t)0x00691310u)         // BodyBoneTracker slot 2 (per-frame update?)
// Player object fields (player = EAX at Player_FrameUpdate).
#define MC2_PLAYER_MODE_COUNT_OFF ((uintptr_t)0x1bcu)    // int
#define MC2_PLAYER_MODES_OFF ((uintptr_t)0x1c0u)         // mode*[8]
#define MC2_PLAYER_MODE_MASK_OFF ((uintptr_t)0x1e0u)     // u8 active bits
#define MC2_PLAYER_AIM_POINT_OFF ((uintptr_t)0x124u)     // vec3 (Player_UpdateAimRay out)
#define MC2_PLAYER_AIM_DIR_OFF ((uintptr_t)0x13cu)       // vec3, normalized
#define MC2_CAMTABLE_SLOT_CAMERA_OFF ((uintptr_t)0x1e0u) // camera object* in a g_CameraTable slot

// ---- first person (docs/first_person.md) ----------------------------------------
// CamHuman_Update: the camera's final world matrix (a 4x4 stack local at
// ESP+0xa0, rows = axes, translation at +0x30) is complete here — after the
// aim-mode sway (FUN_0071e710) — and the next instruction reads its
// translation for the working pose (`mov ecx,[esp+0xd8]`, 7 bytes; also a
// branch target, which is fine: no branch lands inside it). EBX = the camera.
#define MC2_CAMHUMAN_FINAL_MATRIX ((uintptr_t)0x0071e611u)
#define MC2_CAMHUMAN_FINAL_MATRIX_ESP_OFF ((uintptr_t)0xa0u)

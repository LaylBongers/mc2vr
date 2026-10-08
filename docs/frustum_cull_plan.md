# Frustum Culling Alignment Plan (HMD-following, FoV-extended)

Goal: make the engine's frustum culling (and its LOD selection) follow HMD head rotation and cover the
HMD's field of view, instead of the fixed, narrow game-camera frustum — eliminating the pop-in that
appears when turning the head in VR.

Status: **RE COMPLETE (2026-10-06)** — every producer/consumer of the culling inputs has been identified
live (hardware-watchpoint evidence; `docs/reverse_engineering/view_and_camera.md` § Camera-data accessors,
per-address facts in the Ghidra plates). **Design drafted below — not yet implemented.**

**⚠ CAUTION (E2, 2026-10-06 — the D2 injection premise is shaken):** the E2 probe proved that writing
ViewEntry `pos7c4` + serial bump (the exact D2 protocol, into all 16 camera-adjacent views) is REVERTED
by the round-trip copy-back within one frame (refreshes≈injects), and never reached the draw camera
(regression SLOPE 0.00; full result in `stereo_improvements_plan.md` E2 RESULT). The same revert applies
before `ViewEntry_DeriveCullTask` can consume injected values — so D2's "write the culling inputs,
bump the serials" may be ineffective end-to-end. The design must either (a) be live-verified with a
cull-specific probe before implementation, or (b) be re-grounded on the CAMERA OBJECT (the E1b chain:
the draw camera reads ctx+0x28 in `ViewContext_BuildCameraConstants 0x008591ac`; MatrixFromGlobalCam
reads the same object — one injection point for culling + draw camera, E2b).

**✅ RESOLVED (E2b complete, 2026-10-06 — supersedes D1/D2):** the single injection point is
`g_CameraTable` (`0x014A2EE0`) — a STATIC, runtime-LIVE global camera table, plaintext-filled
~1.7/frame by `CameraTable_FillFromPose` (`0x0070ae50`: camera entity quat+pos via D3DX → rotation +
position), and read by BOTH the draw-camera builder path AND the culling/fov consumers
(`0x0048067E` family). **Design: MidHook at 0x0070AEF8** — right after the fill's
Matrix_Copy3x4 call — **rewriting the just-filled entry with the HMD-UNION pose** (mid-point
between eyes — exactly what the widened culling frustum wants; optionally also widen the fov
fields). Per-eye is not this point's job (once-per-frame union; per-eye stays at the record
level). Full chain: `render_path.md` § Draw-camera constant chain; D1's view-matching and D3's
derive-ordering question become moot for the injection itself (the fill hook is inherently
ordered); D4's "inject only into matched views" maps to "rewrite only the entry/entries just
filled by the hooked call". See the IMPLEMENTED note below for the composition.

**✅ IMPLEMENTED + LIVE-VERIFIED (2026-10-07, `stereo_improvements_plan.md` rounds 1-8):**
`view_table_inject=on` (`src/carrier/view_table.cpp`) — MidHook at 0x0070AEF8, rewrites the just-
filled entry with the game pose composed with the HMD-union pose. Composition is PROBE-DERIVED
(the entry's ROWS are the rendered camera's axes, and a write of E·M renders M⁻¹ world-side —
the builder's 4x4 inverse sits in between; closed form E' = S_r·L⁻¹·S_r·E, see the plate on
CameraTable_FillFromPose). Live result: culling/LOD follow the head (rotation confirmed
correct through aim changes, round 8). STILL OPEN here: the FOV-widening half of this plan —
culling still uses the game's widescreen table fov, so watch for late pop-in at the HMD FOV
edges. FOV-field static decode DONE 2026-10-08 (see "Fov field decode" below) — remaining:
one live confirmation run, then implement the widening.

## Fov field decode (static, 2026-10-08)

Decoded the write path for the entry fov fields; this supersedes the "fovCos/fovSin semantics
unknown" open item's static half:

- The live fov write is `ViewEntry_MatrixFromGlobalCam 0x0048a8f0`'s **fallback (kindA4==0)
path** at `0x0048a955` (matches the S5 watch site exactly: `[edi+4]=XMM3, [edi]=XMM1,
[edi+8]=XMM2`, `edi = entry+0x2ec`). Live views take kindA4==0 — the camera-object path
(+0x128 != 0, degree-angle math) is NOT the live one.
- The triple written at `entry+0x2ec/+0x2f0/+0x2f4` is **{fovH, 0, fovV} extents, NOT
{cos, sin, ...}**: `fovH = gcamH * gcamScale * 100`, `fovV = gcamV * gcamScale * 100`,
third = `gcamScale * 100 * [0x00b9b690]` where `0x00b9b690 = 0.0` (`0x00b9b688` turned out to
be a plain constant pool: 0.3/1/2/3/6/15/20/0.1/0.01/0.0001/pi — the "+0x58 fovCos chain"
note was a red herring). So `+0x2f4` is ALWAYS 0; the old `fovSin` name is wrong.
- Source chain: `owner = *(u32*)0x00e79dfc`; `gcam = *(u32*)(owner+0x104)`; `gcamH =
*(float*)(gcam+0x17c)`, `gcamV = *(float*)(gcam+0x180)`, `gcamScale = *(float*)(gcam+0x184)`.
The camera-table entry itself carries NO fov — the E2b idea "widen the fov fields in the
table entry" is dead; the fov must be widened either at the gcam sources (wide blast
radius — same object feeds other things) or by scaling the derived entry triple after the
write (preferred; the cull task scales its bounds vectors by these values linearly).
- Observed `+0x2ec = 0.957826 ≈ tan(43.75°)` — consistent with tan(half-angle) of an ~87.5°
horizontal fov. Even if the exact unit is an extent (tan * something), the write is a pure
product, so widening by a factor `tan(hmdHalf + margin)/tan(gameHalf)` is LINEAR and the
ratio form works either way. The game's tan-extents are already known per-frame from the
VP decomposer (`vp_camera.cpp`), so the ratio needs no new decode of the game side.

Probe prep implemented (this commit): `debug_watch=fov+fovmid+fovsin` now covers all three
floats (`fovmid` = +0x2f0 added), and the camtable window report logs `camtable: fovsrc:`
(gcam h/v/scale + the triple they produce + atan half-angle in degrees) once per 10 s window.

NEXT STEP (needs the human): live run with `view_table_inject=on`, `debug_watch=fov+fovmid+fovsin`
(in the launch conf), in gameplay: (1) stand still ~30 s (baseline), (2) ADS/zoom a few times,
(3) change the game FOV/widescreen option if the menu has one. Audit `fovsrc:` + watch values:
if h/v track zoom (tan grows when zooming narrows fov? or the inverse — extents shrink) the
tan-linearity is confirmed, and the widening design below can be implemented.

**Live run RESULT (2026-10-08, hardware + HMD-off passes):** semantics nailed — richer than the
static guess:

- Gameplay gcam: `h=0.957826 v=0.287348 scale=0.003`, giving triple `{0.287348, 0, 0.0862044}`.
- `v = h·0.3` EXACTLY, and `h² + v² = 1` EXACTLY → `{h,v}` is the game's UNIT frustum-corner
  direction; the tan shape is 3:1 (tanV/tanH = 0.3), and the written triple = unit corner ×
  `k = scale·100` (k = 0.3 = near-plane distance; the following sqrt at 0x0048a9a1 re-derives
  exactly k, confirming the model end-to-end).
- Rock-steady across all 7 windows including ADS passes — the game's CULLING fov never changes
  with zoom (ADS only touches the draw camera). So the pop-in at HMD-FOV edges is purely the
  3:1 game shape vs the HMD's ~per-eye fov.
- `FUN_00401740` (called right after the write) is just `sqrt` (396 callers) — it computes the
  triple's length (k), not a normalize. `Vector_Scale` (0x00401750, the cull-task consumer)
  uses the triple as the SOURCE VECTOR scaled out to bound distances.
- Watch lesson: the auto-picked "most-walked" view (idx0) had a dead (all-zero) fov triple —
  zero traps, zero value. The S5 write evidence came from idx13. Watch view choice needs the
  write-site filter, not walk counts (future work if needed).

**DESIGN CONSEQUENCE:** the widening is `triple[0] *= tan(hmdHalfH+margin)/h`,
`triple[2] *= tan(hmdHalfV+margin)/v` — i.e. replace the game corner with the HMD corner in
the game's own parametrization. Correct whether the consumer reads the values as tans or as a
scaled direction (both interpretations converge: the write becomes
`(tanHmdHalfH, 0, tanHmdHalfV)·k`).

**✅ D0 IMPLEMENTED (2026-10-08) + FIRST LIVE RUN — improvement, not complete:**** `cull_fov_widen=on` +
`cull_fov_margin=<deg>` (default 5) in `view_table.cpp`: MidHook at 0x0048a97a (after the triple
write, before the length re-read). Live run: widens fired ~1000/s (the fallback fov write is PER-FRAME,
not change-gated), lastWh=1.962 lastWv=5.949 — the cull corner became the HMD union (57°/55° half +
5° margin). Head-turn pop-in improved but persists.

**Post-run static decode (2026-10-08) — the mechanism is provably complete for kind-0 slots:** the
write tail (raw-decoded 0x0048a9a1-0x0048a9c4) does `sqrt -> FSTP [EBX+0xA8]` (slot param +0xa8 =
|corner|·k) then `Vector_Normalize` (0x00401630, renamed) overwrites the triple with the UNIT
corner. DeriveCullTask case-0 tail (0x0087712c → Vector_Scale @ 0x0087718b, the S5-observed read):
`bound = fovTriple × (+0xa8) × [scalar]` — and since triple=unit(w) and +0xa8=|w|·k, the product
algebraically reconstructs the pre-normalize corner × k × scalar. Our widened corner therefore
reaches the cull bound vectors EXACTLY. (Ghidra: plates on 0x0048a8f0 / 0x00876a90; renames
Float_Sqrt 0x00401740, Vector_Normalize 0x00401630, LOD_DistanceSelect 0x00490b80, g_ConstPool
0x00b9b688, g_GlobalCamCtx 0x00e79dfc.)

**Where the residual pop-in can hide (decision tree for the next run):**
- **H1 (cheap test): margin too small — FALSIFIED (live 2026-10-08, margin=30):** lastWh=19.8 lastWv=37.3
  (near-hemisphere cull corner) and pop-in persisted → the kind-0 cull box is NOT the operative
  per-object test (or not the only one). The box widening stays (it demonstrably improved things —
  probably LOD/coarse layers) but margin returned to 5 for production.
- **H2: the operative cull reads the CPU-side record VP — CONFIRMED as the remaining suspect and
  ADDRESSED (I3 below).** The record VP's VIEW part follows the head (built from g_CameraTable, our
  union injection) while its PROJECTION is always the game's screen-aspect fov (plate on
  0x008591ac) — a VP-based cull test would show EXACTLY the observed symptom (rotation follows,
  fov doesn't).
- **H3: other slot kinds (2/3/4) contribute main bounds** — unlikely (box union is monotone), untested.

**✅ I3 round 2 (2026-10-08) — the wrong-fill-site diagnosis:** with the two first-round bugs fixed
(byte idx + staleness window), the epilogue hook fired but saw ONLY identity/aux VPs
(`MISMATCH ... rec F=(0,0,-1) C=0` vs the real gameplay camera; `decomp` skips = the same class) —
`BuildCameraConstants`' inline fill is NOT the main-record path. Raw-decoded the REAL record fill
block (the mutated 0x004671xx region, task-dispatched, no static callers — E1's live watch
implicated it): TWO sites share the idx byte at `0x00ED9D42`:
- **Site A** `0x00467150-0x004672c0`: `Matrix_Copy3x4` call `0x0046718c` copies only 12 dwords
  (VP rows 0-2) + record+0x40..0x64 PS/misc fields — row 3 (`+0x30..0x3c`) is NEVER written on
  this path → these records fail the 4-row decompose (aux records).
- **Site B** `0x0046738c-0x00467429`: the FULL inline `fld/fstp` copy of the complete 16-dword VP
  with EAX = record — the only site that writes row 3. The gate's 4-row decompose (hmd_delta)
succeeds on the main records every frame ⇒ the MAIN records fill HERE (matches E1's observed
row0 write at `0x004673bf`).
FIX SHIPPED: the primary hook moved to site B's completion `MC2_VIEWCTX_FILLB_COMPLETE
0x0046742a` (after the last `FSTP [EAX+0x3C]`; EAX = the record directly — no global reindex at
all), with the BuildCameraConstants epilogue kept as secondary coverage. Shared handler
(`recfov_apply`), site-tagged diagnostics. Ghidra: `RecordFillB_VP_Copy`/`RecordFillA_Copy3x4_Call`
labels + plate on the block.

**I3 round 4 RESULT (2026-10-08):** site-C hook (0x0085982F, the inline-fill VP completion) fires on
ALL ~300 fills/s (same call set as the epi hook) but STILL never sees a main record — only the
identity/aux ones (idx 10/13) — despite the DR hits pointing at that same linear code, and the
code between the row-3 store and 0x0085982F has NO branches. Wine's multi-DR attribution is
unreliable (the tool warns exactly this; the 3 hit EIPs had suspiciously EQUAL counts) — the
watch evidence does NOT prove where main records fill. VERDICT: stop chasing the record fill.

**I3 round 5 — the camera-entry fov channel (ADS clue):** ADS narrows culling while the ViewEntry
fov sources stay constant → the cull input tracks the PROJECTION. The projection + the
0x0048067E cull readers both trace to the camera entry's fov `+0x58` — decoded the entry template
fill (`CamPose_CopyGlobal_To_Entry 0x004665b0`): near `+0x50=0.1`, far `+0x54=100.0`, fov
`+0x58 = [0x00BEAB5C] = 300.0` (ENGINE UNITS, not a cos — "fovCos" name is wrong; it indexes the
tan table via `value*0.75*aspect*8192/DAT_00d2ea5c`), filled 6.5×/frame per entry. IMPLEMENTED:
`entry_fov_scale` conf (scales +0x58 at every fill, hook at 0x0046662F — NOTE round-5 staging hooked
0x0046662e, ONE BYTE into the previous MOVSS, and crashed the game at boot; fixed to the real
MOVSS [ESI+0x64] boundary, `MC2_CAMENTRY_FILL_EPILOG`)
+ per-entry fov census logging ("entry ... first seen" / "FOV CHANGE" lines) + `view: gameproj:`
window line logging the rendered projection's a/b/half-angles (from the gate's decomposed game
camera). RUN: entry_fov_scale=1.2 — checks: (1) gameproj a/b change on ADS? (2) scale → rendered
halfH grows ~20%? (3) culling/water follows the scale? (4) any entry FOV CHANGE during ADS?
**I3 round 5 RESULT (2026-10-08):** the entry census caught 8 STATIC g_CameraTable entries
(014a2ef0-family, near=0.1/far=100/fov=300→360 SCALED ✓ hook worked) — but `view: gameproj:`
(NEW: the rendered projection from the gate's decomposed camera) showed a=1.3440 b=2.3894 →
halfH=36.65° halfV=22.71° (aspect-locked 16:9 tan ratio 0.5625) CONSTANT — the ×1.2 entry scale
DID NOT move the projection (and notably NEITHER did ADS — the game's projection never changes on
ADS; the ADS cull-narrowing must live in the 0x0070axx cull-derivation chain, not the projection).
The projection's source is a camera entry our hook didn't scale (likely the stack-local gameplay
entry filled by FUN_004660a6).

**I3 round 6 RESULT (2026-10-08, the flip decoded):** the constant patch WORKED causally — the game
rendered UPSIDE DOWN at a wild FoV (unplayable). Decode: the tan table `0x00CF1900` step = π/4000
(entry[1] = 7.854e-4 exactly), `DAT_00d2ea5c` = π, so index = angle×4000/π — LINEAR in the fov value.
Working back from the baseline (V=300 → halfH 36.65° → horizontal index ≈ 814): the tan flips sign
at index 2000 = 90° → **V_max = 300 × 90/36.65 ≈ 736** — our 780 crossed the pole → negative
horizontal scale → the flip. CONSTRAINT: the projection is 16:9-locked, so halfV 60° would need
halfH 96.7° — PAST THE POLE, impossible via this constant. Max sane: V≈727 → halfV ≈ 55° (raw HMD
union, ZERO margin, 1% from a singularity — too risky as the resting state). NOTE: the cull fov
chain reads the SAME constant (the 0x0070axx derivations) and its shape is NOT 16:9 (the cull
corner v/h = 0.3 — possibly literally value/1000 — vs the projection's 0.5625): the cull chain
has its own geometry. NEXT (round 7, staged): SAFE CALIBRATION V=400 (×1.333, halfV≈30° halfH≈49°)
— checks: (1) gameproj matches the linear model (2nd data point), (2) does the OPERATIVE cull
(water!) follow the constant at all? If yes, push toward the max-safe value (~650-700) for
near-HMD coverage; the ADS-zoom composition gets assessed after.

**I3 round 8 v5 RESULT (2026-10-08) — the constant path is a DEAD END for decoupling:** 5/5 sites
patched, yet `gameproj` still built at exactly 84.33° — because `FUN_0070aff6` (decompiled at last)
is a **THIRD entry FILLER**: it templates THREE STACK camera entries (ESI+0x00/+0x70/+0xE0) with
near/far/fov from the constant (three loads at 0x0070b039/bd/144, constant-folded by the decompiler
into the 300 immediates). CONCLUSION: **all 8 constant readers are FILLERS — no separate cull-derivation
readers exist.** The cull and the projection read the SAME entry fields; decoupling via the constant
is impossible. The only remaining split: WHICH entries — cull vs projection. E2b says the cull
readers (0x0048067E family) read the STATIC g_CameraTable; the projection reads the STACK entries
(FUN_0070aff6-filled, proven v5).

**I3 round 10 RESULT (2026-10-08, the watch decode that corrects the model):** DR watchpoints on
both slots, full mode, stock game. HARD FACTS: (1) `[0x00BEAB5C]` = **0.95975 during gameplay** —
the runtime fov in COS form (file 1.0 → boot writes 300 → boot-end writes 0.95975; no writes during
gameplay); the "300 engine-unit fov" was wrong all along — (2) `[0x00BAD260]` = 300 is a SEPARATE
scale constant (feeds entry +0x44 and heavy math: `MULSS` @ 0x0070B64F x365/s, `SUBSS` in the
projection-side 0x00859C85 x290/s); (3) gameplay fills target STACK entries (0x072E9xxx in the
hit registers) — the static g_CameraTable entries are BOOT-TIME snapshots only, so the
"statics-wide" split was never real; (4) every 84.33° run is explained: the decouple wrote **300
into a cos-form slot** (expects ~0.96) → garbage-wide projection. ALSO: readers of [BEAB5C] beyond
the fillers: `0x0071BBCE` (MULSS -> [EAX+0x5E8], ~2.4/frame — a camera/view computation). The
boot writer itself wasn't captured (watch armed after boot — a boot-phase capture would need
install-time arming; not needed so far). IMPLICATIONS: cull and projection share the stack
entries' +0x58 — but the ADS observation (cull narrows while gameproj doesn't) proves a SECOND,
zoom-coupled cull channel exists (note gcam h=0.957826 ≠ 0.95975 — a different cos!). Full
decoupling = consumer-side (or the second-channel hunt). PRAGMATIC PATH (round 11, staged):
scale 1.5, decouple off — the middle ground between playable 1.333 (round 7, water moved) and
camera-breaking 2.3. If acceptable, it ships as the interim while the consumer-side work waits. 7/7 sites patched,
statics census = **300** (⇒ [0x00BEAB5C] was 300 at CamPose-fill time — NOT our wide 2.3×300!), yet
gameproj STILL exactly the 690-derived 84.33° — with all stack fillers reading [0x00BAD260]. The
value choreography between 0x00BEAB5C, 0x00BAD260 (which I assumed was a static stock-300 slot but
is evidently part of a boot-RESOLVED chain — plausibly the internalizer lives among the very
functions we patched) can no longer be resolved by reasoning. ROUND 10 (staged): STOCK game
(scale 1.0, decouple off) + DR watchpoints on BOTH addresses, full mode — the hit table names the
boot writer and every reader with cadence; single pair = still reliable attribution. THEN: map
the real chain (multiplier slot vs resolved slot vs consumers) and design the split from data,
not inference. constant patched wide (as before) but now every
NON-CamPose filler load (7 sites: FUN_004660a6 ×2, FUN_0070aa60, FUN_0070a910, FUN_0070aff6 ×3 —
imm32s 0x00466105/0x0046618C/0x0070AA8A/0x0070A94F/0x0070B03D/0x0070B0C1/0x0070B148, all raw-verified)
is repointed at stock 300, while CamPose (imm32 0x004665FF) keeps reading the wide constant → the
STATIC g_CameraTable entries go WIDE (cull-reader candidates), all stack/aux entries (projection,
camera) stay STOCK. CHECKS: (1) `DECOUPLE v2 ... PATCHED (7/7)`; (2) census shows the statics at
fov≈690; (3) gameproj = STOCK 36.65°/22.71°, camera normal, monitor normal; (4) THE TEST: is
culling/water WIDE? wide ⇒ cull reads the statics ⇒ ARCHITECTURE COMPLETE (statics = cull channel,
conf-controlled width); stock ⇒ cull reads the stack entries like the projection ⇒ consumer-side
read interception is the only path left. NOTE: menus may render wide in this mode (E2b: the menu
camera uses the statics directly) — expected, non-blocking for the test. still wide — but the per-site value guard caught the cause:
`site 0070a949 = 0FF34424 — SKIPPED` — the v4 imm32 address for `FUN_0070a910`'s load was SIX
BYTES OFF (miscounted past the preceding `MOVSS [ESP+0x44],XMM0`; real imm32 @ **0x0070A94F**,
load at 0x0070a94b per the original xref). One wide reader left → the same 690-derived projection
(`gameproj` byte-identical to v3). v5 staged: corrected site; 5/5 expected. all patches applied (constant 1.0→2.3, decouple 3 sites,
census statics at stock 300) — but `gameproj` built at **84.33° halfH = exactly the 690 wide value**
(tan-16:9-consistent vertical: the 84.33/79.99 pair matches tanH×0.5625). So the projection's
ACTIVE (stack) entry got the wide value through a **4th/5th UNPATCHED constant reader** — the
0x0070axx camera-code pair `FUN_0070aa60` (load at 0x0070aa86, imm32 @ 0x0070aa8A) and
`FUN_0070a910` (load at 0x0070a945, imm32 @ 0x0070a949), same `MOVSS XMM0,[0x00BEAB5C]` shape.
FIX v4: decouple extended to all 5 filler/camera loads (→ 0x00BAD260 = 300.0 stock); ONLY
`FUN_0070aff6`'s three reads (0x0070b039/0x0070b0bd/0x0070b144) keep the wide constant — the
cull-derivation candidates. Also fixed a latent bug: patch_imm32s now VirtualProtects per site
(v3's version only protected the first site's region — only correct while all sites shared a page).
Round 8 v4 = THE CLEAN TEST: if the projection/camera are stock AND water/culling stays wide →
FUN_0070aff6 feeds the cull → SHIP. If culling is stock too → the cull reads the entries and the
next move is consumer-side clamps. the bool fix made the patches actually apply — but the
decouple pointed the fillers at a raw **1.0** (the constant's *init-time* value) while the fillers
write the loaded value RAW into entry+0x58 — the stock post-boot value is **300**, so the entries
got 1.0 → the game projection at ~0.08° half-angle → extreme telephoto = "camera incredibly
close" (NOT the boom — the projection). FIXED: the decouple now repoints the filler loads at
`.rdata 300.0f` at **0x00BAD260** (the only aligned 300.0f in the image — also loaded by the fill
for +0x44, so it IS the game's stock-fov constant). Round 8 v3 staged, same conf. a bool-typed `g_entry_fov_scale` decl (from the messy
edit sequence) collapsed the conf's 2.3 to `true`(=1), so `install()` skipped BOTH the constant
patch and the decouple entirely (the log showed `(entry fov census, observe)` + no PATCHED lines —
the tell). "Culling back to before" was just the stock game. THE DECOUPLE HYPOTHESIS IS STILL
UNTESTED. Fixed the type; round 8 re-run staged with the same conf. scale 2.3 broke the game's own systems (camera
boom pulls in hard + offscreen-pass artifacts at 168°-wide projection — the constant is a BLUNT
instrument: cull + projection + camera logic all read it). But the reader split is already proven
by rounds 5 vs 7: the WATER cull follows the CONSTANT readers (0x0070aa60/0x0070a910/0x0070aff6 —
round 7: constant scaled → water moved; round 5: only the entries scaled → nothing moved), while the
projection/boom follow the ENTRY FILLERS. DECOUPLE IMPLEMENTED: `entry_fov_decouple=on` repoints the
THREE filler loads of the constant (`MOVSS XMM0,[0x00BEAB5C]` imm32 operands at 0x00466105 /
0x0046618C / 0x004665FF — FUN_004660a6 ×2 + CamPose_CopyGlobal_To_Entry; all one .text page) at
g_ConstPool's 1.0 (0x00B9B694) — the fillers write multiplier 1.0 (stock projection + camera)
while the cull derivations keep reading the wide patched constant. This imm32-repoint technique is
also the general tool if any single reader needs its own value later. CHECKS: (1) "fov DECOUPLE
... PATCHED (3 sites)"; (2) camera/projection back to NORMAL; (3) water still large / no pop-in
→ SHIP CANDIDATE; (4) if water shrank back, the cull reads the entries too → redesign.

**I3 round 7 RESULT (2026-10-08) — CHANNEL CONFIRMED:** V-scale 1.333 → gameproj halfH=48.87°
(= 36.65 × 1.333 EXACTLY — linear model verified, upright, playable) and **WATER GREW
SIGNIFICANTLY — the operative cull follows the fov constant**. (Detail: at carrier-init
`0x00BEAB5C` reads **1.0** — it is a BOOT-TIME FOV MULTIPLIER the game internalizes as
300×mult before gameplay; the patch semantics are unchanged.) Ceiling math: pole at
halfH=90° → scale_max = 90/36.65 = 2.455; the 16:9-locked vertical at the pole = 55.8° —
just covers the HMD union (55°) with ZERO margin. STAGED (round 8): scale 2.3 → predicted
halfH 84.3° halfV 52.2° — covers HMD horizontal 57°+margin fully; vertical 3° short at
the extreme top/bottom sliver (acceptable: head rotation keeps the frustum centered;
rare marginal pop). CHECKS: (1) gameproj ≈84°/52°; (2) water hits the view edges? (3) any
pop-in left (esp. looking up/down at tall objects)? (4) ADS composition; (5) shadow
quality (offscreen passes now render 84°-wide — texel density drop?).

**Round 6 background (how the constant was found):** xrefs of the fov constant `[0x00BEAB5C] = 300.0` found
EIGHT readers = the whole fov chain: `CamPose_CopyGlobal_To_Entry` + `FUN_004660a6` (BOTH camera-
entry fillers — the latter likely the stack-local one) + `FUN_0070aa60/FUN_0070a910/FUN_0070aff6`
(the 0x0070axx cull-fov derivations — the gcam sources!). IMPLEMENTED: `entry_fov_scale` now PATCHES
THE CONSTANT once at init (fill hook is census-only) — every consumer widens in the game's own
parametrization, including the cull derivations AND the ADS-zoom path. GOTCHA (round-6 staging
crash): the constant is in READ-ONLY `.rdata` — the patch needs `VirtualProtect` first (fixed).
Baseline mapping:
300 → halfV 22.71° (≈ angle-linear via the tan-table index) → scale 2.6 targets halfV ≈ 59°,
halfH ≈ 95° (16:9-shaped — over-covers the HMD horizontally; conservative for culling). CHECKS
for the staged run (entry_fov_scale=2.6): (1) `camtable: fov CONSTANT PATCHED 300.0 -> 780.0`;
(2) `gameproj` halfH/halfV grow ~2.6× — calibrates the value↔angle mapping; (3) does culling/water
follow? (4) does ADS still narrow it (proportionally is fine)? (5) shadow quality — the shadow
projection may widen too (texel density drop = blockier shadows — report if visible).

**I3 round 3 RESULT (SUPERSEDED by round 4 — the DR attribution below is not trustworthy):** DR
watchpoints on the main records'
row0/row3 dwords produced ~1100 hits/window at exactly TWO writer EIPs, both INSIDE
`ViewContext_BuildCameraConstants`' inline fill: `0x00859774` (`FSTP [EAX]`, row0) and `0x00859807`
(right after `FSTP [EAX+0x30]`, row3), with `EAX = 018c4730/018c5530` (the MAIN records). So the main
records DO fill in the function's inline copy — but that path EXITS WITHOUT REACHING the
0x00859912 epilogue (the epi hook only ever saw identity/aux calls — a second exit in the mutated
code). Decoded the completion: the copy continues `FSTP [EAX+0x34/38/3C]`, last store ends at
**`0x0085982F` (site C, `MC2_VIEWCTX_FILLC_COMPLETE`)** with EAX = record, VP complete. Site B
(the 0x004671xx block) fires ~100/s but its records are all-zero at hook time (aux/scratch path) —
kept as secondary. NEW PRIMARY HOOK: site C. Also: the epi-hook "identity records" (idx 10/13) are
real main-RT but minor passes (sky/overlay) with identity cameras — the gate's dominant main records
are idx-2/3.

**ADS clue (user report, 2026-10-08):** ADS narrows the CULLING noticeably with HMD on. The game's
zoom changes the projection (the record VP's a/b — ADS demonstrably affects it) while the ViewEntry
fov-triple sources (gcam h/v) stay CONSTANT under ADS (probe run). So the operative cull input
TRACKS THE GAME PROJECTION — strong evidence for a VP-consuming cull test (the record channel).
Decisive check for the site-C run: with the record widen actually firing, ADS should STOP narrowing
the cull extent (the record always carries the HMD fov). If ADS still narrows it, the cull input is
something else that ADS changes (next suspect: the camera-entry fov chain).

**Water clue (user report, 2026-10-08):** water follows the culled frustum almost exactly, and shadows
cut at the same pop-in edge — i.e. water/shadow VOLUME culling shares the operative fov channel. If
`record_fov_widen` firing (post-fix) moves water's extent to the HMD fov, the operative cull is the
record VP and this closes the case. If water STILL follows the game fov with recfov confirmed firing,
the next lever is the SOURCE of the projection fov: the camera entry's `fovCos +0x58` (0x70-stride
camera object, ctx+0x28) — filled from a STATIC CONSTANT in `g_ConstPool` at write `0x00466615`
(stereo_improvements_plan.md E2b) — one point feeding the projection AND the `0x0048067E` cull-reader
family. Semantics of that fill (which constant, what angle it encodes) = the next RE item if needed.

## What the RE proved (evidence summary)

The culling data flow, all sites watch-proven on live gameplay:

```
VM'd packet consumer (ORIGIN of camera-pose VALUES; post-walk deref of the staged block
  ctx+0xc2110+idx*0x30, serial-gated round-trip)
        │  copy-back at 0x0048F72D (walk tail; Pose_Copy semantics, serial+1)
        ▼
ViewEntry {pos7c4, quat7d4, serial7d0}   [plaintext, change-gated ~1/10s]
        │  Pose_Copy relay 0x00824a10 (0x20 block, serial = max(src,dest)+1)
        ▼
camera object (ViewRef.camData chain)
        │  ViewEntry_MatrixFromGlobalCam 0x0048a8f0 (via ViewManager_Update -> PropagateMatrices):
        │    writes slot matrices (+0x20, stride 0xc0, via Matrix_Copy3x4 0x00836120)
        │    and fovCos/fovSin (+0x2ec/+0x2f4)
        ▼
ViewEntry_DeriveCullTask 0x00876a90  (task-queued, was undefined in Ghidra until the watch run)
        reads fov + slot matrices + camera-object params, per slot kind (kindA4 0..4):
        LOD distance selection, D3DX transforms, Vector_Scale (0x00401750) by fov,
        Box_Union helpers (0x0040b250/0x0040b4c0)  =>  CULLING BOUNDS + per-slot LOD states
```

Key facts the design must respect:

1. **No VM involvement in the culling math.** The culling volume is built by `ViewEntry_DeriveCullTask`,
   a plaintext function. The VM only originates *pose values* (and produces the draw camera's
   `viewContextData` VP rows — unchanged by this plan; the S4-4 GPU-boundary rewrite stays the draw-camera
   channel).
2. **Everything is change-gated (serial handshake), NOT per-frame.** Copies only happen when the source
   serial advances (`serial = max(src,dest)+1`, `Pose_Copy` semantics). Observed cadence ~once per 10 s
   for a static camera — the gate opens on change. Injection must write fields AND advance the serials,
   or nothing downstream will notice.
3. **View indices are per-run allocations** (live view was idx0/idx13/idx14/idx23 across runs). Never pin
   an index; identify at runtime (below). The companion liveness byte `g_ViewTable3 + idx*0x20 + 0x18`
   (t3) marks live camera data (00 = loading template, 01 = live).
4. **The slot matrices feed general per-object view-transform math** (`Matrix_MakeWorldToView`
   0x004017d0, 23 call sites) — not only culling. Overwriting them is a wide-reaching change; prefer
   the minimal-injection variant below.
5. ~24 live type-2 views are walked per frame; the main draw view must be identified among them.

## Design

### D0. FOV widening (post-decode design, supersedes the fov half of D2)

Widen the culling volume at the DERIVED entry triple, not at the gcam sources: a small MidHook
at `0x0048a975` (right after the three `movss [edi...]` stores, before the normalizing call at
`0x0048a9a1` — VERIFY the normalize doesn't re-derive them; if it does, hook after it instead)
scales `entry+0x2ec *= widenH` and `entry+0x2f4`-adjacent `entry+0x2f0 *= widenV`, where the
widen factors come from the per-frame VP decomposition: `widenH = tanHmdHalfH / tanGameHalfH`
(union over both eyes + `cull_fov_margin`), same for V. Alternatively (no new hook) the
existing camtable injection walk can post-fix the triple per window — but the write is
change-gated (~1/10 s), so a MidHook at the write site is the precise point; post-fixing at
frame cadence also works since the cull task reads later. Keep LOD distance selection on the
unwiden head position (D5 unchanged — the LOD metric reads pos/slot data, not the fov extents).

### D1. Identify the main draw view at runtime (no pinning)

The carrier already decomposes the draw camera from the GPU uploads (S4-4 `vp_camera` decomposer:
`F = row3.xyz`, basis R/U/F, position C). Each frame, for each live (t3=1) type-2 view, compare the
view's slot-0 basis (`slot mtx[0]` rows ≈ camera right/up/forward + pos at row 3) against the decomposed
draw-camera basis/pos (tolerance match). The view that matches is the culling view of the draw camera.
- Match inputs are all plaintext; comparison runs in the existing view-MidHook walk
  (`render_dump.cpp` view_midhook) or the FrameTick path.
- Log mismatches/ambiguity (several views share one camera — e.g. water views); if multiple views match,
  inject into all matched views (cheap, and culling must hold for each).
- The active-view list head is `g_ActiveViewListHead (0x00d29e60)`; entries at `g_ViewTable (0x012865e0)`,
  stride 0x810.

### D2. Injection point — write the culling inputs, bump the serials

Inject once per frame (in the existing slot-5 `PostUpdateHook` or the view MidHook, i.e. BEFORE the
producer walk's staging, so the staged round-trip carries our values):

- **Pose**: `pos7c4` += HMD head offset (same body-on-camera mapping as S4-4 `apply_hmd_eye`, scaled by
  `view_world_scale`); `quat7d4` = q_hmd ∘ q_game (HMD rotation applied on the game camera). Union, not
  per-eye: use the mid-point between the eyes (IPD contributes negligibly to culling bounds; include a
  +IPD/2 margin in the bound scale if paranoid).
- **FOV**: `fovCos2ec`/`fovSin2f4` ← half-angle widened to the HMD's per-eye FOV union: half-angle =
  max over both eyes of the OpenXR angle bounds (+ a safety margin). NOTE: the exact semantic of these
  two floats is not yet decoded (observed fovCos = 0.957826 on a live view — plausibly cos(half-angle) of
  a sub-view, but VERIFY: watch them while zooming/changing the game FOV option; alternatively read them
  alongside the slot `paramA8` params in `MatrixFromGlobalCam`).
- **Serials**: `serial7d0` += 1 (and pos7ac = old pos7c4 first, to keep the "previous position" semantics
  consistent). This opens the change-gate so `MatrixFromGlobalCam` re-derives slots+fov and
  `ViewEntry_DeriveCullTask` re-derives the culling bounds.
- **Slot matrices**: only if D3 shows the derive does not cover rotation (see open items). Writing slots
  directly affects general per-object transforms — avoid unless proven necessary; the pose path should
  propagate rotation through `MatrixFromGlobalCam` automatically.

Alternative considered and rejected: hooking inside `ViewEntry_DeriveCullTask` to rewrite its bounds
outputs. Wider risk surface (task runs for ALL views), and the inputs are cleanly writable anyway.

### D3. Ordering & forcing the derive

Open question: within the change-gated cycle, does `ViewEntry_DeriveCullTask` run after
`MatrixFromGlobalCam` unconditionally on a serial bump, or does the task have its own gate?
- Watch evidence: both fire ~1/window for a static camera (gate = change).
- Implementation probe: inject + bump serial once, watch the window reports (`debug_watch=fov`,
  `debug_watch=slot0`) — if the derive runs on our bump, ordering is fine; if not, also bump the entry
  fields the cull task gates on (its per-slot serials / the 0x62c-array serials at entry+0x62c,
  count = flags804>>4 & 0xF).
- Fallback if gating can't be satisfied cleanly: MidHook `ViewEntry_DeriveCullTask`'s entry (plaintext,
  task-queued — hookable) and re-run it for the matched view after injection (the task is idempotent
  derive math).

### D4. What stays untouched

- The draw camera itself (S4-4 GPU-boundary `viewContextData` rewrite) — unchanged.
- The staged round-trip and the VM consumer — we inject INTO the pipeline upstream; the VM relays our
  values like its own (values are indistinguishable: it never validates them, it just copies serials).
- Shadow atlas culling (light frustum), satellite/PDA/water views — inject only into matched views.

### D5. LOD consideration

`ViewEntry_DeriveCullTask` also does per-slot LOD distance selection using the same camera inputs, so
HMD-aligned injection automatically aligns LOD with the head (removing periphery LOD pop). The widened
FOV must NOT scale the LOD distances (or distant LODs would drop everywhere): inject pose + FOV for the
*volume*, keep the LOD distance metric on the true head position. Verify by reading the task's
distance-selection inputs during the verification run.

## Conf flags (as implemented)

- `cull_fov_widen=off|on` — fov-widening master switch (default off).
- `cull_fov_margin=<deg>` — extra degrees on the HMD-union half-angle (default 5).
- (The rotation half needs no new flag — it's `view_table_inject=on`; the proposed
  `cull_align`/`cull_view_debug` flags were not needed: view matching turned out moot with
  the fill-site/fov-site hooks, which are inherently per-live-entry.)

## Verification plan

1. `view/hmd` + `cull_align=on`, stand still, turn the head ±90° quickly: no geometry pop-in at the
   periphery (today: heavy pop-in). Watch the `watch:`-style logs (extend the view MidHook to log the
   matched view idx + injected values once per window).
2. FOV: with `cull_fov_margin` deliberately huge (e.g. 30°), culling should never pop even on fast
   spins; with 0 margin, slight periphery pop is acceptable/expected.
3. Regression: monitor stereo (shadows glued, IPD parallax) and LOD behavior at the screen center
   (unchanged).
4. If a culling artifact appears only for one eye: the union margin is too small (increase IPD margin).

## Open items (before implementation)

- [x] Decode the exact semantics of `fovCos2ec`/`fovSin2f4` — DONE statically + live-confirmed
      2026-10-08 (see "Fov field decode" + live RESULT): unit corner direction × near, written
      by the fallback path; `+0x2f4` always 0.
- [x] Live probe run — DONE 2026-10-08 (values stable under ADS; culling fov is constant).
- [x] Implement D0 widening — DONE 2026-10-08 (`cull_fov_widen`), first live run: improvement,
      residual pop-in — see post-run decode + H1/H2/H3 decision tree above.
- [ ] Live verification of I3 `record_fov_widen=on` (margin back at 5): periphery pop-in gone?
      `view: recfov:` should show widens≈2/frame (records 6+12), skips≈0, mismatches= the shadow/
      offscreen records. If pop-in STILL persists with both channels on, remaining suspects: the
      cull-box consumer path (map the Box_Union dest + per-object test), H3, or a VM-internal fov.
- [ ] Map the cull-box consumer: where the final Box_Union output of ViewEntry_DeriveCullTask is
      tested per-object (even if I3 succeeds, this completes the culling map).
- [ ] Confirm the widened FOV reaches the box-union vectors via `Vector_Scale` alone
      (i.e. the bounds scale with fov as intended) — read the task's outputs after injection.
- [ ] The camera-object params (+0x128 kind, +0xa8/+0xac/+0xb0 slot params) — check whether any of
      them encode frustum extents that also need widening (kind 3/4 paths in the cull task).
- [ ] Unit scale `view_world_scale` (shared with S4-4, still unverified).

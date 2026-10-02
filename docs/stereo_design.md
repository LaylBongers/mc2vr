# Stereo Submission Design

Design for M4+: dual-eye world rendering + HMD presentation. Consumes the M3 outputs
(`ViewEntry` struct + plate comment in Ghidra, hook mechanism proofs, command histogram).
Per-address facts live in Ghidra; this doc carries design decisions, phases, and open work.
Mechanism rules: `docs/launcher_plan.md`. Runtime-mapped frame chain: `docs/render_path.md`.

## Facts this design builds on

- Frame chain (producer side all plaintext, main thread only): `GameShell_FrameTick` →
  frame pipeline → `RenderQueue_SubmitWorldPackets` (`0x0048e620`, producer: walks active
  `ViewEntry`s at `g_ViewTable` `0x012865e0`, publishes packet elements into the
  `g_RenderQueue` ring) → [SecuROM-VM'd packet interpreter, S0 — see below] →
  `RenderShell_RenderFrame` (`0x00855690`) → `BeginSubmit` (Present prev + BeginScene) →
  `RenderCmd_ExecuteStream` (`0x008569d0`, 27-opcode interpreter, ~1.5–3.4k cmds/frame) →
  `EndSubmit` (EndScene).
- `ViewEntry` (Ghidra `/RenderPath`): nine D3D-style 4×4 matrices at `+0x020` (m[0]
  view-to-world with camera pos, m[1] world-to-view with negated pos, others near-identical
  camera variants); FOV half-angle sin/cos at `+0x2ec/+0x2f4`; camera position copies at
  `+0x7ac/+0x7c4`; near-plane-ish params near `+0x188`; `ViewRef` pointers at
  `+0x7e4..+0x7ec`. Entries are template/zero at load; camera data populates when live.
- Mechanisms proven at runtime: trap-based inline/Mid/Vmt installs (no suspension), device
  VmtHook surviving device-lost + `Reset`, `g_RenderShell` slots 4/5 claimable (called 1:1
  with frames), Present-hook caller attribution. DXVK runtime (MinGW-built, Itanium vtables).
- Two primary sub-objects exist inside `g_RenderShell` (at `+0xFDC`, stride `0x3a0`,
  count `*(WORD*)(g_RenderShell+0x2b90)` = 2) — **S0: copied once per FRAME into the
  frame-ctx 0x680 block, NOT per view, NOT eye slots.**
- Queue: `g_RenderQueue` ring (elem 96, cap 4096, countersA `+0x10`, countersB `+0x14`;
  producers step `countersB.low += count` / `countersA.high += count`, the VM'd consumer
  advances `countersA.low` — explains M3's "ring position" observation). `g_RenderQueue2`
  carries 2D/overlay submissions.
- Special cameras (satellite designation) submit up to 608 views in one frame; normal
  gameplay tens per frame.
- **S0 (2026-10-02)**: the active views are an intrusive linked list (head =
  `DAT_00d29e60` = an index, not the count M3 assumed; link `ViewEntry+0x4`, negative
  terminates), and the ring/element consumer is SecuROM-VM'd (see S0 results below) —
  the `{size,ptr}` pairs are dereferenced AFTER the whole walk, which reshapes S2/S3.

## Architecture

**Producer-side eye duplication**: inside the `SubmitWorldPackets` per-view loop, submit
each type-2 world view's packets twice — once per eye — with eye-offset camera data. 2D/overlay
(`g_RenderQueue2`) renders once. Compositor at `Present` delivers the result to the HMD.

Chosen over the alternatives:

- Consumer-side replay (`ExecuteStream` opcode dispatch duplication): one interception point,
  but the stream is consumed linearly (replay needs buffering), draw state would apply twice,
  and per-eye render-target switching mid-replay is harder than doing it at the source.
  Keep as fallback if the producer loop body proves unmappable.
- Whole-frame duplication via the slot-4/5 hooks: duplicates game logic ticks and 2D UI;
  rejected.

Rationale for producer-side: the game's own pipeline already handles per-view visibility, LOD,
and state caching correctly — duplicating at the source inherits all of it, per-eye, for free.

## Phases

### S0 — Loop-body RE (prerequisite, static) — COMPLETE (2026-10-02)

All facts recorded in Ghidra (plate comment on `SubmitWorldPackets` + site comments +
`Analysis/render-path` bookmarks). Summary:

**Walk structure** (the M3 "array of active views" model was wrong): the views are an
intrusive linked list — head = `DAT_00d29e60` (an INDEX into `g_ViewTable`, also stored
to frame-ctx `+0xd2a10`; the M3 "dynamic registered-view count" label was wrong), link
= `ViewEntry+0x4` (`id_b`), negative index terminates. Loop head `0x0048e9d0`
(`mov eax,esi`), type check `0x0048e9e2` (`ViewRef` = entry `+0x7e4`, type =
`*(WORD*)(ref+0x14)`, accept 2/4, skip target `0x0048f00f`), iterator reload
`0x0048f013` (`esi = *(entry+4)`), back-edge `jge 0x0048e9d0` at `0x0048f01f`.

**1. Packet data residency — pointers, but deref is POST-WALK (falsifies the
"patch between eye passes" branch).** Each type-2/4 view emits exactly ONE 96-byte
element (= queue elemSize): header + three `{u32 byte-size, ptr-to-live-data}` pairs:
- `{0x30, this+0xc2110+idx*0x30}` — camera staging slot: inline copy of entry
  `pos7c4`(+0x7c4 xyz), `param7d0`(+0x7d0 serial), `f7d4`(+0x7d4 16B rot), LOD byte at
  slot `+0x20`. Copy site `0x0048ec3e–0x0048ecd5` (LOD path sources `FUN_00434fe0`).
- `{0x810, ViewEntry*}` — the whole live entry, BY POINTER (all nine matrices, FOV,
  near/far handed over untouched).
- `{0x680, this+0xd2950}` — the whole frame-ctx block (embeds the queue pointers, the
  active-head index at `+0xC0`, `g_ViewTable` at `+0xC4`, companion tables, the
  resolved primary-subobject pointers at `+0x74`, and the primary-subobject copies at
  `+0xEC`, 2 × 0x164).
The pair dword is a SIZE (0x810 = entry stride, 0x30 = slot size, 0x680 = block size),
so the elements are serialization descriptors — the consumer copies/derefs live
objects at consume time. **The consumer is SecuROM-VM'd**: no plaintext function reads
the ring, the frame-ctx `0xd29xx` fields, or the `this+0xcb110` camera-record ring, and
the draw-record tables `RenderFrame` walks (`0x58`-records at `_DAT_0116977c`, head
`DAT_01169780`, sub-tables `01169774/01169778/00ff36f4/00ff36f8/01163760`) are only
zeroed in plaintext (`FUN_00853ee0`). Prime suspect: VM entry stub `0x0050f660`,
called at `0x004c99f9` in the frame pipeline IMMEDIATELY after `SubmitWorldPackets`.
Since consumption is after the walk (and the consumer even receives the view list head
inside the 0x680 block), two eye elements sharing one `ViewEntry`/staging/ctx would
render identically → S3 needs per-eye SHADOW COPIES (see below).

**2. Viewport/render-target flow — not a ViewEntry field.** `ViewEntry` has no rect
fields (struct unchanged). Viewport/RT selection happens per draw-record at consume
time in `RenderFrame` via static per-type target objects (`FUN_00858870`/`FUN_008587a0`
pick from `DAT_0196xxxx` tables by record flags, apply via vtable `+0x14`); the record
tables are VM-filled. Per-eye rects are therefore compositor/RT work in S4, not a
patchable producer field.

**Back-edge / S3 sites**: back-edge `0x0048f01f`; best re-emit hook = `0x0048f013`
(ESI = current idx, about to be overwritten; redirecting EIP → `0x0048e9d0` re-runs the
same view). Element staging copy at `0x0048ef71` → staging array `[ESP+0x79a0+n*0x60]`,
count at `[ESP+0x19a00]`, **cap 0x300 = 768 elements** — full duplication of a 608-view
satellite frame (1216) would overflow the staging array; duplication must be limited
to classified world views (expected a handful per frame).

**Primary sub-objects** (`g_RenderShell+0xFDC`, count 2): participate per FRAME, not
per view — copied once pre-walk into the 0x680 frame-ctx block (`+0xEC`, 0x164 each,
resolved pointers `+0x74`). They are NOT eye slots (open question closed). The 40-byte
per-view camera records go through `FUN_004906b0` into a 0x28-stride ring at
`this+0xcb110` (reader is also VM-hidden).

### S1 — Consumer + field-use instrumentation (one run)

**IMPLEMENTED (2026-10-02, pending run)** — carrier code: `src/carrier/s1_probe.cpp`
(new MidHooks + all S1 state; M3 handlers and the device VmtHook feed it via
`s1::note_*` / `s1::on_*` taps), `device.cpp` (SetTransform 44 / SetViewport 47
slots), `render_dump.cpp` (view-loop flags tap, opcode/slot-4 bracket taps, S1
window report). `tools/analyze_dumps.py` decodes the S1 blocks (element layout
check, xform float decode, evidence echo). Implementation deltas vs. this
spec (all behavior-preserving):

- `0x004c99f9` IS the 5-byte `call 0x0050f660` itself (verified by decode);
  the MidHook there fires just before the interpreter and SafetyHook relocates
  the rel32 call into its trampoline. RenderFrame is bracketed by a MidHook at
  its first instruction (same evidence as an entry InlineHook, no convention
  hazard); two extra bracket points were added for free on existing hooks:
  first stream opcode of the frame (opcode MidHook) and EndOfFrameHook
  (slot 4) — countersA.low is now sampled at 4 points (pre-VM → RenderFrame
  entry → first cmd → end of frame), resolving the "consumes inside
  RenderFrame" branch of the decision tree in one run.
- The other post-submission calls decode to `0x006b93e0` (from `0x004c99fe`)
  and `0x006f9490` (from `0x004ca003`) — the earlier `0x006B99E0` in this doc
  was a transcription typo.
- Ring-element dumps are taken at the S0 formula positions AND the naive
  candidates (`countersA.low % cap`, `countersA.high % cap`) — if the doc
  formula mis-decodes, the analyzer identifies the true element positions
  against the {0x30, 0x810, 0x680} pair layout (element layout check is
  offline in `analyze_dumps.py`).
- S1.3 patches m[1][3] (translation x) with +4.0 for a 5-frame window
  (restore at each frame's pre-hook, re-apply relative to current values) —
  long enough to survive an odd frame, short enough to limit exposure of the
  patched field to game logic. Head view must be type-2 (checked per frame
  before starting); the proof is logged when any SetTransform matrix equals
  the live patched entry's m[1] while the patch is active.
- S1.4 logs the full `(idx, type, flags)` list (flags = high word of the
  ViewRef `+0x14` dword) only on: the first two frames ever, the first frame
  of each new list signature (FNV over head + recs, ≤6/window), and satellite
  frames (n > 100, ≤2/window) — steady state must not produce a line per
  frame. Window reports carry t2/frame min/max + satellite counts.
- Read-safety: the frame-ctx pointer (EBX at the loop-head site) is
  structurally validated before any scan (0x680 block must carry
  `&g_RenderQueue` at +0x60 and `g_ViewTable` at +0xC4); the ctx 0x680 block
  and the two live primary subobjects (ptrs at block +0x74) are also scanned
  for SetTransform matches ("ctx+off" / "subN+off" tags).

### S1 run results (2026-10-02, one gameplay run: cutscene → boat, incl. satellite designation) — answers + one instrumentation bug

1. **S1.1 ANSWERED (main branch)**: countersA advances between the pre-VM site
   and RenderFrame entry in 80–100% of frames, in every window → **the VM'd
   call at `0x004c99f9` consumes the ring at pipeline time**; clone-at-stage
   S3 is structurally valid. Additional advance RenderFrame-entry → first
   stream cmd (most frames): consumption also spills into RenderFrame's setup
   walk; the bulk of the per-frame advance actually lands between slot-4 and
   the next pipeline (counter semantics below) — the interpreter is likely
   invoked from more sites than the pipeline one.
2. **Counter decode REVISED (S0 was wrong)**: queue `+0x10` is a 32-bit
   cumulative monotonic counter (dword delta 213/frame at menu; poller saw it
   grow to ~40M during satellite), NOT packed u16 consumer/producer halves;
   `+0x14` reads 0 for whole windows (2 at boot). The doc's ring-position
   formulas are invalid; S1b logs raw `A`/`B` dwords at all 4 bracket points
   for the first 60 world-view frames ("S1 raw:" lines) to re-derive offline.
3. **Element format CONFIRMED at runtime**: captured elements are
   `{u32 size, ptr-to-live-data}` serialization descriptors — one captured
   non-world element = header `{17, 0x00ed870c}` + pairs `{0x1f0,ptr}×4` +
   `{0x10}` + `{0x50}` + `{0x80}` (0x1f0-stride blocks at 0x01669xxx). The
   world-view `{0x30, 0x810, 0x680}` element was NOT captured — the one-shot
   dumps fired at boot, before world views existed (S1b re-arms them on
   world-view frames and dumps forward of the consumer position).
4. **S1.2 NEGATIVE RESULT (important)**: SetTransform (44) is NEVER called
   (calls=0 the entire run) — the engine is shader-driven; the draw camera
   reaches the GPU via **vertex shader constants**. SetViewport (47) fires
   ~876–3638×/frame with mipmap-cascade rects (2560x1440 → … → 1x1, plus
   1024x1024@0/1024/2048/3072 pages) — per-draw viewport/RT switching,
   runtime-confirmed (S0's per-record RT finding).
5. **S1.3 INCONCLUSIVE — the patch hit a dead entry**: head view 0 (type 2,
   flags 5ad8) is an ALL-ZERO template (M3 ViewDump at patch time: every
   matrix zero) — a dormant view that leads the list during cutscene/menu
   phases. No nudge + no match proves nothing about live-entry deref. During
   real gameplay the head is idx 65 (submitted in 602/602 frames; idx 14
   likewise — main-camera candidates for S3 duplication).
6. **S1.4 CONFIRMED + bug**: satellite frames present (t2 max 667/581,
   98k–118k submits/10s window) but `satelliteFrames=0` — the detector used
   the capped 64-record list count. Fixed in S1b (uncapped per-frame total).
   View lists: gameplay 14–44 type-2 views/frame, ALL flags f5ad8 (flags are
   not discriminative); list heads observed: 0 (cutscene), 1/14 (boat), 65
   (gameplay), 71/74 (satellite designation).
7. **Stability**: 0 FATALs, no Reset, all hooks survived sustained ~200
   views/frame — the mechanism set is robust at load.

### S1b — follow-up run (camera-channel re-instrumentation; run 2 done → results below)


### S1b run results (2026-10-02, run 2 — new game boat start, then a save load)

1. **World element VERIFIED (S1.1 element layout now closed)**: the dump at the
   pre-VM site found the `{0x30, 0x810, 0x680}` element one slot ahead of the
   consumer position — staging slot `0x01644550` (= ctx+0xc2110+0*0x30),
   `ViewEntry* 0x012865e0`, ctx block `0x01654d90`, all pointers exact. Ring
   position = `queue+0x10` low16 % cap, confirmed.
2. **Counter decode CLOSED**: `+0x10` low16 = ring POSITION (mod 4096;
   +624/frame boat scene, +213/frame menu); high16 = VM SCRATCH (mutated
   transiently mid-frame; spikes 278–506 at RenderFrame entry); `+0x14` ≈ 0–2
   always. Per-point bracket deltas are therefore noise — the S1.1 evidence is
   "the header dword changes between pre-VM and RenderFrame entry in ~100% of
   frames" = the `0x0050f660` call does queue work at pipeline time (answer
   unchanged). The "S1 raw:" series captures the values for the record.
3. **S1.2: the GPU matrices are DERIVED** — no exact matches vs any entry
   matrix except one 4x transposed match on c12 (tag not logged — fixed in
   S1c). Exact-match classification can only attribute copies, so S1c adds a
   bounded matrix exfil + offline relationship search in analyze_dumps.py.
4. **S1.3: windows A (m[1]) and B (staging slot) NEGATIVE on a live view**:
   the boat intro renders exactly ONE world view (head 0, live camera
   (−1729, −33, 2064), n=1 for ~1500 frames) — both patches ran on it and
   produced NO nudge and no residency match → neither entry m[1] nor the
   staging-slot pos feeds the draw camera for that scene. Remaining
   candidates: the walk-time camera-record ring (ctx+0xcb110), entry
   pos7c4, entry m[0], or a derived-from-something-else path. Caveat: the
   save-load section was never patched (windows are one-shot per process).
5. **REGRESSION (user-visible)**: the per-group classification cost (~13k
   groups/frame x ~800 memcmps) dropped the game to ~30fps average with
   multi-second spikes. S1c memoizes by content hash and budgets per-frame
   work.
6. Stability: 0 FATALs, no Reset; ~620 elements/frame flow through the ring
   in the boat scene (the world element is one of them).

### S1c — follow-up run (camera-channel discrimination; run 3 done → results below)


### S1c run results (2026-10-02, run 3 — new game, played through boat + gameplay; NO nudges reported)

1. **Performance fix CONFIRMED**: steady 16.67ms average (full 60fps) through
   gameplay; only the level-load spikes remain. The memo/budget machinery works.
2. **Two of the three evidence channels were defeated by carrier bugs** (so
   the "no nudges" result tests nothing yet):
   - **Exfil: 12 frames selected, ZERO emitted.** The emit keyed on the live
     view list, which has already moved on to the next frame by emit time —
     dead code. (S1d fix: entries snapshotted at selection time; the block is
     emitted unconditionally at the next pre-VM hook.)
   - **Patch windows mistimed**: A/B fired ~30s in on the menu-background
     phase (2 walked views i0/i1, head 0 — before the level load), and stage C
     then retried for ~35s with ZERO matching ring records while the load
     screen consumed the shared retry budget → "windows skipped": C/D/E never
     ran, and A/B were over before gameplay began. (S1d fixes: window starts
     gate on gameplay-like frames — menu submits 1–2 views, gameplay 15–85,
     gate ≥ 4; per-stage retry budgets instead of one shared budget; on
     window-C failure the raw ring records are dumped ("S1 crec:") so the
     record layout can be re-derived offline.)
3. **Classification starved**: ~24k float4s/frame exhaust the flat 256/frame
   budget on per-object matrices before the camera registers get a look
   (0 matches this run vs run 2's 4 transposed on c12). S1d: registers < c32
   are classified with priority (memo-deduped, budget-exempt).
4. Working as intended: full-ring element scan (6 world elements captured),
   satellite detection (satMax 109, 137 satellite frames), gameplay view
   lists (15–85 views/frame, distinct up to 96, head 15 in the wide phase),
   bracket steady.

### S1d — run 4 (audit fixes; run 4 done → results below)


### S1d run results (2026-10-02, run 4 — new game, full gameplay session; NO nudges)

All instrumentation channels finally worked (60fps steady, windows gated on
gameplay, exfil emitted, 0 FATALs), and the run 4 evidence is decisive:

1. **Windows A (m[1]), B (staging), D (pos7c4), E (m[0]) all ran during
   gameplay on 18–21 live walked views (views 15/16/17/49/50 area) — NO nudge
   in any of them.** None of those per-view fields feeds the visible draw
   camera.
2. **The camera-record ring layout is CRACKED** (the "S1 crec:" dump):
   records are 0x28-stride `{2 pos-ish floats +0x00/+0x04, quaternion
   +0x08..+0x14, ViewEntry* at +0x18, flags +0x1c, 2 floats +0x20/+0x24}`,
   sitting consecutively per walked view (views 15–21 seen), with world
   positions ≈2960–3130. The S0 plate comment's layout put the entry ptr at
   +0x20 — wrong, which is why window C starved. Window C is now fixed
   (+0x18) and is the TOP remaining suspect: walk-time pos+quat snapshots are
   the classic consume-time camera source.
3. **Register-priority classification produced real tags**: m[0] AND m[6]
   of views 70/71/72/74 reach the GPU byte-exact at c8/c9/c12 (multiple
   shader constant bases; c12=e0.m0T/e1.m0T appear in the early phase).
   Views 70–74 are the satellite/impostor tile cameras (high indices from
   the wide-view phases) — their draws use the entry matrices verbatim.
   **The main camera's matrices NEVER appear exactly** → its draws use
   derived data from somewhere else.
4. **Exfil relationship search (12 frames)**: the per-frame GPU constants are
   mostly global-constant soup (fog/light/view params — not matrices); c8
   carries a STATIC world position (−1470, −18.6, 2804) across gameplay
   (a level-wide constant, not the live camera). The M3 "FOV sin/cos" field
   offsets (+0x2ec/+0x2f4/+0x188) decode as garbage for the exfil head views
   (sin=−1, cos=0, near=−3) — those offsets don't generalize, so projection
   products could not be constructed offline yet.
5. Remaining main-camera candidates, in order: the **camera-record ring**
   (window C, fixed), the **other entry matrices m[2]..m[8]** (m[6] is real
   for satellite views; never patched), and the **ctx-block primary-
   subobject copies** (2 × 0x164 at block +0xEC — never dumped).

### S1e — run 5 (remaining channels; run 5 done → results below)


### S1e run results (2026-10-02, run 5 — new game, full gameplay; NO nudges)

All six windows A–F ran during gameplay (60fps-ish; C finally patched 25 ring
records with the +0x18 layout, F patched 160 matrix targets) — **no nudge in
any**. The per-view channel space is exhausted: every camera data field in
the ViewEntry (m[0..m[8], pos7c4), the camera staging slot pos, and the
walk-time ring-record pos has been patched post-walk with no visible effect.
Nudge magnitude is NOT the issue: +4 world units ≈ 4 m (two character
heights; logged camera positions are meter-scale) for ~5 frames, plus the
restore/re-apply cycle freezes the patched field (a moving camera would also
stutter).

The extended exfil (9 matrices per view + ctx subobject copies) found **zero
exact sub↔entry hits** and no close GPU↔entry matches — the visible camera is
not a raw copy of anything dumped so far. `vsmatch` this run: only the early
e0.m0T/e1.m0T (no satellite section performed, so views 70–74 never
activated).

**Open possibilities after run 5:** (a) the visible draw camera reads a
source we have not touched — the ctx 0x680 block's non-copy fields, the LIVE
primary subobjects (only their +0xEC copies were dumped), the entry's second
pos copy +0x7ac, or a game-side camera object; or (b) something in the
patch→consume chain is broken in a way the run cannot distinguish from (a)
— run 5 had no way to prove a patch ever reached the GPU.

### S1f — run 6 (positive control + last sources; run 6 done → results below)


### S1f run results (2026-10-02, run 6 — full gameplay; NO nudges; DECISIVE)

1. All eight windows A–H ran during gameplay (A 2 targets, B 18, C 32 ring
   records, D 40, E 31, F 160 matrix targets, G 3 ctx/live-sub pos matches,
   H 53 pos7ac targets) — NO nudge.
2. **GPU proof ZERO** ("S1 vspatched:" never fired): during matrix windows
   A/E/F not one patched matrix appeared in any GPU upload — while UNPATCHED
   entry m[0] uploads DO occur (vsmatch e0.m0T/e1.m0T; satellite views 70–74
   in runs 4–5). Patches are applied at the pre-VM hook, before the consumer
   runs, so a consume-time read would see them: their total absence means
   **the VM consumer does not read the walked views' matrices at all**.
3. **The main camera is LOCATED on the GPU** (exfil dynamic-register
   analysis): c21 = the camera world position (dynamic), c23–c26 = the
   world-to-view matrix (dynamic x7 — changes every frame; near-identity
   rotation with the camera position in the translation slot). This is the
   visible draw camera.
4. **Static RE found the camera chain** (annotated in Ghidra):
   `ViewEntry.camData` (entry+0x7ec — copied from the game view object's
   +0x8c at activation, FUN_00488e40) points at the UPSTREAM camera objects;
   FUN_0048f9d0 (per-view camera update, called from FUN_0082c390) converts
   the entry's QUATERNION (+0x7d4..+0x7e0) + pos7c4 into the nine matrices
   via FUN_0048a7b0/FUN_0048a8f0 — the entry is DERIVED state; FUN_0048a8f0
   also reads a global camera chain (`*(DAT_00e79dfc+0x104)`, FOV-ish
   floats at +0x17c/+0x180/+0x184).
5. Synthesis: the view entries (and staging/ring records) are per-frame
   DERIVED copies; the VM consumer reads the camera from the upstream
   chain, which no patch window touched — explaining all negative results
   across runs 4–6 coherently.

### S1g — run 7 (the upstream chain; IMPLEMENTED 2026-10-02, pending run)

- **Window I (stage 8)**: patches the entry QUATERNION x (+0x7d4, delta
  +0.1 — the per-frame matrix source per FUN_0048f9d0; never patched; a hit
  shows as a camera tilt/spin rather than a shift).
- **Window J (stage 9)**: scans each live view's UPSTREAM camData object
  (entry+0x7ec, deduped, 0x200 bytes) for camera-pos-matching floats and
  patches them — the chain the VM evidently reads.
- **Exfil**: dumps the camData objects (0x100 per snapshotted view,
  "S1 excam:") and the global-chain inner object ("S1 exglob:") per exfil
  frame; the relationship search scans them — a dist=0 or exact hit vs the
  c21–c26 GPU camera locates the upstream camera fields, which become the
  S2/S3 per-eye injection point.


- **Per-window GPU proof (the "is the experiment wired" check)**: during
  matrix windows A/E/F the carrier snapshots each target's full 16-float
  matrix right after patching and compares EVERY GPU-bound group against the
  expected set — a hit ("S1 vspatched:") proves the patched field reaches the
  GPU even if the visible result is nil (offscreen view); silence proves the
  consumer does not read the field at all. This disambiguates (a) vs (b)
  above without depending on the visual signal.
- **Window G (stage 6)**: scans the ctx 0x680 block AND the two live primary
  subobjects (via the block's +0x74 pointers, the never-touched VM-visible
  camera homes) for floats equal to any live view's camera position
  component, and patches them — surgical, only current-camera-coordinate
  holders. Per-frame rescan with restore-with-verify (the content moves).
- **Window H (stage 7)**: entry pos7ac[0] — the OTHER M3 camera position
  copy, never patched before.
- **Exfil additions**: the live subobjects dumped per gameplay exfil frame
  (as exsub2/exsub3 — the analyzer's window search covers them unchanged),
  plus the full c0–c31 VS register cache at emit time ("S1 excache:") — the
  offline dynamic-register report lists which low registers change across
  exfil frames (camera candidates) vs static global constants.


- **Window C fixed** (ViewEntry* at +0x18) — the top suspect finally testable.
- **Window F added** (stage 5): patches the translation x of m[2]..m[8] on
  all live walked views — the never-patched matrices (m[6] is proven real
  for satellite draws).
- **Exfil extended**: the first 3 walked views snapshot ALL NINE matrices;
  the ctx-block primary-subobject copies (2 × 0x164 at +0xEC) are dumped per
  exfil frame ("S1 exsub:"); high-register GPU matrices are only collected
  when their translation looks like a world position (|tx| > 5 — camera-basis
  signature), so the 48-matrix budget is not wasted on global-constant soup;
  exfov carries pos7c4.
- analyze_dumps.py: all-9-matrix attribution, sub-vs-GPU and sub-vs-entry
  window searches (an exact sub==entry hit would locate the camera copy
  inside the subobject).


- Exfil snapshot+emit fix, gameplay-gated windows A–E with per-stage budgets,
  "S1 crec:" ring-record dumps on window-C failure (offline layout analysis
  via analyze_dumps.py, which flags ViewEntry pointers at any record offset),
  register-priority VS classification, and per-window target lists in the
  patch logs (which views are being nudged).
- Everything else unchanged from S1c; the run procedure is identical: count
  up to five ~5-frame nudges ~1s apart once gameplay starts.


- **Performance fix** for the run-2 regression: content-hash memoization of
  classification results (512-entry direct-mapped, full 64-byte verify),
  per-frame budgets (≤256 NEW classifications; the expensive ctx/subobject
  region scans only for the first 64 of those), memo/skip counters in the
  window report.
- **Match-tag logging**: every distinct (register, tag) matrix match logged
  one-shot ("S1 vsmatch:") — run 2's only real match never got a tag.
- **Patch windows A–E, on ALL walked views** (not just the head — removes the
  "maybe the head wasn't the visible camera" ambiguity), 5 frames each,
  ~1s apart so up to five nudges are individually countable:
  A entry m[1][3] — re-run of the run-2 channel, now all views;
  B camera staging slot pos[0] (ctx+0xc2110+idx*0x30) — re-run, all views;
  C camera-record RING records pos[0] (ctx+0xcb110, 0x28-stride
  {pos3, serial, rot16, ViewEntry* at +0x20, lodByte}; per-frame re-scan with
  restore-with-verify — the ring keeps advancing) — the walk-time snapshot
  the VM consumer most plausibly reads;
  D entry pos7c4[0] — the raw camera position copy;
  E entry m[0][12] — the viewToWorld translation (the consumer might invert
  it on-GPU).
- **Full-ring element SCAN** (replaces the position-window dumps): scan the
  whole ring for {0x30,0x810,0x680} world elements directly — deterministic,
  no position arithmetic.
- **Matrix exfil** for the offline derivation search (the GPU matrices are
  derived — exact-match can never attribute them): first 4 world-view frames
  + every 200th up to 12 total; per frame the walked views' m[0]/m[1] hex +
  head FOV/near dwords + ≤24 unique GPU matrices with their registers;
  analyze_dumps.py then reports, per GPU matrix, the closest entry matrix
  (m0/m1 + transposed) by mean |diff| — a small non-zero distance reveals
  the derivation (projection product, conjugation, rebuilt view matrix).
- Expected evidence: up to five ~5-frame nudges ~1s apart after the first
  world-view frame (count them); "S1 vsmatch:" tags; world elements from
  the scan; and the exfil blocks for offline analysis if all five windows
  come back negative.


- **SetVertexShaderConstantF (device slot 94, per d3d9.h order that matches
  every runtime-pinned slot) VmtHook** — the S1.2 replacement channel. Every
  GPU-bound 4-float group (bulk upload path) and every row-wise float4 upload
  (register cache, assembled when 4 consecutive registers fill) is classified
  against the live ViewEntry matrices (exact + transposed), the frame-ctx
  0x680 block, and the two live primary subobjects; tags like `e65.m1`,
  `ctx+0x1ec`; per-register hit counts reported per window (`S1 vsreg:`).
- **Residency patch v2** on a LIVE head (precondition: nonzero m[0]
  translation — kills the view-0 template case): window A patches m[1][3] for
  5 frames; after a ~2s gap (so the two nudges are separately visible), window
  B patches the camera STAGING SLOT pos[0] (`frameCtx+0xc2110+head*0x30`) for
  5 frames. Nudge A = draw camera reads the live entry m[1]; nudge B = draw
  camera reads the staging slot at consume time; neither (plus VS-constant
  `none` tags) = the camera comes from elsewhere (candidate: the 0x28-stride
  camera-record ring at `this+0xcb110`) — S1c would bracket that.
- Element dumps re-armed on world-view frames, dumped forward of the
  consumer position; counter reads are 32-bit ("S1 raw:" series over the
  first 60 world-view frames for offline re-derivation); satellite detection
  fixed (uncapped totals).
- Expected evidence per this run: two distinct ~5-frame nudges ~2s apart
  (or their absence), `S1 vsmat:` matrix-classification details, a
  `{0x30,0x810,0x680}` world element in the `@consPos` dumps, and the
  `S1 vsreg:` register table naming the camera registers.

Static work hit the SecuROM wall at the consumer; S1 settles the open semantics at
runtime, all via proven mechanisms (no new hook species):

1. **Bracket the VM consumer**: MidHook at `0x004c99f9` (pipeline, just before the
   `0x0050f660` VM stub call — read `countersA`, snapshot a ring element) and at
   `RenderShell_RenderFrame` entry (`0x00855690`, inline hook). If `countersA` advances
   between the brackets, the interpreter runs at pipeline time as suspected. Also
   verify the staged-element layout by dumping ring bytes at the consumer position
   (expect header + 3 pairs {0x30, 0x810, 0x680} per element).
2. **Which camera data the draw actually consumes**: add `SetTransform` (device vtable
   slot 44 = `+0xB0`) and `SetViewport` (slot 47 = `+0xBC`) logging to the proven device
   VmtHook (slots were pinned in M2; same cloned-vtable mechanism). Attribute per
   frame: does `D3DTS_VIEW` match a `ViewEntry` matrix (m[1] worldToView expected) or
   the frame-ctx 0x680 block — this decides whether the per-eye camera patch targets
   the shadow entry alone or also a 0x680 ctx clone.
3. **Residency proof**: in the `0x004c99f9` pre-hook, patch the head view's m[1]
   translation by a small offset; if the same frame's `SetTransform` log shows the
   patched value, live-entry deref at consume time is confirmed.
4. **Satellite classification** (unchanged goal): extend the M3 view aggregation to log
   per frame the head index, type-2 view count, and `(idx, type, flags)` list, to pick
   the views S3 duplicates (expect: main camera = list head; satellite 608-view frames
   must NOT be duplicated — also forced by the 768-element staging cap).

**Operational notes (for the implementing agent):**

- All addresses/constants needed are in the `SubmitWorldPackets` plate comment and the
  site comments (S0): counters at `0x00ff3628`/`0x00ff362c`, ring buffer `0x00ff3624`,
  elem 96/cap 4096, head index `0x00d29e60`, `g_ViewTable` `0x012865e0` (stride 0x810),
  frame-ctx object = EBX inside `SubmitWorldPackets` body (capture at the existing
  `0x0048e9ea` MidHook, or ECX at entry), 0x680 block at `frameCtx+0xd2950`, staging
  array/count/addresses (`[ESP+0x79a0]`, `[ESP+0x19a00]` at the `0x0048ef71` site).
- Ring-walk arithmetic for the element dump (from the S0 counter decode): consumer
  position = `(countersA.low + countersA.high) % cap`; producer position =
  `(countersB.low + countersA.low) % cap`. In the `0x004c99f9` pre-hook, the just-
  published (unconsumed) elements sit between those two positions.
- Residency-proof timing (item 3): the interpreter and the whole RenderFrame record
  walk must see the patched entry, so patch at the `0x004c99f9` pre-hook and RESTORE at
  the next frame's pre-hook — one frame with a slightly offset camera is the expected
  visual signal; the same frame's SetTransform log should show the patched values.
- Decision tree for S1.1 results: countersA advances between `0x004c99f9` and
  RenderFrame entry → interpreter confirmed at pipeline time (`0x0050f660` identified,
  clone-at-stage S3 proceeds as designed). CountersA only moves during/after RenderFrame
  entry → the interpreter is a different call (re-check the other post-submission calls:
  `0x006b93e0`, `0x006f9490` from `0x004c99fe`/`0x004ca003`) or consume
  happens inside RenderFrame — re-bracket inside RenderFrame before concluding anything.
  SetTransform shows per-view matrices matching live `ViewEntry` content → residency +
  m[1] attribution confirmed. SetTransform shows ctx-derived values only → the 0x680
  block feeds the camera and S2 needs the per-eye ctx clone as primary, not optional.
- Extend, don't replace, the M3 ambient telemetry (same hook objects where possible);
  the existing `view_midhook` already gives `(idx, type, entry)` per iteration and is
  the right place for the S1.4 aggregation changes. The queue poller already reads the
  counters — reuse its state rather than adding a second reader.
- Build-lock/self-test rules apply as usual (carrier change → `tools/selftest/run.sh`).
  Note: `render_dump.cpp` already logs the head as `listHead=` (was `tableCount=`,
  S0-corrected; `analyze_dumps.py` does not parse that token).

### S2 — Eye math

Per selected type-2 view, build EYE-2 SHADOW BLOCKS (the S0-residency answer moved the
patch target off the live entry — it is consumed after the walk, so both eyes must own
their data):

- Shadow `ViewEntry` (0x810, carrier-owned): memcpy of the live entry, then patch the
  camera fields — `pos' = pos ± right * IPD/2` in m[0] (+0x20) and the negated position
  in m[1], rotation unchanged; per-eye FOV half-angle sin/cos at `+0x2ec/+0x2f4` (and
  the near-plane region near `+0x188`) if S1 shows the pipeline consumes them.
- Shadow camera-staging slot (0x30): {pos', serial, rot16, lodByte} per the S0 slot
  layout.
- Optionally a per-eye 0x680 frame-ctx clone (0x680 bytes) if S1.2 shows the draw
  camera comes from the ctx block; otherwise the block stays shared.
- IPD + pose from the HMD runtime, marshaled to the main thread before the producer
  loop (slot-5 claim, S4). All shadow building happens on the main thread inside the
  producer loop — same-thread writes, no synchronization.

### S3 — First duplication (first visual milestone)

Mechanism — **clone-at-stage** (chosen over body re-emit; both sites found in S0):
MidHook after the per-view element staging copy (`0x0048ef71`; count at
`[ESP+0x19a00]`, staging array at `[ESP+0x79a0]`): for each selected view, memcpy the
just-staged 96-byte element into the next staging slot, rewrite the pair pointers
(`{0x810}` → shadow entry, `{0x30}` → shadow staging), bump the staging count, respect
the 768 cap. This never re-runs the loop body, so the body's side effects (the
`DAT_01174a0c` render-slot allocation, `DAT_00d29e70` portal registration, `const7a4`
writes, camera-record ring) stay single-execution.

Fallback if the cloned element turns out to be ignored by the consumer (S1.1 will
tell): re-emit via MidHook at `0x0048f013` redirecting EIP → `0x0048e9d0` with
`[ESP+0x10]`/EAX pointed at the shadow entry (the whole body then sources from the
shadow — ViewRef, LOD, staging write; the shadow staging slot must be pre-offset so
the eye-1 slot isn't clobbered). Side effects double-run in this variant — the portal
registration must then be suppressed or absorbed.

Risks to watch: staging cap 768 (608-view satellite frames → only duplicate selected
world views); queue pressure at 2× selected views (ring cap 4096, large headroom —
poller confirms); the render-state cache is per-stream (render_path "cache desync"
warning) — whole-element duplication should stay cache-coherent since blocks are
self-contained, but verify visually.

### S4 — Presentation / HMD runtime

- `Present` VmtHook (proven) as the compositor entry: submit the frame to OpenVR/OpenXR
  (submit both eye textures, or the SBS target). Interop blit needs no swapchain changes —
  the game's Present continues to the monitor untouched (debug-friendly).
- Slot-5 (`PostUpdateHook`) claim as the per-frame VR orchestration point: sample HMD pose,
  marshal to the main thread before the producer loop runs, feed S2. Slot-4
  (`EndOfFrameHook`) for end-of-frame bookkeeping (timewarp input, frame pacing via the
  game's own `g_FrameDeltaSec`/framerate policy bypass — see `main_game_loop.md`).
- Decide OpenVR vs OpenXR when integrating (OpenXR preferred on Proton/RADV via
  wineopenxr, which the prefix already has; verify runtime availability in the prefix —
  `openvrpaths.vrpath` and `wineopenxr64.json` exist from prior use).
- UI/2D layer (`g_RenderQueue2`): render once; composite over both eyes via a world-space
  panel or screen-space overlay in the compositor (decide in S4 prototypes). Per-eye
  pixel rects are NOT a producer-side field (S0): viewport/RT selection is per draw-record
  at consume time — the compositor owns per-eye targeting (separate eye RTs or SBS halves
  via its own device usage, e.g. a second swapchain/RT pair created by the carrier).
- Note (S0): the draw-record/RT tables are VM-filled — if per-eye RTs must differ at the
  D3D level per view (not just at Present), the fallback is Present-hook interop blit of
  the single backbuffer into per-eye targets, which needs no RT plumbing at all.

### S5 — Motion controls (separate track)

Follows the logic-mod track in `docs/launcher_plan.md` (XInput stubs `0x00a64d56/0x00a64d5c`,
idle-reset buffer pair `0x017d30e8`/`0x00f7fb90` resolution first). Pose marshal point is the
slot-5 hook (S4). No new mechanism — this doc ends at visual VR.

## Hook inventory (all sites proven mechanisms unless noted)

| Site | Mechanism | Phase | Status |
|---|---|---|---|
| SubmitWorldPackets loop head `0x0048e9ea` | MidHook | M3 anchor + S1.4 tap (flags, frame-ctx EBX) | installed (M3, extended S1) |
| SubmitWorldPackets element staging `0x0048ef71` (count `[ESP+0x19a00]`, array `[ESP+0x79a0]`, cap 768) | MidHook | S3 clone-at-stage | S0-mapped, pending install |
| SubmitWorldPackets iterator reload `0x0048f013` (ESI=`*(entry+4)`; EIP→`0x0048e9d0` re-runs view) / back-edge `0x0048f01f` | MidHook (+context EIP redirect) | S3 fallback re-emit | S0-mapped |
| Pipeline pre-VM-stub `0x004c99f9` + RenderFrame entry `0x00855690` | MidHook / MidHook at first instruction | S1 consumer bracket | **installed (S1)** — plus taps at first stream opcode and slot-4 for a 4-point bracket |
| Device `SetTransform` (44) / `SetViewport` (47) | VmtHook (proven device clone) | S1 camera attribution | **installed (S1)** — SetTransform proved UNUSED by the run (kept as the per-run deadness check) |
| Device `SetVertexShaderConstantF` (94) | VmtHook (proven device clone) | S1b camera attribution (real channel) | **installed (S1b)** |
| `ViewEntry` camera fields | shadow copies (S2), never in-place on live entries | S2/S3 | S0 design settled |
| device `Present` (slot 17) | VmtHook | S4 compositor | installed (M2) |
| device `Reset` (slot 16) | VmtHook | S4 param changes | installed (M2) |
| `g_RenderShell` slot 5 (`PostUpdateHook`) | cloned-vtable claim | S4 pose sampling | installed (M3, counting noop) |
| `g_RenderShell` slot 4 (`EndOfFrameHook`) | cloned-vtable claim | S4 bookkeeping | installed (M3, counting noop) |

**Never hook**: the VM entry stub `0x0050f660` (and anything at `0x01a48000+`) — the
suspected consumer lives there; bracket it from the plaintext call sites instead.

## Open questions (tracked, non-blocking for S1)

- ~~Primary sub-objects `g_RenderShell+0xFDC` (count 2)~~ — **RESOLVED (S0)**: per-frame
  frame-ctx copies, not per-view, not eye slots.
- ~~Which of the nine matrices feeds the actual draw~~ — S1.2 REFRAMED: the engine
  never calls SetTransform (shader-driven); the question is now WHICH VS constant
  registers carry the camera matrices and whether they match a live ViewEntry
  matrix, a ctx-block value, or a derived product — S1b answers (window A/B nudges
  discriminate m[1] vs staging-slot provenance).
- Does the VM consumer iterate the ring elements (duplication works) or re-walk the
  active list itself via the ctx head (duplication bypassed — consumer-side fallback
  needed)? S1.1/S1.3 answer; this is the last structural risk to the producer-side plan.
- `g_RenderQueue2` consumer — likely the same VM interpreter (it receives both queue
  pointers in the 0x680 block); S4 needs to know when the 2D stream is consumed relative
  to Present — extend the S1 bracket with the queue2 counters.
- Frame pacing: game runs vsync-locked 60 Hz (`interval=0`, DISCARD); HMD typically 90 Hz.
  Present-hook compositor can run at HMD cadence independent of the game loop (pose
  extrapolation/timewarp via the HMD runtime), leaving game pacing untouched. Decide in S4.
- Element staging cap is 768 (`[ESP+0x19a00]` guard in the producer) — hard producer-side
  ceiling; S3 view selection must keep total (originals + clones) under it.

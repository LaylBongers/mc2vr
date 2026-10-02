# Next Steps — Camera Hunt Follow-Ups (S2 pre-work)

Terse, actionable. Order: #1 first (one run, decisive), #3 in parallel (static, no runs), #2
follows from whichever way #1 goes. Background: `stereo_design.md` (S2a/S2b), `s1_camera_hunt.md`
§handoff, plates on `RenderShell_RenderFrame` / `g_PrimitiveBase`. Consume-side naming
(PgPrimitive/PgMaterial) is done — none of these re-open it.

## 1. Verify the w≠1.0 rows drive the view — load-bearing unverified assumption

S1 proved rewriting w==1.0 rows moves EFFECTS, not the camera. The claim that the c23–c26 family
(near-identity rotation, camera position in translation, w≠1.0; 3–4 consecutive dynamic rows after
a w==1.0 position row) is the actual view transform is RUN-6 INFERENCE, never verified — the
current filter doesn't match w≠1.0.

- How: extend the ambient rewrite filter in `on_set_vs_constant` (`src/carrier/s1_probe.cpp`,
  `mc2vr.conf gpu_boundary_rewrite=pulse`) to also shift those rows (translation only first — minimal
  risk). One verification run.
- View moves → S2b design confirmed; go straight to S2a identification.
- View doesn't move → the real VP is elsewhere; #2 and #3 become the hunt. Do NOT re-litigate the
  producer side (s1_camera_hunt runs 4–15).

## 2. Characterize `prim->vsConstData` (+0x50, count +0x54)

The only unclassified plaintext per-record VS-constant channel: uploaded as
`(wrapper, technique+0xac reg, prim->vsConstData, prim->vsConstCount * 3)`, gate technique+0xb0
(call site + plate item 2 on RenderShell_RenderFrame). VM-filled; the `*3` unit is unexplained.

- How: static layout facts are on the plate already; runtime — attribute uploads by call site via
  the existing SetVertexShaderConstantF VmtHook (count == vsConstCount*3), dump the float4 blocks
  per frame, compare against known values (identity? object world matrix? VP?).
- Outcomes: per-object world matrices (likely — fine for S2c replay) vs. view-projection
  (game-changer: per-eye rewrite target moves here). Answer also settles the `*3` semantics.
- Feeds S2c: these re-upload during the eye-2 replay — must know what they are.

## 3. Find the projection matrix — genuine hole, required for per-eye

Nobody has located it: SetTransform NEVER fires (S1), and the c23–c26 rows are near-identity
rotation (no perspective terms). Projection must be folded into shader constants or `vsConstData`.
Per-eye VR needs it (ideally asymmetric), not just camera position.

- How (a) — static, no runs: disassemble the `.sho vertex shaders` from the game dir (D3D9
  bytecode; constant registers are positional). Map registers holding VP/world/camera per
  technique; correlate with the technique-table register fields (+0xac/+0xb0/+0xb8/+0xd0/+0xd8/
  +0xe8/+0xec/+0xf0 gates — RenderFrame plate item 2) and the `PgShader_Register` names. First
  check the `.sho` container format (likely a small header + raw bytecode).
- How (b) — analysis of existing S1 capture data: classify all captured VS row families for a
  perspective signature (non-uniform scale row, w-row with z terms).
- Outcome: deterministic register-role map per shader — S2a identification becomes a lookup
  instead of statistics, and the sliding-bases problem becomes predictable.

## Related open questions (do not duplicate here)

- Eye-2 material-texgen mono-projection (future SetPixelShaderConstantF hook, wrapper slot 109) —
  `stereo_design.md` §Open questions.
- RenderCmd_ExecuteStream opcode map (S2c + last plaintext hop) — `stereo_design.md` S2c,
  `render_path.md`.

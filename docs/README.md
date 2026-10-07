# docs/ index

Two kinds of documentation:

- **This directory** — our mod's implementation: launcher/carrier/host architecture, stereo design, status
  and handover briefs. Nothing here is a claim about how the game works beyond what the design depends on
  (those premises link into `reverse_engineering/`).
- **`reverse_engineering/`** — what we learned about the game itself (binary layout, SecuROM, engine, render
  path, camera/shader data) plus Ghidra methodology. See its `README.md`.

| Doc | Read it for |
|---|---|
| `launcher_plan.md` | Launcher/carrier/host architecture, build/test/live-run workflow + healthy-run log signatures, proven mechanism rules, hook inventory, milestone history (M0–M4) |
| `stereo_design.md` | Stereo design + status (S0–S6), the view-rewrite channel, S2c second pass, OpenXR host/IPC (§S4), HMD camera (§S4-4), the S4-5 record (events/pacing/HUD) incl. the **known reprojection-staleness issue (must fix eventually)**, frame-level hook strategy. §S4-4 follow-ups record the CLOSED world-scale/IPD verdict (2026-10-07) |
| `stereo_improvements_plan.md` | Improving the S4-4 injection with the 2026-10-06 RE: record-level per-eye rewrite (PS camera data per-eye, less per-upload math), the upstream-ViewEntry decisive experiment (E2), ViewEntry-direct camera (no VP decomposition), what stays hard; experiments E1–E3 + rollout order |
| `frustrum_cull_plan.md` | S6 culling alignment: HMD-following, FoV-extended frustum culling — RE evidence summary (the culling data flow, change-gating, all function addresses), injection design (runtime view matching, serial bumps), conf flags, verification + open items |
| `hud_plan.md` | **OPEN track (2026-10-07)**: HUD/crosshair depth fix — root cause (screen-space HUD ignores the per-eye asymmetric-frustum centers ⇒ ~0.47 tangent units CROSSED disparity ⇒ perceived ~14 cm from the face; plus ~1.9× horizontal aspect distortion from the un-squeeze), fix directions (per-eye HUD shift at the upload gate), verification plan incl. the per-eye dump-pair measurement recipe, open items |
| `render_diagram.svg` | Frame flow with the opaque VM stub and every MC2VR hook point (keep in sync when hooks change) |

Closed tracks are dissolved into the long-lived docs once their verdict is recorded (world-scale:
`reverse_engineering/pandemic_engine.md` § World units + `stereo_design.md` §S4-4 follow-ups;
history in `git log --follow -- docs/`).

Per-address facts live in the Ghidra project (plates, labels, structs). Config keys are documented in
`conf/mc2vr.conf` (debug tools: `debug_*`, code in `src/carrier/debug/`). Per-milestone handover briefs
were dissolved into the long-lived docs once each milestone closed (history in git,
`git log --follow -- docs/*_handover.md`).

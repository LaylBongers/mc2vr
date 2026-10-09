# Deprecated reference systems

If you find bare plan tags (`M0…M4`, `S0…S6`, `S2c-0/-1/-2`, `S4-0…S4-5`, `S1b`) or `docs/plans/*` paths anywhere — logs, old notes, git history, stale Ghidra comments — this is what they were. Do not re-introduce them; rewrite with plain language and the current doc pointer.

Two implementation plans existed under `docs/plans/` and were **dissolved 2026-10-09** into the atomic notes; the stage tags they stamped everywhere were **removed 2026-10-09**:

- **M-series** (`M0`–`M4`) — the old *launcher implementation plan* milestones (boot log, SecuROM pre-probes, device capture, render instrument, present-params). Live code now identifies these stages by name in [hooks.md](hooks.md) / [carrier.md](carrier.md).
- **S-series** (`S0`–`S6`, sub-stages `S1b`, `S2c-0/-1/-2`, `S4-0`–`S4-5`) — the old *stereo implementation plan*: S0 static render RE, S2 the viewContextData GPU-boundary rewrite, S2c stream capture (`-0`) / second draw pass (`-1`) / per-eye passes (`-2`), S4 the OpenXR host (sub-stages: test pattern `-0`, IPC `-1`, shared-handle path `-2`, submission `-3`, pose-id `-4`, pacing/events `-5`), S5 culling RE, S6 culling acceptance. Content lives in [stereo.md](stereo.md), [view_rewrite.md](view_rewrite.md), [camera.md](camera.md), [culling.md](culling.md), [hud.md](hud.md), [host.md](host.md), [shared_textures.md](shared_textures.md), [ipc.md](ipc.md).

Dissolved plan files and where their content went (also encoded in `tools/ghidra/fix_doc_refs_ghidra.py`, used to patch Ghidra comments): `docs/plans/stereo_improvements.md` → [camera.md](camera.md); `frustum_cull.md` → [culling.md](culling.md); `launcher.md` → [hooks.md](hooks.md); `hud.md` → [hud.md](hud.md); `stereo_design.md` → [stereo.md](stereo.md).

## Live-artifact caveats

- **Old logs**: carrier logs from binaries built before 2026-10-09 carry plan-tagged lines (`M3 ViewDump`, `S2c: …`, `S2c replay: avgMs`). `tools/analyze_dumps.py` matches the NEW tags (`ViewDump`, `stream: …`, `stream replay: …`) and will not parse those logs — use a pre-cleanup checkout of the tool, or ignore them.
- **E-series is NOT deprecated** (`E1`, `E1b`, `E2`, `E2b`, `E3`): those are the camera-reverse-engineering experiments, still defined and cited in [camera.md](camera.md) § Experiments.
- If a stray tag survives somewhere (e.g. an unannotated Ghidra comment), replace it the same way: drop the tag, keep the date/prose, point at the note that now holds the fact.

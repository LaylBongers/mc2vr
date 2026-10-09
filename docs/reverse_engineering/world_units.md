# World units (VERIFIED 2026-10-07)

**1 game unit = 1 metre.** Havok physics: world gravity is 9.8–9.81 game units/s² — the Earth value.

- `hkpWorld` ctor `0x008d8f40` (plate + labels there) defaults the cinfo gravity magnitude (vector at cinfo+0x10, matches `hkpWorldCinfo::m_gravity`) to 9.81 when zero (`0x00b58ff0`, guarded at `0x008d9393`/`0x008d9db6`); 21 stored `(0,−9.8,0)` hkClass default member vectors in `.rdata`; ZERO feet-convention constants (32.174/32.2/0.3048/3.28084) anywhere in the image (full initialized-memory bit-pattern sweep, 51.4 MB).
- Character capsule / eye heights are NOT in the exe — data-driven via Havok reflection metadata (`hkpCharacterProxyCinfo` etc.) and the game tweak system (`JumpHeight`, `MinAltitude` string keys → values in game data archives).
- Consequence for the mod: `view_world_scale = 1.0` (conf key) — verified statically AND live (per-eye IPD parallax, 2026-10-07; [../camera.md](../camera.md)). Do not change without re-verifying.

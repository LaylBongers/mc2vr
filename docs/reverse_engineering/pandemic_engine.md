# Pandemic Engine — Cross-Cutting Notes

Terse engine-wide knowledge for working with the decompilation. Per-address facts (layouts,
addresses, slot maps) live in Ghidra plates; render specifics in `render_path.md`, camera/view data in
`view_and_camera.md`, the SecuROM/VM layer in `securom_vm.md`, vtable recipe in `vtables.md`.

## World units & physics (VERIFIED 2026-10-07)

- **1 game unit = 1 metre.** Havok physics: world gravity is 9.8–9.81 game units/s² — the
  Earth value. `hkpWorld` ctor `0x008d8f40` (plate + labels there) defaults the cinfo gravity
  magnitude (vector at cinfo+0x10, matches `hkpWorldCinfo::m_gravity`) to 9.81 when zero
  (`0x00b58ff0`, guarded at `0x008d9393`/`0x008d9db6`); 21 stored `(0,−9.8,0)` hkClass default
  member vectors in `.rdata`; ZERO feet-convention constants (32.174/32.2/0.3048/3.28084)
  anywhere in the image (full initialized-memory bit-pattern sweep, 51.4 MB).
- Character capsule / eye heights are NOT in the exe — data-driven via Havok reflection
  metadata (`hkpCharacterProxyCinfo` etc.) and the game tweak system (`JumpHeight`,
  `MinAltitude` string keys → values in game data archives).
- Consequence for the mod: `view_world_scale = 1.0` (conf key) — verified statically AND
  live (per-eye IPD parallax, 2026-10-07). Do not change without re-verifying.

## Naming layers

- `Pg*` = engine/render core (PgPrimitive, PgMaterial, Pg*.sho shaders). `Lti*` = platform layer
  (LtiRenderer, Lti_DebugPrintf, Dx9_* helpers). Source roots in debug strings:
  `D:\projects\Mercs2_PC\LTI\Src\...`, `D:\projects\Mercs2_PC\Odin\Win32\...` ("Odin" = this game's project).

## Fastest identification oracles (use in this order)

- **String search first.** `"Class::Method"` strings + source `.cpp` paths identify classes outright
  (e.g. the `PgPrimitive::*` method strings + `PgPrimitiveWin32.cpp` named the whole primitive
  system). A method string is usually referenced by the very function it names, often only lazily
  hashed at entry.
- Debug/assert strings name things (`RenderSystem_Init` via its thread assert, BeginSubmit/EndSubmit
  via their error prints).
- No MSVC RTTI — derive classes from ctor vtable writes + strings; annotate vtables pre-emptively.

## Name hashing (two distinct mechanisms — don't confuse them)

- `Lti_LazyNameHash` (0x008244a0, ~139 callers): per-site FNV-1a hash of `"Class::Method"` cached in
  `.bss`, read only by VM'd code — inert telemetry, not feature/device checks.
- Inline FNV-1a for **object names** (e.g. `PgMaterial::nameHash64`): basis `0x811c9dc5`, prime
  `0x1000193`, case-insensitive (`c | 0x20`), NUL terminator hashed as `0x2a`.

## Render data model (consume side, all plaintext; layouts on Ghidra plates)

- **`PgPrimitive`** (0x58): per-draw submit record — draw params, VS, technique, stencil, and table
  INDICES (`materialIdx/viewIdx/envIdx/viewContextIdx/screenIdx`). Singly-linked submit list
  (base/head/next-table in the `g_PrimitiveBase` plate; next entries are {6-byte sort key,
  ushort next}, `0xffff` terminates). Lifecycle: Reset → VM'd build/AssignKeys →
  `PgPrimitive_SortList` → SubmitToGPU (`0x00855690`, formerly mislabeled RenderShell_RenderFrame).
- **`PgMaterial`** (0x190): NAMED material ("OcclusionMaterial", "PgPrimitiveSubmitToGPU") —
  texture-projection transform `rows[6][4]` × view scale/offset goes to **pixel**-shader constants
  (texgen). Rule of thumb: PS-constant transform + textures + blend flags = material, not camera.
- Shaders: `.sho` files via `PgShader_Register(name, file, variant)`; `_pl/_sl/_pl_sl` screen-effect
  variants + `_li` fallbacks, gated by `g_ScreenEffectsEnabled`.
- Per-draw state is dirty-check cached with `0xffff/0xff` sentinel invalidation; caches are
  caller-side (Dx9_* layer), not inside the device wrapper.

## Code patterns to expect when reading the decompilation

- Custom register-arg conventions: `this` in ESI/ECX (ctors leak as `unaff_*`), packed EAX pairs
  (`in_EAX = {startReg, count}` in the Dx9 constant helpers), `unaff_EDI` record pointers.
- 10-byte thunk chains (jmp wrapper → SecuROM call gate → real body — see `securom_vm.md`);
  pool allocators `FUN_0084ae70(size, n)` / `FUN_0084d9d0`.
- Global-ctor-built `.bss` statics: instance memory is zero in the file image (no static vtables) —
  find ctors by xref to the `.bss` address.
- Watch int* pointer arithmetic in reads: `*(ushort *)(p + 0x10)` on `int *p` is +0x40 bytes, not
  +0x10.

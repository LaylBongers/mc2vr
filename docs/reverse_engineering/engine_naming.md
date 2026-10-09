# Engine naming layers & name hashing

## Naming layers

- `Pg*` = engine/render core (PgPrimitive, PgMaterial, Pg*.sho shaders). `Lti*` = platform layer (LtiRenderer, Lti_DebugPrintf, Dx9_* helpers). Source roots in debug strings: `D:\projects\Mercs2_PC\LTI\Src\...`, `D:\projects\Mercs2_PC\Odin\Win32\...` ("Odin" = this game's project).
- No MSVC RTTI in this binary (only Havok classes have it) — derive class names from ctor vtable writes + strings; annotate vtables pre-emptively ([vtables.md](vtables.md)).

## Name hashing (two distinct mechanisms — don't confuse them)

- `Lti_LazyNameHash` (`0x008244a0`, ~139 callers): per-site FNV-1a hash of `"Class::Method"` cached in `.bss`, read only by VM'd code — inert telemetry, NOT feature/device checks.
- Inline FNV-1a for **object names** (e.g. `PgMaterial::nameHash64`): basis `0x811c9dc5`, prime `0x1000193`, case-insensitive (`c | 0x20`), NUL terminator hashed as `0x2a`.

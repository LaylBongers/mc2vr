# Shader containers and shader names

Static read (2026-11-13). Per-address detail is in the Ghidra plates of `PgShaderContainer_Load`, `PgShaderContainer_FindByFile` and `PgShader_Load`.

## Format

`data/shader3.bin` (and `shader3Low.bin`, `shaderVT.bin`, `shaderR2VB.bin`):
- a u32 count, then that many `{u32 key, u32 offset, u32 size, u32 type}` entries, type 1 = VS and 0 = PS;
- then the raw D3D9 bytecode. The offset points at the version token.

`shader3.bin` holds 556 entries, sorted by upper-cased file name (`_` sorts after letters). The bytecode has CTAB constant tables but no shader names.

## Names and keys

- **Registration:** `Pg_RegisterShaders` (`0x0084f3a0`) calls `PgShader_Register(name, "<file>.sho", variant)` for every shader. `FUN_00484450` does the same for water. A few calls come from SecuROM-relocated code: decals, Scaleform, billboard trees.
- **Key:** `PgShader_Load` (vtable slot 2) finds a shader's bytecode with `PgShaderContainer_FindByFile` (`0x0085b960`):
  - It turns the `.sho` file name into `<file>_3.sho`, or `<file>_3l.sho` when `g_ScreenEffectsEnabled == 0`, which selects `shader3Low.bin`.
  - The key is the fold-case FNV-1a of that string (`Lti_LazyNameHash`): seed `0x811c9dc5`; per character `h = (h ^ (signed char)c | 0x20) * 0x01000193`; then `h = (h ^ 0x2a) * 0x01000193`.
- **Loading:** `PgShaderContainer_Load` (`0x0085b660`) creates every shader of a container up front and stores it under its key.
- **Light variants:** the file suffixes are `_pl` (point lights), `_sl` (spot lights) and `_pl_sl`, each with an `_li` version. An `_li` version is registered under the same name as its base (`PgFastFP_pl_li.sho` under `PgFastFP_pl`). [../shaders.md](../shaders.md) describes how the plain and `_li` versions count their lights.

## The name map

`data/shader3_names.csv`: offset, key, type, size, registered name, file, variant for all 556 entries of `shader3.bin`.
- **Named:** 540 have a file name. 522 come straight from `.sho` strings in the exe; 18 were brute-forced from name fragments and the alphabetical gaps. The hash folds case, so the capitalisation of those 18 is a guess.
- **Weak match:** `0x45f0` `PgBillboardTreeShadowVP_modeled`.
- **Unnamed (16):**
  - `0x8c20`, `0x1e9e80`, `0x1fd310`, `0x1fd3f0`, `0x1fdad0`, `0x211230`, `0x24fd90`;
  - nine `PgMeshVP*` vertex shaders at `0x1f3bf0`–`0x1f82a0`.
- **Registered name:** 505 entries have one.
- **The variant column is unreliable** where it was a register push: pointer-sized values, and `1?`/`3?` on the decal rows.
- **Cross-check:** solving the seed over every `<file>_3.sho` string gives `0x811c9dc5` for 515 entries, against 56 for the next most common value. The other containers match too: `shader3Low.bin` 372/411 with `_3l.sho`, `shaderVT.bin` 13/15, `shaderR2VB.bin` 13/13.

`tools/shader_hash.py` prints the original name next to each `shader3.bin` hash it finds.

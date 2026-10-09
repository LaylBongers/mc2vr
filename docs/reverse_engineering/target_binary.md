# Target binary

`Mercenaries2.exe` — PE32, i386 (32-bit), ~52 MB, 12 sections, image base `0x00400000`. Mercenaries 2: World in Flames (Pandemic Studios / EA, 2008), built on Pandemic's "G" engine. No PDB available.

## Sections

| Section | VA range | Contents |
|---|---|---|
| `.text` | `0x00401000`–`0x00b04fff` (7.3 MB) | Game code — plaintext, symbolized |
| `.rdata` | `0x00b05000`–`0x00bf4fff` | Game read-only data |
| `.data` | `0x00bf5000`–`0x019f8fff` (14.7 MB) | Game writable data |
| `extdata`, `.tls`, `.rsrc` | `0x019f9000`–`0x01a47fff` | Game data |
| `Stext`, `Sitext`, `Srdata`, `Sdata`, `Sidata` | `0x01a48000`–`0x02463fff` | SecuROM VM — not game code |
| `.securom` | `0x02464000`–`0x03771f0f` (20 MB) | SecuROM wrapper — not game code |

The `S*`/`.securom` regions are VM territory — never analyze or hook there ([securom_vm.md](securom_vm.md)); on-disk state and the loader stub: [securom_on_disk.md](securom_on_disk.md).

## PE header (launcher/patching-relevant)

- `DllCharacteristics = 0` — no `DYNAMIC_BASE` (ASLR), no `NX_COMPAT`, no CFG.
- `RELOCS_STRIPPED` + empty base-reloc directory — image cannot relocate; loads at `0x00400000` every run.
- Consequence: every static VA from Ghidra is an absolute runtime address; a launcher can inline-patch at literal VAs with no module-base resolution. Image is 51.4 MB, so rel32 (±2 GB) trampolines reach any hook site from anywhere near the image.
- `CheckSum` present (`0x00a4f080`) — Wine/Proton ignores it.
- Entry VA `0x03770b96` (SecuROM loader stub). Patch `.text` only, never the `S*`/`.securom` regions or the `thunk_FUN_02xxxxxx` jump targets — hook the plaintext callers instead.

## Symbols

- Rich embedded MSVC mangled symbols (G-engine classes: `GMatrix2D`, `GImage`, `GZLibFile`, `GColor`, `GRefCountBaseImpl`, ...).
- 29,143 functions total; 3,762 named (symbol-derived). 26,472 defined strings.

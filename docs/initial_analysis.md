# Initial Analysis — Mercenaries2.exe

## Target

- `Mercenaries2.exe` — PE32, i386 (32-bit), ~52 MB, 12 sections, image base `0x00400000`.
- Mercenaries 2: World in Flames (Pandemic Studios / EA, 2008), built on Pandemic's "G" engine.

## Sections

| Section | VA range | Contents |
|---|---|---|
| `.text` | `0x00401000`–`0x00b04fff` (7.3 MB) | Game code — plaintext, symbolized |
| `.rdata` | `0x00b05000`–`0x00bf4fff` | Game read-only data |
| `.data` | `0x00bf5000`–`0x019f8fff` (14.7 MB) | Game writable data |
| `extdata`, `.tls`, `.rsrc` | `0x019f9000`–`0x01a47fff` | Game data |
| `Stext`, `Sitext`, `Srdata`, `Sdata`, `Sidata` | `0x01a48000`–`0x02463fff` | SecuROM VM — not game code |
| `.securom` | `0x02464000`–`0x03771f0f` (20 MB) | SecuROM wrapper — not game code |

## SecuROM v7

Bypassed and inert. Details:

- The PE entry point (`0x03770b96`, in `.securom`) is the SecuROM v7 loader stub: it resolves `sprintf` from `msvcrt.dll`, builds a `v7_%04d` event name from `GetCurrentProcessId() ^ 0x19ea3fd3`, spawns the SecuROM VM thread (entry `0x01ad5170`, in `Stext`), waits on a flag, then jumps through the pointer at `0x03770b1f`.
- That pointer holds the real entry point: `0x009ee80a` in `.text`.
- Game code (`.text`/`.rdata`/`.data`) is plaintext on disk — no unpacking required.
- Only remaining risk: potential anti-tamper/anti-debug during live debugging or runtime patching. If so, attach after the stub completes or break at `0x009ee80a`.

## Symbols

- Rich embedded MSVC mangled symbols (G-engine classes: `GMatrix2D`, `GImage`, `GZLibFile`, `GColor`, `GRefCountBaseImpl`, ...).
- 29,143 functions total; 3,762 named (symbol-derived). 26,472 defined strings.
- No PDB available.

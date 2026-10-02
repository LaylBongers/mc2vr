# Initial Analysis

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

## PE header (launcher/patching-relevant)

- `DllCharacteristics = 0` — no `DYNAMIC_BASE` (ASLR), no `NX_COMPAT`, no CFG.
- `RELOCS_STRIPPED` + empty base-reloc directory — image cannot relocate; loads at `0x00400000` every run.
- Consequence: every static VA from Ghidra is an absolute runtime address; a launcher can inline-patch at literal VAs with no module-base resolution. Image is 51.4 MB, so rel32 (±2 GB) trampolines reach any hook site from anywhere near the image.
- `CheckSum` present (`0x00a4f080`) — Wine/Proton ignores it.
- Entry VA `0x03770b96` (SecuROM stub, see below). Patch `.text` only, never the `S*`/`.securom` regions or the `thunk_FUN_02xxxxxx` jump targets — hook the plaintext callers instead.

## SecuROM v7

Bypassed and inert. Details:

- The PE entry point (`0x03770b96`, in `.securom`) is the SecuROM v7 loader stub: it resolves `sprintf` from `msvcrt.dll`, builds a `v7_%04d` event name from `GetCurrentProcessId() ^ 0x19ea3fd3`, spawns the SecuROM VM thread (entry `0x01ad5170`, in `Stext`), waits on a flag, then jumps through the pointer at `0x03770b1f`.
- That pointer holds the real entry point: `0x009ee80a` in `.text`.
- Game code (`.text`/`.rdata`/`.data`) is plaintext on disk — no unpacking required.
- Only remaining risk: potential anti-tamper/anti-debug during live debugging or runtime patching. If so, attach after the stub completes or break at `0x009ee80a`.

### On-disk state (verified by byte dump + entropy, 2026-10-02)

- Nothing is encrypted on disk — stub, `Stext` VM interpreter, `.securom` function tables are all plaintext. There is no runtime decryption gate; the game runs directly from mapped plaintext sections.
- Real game functionality *is* virtualized inside `.securom` as plaintext VM stubs (`push marker; call handler` chains — e.g. `GetD3DDevice`'s body at `0x02562cd0`, reached via the plaintext `.text` thunk at `0x0047f2f0`). "Protection VM, not game code" is therefore a simplification: treat the sections as VM territory (don't analyze or hook the stubs — their marker chains are VM-addressed, which is also why Ghidra cannot recover the `0x02xxxxxx` jumptables), and reach the functionality through the `.text` thunks instead.
- The OEP pointer at `0x03770b1f` is static in the file (`0x009ee80a`).
- Consequence for patching: skipping the stub would save nothing (no decryption to trigger) and would break its init — VM-thread spawn + readiness flag + the runtime pointer resolution that `WinMain` itself depends on (documented in `main_game_loop.md`). Let the stub run; use a post-boot handshake (per-frame counter poll) before installing hooks.

### Tamper-check capability (static scan, 2026-10-02)

- Anti-tamper/anti-debug machinery is present in the SecuROM regions: name strings for `IsDebuggerPresent`, `NtQuerySystemInformation`, `ReadProcessMemory`, `FindWindow`, `CreateToolhelp32Snapshot`; CRC32 tables (reflected poly `0xEDB88320`) in `Sdata`/`Sidata` around `0x02455560`. `.securom`'s first 4 KB is high-entropy (VM payload).
- Capability ≠ activity: whether/when a check runs is VM bytecode — not cheaply decidable statically, and not worth reversing. The game booting only proves the licensing gate passed; do NOT assume the tamper layer is neutered.
- Startup-phase checks are irrelevant to us by design (hooks go in after the boot poll). The only open question — post-boot re-verification — is discharged empirically by the carrier's first hook test (`launcher_plan.md` M1): plain in-process memory writes, no debugger.
- Never attach a debugger to the live game (anti-debug APIs would confound results and may kill the process). All runtime experiments go through the carrier.

## Symbols

- Rich embedded MSVC mangled symbols (G-engine classes: `GMatrix2D`, `GImage`, `GZLibFile`, `GColor`, `GRefCountBaseImpl`, ...).
- 29,143 functions total; 3,762 named (symbol-derived). 26,472 defined strings.
- No PDB available.

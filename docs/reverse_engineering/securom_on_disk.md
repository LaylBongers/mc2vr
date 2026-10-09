# SecuROM on-disk state (v7)

Bypassed and inert (no license/anti-tamper/anti-debug response — proven in live runs; full probe list in [../hooks.md](../hooks.md)). How to classify code near the VM: [securom_vm.md](securom_vm.md). PE layout: [target_binary.md](target_binary.md).

## Loader stub

- The PE entry point (`0x03770b96`, in `.securom`) is the SecuROM v7 loader stub: it resolves `sprintf` from `msvcrt.dll`, builds a `v7_%04d` event name from `GetCurrentProcessId() ^ 0x19ea3fd3`, spawns the SecuROM VM thread (entry `0x01ad5170`, in `Stext`), waits on a flag, then jumps through the pointer at `0x03770b1f`.
- That pointer holds the real entry point: `0x009ee80a` in `.text` (static in the file).
- Game code (`.text`/`.rdata`/`.data`) is plaintext on disk — no unpacking required.

## On-disk state (verified by byte dump + entropy, 2026-10-02)

- Nothing is encrypted on disk — stub, `Stext` VM interpreter, `.securom` function tables are all plaintext. There is no runtime decryption gate; the game runs directly from mapped plaintext sections.
- Real game functionality *is* virtualized inside `.securom` as plaintext VM stubs (`push marker; call handler` chains — e.g. the file-image target of the `GetD3DDevice` thunk, `0x02562cd0`, reached via the plaintext `.text` thunk at `0x0047f2f0`). **Correction (2026-10-03):** that thunk's slot is patched at runtime to the native `.text` function `0x00403160`; ~17% of such thunks are ([vm_thunk_patches.md](vm_thunk_patches.md)), so the file-image target is only the default. "Protection VM, not game code" is a simplification: treat the sections as VM territory (don't analyze or hook the stubs — their marker chains are VM-addressed, which is also why Ghidra cannot recover the `0x02xxxxxx` jumptables), and reach the functionality through the `.text` thunks instead.

## Patching consequences

- Skipping the stub would save nothing (no decryption to trigger) and would break its init — VM-thread spawn + readiness flag + the runtime pointer resolution that `WinMain` itself depends on ([main_game_loop.md](main_game_loop.md)). Let the stub run.
- Plaintext `.text` hooks do NOT need to wait for it (early attach proven 2026-10-04: hooks installed before the game's first instruction, no reaction); only CALLING VM-slot thunks (e.g. `GetD3DDevice`) needs the engine to be up, because the loader patches those slots at runtime — the carrier waits for the device (`g_LtiRenderer->dx9State` != NULL) instead of calling the thunk to find out.

## Tamper-check capability (static scan, 2026-10-02)

- Anti-tamper/anti-debug machinery is present in the SecuROM regions: name strings for `IsDebuggerPresent`, `NtQuerySystemInformation`, `ReadProcessMemory`, `FindWindow`, `CreateToolhelp32Snapshot`; CRC32 tables (reflected poly `0xEDB88320`) in `Sdata`/`Sidata` around `0x02455560`. `.securom`'s first 4 KB is high-entropy (VM payload).
- Capability ≠ activity, and the question is settled empirically: post-boot re-verification is **DISCHARGED (live-verified, 2026-10-02)** — multi-minute live runs with inline `.text` patches, in-process `.data` write/restores, and direct VM-stub calls produced zero reaction; the enforcement layer is inert ([../hooks.md](../hooks.md)).
- Residual risk is limited to live debugging — only if live debugging/hooking misbehaves, suspect leftover anti-tamper in the SecuROM regions.

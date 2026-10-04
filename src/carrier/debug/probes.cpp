#include "probes.hpp"

#include <windows.h>

#include <cstdint>

#include "game_addresses.h"
#include "log.hpp"

namespace mc2vr::probes {

bool data_write_restore()
{
    // Target: a per-frame diagnostic counter in .data. It ticks ~60/s on the
    // main thread, so a tick can land between our flip and restore; that
    // fails the strict readback check but is harmless (logged, never
    // re-"fixed" — see probes.hpp). Retry a few times for a clean pass.
    volatile uint8_t *p = (volatile uint8_t *)MC2_PROBE_DATA_BYTE;

    for (int attempt = 1; attempt <= 3; attempt++) {
        uint8_t original = *p;
        *p = (uint8_t)~original;
        uint8_t after_write = *p;
        *p = original;
        uint8_t after_restore = *p;

        bool write_ok = (after_write == (uint8_t)~original);
        bool restore_ok = (after_restore == original);

        MC2VR_LOG("probe (a) .data write+restore @ %p [try %d]: "
                  "orig=%02x wrote=%02x readback=%02x (%s) restored=%02x (%s)",
                  (void *)p, attempt, original, (uint8_t)~original, after_write,
                  write_ok ? "ok" : "MISMATCH", after_restore,
                  restore_ok ? "ok" : "MISMATCH");

        if (write_ok && restore_ok) {
            return true;
        }
    }

    MC2VR_LOG("probe (a) FAILED after 3 tries — carrier .data writes are "
              "being reverted or the address assumption is wrong");
    return false;
}

bool call_vm_thunk()
{
    // The thunk is 6 bytes of plaintext .text that jmp into the SecuROM VM
    // stub. void* (void): zero args means cdecl/stdcall are identical on
    // i386, so a plain call is the "correct convention" by construction.
    // Signature verified in Ghidra: isThunk=true, callers use the return
    // value directly as the IDirect3DDevice9*.
    using GetD3DDevice_t = void *(*)();

    void *device = ((GetD3DDevice_t)MC2_GETD3DDEVICE_THUNK)();

    // NOTE: reaching this line at all is the pass — the VM stub executed from
    // injected code and returned cleanly. If it ever crashes instead, the
    // absence of this log line (and a dead game) is the failure signal.
    MC2VR_LOG("probe (b) direct VM-stub call GetD3DDevice() @ %p -> %p (%s)",
              (void *)MC2_GETD3DDEVICE_THUNK, device,
              device ? "device exists" : "NULL — expected, D3D init is a few frames later");
    return true;
}

} // namespace mc2vr::probes

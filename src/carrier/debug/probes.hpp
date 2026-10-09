// SecuROM pre-probes (docs/plans/launcher.md, M1): prove the two things the
// motion-control/logic-mod track depends on, using the carrier itself (the
// only sanctioned probe — no debugger, no ptrace):
//   (a) the carrier can write+restore game .data in-process — logic modding
//       writes game structures constantly, so this must be routine;
//   (b) mod code may CALL a SecuROM-virtualized function (VM-stub thunk) —
//       the counterpart of "never HOOK a VM stub".
// Both run on the carrier init thread before any hook is installed, so any
// crash is attributable to the probe itself, not to hook machinery.
#pragma once

namespace mc2vr::probes {

// Write-flip-readback-restore a single .data byte. Returns true if one fully
// clean pass was observed (retrying a couple of times for counter races).
bool data_write_restore();

// Directly call the GetD3DDevice VM-stub thunk. A clean return is the pass
// condition; the returned pointer is informational (init.cpp runs this after the device wait, so NULL is unexpected).
bool call_vm_thunk();

} // namespace mc2vr::probes

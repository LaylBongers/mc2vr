// Live read of the SecuROM VM chain behind the RenderTask_RenderFrame
// follow-up (thunk 0x0046ab80 -> slot 0x024dc4ec -> stub 0x025628e0 ->
// handler 0x01b950a0 -> slot 0x02299230 -> VM entry). Read-only: the carrier
// shares the process address space, so it copies the runtime bytes, diffs
// them against the on-disk image, and re-reads once later to catch runtime
// patching. Nothing in the VM is hooked or written.
// Controlled by mc2vr.conf debug_vm_dump=on|off (default off).
#pragma once

namespace mc2vr::vmdump {

// Parse the conf value; returns false on unrecognized input.
bool set_enabled(const char *value);

// Start the dump thread (no-op when disabled).
void install();

} // namespace mc2vr::vmdump

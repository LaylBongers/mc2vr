// Carrier attach: log, verify the build lock, then install every hook while
// the game is suspended. Installs that need a running engine hang off engine
// events instead of a polling thread (docs/carrier.md, docs/hooks.md).
#pragma once

namespace mc2vr {

void init();

// First Present (main thread; device.cpp): the engine is past boot. Runs the
// installs that need constructed engine objects or a patched SecuROM loader.
void on_engine_running();

} // namespace mc2vr

// Shadow verification of reimplementations (debug_reimpl_shadow=on;
// docs/reimpl.md). The originals keep running; each reimplementation is
// dry-run alongside and compared (harness.hpp). One spec per function
// (render_cmd.cpp, ...), all installed by install().
#pragma once

namespace mc2vr::shadow {

void install();

} // namespace mc2vr::shadow

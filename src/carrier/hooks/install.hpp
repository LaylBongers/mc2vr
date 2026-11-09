// Every attachment of the VR implementation to the game, in install order.
// Called once from early init (init.cpp), before the game's first
// instruction runs. Debug tools install their own hooks separately.
#pragma once

namespace mc2vr::hooks {

void install_attach();

} // namespace mc2vr::hooks

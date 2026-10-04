#!/usr/bin/env bash

# Launch Mercenaries 2 under Steam Proton via the mc2vr launcher.
# Settings live in launch.conf.
#
# The launcher + carrier are built into build/win32/bin/ (see README.md) and
# deployed to <GAME_DIR>/mc2vr/ before starting; the launcher then starts the
# game, waits for boot-complete, and injects the carrier.

set -e
source "$(dirname "$0")/launch.conf"

MC2VR_ROOT="$(cd "$(dirname "$0")" && pwd)"
PROTON_PATH="$STEAM_HOME/steamapps/common/$PROTON_NAME"

export STEAM_COMPAT_DATA_PATH="$COMPAT_DATA_PATH"
export STEAM_COMPAT_CLIENT_INSTALL_PATH="$STEAM_HOME"

LAUNCHER_BIN="$MC2VR_ROOT/build/win32/bin/mc2vr_launcher.exe"
CARRIER_BIN="$MC2VR_ROOT/build/win32/bin/mc2vr_carrier.dll"

if [ ! -f "$LAUNCHER_BIN" ] || [ ! -f "$CARRIER_BIN" ]; then
    echo "mc2vr is not built yet (or out of date). Building now:"
    cmake -B "$MC2VR_ROOT/build/win32" -S "$MC2VR_ROOT" \
        -DCMAKE_TOOLCHAIN_FILE="$MC2VR_ROOT/cmake/i686-w64-mingw32.cmake" >&2
    cmake --build "$MC2VR_ROOT/build/win32" >&2
fi

DEPLOY_DIR="$GAME_DIR/mc2vr"
mkdir -p "$DEPLOY_DIR"
cp -u "$LAUNCHER_BIN" "$CARRIER_BIN" "$DEPLOY_DIR/"
# Config: deploy only if absent — edits to the deployed mc2vr.conf persist
# across launches (view_row_rewrite=off|on|pulse; see conf/mc2vr.conf).
cp -n "$MC2VR_ROOT/conf/mc2vr.conf" "$DEPLOY_DIR/mc2vr.conf" 2>/dev/null || true

# S4-1 (docs/s4_handover.md): the VR registry values the carrier needs are
# written BY THE CARRIER at attach time (openvr_bridge.cpp
# ensure_vr_registry) — deliberately NOT here: the same HKCU values arm
# DXVK's d3d9 boot-time OpenVR interop, and writing them pre-game hung the
# game at boot with SteamVR mid-transition (run 6a, live-proven). Game boots
# must find the registry WITHOUT the VR key; the carrier adds it after the
# device already exists.

cd "$GAME_DIR"
exec "$PROTON_PATH/proton" run "$DEPLOY_DIR/mc2vr_launcher.exe" "$@"

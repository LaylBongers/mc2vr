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

mkdir -p "$COMPAT_DATA_PATH" # Proton requires the dir; it creates pfx/ inside on first run
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
# The OpenXR host is gone (OpenXR runs in the game process): drop a stale copy.
rm -f "$DEPLOY_DIR/mc2vr_host.exe" "$DEPLOY_DIR/mc2vr_host.log"
# Config: deploy only if absent — edits to the deployed mc2vr.conf persist
# across launches (view_row_rewrite=off|on|pulse; see conf/mc2vr.conf).
cp -n "$MC2VR_ROOT/conf/mc2vr.conf" "$DEPLOY_DIR/mc2vr.conf" 2>/dev/null || true

# MC2VR_VK_VALIDATION=1: the Khronos validation layer (with synchronization
# validation) in the game's Vulkan stack, logged to <deploy dir>/mc2vr_vk_validation.log.
# Needs the 32-bit layer (lib32-vulkan-validation-layers): the game's Vulkan
# calls go through Proton's i386 Unix side. Slow; a short run is enough.
if [ -n "$MC2VR_VK_VALIDATION" ]; then
    export VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
    export VK_LAYER_VALIDATE_SYNC=true
    export VK_LAYER_REPORT_FLAGS=error,warn,perf
    export VK_LAYER_DEBUG_ACTION=VK_DBG_LAYER_ACTION_LOG_MSG
    export VK_LAYER_LOG_FILENAME="$DEPLOY_DIR/mc2vr_vk_validation.log"
    # No per-message cap: DXVK's own repeated messages (16x16 internal copies,
    # 2026-11-10) would otherwise use up the cap of an ID ours could share.
    export VK_LAYER_ENABLE_MESSAGE_LIMIT=false
    rm -f "$VK_LAYER_LOG_FILENAME"
    echo "Vulkan validation on: $VK_LAYER_LOG_FILENAME"
fi

cd "$GAME_DIR"
exec "$PROTON_PATH/proton" run "$DEPLOY_DIR/mc2vr_launcher.exe" "$@"

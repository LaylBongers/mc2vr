#!/usr/bin/env bash

# Launch Mercenaries 2 under Steam Proton
# Settings live in launch.conf

set -e
source "$(dirname "$0")/launch.conf"

PROTON_PATH="$STEAM_HOME/steamapps/common/$PROTON_NAME"

export STEAM_COMPAT_DATA_PATH="$COMPAT_DATA_PATH"
export STEAM_COMPAT_CLIENT_INSTALL_PATH="$STEAM_HOME"

cd "$GAME_DIR"
exec "$PROTON_PATH/proton" run "$GAME_EXE" "$@"

#!/usr/bin/env bash

# Launch Mercenaries 2 under Steam Proton without mc2vr: the game exe
# directly, no launcher or carrier. Same prefix and Proton as launch.sh.
# Settings live in launch.conf.

set -e
source "$(dirname "$0")/launch.conf"

PROTON_PATH="$STEAM_HOME/steamapps/common/$PROTON_NAME"

mkdir -p "$COMPAT_DATA_PATH" # Proton requires the dir; it creates pfx/ inside on first run
export STEAM_COMPAT_DATA_PATH="$COMPAT_DATA_PATH"
export STEAM_COMPAT_CLIENT_INSTALL_PATH="$STEAM_HOME"

cd "$GAME_DIR"
exec "$PROTON_PATH/proton" run "$GAME_DIR/$GAME_EXE" "$@"

#!/usr/bin/env bash
# S4-2c probe A: run the shared-handle handoff (writer32 + reader64) under
# the CLEAN prefix, both via `proton run` so they share the wineserver.
#
# Prereqs: built binaries (build.sh), launch.conf-sourced env.
# No SteamVR needed — this probes DXVK shared handles only.
set -e
cd "$(dirname "$0")"
MC2VR_ROOT="$(cd ../.. && pwd)"
source "$MC2VR_ROOT/launch.conf"

rm -f share_handle.txt share_ok.txt writer32.log reader64.log

export STEAM_COMPAT_DATA_PATH="$COMPAT_DATA_PATH"
export STEAM_COMPAT_CLIENT_INSTALL_PATH="$STEAM_HOME"
PROTON="$STEAM_HOME/steamapps/common/$PROTON_NAME/proton"

echo "[probe] starting writer32 (background) ..."
"$PROTON" run ./writer32.exe &
WRITER_PID=$!

echo "[probe] starting reader64 ..."
"$PROTON" run ./reader64.exe || true

wait $WRITER_PID || true

echo "=== writer32.log ==="
cat writer32.log
echo "=== reader64.log ==="
cat reader64.log
echo "=== verdict ==="
cat share_ok.txt 2>/dev/null || echo "(no verdict written)"

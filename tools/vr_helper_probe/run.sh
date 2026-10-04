#!/usr/bin/env bash
# S4-2c probe B: build + run the 64-bit helper core (D3D11 + OpenVR test
# pattern in the HMD). SteamVR must be running; the HMD wakes on connect.
set -e
cd "$(dirname "$0")"
x86_64-w64-mingw32-g++ -static -mconsole -O2 -o helper64.exe helper64.cpp \
    -ld3d11 -ladvapi32
echo "built helper64.exe"

MC2VR_ROOT="$(cd ../.. && pwd)"
source "$MC2VR_ROOT/launch.conf"
export STEAM_COMPAT_DATA_PATH="$COMPAT_DATA_PATH"
export STEAM_COMPAT_CLIENT_INSTALL_PATH="$STEAM_HOME"
rm -f helper64.log
"$STEAM_HOME/steamapps/common/$PROTON_NAME/proton" run ./helper64.exe
RC=$?
echo "=== helper64.log ==="
cat helper64.log
exit $RC

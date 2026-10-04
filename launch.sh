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

# ---- S4-2 option (c): SteamVR stability gate (runs 6a/9, live-proven) -----------
# With openvr_boot_interop=on the launcher arms DXVK's boot-time OpenVR
# interop, and DXVK connects to SteamVR during the game's device creation
# (~0.5 s into boot). If SteamVR is mid-transition (HMD standby wake/sleep)
# at that instant, the game process dies in early boot before the main loop
# (boot marker never ticks, rc=5) — observed in run 6a AND run 9 (vrserver
# log shows the launch window inside a wake transition both times). Human
# timing can't reliably avoid it, so require: HMD AWAKE (last standby line
# is "leaving standby") and NO standby transition in the last 8 s. Wait up
# to 120 s (wake the HMD / put it on), then abort rather than launch into
# the known-crash state. Without openvr_boot_interop, boots stay
# SteamVR-independent (run 6b) and are NOT gated.
VRSERVER_LOG="$STEAM_HOME/logs/vrserver.txt"

vr_last_standby_line() {
    [ -f "$VRSERVER_LOG" ] || return 1
    grep -E "(entering|leaving) standby" "$VRSERVER_LOG" | tail -1
}

vr_standby_age_s() {
    # Seconds since the last standby transition; huge if the log has none
    # (freshly started SteamVR also counts as quiet — no transitions yet).
    local line ts then
    line=$(vr_last_standby_line) || return 1
    if [ -z "$line" ]; then echo 99999; return 0; fi
    ts=$(printf '%s\n' "$line" | awk '{print $5}' | cut -d. -f1)
    then=$(date -d "today $ts" +%s 2>/dev/null) || { echo 99999; return 0; }
    echo $(( $(date +%s) - then ))
}

if grep -Eq '^[[:space:]]*openvr_boot_interop=on' "$DEPLOY_DIR/mc2vr.conf" 2>/dev/null; then
    echo "openvr_boot_interop=on — requiring SteamVR stable (HMD awake, no standby transition for 8s)"
    gate_ok=0
    for _ in $(seq 1 60); do
        last=$(vr_last_standby_line)
        if [ -z "$last" ] && [ ! -f "$VRSERVER_LOG" ]; then
            echo "  SteamVR is not running (no vrserver log). Start SteamVR and wake the HMD ..."
        elif [ -z "$last" ]; then
            echo "  SteamVR up, no standby transitions yet — stable. Proceeding."
            gate_ok=1; break
        elif printf '%s' "$last" | grep -q "leaving standby"; then
            age=$(vr_standby_age_s)
            if [ "$age" -ge 8 ] 2>/dev/null; then
                echo "  HMD awake and stable for ${age}s. Proceeding."
                gate_ok=1; break
            fi
            echo "  HMD waking (transition ${age}s ago) — settling ..."
        else
            echo "  HMD in STANDBY — put the HMD on / move it to wake it ..."
        fi
        sleep 2
    done
    if [ "$gate_ok" -ne 1 ]; then
        echo "fatal: SteamVR never reached a stable awake state within 120s —"
        echo "       launching now would crash the game in early boot (runs 6a/9)."
        echo "       Wake the HMD (put it on) and re-run ./launch.sh."
        exit 1
    fi
fi

# S4-1 (docs/s4_handover.md): the VR registry values the carrier needs are
# written BY THE CARRIER at attach time (openvr_bridge.cpp
# ensure_vr_registry) — when openvr_boot_interop=on the LAUNCHER additionally
# arms them BEFORE the game starts (see src/launcher/main.c; DXVK reads them
# at device creation, gated by the stability check above).

cd "$GAME_DIR"
exec "$PROTON_PATH/proton" run "$DEPLOY_DIR/mc2vr_launcher.exe" "$@"

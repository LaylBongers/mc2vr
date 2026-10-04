#!/usr/bin/env bash
# S4-2 probe driver: does a DXVK D3D9 shared texture (legacy pSharedHandle)
# open in DXVK D3D11 (OpenSharedResource) in ANOTHER process of the same
# prefix? Runs the win32 producer and the win64 consumer CONCURRENTLY under
# the game's Proton (launch.conf), through the full matrix:
#   device: D3D9Ex vs plain D3D9  x  fmt: A8R8G8B8(21) vs X8R8G8B8(22)
#
# Verdict per case: consumer exit 0 + "consumer: PASS" in its log (pattern
# exact + live redraw observed cross-process). The producer writes its
# evidence to the workdir even if nothing works ("proton run" eats stdout).
#
# Needs the unsandboxed terminal only because wineserver uses Unix sockets
# (same as tools/selftest/run.sh). Does NOT touch the game dir.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/launch.conf"
PROTON="$STEAM_HOME/steamapps/common/$PROTON_NAME/proton"
export STEAM_COMPAT_DATA_PATH="$COMPAT_DATA_PATH"
export STEAM_COMPAT_CLIENT_INSTALL_PATH="$STEAM_HOME"
P32="$ROOT/build/win32/bin/mc2vr_probe_shared_producer.exe"
P64="$ROOT/build/win64/bin/mc2vr_probe_shared_consumer.exe"

echo "building..."
cmake --build "$ROOT/build/win32" --target mc2vr_probe_shared_producer >&2
cmake --build "$ROOT/build/win64" --target mc2vr_probe_shared_consumer >&2

mkdir -p /tmp/opencode
WORK="$(mktemp -d /tmp/opencode/mc2vr-shprobe.XXXXXX)"
WORK_TAG="$(basename "$WORK")"  # unique; safe pkill pattern

cleanup() {
    pkill -f "$WORK_TAG" 2>/dev/null || true
    rm -rf "$WORK"
}
trap cleanup EXIT

OVERALL=0
for EX in 1 0; do
    for FMT in 21 22; do
        echo
        echo "== case ex=$EX fmt=$FMT =="
        rm -f "$WORK"/handle.txt "$WORK"/mc2vr_probe_done.txt "$WORK"/*.log
        ( cd "$WORK" && WINEDEBUG=-all timeout 150 "$PROTON" run "$P32" \
            --ex "$EX" --fmt "$FMT" --seconds 90 > producer_stdout.log 2>&1 ) &
        PSHELL=$!

        H=""
        for _ in $(seq 1 60); do   # up to 30s for the device + shared texture
            [ -f "$WORK/handle.txt" ] && { H="$(cat "$WORK/handle.txt")"; break; }
            sleep 0.5
        done
        if [ -z "$H" ]; then
            echo "case FAIL: producer never published a handle (see logs)"
            kill "$PSHELL" 2>/dev/null; wait "$PSHELL" 2>/dev/null
            sed -n '1,15p' "$WORK/mc2vr_probe_producer.log" 2>/dev/null
            OVERALL=1
            continue
        fi

        read -r FMTGOT HANDLE W H <<< "$H"
        if [ "$FMTGOT" = "ERR" ]; then
            echo "case FAIL: CreateTexture(shared) refused by the runtime ($HANDLE)"
            kill "$PSHELL" 2>/dev/null; wait "$PSHELL" 2>/dev/null
            OVERALL=1
            continue
        fi
        echo "handle: $H"

        ( cd "$WORK" && WINEDEBUG=-all timeout 120 "$PROTON" run "$P64" \
            --handle "$HANDLE" --w "$W" --h "$H" > consumer_stdout.log 2>&1 )
        RC=$?
        wait "$PSHELL" 2>/dev/null

        sed -n '1,20p' "$WORK/mc2vr_probe_producer.log" 2>/dev/null
        sed -n '1,20p' "$WORK/mc2vr_probe_consumer.log" 2>/dev/null

        if [ "$RC" -eq 0 ] && grep -q "consumer: PASS" "$WORK/mc2vr_probe_consumer.log" 2>/dev/null; then
            echo "case PASS (ex=$EX fmt=$FMT)"
        else
            echo "case FAIL (ex=$EX fmt=$FMT consumer rc=$RC)"
            OVERALL=1
        fi
    done
done

echo
if [ "$OVERALL" -eq 0 ]; then
    echo "PROBE VERDICT: shared handles work in every combination tested"
    exit 0
fi
echo "PROBE VERDICT: at least one combination FAILED — see per-case lines above"
exit 1

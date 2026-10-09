#!/usr/bin/env bash
# End-to-end self-test of the launcher -> host -> carrier chain, using the
# sleeper stand-in instead of the real game (see sleeper.c for what it mimics).
#
# Verifies, under plain Wine (no Steam/Proton needed):
#   PHASE A (launcher chain, as before):
#     - launcher path resolution, process start, module-base check
#     - CreateRemoteThread + LoadLibraryW injection works
#     - carrier logs "attached"
#     - carrier build lock correctly REFUSES to hook (sleeper != real exe)
#     - S4-1: launcher spawns the host when deployed (under plain Wine the
#       host's OpenXR setup fails fast -> "host exited early" is expected and
#       must NOT block the game), or logs the skip line when not deployed.
#   PHASE B (S4-1 IPC round trip):
#     - mc2vr_host --mock creates the section; the win32 probe (stand-in
#       carrier) validates header/version, registers, reads seqlocked poses,
#       drains session-state events up to FOCUSED, sends Shutdown and checks
#       the host exits cleanly. Validates the cross-bitness win64<->win32 path.
#
# The EXIT trap kills anything from this run's workdir so no wine process
# outlives the script (lingering children would also hold the stdout pipe
# open, hanging piped callers).
#
# With the real game, the same flow is the M0 milestone test
# (docs/launcher.md) — use ./launch.sh for that.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
WINE="${WINE:-wine}"
BUILD="$ROOT/build/win32/bin"
HOST_BUILD="$ROOT/build/win64/bin/mc2vr_host.exe"
WORK="$(mktemp -d /tmp/opencode/mc2vr-selftest.XXXXXX)"
WORK_TAG="$(basename "$WORK")" # unique to this run; safe pkill pattern

cleanup() {
    # Anything spawned from the workdir embeds its path in the cmdline. The
    # tag is unique to this run and appears nowhere in this script's own
    # command line, so the pkill can't hit us.
    pkill -f "$WORK_TAG" 2>/dev/null || true
    rm -rf "$WORK"
}
trap cleanup EXIT

echo "building..."
cmake --build "$ROOT/build/win32" --target mc2vr_selftest_sleeper mc2vr_selftest_ipc_probe >&2
if [ ! -x "$HOST_BUILD" ]; then
    echo "building host (win64)..."
    cmake -B "$ROOT/build/win64" -S "$ROOT" \
        -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/x86_64-w64-mingw32.cmake" >&2
    cmake --build "$ROOT/build/win64" --target mc2vr_host >&2
fi

# Assemble the deploy layout: <game dir>/mc2vr/{launcher,carrier} + game exe.
# The host is deployed when built so phase A exercises the S4-1 spawn path.
mkdir -p "$WORK/mc2vr"
cp "$BUILD/mc2vr_launcher.exe" "$BUILD/mc2vr_carrier.dll" "$WORK/mc2vr/"
cp "$BUILD/mc2vr_selftest_ipc_probe.exe" "$WORK/mc2vr/"
if [ -x "$HOST_BUILD" ]; then
    cp "$HOST_BUILD" "$WORK/mc2vr/"
fi
cp "$BUILD/mc2vr_selftest_sleeper.exe" "$WORK/MERCENARIES2.EXE"

echo
echo "== PHASE A: launcher chain =="
set +e
( cd "$WORK" && WINEDEBUG=-all timeout 90 "$WINE" mc2vr/mc2vr_launcher.exe )
RC=$?
set -e

echo
echo "==== mc2vr_carrier.log ===="
cat "$WORK/mc2vr/mc2vr_carrier.log" 2>/dev/null || echo "(missing)"
echo "==== mc2vr_launcher.log (tail) ===="
tail -n 20 "$WORK/mc2vr/mc2vr_launcher.log" 2>/dev/null || echo "(missing)"

# Verdict: launcher exited 0 and the carrier logged both the attach and the
# expected build-lock refusal. The host outcome line must also be present
# (spawned-and-failed-fast, ready, or skipped — never a 30s hang or a crash).
PASS_A=0
if [ "$RC" -eq 0 ] \
   && grep -q "carrier attached" "$WORK/mc2vr/mc2vr_carrier.log" 2>/dev/null \
   && grep -q "build lock FAILED" "$WORK/mc2vr/mc2vr_carrier.log" 2>/dev/null \
   && { grep -q "host exited early" "$WORK/mc2vr/mc2vr_launcher.log" 2>/dev/null \
        || grep -q "host ready" "$WORK/mc2vr/mc2vr_launcher.log" 2>/dev/null \
        || grep -q "host exe not found" "$WORK/mc2vr/mc2vr_launcher.log" 2>/dev/null; }; then
    PASS_A=1
fi
echo
if [ "$PASS_A" -eq 1 ]; then
    echo "PHASE A: PASS (chain + host lifecycle line)"
else
    echo "PHASE A: FAIL (launcher rc=$RC)"
fi

echo
echo "== PHASE B: host --mock <-> IPC probe round trip =="
set +e
IPCN="mc2vr_selftest_ipc_$$"
( cd "$WORK/mc2vr" && WINEDEBUG=-all "$WINE" mc2vr_host.exe --mock --ipc-name "$IPCN" ) &
HOST_SHELL_PID=$!
WINEDEBUG=-all timeout 90 "$WINE" "$WORK/mc2vr/mc2vr_selftest_ipc_probe.exe" --name "$IPCN"
PROBE_RC=$?
HOST_RC=0
if [ "$PROBE_RC" -eq 0 ]; then
    # Probe passed — the host should exit on its own within moments; give it
    # 15s before failing (kill order: shell, then the wine stragglers via trap).
    for _ in $(seq 1 30); do
        kill -0 "$HOST_SHELL_PID" 2>/dev/null || break
        sleep 0.5
    done
    wait "$HOST_SHELL_PID" || HOST_RC=$?
else
    kill "$HOST_SHELL_PID" 2>/dev/null || true
    wait "$HOST_SHELL_PID" 2>/dev/null || true
    HOST_RC=1
fi
set -e

echo
echo "==== mc2vr_host.log ===="
cat "$WORK/mc2vr/mc2vr_host.log" 2>/dev/null || echo "(missing)"

PASS_B=0
if [ "$PROBE_RC" -eq 0 ] && [ "$HOST_RC" -eq 0 ]; then
    PASS_B=1
fi
echo
if [ "$PASS_B" -eq 1 ]; then
    echo "PHASE B: PASS (connect, poses, events, clean shutdown)"
else
    echo "PHASE B: FAIL (probe rc=$PROBE_RC host rc=$HOST_RC)"
fi

echo
if [ "$PASS_A" -eq 1 ] && [ "$PASS_B" -eq 1 ]; then
    echo "PASS: full chain + IPC round trip"
    exit 0
fi
echo "FAIL"
exit 1

#!/usr/bin/env bash
# End-to-end self-test of the launcher → carrier chain, using the sleeper
# stand-in instead of the real game (see sleeper.c for what it mimics).
#
# Verifies, under plain Wine (no Steam/Proton needed):
#   - launcher path resolution, process start, module-base check
#   - boot-counter poll detects the "main loop"
#   - CreateRemoteThread + LoadLibraryW injection works
#   - carrier logs "attached"
#   - carrier build lock correctly REFUSES to hook (sleeper != real exe)
#
# The launcher spawns its own sleeper child; the EXIT trap kills anything
# from this run's workdir so no wine process outlives the script (lingering
# children would also hold the stdout pipe open, hanging piped callers).
#
# With the real game, the same flow is the M0 milestone test
# (docs/launcher_plan.md) — use ./launch.sh for that.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
WINE="${WINE:-wine}"
BUILD="$ROOT/build/win32/bin"
WORK="$(mktemp -d /tmp/opencode/mc2vr-selftest.XXXXXX)"
WORK_TAG="$(basename "$WORK")" # unique to this run; safe pkill pattern

cleanup() {
    # The launcher-spawned sleeper runs in its own wine process whose cmdline
    # embeds the workdir path. The tag is unique to this run and appears
    # nowhere in this script's own command line, so the pkill can't hit us.
    pkill -f "$WORK_TAG" 2>/dev/null || true
    rm -rf "$WORK"
}
trap cleanup EXIT

echo "building..."
cmake --build "$ROOT/build/win32" --target mc2vr_selftest_sleeper >&2

# Assemble the deploy layout: <game dir>/mc2vr/{launcher,carrier} + game exe.
mkdir -p "$WORK/mc2vr"
cp "$BUILD/mc2vr_launcher.exe" "$BUILD/mc2vr_carrier.dll" "$WORK/mc2vr/"
cp "$BUILD/mc2vr_selftest_sleeper.exe" "$WORK/MERCENARIES2.EXE"

echo "running launcher..."
set +e
( cd "$WORK" && WINEDEBUG=-all timeout 90 "$WINE" mc2vr/mc2vr_launcher.exe )
RC=$?
set -e

echo
echo "==== mc2vr_carrier.log ===="
cat "$WORK/mc2vr/mc2vr_carrier.log" 2>/dev/null || echo "(missing)"

# Verdict: launcher exited 0 and the carrier logged both the attach and the
# expected build-lock refusal.
PASS=0
if [ "$RC" -eq 0 ] \
   && grep -q "carrier attached" "$WORK/mc2vr/mc2vr_carrier.log" 2>/dev/null \
   && grep -q "build lock FAILED" "$WORK/mc2vr/mc2vr_carrier.log" 2>/dev/null; then
    PASS=1
fi

echo
if [ "$PASS" -eq 1 ]; then
    echo "PASS: full chain works (attach + build-lock refusal verified)"
else
    echo "FAIL (launcher rc=$RC)"
fi
exit $((1 - PASS))

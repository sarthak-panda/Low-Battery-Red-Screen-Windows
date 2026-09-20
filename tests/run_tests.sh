#!/bin/sh
# Runs the real Windows binaries under Wine + a virtual X display.
# Needs: mingw-w64, wine64, xvfb   (Ubuntu: sudo apt install gcc-mingw-w64-x86-64 wine64 xvfb)
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
"$ROOT/build.sh" test

export WINEDEBUG=-all
export WINEPREFIX=${WINEPREFIX:-/tmp/lbr-wineprefix}
export WINEDLLOVERRIDES="schtasks.exe=n"      # use our schtasks test-double, not Wine's built-in

xvfb-run -a wineboot -u >/dev/null 2>&1 || true
# schtasks test double: records the Task Scheduler XML the installer generates
cp "$ROOT/build/fake_schtasks.exe" "$WINEPREFIX/drive_c/windows/system32/schtasks.exe"

RUN="$ROOT/build/testrun"
rm -rf "$RUN"; mkdir -p "$RUN"
cp "$ROOT/build/LowBatteryRed_test.exe" "$ROOT/build/test_harness.exe" "$RUN/"
cd "$RUN"
xvfb-run -a -s "-screen 0 1366x768x24" wine test_harness.exe

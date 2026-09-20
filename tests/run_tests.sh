#!/bin/sh
# Runs the real Windows binaries under Wine + a virtual display.
# Needs: mingw-w64, wine, xvfb
#   Ubuntu: sudo apt install gcc-mingw-w64-x86-64 wine wine64 xvfb xauth
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)

# Fail early and clearly when a tool is missing. (A missing `wine` package once showed
# up only as a confusing "cp: cannot create ... schtasks.exe" much later.)
for tool in "${CC:-x86_64-w64-mingw32-gcc}" wine wineboot xvfb-run; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "error: '$tool' not found in PATH." >&2
    echo "       Ubuntu: sudo apt install gcc-mingw-w64-x86-64 wine wine64 xvfb xauth" >&2
    exit 1
  fi
done

"$ROOT/build.sh" test

# The prefix lives inside the workspace: Wine refuses to create one in a directory the
# current user does not own (e.g. directly under /tmp when not running as root).
export WINEPREFIX="${WINEPREFIX:-$ROOT/build/wineprefix}"
# no Mono/Gecko download prompts; use our schtasks test-double instead of Wine's built-in
export WINEDLLOVERRIDES="mscoree,mshtml=;schtasks.exe=n"

# Create the prefix. Never hide failures: show Wine's own output if it did not work.
BOOTLOG=$(mktemp)
xvfb-run -a wineboot -u >"$BOOTLOG" 2>&1 || true
SYS32="$WINEPREFIX/drive_c/windows/system32"
if [ ! -d "$SYS32" ]; then
  echo "error: Wine prefix was not created at $WINEPREFIX" >&2
  echo "--- wineboot output ---" >&2
  cat "$BOOTLOG" >&2
  exit 1
fi
rm -f "$BOOTLOG"

export WINEDEBUG=-all
# schtasks test double: records the Task Scheduler XML the installer generates
cp "$ROOT/build/fake_schtasks.exe" "$SYS32/schtasks.exe"

RUN="$ROOT/build/testrun"
rm -rf "$RUN"; mkdir -p "$RUN"
cp "$ROOT/build/LowBatteryRed_test.exe" "$ROOT/build/test_harness.exe" "$RUN/"
cd "$RUN"
xvfb-run -a -s "-screen 0 1366x768x24" wine test_harness.exe

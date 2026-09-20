#!/bin/sh
# Build LowBatteryRed.exe (64-bit Windows) with MinGW-w64.
#   ./build.sh          -> build/LowBatteryRed.exe               (release)
#   ./build.sh test     -> also build the test binary + harness   (see tests/run_tests.sh)
set -e
cd "$(dirname "$0")"
CC=${CC:-x86_64-w64-mingw32-gcc}
WINDRES=${WINDRES:-x86_64-w64-mingw32-windres}
mkdir -p build

# manifest (per-monitor DPI awareness, asInvoker) -> resource object
( cd src && "$WINDRES" app.rc -O coff -o ../build/app.o )

FLAGS="-O2 -Wall -Wextra -municode -mwindows -static -static-libgcc -Wl,--no-insert-timestamp"
LIBS="-luser32 -lgdi32 -ladvapi32 -lshell32"

"$CC" $FLAGS -s -o build/LowBatteryRed.exe src/lowbatteryred.c build/app.o $LIBS
echo "built build/LowBatteryRed.exe"

if [ "$1" = "test" ]; then
  # test build: simulated battery (LBR_SIM_FILE) + fast timers. Never ship this one.
  "$CC" $FLAGS -DLBR_TEST -o build/LowBatteryRed_test.exe src/lowbatteryred.c build/app.o $LIBS
  "$CC" -O1 -Wall -municode -o build/test_harness.exe tests/test_harness.c -luser32 -ladvapi32
  "$CC" -O1 -municode -o build/fake_schtasks.exe tests/fake_schtasks.c
  echo "built test binaries"
fi

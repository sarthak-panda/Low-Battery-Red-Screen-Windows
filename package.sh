#!/bin/sh
# Create release assets in build/release/ :
#   LowBatteryRed.exe, LowBatteryRed-v<version>-win64.zip, SHA256SUMS.txt
set -e
VER=${1:?usage: ./package.sh <version>   e.g. ./package.sh 1.0.0}
cd "$(dirname "$0")"
./build.sh
OUT=build/release
STAGE=$OUT/LowBatteryRed
rm -rf "$OUT"; mkdir -p "$STAGE"
cp build/LowBatteryRed.exe dist/Install.bat dist/Test.bat dist/Uninstall.bat dist/README.txt "$STAGE/"
( cd "$OUT" && zip -qr "LowBatteryRed-v$VER-win64.zip" LowBatteryRed )
cp build/LowBatteryRed.exe "$OUT/"
rm -rf "$STAGE"
( cd "$OUT" && sha256sum LowBatteryRed.exe "LowBatteryRed-v$VER-win64.zip" > SHA256SUMS.txt )
ls -l "$OUT"

#!/bin/bash
# Baut BootBeacon.kext auf macOS (lokal oder GitHub Actions).
# Benoetigt: Xcode-Kommandozeilentools, git, curl.
set -euo pipefail
cd "$(dirname "$0")"
ROOT="$(pwd)"
DEPS="$ROOT/deps"
OUT="$ROOT/build"
VERSION="1.0.0"
mkdir -p "$DEPS" "$OUT"

# 1) MacKernelSDK (Kernel-Header + libkmod.a)
if [ ! -d "$DEPS/MacKernelSDK" ]; then
  git clone --depth 1 https://github.com/acidanthera/MacKernelSDK "$DEPS/MacKernelSDK"
fi

# 2) Lilu-SDK (Header + plugin_start.cpp aus der DEBUG-Release)
if [ ! -d "$DEPS/Lilu.kext" ]; then
  ( cd "$DEPS" && src=$(/usr/bin/curl -Lfs https://raw.githubusercontent.com/acidanthera/Lilu/master/Lilu/Scripts/bootstrap.sh) && eval "$src" )
fi
LILU_SDK="$DEPS/Lilu.kext/Contents/Resources"
test -f "$LILU_SDK/Headers/kern_api.hpp"
test -f "$LILU_SDK/Library/plugin_start.cpp"

CXX="$(xcrun -f clang++)"
CC="$(xcrun -f clang)"
RES="$("$CC" -print-resource-dir)"
CCKEXT="$RES/lib/darwin/libclang_rt.cc_kext.a"
echo "clang: $("$CC" --version | head -1)"
echo "cc_kext: $CCKEXT"; ls -l "$CCKEXT" || true

COMMON=(-target x86_64-apple-macos10.13 -mkernel -nostdinc -Os -g
        -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT
        -DPRODUCT_NAME=BootBeacon -DMODULE_VERSION=$VERSION
        -I "$LILU_SDK" -I "$DEPS/MacKernelSDK/Headers" -isystem "$RES/include"
        -Wno-format)
CXXFLAGS=(-fapple-kext -fno-exceptions -fno-rtti -std=c++17 -nostdinc++)

OBJ="$OUT/obj"; mkdir -p "$OBJ"
"$CXX" "${COMMON[@]}" "${CXXFLAGS[@]}" -c BootBeacon/BootBeacon.cpp -o "$OBJ/BootBeacon.o"
"$CXX" "${COMMON[@]}" "${CXXFLAGS[@]}" -c "$LILU_SDK/Library/plugin_start.cpp" -o "$OBJ/plugin_start.o"
"$CC"  "${COMMON[@]}" -c BootBeacon/kmod_info.c -o "$OBJ/kmod_info.o"

KEXT="$OUT/BootBeacon.kext"
rm -rf "$KEXT"; mkdir -p "$KEXT/Contents/MacOS"
LINK=(-target x86_64-apple-macos10.13 -nostdlib -Xlinker -kext
      "$OBJ/kmod_info.o" "$OBJ/BootBeacon.o" "$OBJ/plugin_start.o"
      -L "$DEPS/MacKernelSDK/Library/x86_64" -lkmod)
[ -f "$CCKEXT" ] && LINK+=("$CCKEXT")
"$CXX" "${LINK[@]}" -o "$KEXT/Contents/MacOS/BootBeacon"
cp BootBeacon/Info.plist "$KEXT/Contents/Info.plist"
plutil -lint "$KEXT/Contents/Info.plist"

echo "== Diagnose =="
file "$KEXT/Contents/MacOS/BootBeacon"
otool -hv "$KEXT/Contents/MacOS/BootBeacon"
echo "-- kmod_info:"; nm "$KEXT/Contents/MacOS/BootBeacon" | grep -E "_kmod_info|_realmain|_start$|BootBeacon_kern_start" || true
echo "-- undefinierte Symbole (muessen alle vom Kernel/Lilu kommen):"
nm -u "$KEXT/Contents/MacOS/BootBeacon"
# Pruefen, ob die Kext von macOS akzeptiert wuerde (nur Info, Fehler wegen fehlendem Lilu sind normal)
kextlibs -xml "$KEXT" 2>&1 | head -40 || true

( cd "$OUT" && rm -f BootBeacon.zip && zip -qr BootBeacon.zip BootBeacon.kext )
echo "Fertig: $OUT/BootBeacon.zip"

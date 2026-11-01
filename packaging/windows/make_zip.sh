#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Makes NativeNX-windows-x86_64.zip: nxlinux and NativeNX built for Windows (docs/WINDOWS.md), with SDL3.dll, a font
# for systems without Segoe UI, nxnative's game profiles and the licences, in one folder to unpack anywhere. The GPU
# driver brings the Vulkan loader (vulkan-1.dll).
#
# usage: packaging/windows/make_zip.sh <Windows build folder> <SDL3.dll> [llvm-strip]
set -e
BUILD=$(readlink -f "$1"); SDL=$(readlink -f "$2"); STRIP=${3:-llvm-strip}
[ -f "$BUILD/nxlinux.exe" ] && [ -f "$SDL" ] || { sed -n '6,7p' "$0" >&2; exit 2; }
SRC=$(cd "$(dirname "$0")/../.." && pwd)
OUT=$(pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
D=$WORK/NativeNX
cmake --install "$BUILD" --prefix "$D" > /dev/null
"$STRIP" "$D/nxlinux.exe" "$D/nativenx.exe"
cp "$SDL" "$D/SDL3.dll"
mkdir -p "$D/data/fonts" "$D/licenses"
for f in DejaVuSans.ttf DejaVuSans-Bold.ttf; do   # (Windows has Segoe UI; this is the last resort, and Wine's)
    for d in /usr/share/fonts/truetype/dejavu /usr/share/fonts/dejavu-sans-fonts /usr/share/fonts/TTF /usr/share/fonts/dejavu; do
        [ -f "$d/$f" ] && cp "$d/$f" "$D/data/fonts/" && break
    done
done
for l in /usr/share/doc/fonts-dejavu-core/copyright /usr/share/licenses/dejavu-sans-fonts/LICENSE /usr/share/doc/dejavu-fonts/LICENSE; do
    [ -f "$l" ] && cp "$l" "$D/data/fonts/LICENSE-DejaVu" && break
done
cp "$SRC/LICENSE" "$SRC/NOTICE.md" "$D/licenses/"
cp "$SRC/docs/WINDOWS.md" "$D/README-Windows.md"
rm -f "$OUT/NativeNX-windows-x86_64.zip"
(cd "$WORK" && zip -qr9 "$OUT/NativeNX-windows-x86_64.zip" NativeNX)
echo "made $OUT/NativeNX-windows-x86_64.zip"

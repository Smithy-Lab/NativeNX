#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Makes NativeNX-x86_64.AppImage: NativeNX and nxlinux built for any Linux distribution with glibc 2.34 or newer
# (-DNXL_PORTABLE=ON), with SDL3 and a font of their own. The GPU driver and the Vulkan loader are the system's.
#
# usage: packaging/appimage.sh <portable build folder> <libSDL3.so.0> [appimagetool] [runtime]
#   the build folder: configured with -DNXL_PORTABLE=ON and built (docs/BUILDING.md)
#   libSDL3.so.0: SDL 3.2 or newer built for an old glibc with its audio and display back ends (PipeWire, PulseAudio,
#     ALSA, Wayland, X11), for example in Valve's Steam Runtime "sniper" SDK; docs/BUILDING.md says how
#   appimagetool and its runtime: from github.com/AppImage (downloaded when not given)
set -e
BUILD=$(readlink -f "$1"); SDL=$(readlink -f "$2"); TOOL=$3; RUNTIME=$4
[ -d "$BUILD" ] && [ -f "$SDL" ] || { sed -n '6,11p' "$0" >&2; exit 2; }
SRC=$(cd "$(dirname "$0")/.." && pwd)
OUT=$(pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
APPDIR=$WORK/NativeNX.AppDir

grep -q 'NXL_PORTABLE:BOOL=ON' "$BUILD/CMakeCache.txt" || { echo "$BUILD is not a portable build (-DNXL_PORTABLE=ON)" >&2; exit 1; }
cmake --install "$BUILD" --prefix "$APPDIR/usr" > /dev/null
for b in nxlinux nativenx; do   # check: nothing newer than glibc 2.34
    v=$(objdump -T "$APPDIR/usr/bin/$b" | grep -oE 'GLIBC_[0-9.]+' | sort -V | uniq | tail -1)
    case "$v" in GLIBC_2.3[5-9]|GLIBC_2.[4-9]*) echo "$b needs $v: not portable" >&2; exit 1;; esac
done
strip "$APPDIR/usr/bin/nxlinux" "$APPDIR/usr/bin/nativenx"
mkdir -p "$APPDIR/usr/lib" "$APPDIR/usr/share/nativenx/fonts"
cp "$SDL" "$APPDIR/usr/lib/libSDL3.so.0"
# a font for NativeNX's text where the system has none it knows (DejaVu: free to redistribute, its licence beside it)
for f in DejaVuSans.ttf DejaVuSans-Bold.ttf; do
    for d in /usr/share/fonts/truetype/dejavu /usr/share/fonts/dejavu-sans-fonts /usr/share/fonts/TTF /usr/share/fonts/dejavu; do
        [ -f "$d/$f" ] && cp "$d/$f" "$APPDIR/usr/share/nativenx/fonts/" && break
    done
done
for l in /usr/share/doc/fonts-dejavu-core/copyright /usr/share/licenses/dejavu-sans-fonts/LICENSE /usr/share/doc/dejavu-fonts/LICENSE; do
    [ -f "$l" ] && cp "$l" "$APPDIR/usr/share/nativenx/fonts/LICENSE-DejaVu" && break
done
cp "$SRC/packaging/AppRun" "$APPDIR/AppRun"
cp "$SRC/packaging/nativenx.desktop" "$APPDIR/nativenx.desktop"
cp "$SRC/launcher/data/nativenx-icon.png" "$APPDIR/nativenx.png"
mkdir -p "$APPDIR/usr/share/doc/nativenx"
cp "$SRC/LICENSE" "$SRC/NOTICE.md" "$APPDIR/usr/share/doc/nativenx/"

if [ -z "$TOOL" ]; then
    TOOL=$WORK/appimagetool
    curl -sSL -o "$TOOL" https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage
fi
if [ -z "$RUNTIME" ]; then
    RUNTIME=$WORK/runtime
    curl -sSL -o "$RUNTIME" https://github.com/AppImage/type2-runtime/releases/download/continuous/runtime-x86_64
fi
chmod +x "$TOOL"
ARCH=x86_64 "$TOOL" --appimage-extract-and-run --runtime-file "$RUNTIME" "$APPDIR" "$OUT/NativeNX-x86_64.AppImage"
echo "made $OUT/NativeNX-x86_64.AppImage"

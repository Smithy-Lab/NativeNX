#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# NativeNX's graphics check (launcher/gfxapi.c) on the guest test programs: t_draw draws with NVN, t_gl with OpenGL,
# t_vk with Vulkan. usage: t_gfxapi.sh <nativenx> <guest build folder> <work folder>
set -e
nx=$1 gb=$2 w=$3
rm -rf "$w"
for g in nvn:t_draw gl:t_gl vk:t_vk; do mkdir -p "$w/${g%%:*}/ExeFS"; cp "$gb/${g#*:}.nso" "$w/${g%%:*}/ExeFS/main"; done
out=$("$nx" --games "$w" --list)
echo "$out"
fail=0
check() { if echo "$out" | grep -A3 "^[0-9]*\. $1\$" | grep -q "graphics: $2"; then echo "ok - $1: $2"; else echo "not ok - $1: $2"; fail=1; fi; }
check nvn NVN
check gl OpenGL
check vk Vulkan
[ $fail = 0 ] && echo "nxl-tests: passed"

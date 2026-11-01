#!/bin/sh
# FSR's upscaling of the window (gfx/vk_upscale.c) on a test picture: the same picture as bilinear filtering where it is
# smooth, steeper edges where it is not. Usage: t_upscale.sh <nxlinux> <a guest program that presents> <folder>
mkdir -p "$3"
out=$(NXL_HEADLESS=1 NXL_TEST_UPSCALE="$3" "$1" --no-cache "$2" 2>&1 | grep 'upscale self-test')
echo "$out"
echo "$out" | awk '/flat difference/ {
    for (i = 1; i <= NF; i++) { if ($i == "difference") d = $(i + 1); if ($i == "FSR") f = $(i + 1); if ($i == "bilinear") b = $(i + 1) }
    ok = d < 2 && f > b * 1.1
    print (d < 2 ? "ok" : "not ok") " - smooth parts as bilinear (mean difference " d ")"
    print (f > b * 1.1 ? "ok" : "not ok") " - edges steeper than bilinear (" f " against " b ")"
    if (ok) print "nxl-tests: passed"; exit }'

#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# NativeNX for Windows under Wine: t_launcher_win.sh <Windows build folder> <tests/launcher dir> <work dir>
#   the library (make_library.py's made-up games), a game started (fake_nxlinux.exe: its arguments, settings and the two
#   pipes, which need a window), the dashboard drawn offscreen
# WINE (default: wine) runs the programs, in WINEPREFIX; NativeNX's folders are the Wine user's (%APPDATA% ...).
B=$1; T=$2; W=$3; WINE=${WINE:-wine}
fail=0
check() { if eval "$2"; then echo "ok - $1"; else echo "not ok - $1"; fail=1; fi; }
rm -rf "$W"; mkdir -p "$W"
python3 "$T/make_library.py" "$W/games" >/dev/null || { echo "not ok - make the library"; exit 1; }
[ -d "$B/data" ] || cp -r "$T/../../launcher/data" "$B/data"
mkdir -p "$B/data/fonts"   # (Wine has no Segoe UI: the zip's last-resort font)
for f in /usr/share/fonts/truetype/dejavu/DejaVuSans*.ttf /usr/share/fonts/dejavu-sans-fonts/DejaVuSans*.ttf; do [ -f "$f" ] && cp "$f" "$B/data/fonts/"; done
export WINEDEBUG=-all SDL_VIDEO_DRIVER=offscreen SDL_RENDER_DRIVER=software SDL_AUDIO_DRIVER=dummy
$WINE "$B/nativenx.exe" --games "$W/games" --list > "$W/list.txt" 2>&1
check "eight games found" "grep -q '^8 games' '$W/list.txt'"
check "a title from control.nacp" "grep -A2 'Sky Garden Adventures' '$W/list.txt' | grep -q 'title from control.nacp'"
n=$(grep '\. Sky Garden Adventures' "$W/list.txt" | cut -d. -f1)
NXL_LAUNCHER_NXLINUX="$B/fake_nxlinux.exe" $WINE "$B/nativenx.exe" --run "$n" > "$W/run.txt" 2>&1
check "the game's end and its status are reported" "grep -q 'game ended: exit status 3' '$W/run.txt'"
log=$($WINE winepath -u "$(grep -o 'log [^ ]*' "$W/run.txt" | tail -1 | cut -c5- | tr -d '\r')" 2>/dev/null | tr -d '\r')
check "nxlinux gets the ExeFS, with -v, and the settings" "grep -q 'args: -v .*Sky-Garden-1.2/ExeFS' '$log' && grep -q 'NXL_SEGMENT=0' '$log'"
NXL_LAUNCHER_NXLINUX="$B/fake_nxlinux.exe" $WINE "$B/nativenx.exe" --screenshot "$W/after.png" --select-title "Sky Garden Adventures" --press a > "$W/load.txt" 2>&1
check "with a window: the pipes to and from the game work" "tail -1 '$log' | grep -q 'pipes: from NativeNX yes, to it written'"
$WINE "$B/nativenx.exe" --games "$W/games" --screenshot "$W/dash.png" > /dev/null 2>&1
check "the dashboard draws" "test -s '$W/dash.png'"
[ $fail = 0 ] && echo "nxl-tests: passed" || echo "nxl-tests: FAILED"

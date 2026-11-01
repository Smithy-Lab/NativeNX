#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# The launcher's tests: t_launcher.sh <nxlinux-launcher> <tests/launcher dir> <work dir>
#   the library (a made-up one, make_library.py): titles from control.nacp or the folder, pictures, nxlinux.env
#   starting a game (a stand-in nxlinux): its ExeFS, RomFS and save folders, the game's settings, the log
#   the screens drawn offscreen: the dashboard, the settings, the first start without a games folder
L=$1; T=$2; W=$3
fail=0
check() { if eval "$2"; then echo "ok - $1"; else echo "not ok - $1"; fail=1; fi; }
rm -rf "$W"; mkdir -p "$W"
python3 "$T/make_library.py" "$W/games" >/dev/null || { echo "not ok - make the library"; exit 1; }
export XDG_CONFIG_HOME="$W/config" XDG_DATA_HOME="$W/data" HOME="$W" USER=sam
"$L" --games "$W/games" --list > "$W/list.txt" 2>&1
cat "$W/list.txt" | sed 's/^/# /'
check "eight games found, the folder without ExeFS left out" "grep -q '^8 games' '$W/list.txt'"
check "a compressed control.nacp (system 21.0.0) gives its title" "grep -A2 'Packed Titles Deluxe' '$W/list.txt' | grep -q 'title from control.nacp'"
check "a title from control.nacp" "grep -q 'Sky Garden Adventures' '$W/list.txt' && grep -A2 'Sky Garden Adventures' '$W/list.txt' | grep -q 'title from control.nacp'"
check "a title from the folder name without one" "grep -A2 '^[0-9]*\. Deep Caves' '$W/list.txt' | grep -q 'title from the folder name'"
check "the console's icon, a cover.png, or none" "grep -A2 'Moon Farm Story' '$W/list.txt' | grep -q icon_AmericanEnglish.dat && grep -A2 'Pixel Racer Turbo' '$W/list.txt' | grep -q cover.png && grep -A2 'Untitled Game' '$W/list.txt' | grep -q '(none'"
check "a game's DLC folders (DLC/<number>/) are counted" "grep -A6 'Sky Garden Adventures' '$W/list.txt' | grep -q 'DLC: 2 in .*Sky-Garden-1.2/DLC'"
check "the program ID from main.npdm" "grep -A2 'Ocean Song' '$W/list.txt' | grep -q 'program 0100aaaa00060000'"
check "the settings remember the games folder" "grep -q 'games = $W/games' '$W/config/nxlinux/launcher.ini'"
n=$(grep -n '\. Sky Garden Adventures' "$W/list.txt" | cut -d: -f2 | cut -d. -f1)
n=$(grep '\. Sky Garden Adventures' "$W/list.txt" | cut -d. -f1)
NXL_LAUNCHER_NXLINUX="$T/fake_nxlinux.sh" "$L" --run "$n" > "$W/run.txt" 2>&1
log="$W/data/nxlinux/logs/Sky-Garden-1.2.log"
cat "$W/run.txt" "$log" 2>/dev/null | sed 's/^/# /'
check "the game's end and its status are reported" "grep -q 'game ended: exit status 3' '$W/run.txt'"
check "nxlinux gets the ExeFS, with -v" "grep -q 'args: -v $W/games/Sky-Garden-1.2/ExeFS' '$log'"
check "... the RomFS and a save folder in the game folder" "grep -q 'DM_ROMFS=$W/games/Sky-Garden-1.2/RomFS' '$log' && grep -q 'DM_SAVE=$W/games/Sky-Garden-1.2/save' '$log' && test -d '$W/games/Sky-Garden-1.2/save'"
check "... and the settings of the game's nxlinux.env" "grep -q 'NXL_SEGMENT=0' '$log'"
check "... and its DLC folder" "grep -q 'DM_AOC_DIR=$W/games/Sky-Garden-1.2/DLC' '$log'"
export SDL_VIDEO_DRIVER=offscreen SDL_RENDER_DRIVER=software
"$L" --screenshot "$W/dash.png" >/dev/null 2>&1; check "the dashboard draws" "test -s '$W/dash.png'"
for c in 0 11 2 3; do "$L" --screenshot "$W/settings$c.png" --settings $c >/dev/null 2>&1; done
check "the settings screen draws, each category" "test -s '$W/settings0.png' && test -s '$W/settings11.png' && test -s '$W/settings2.png' && test -s '$W/settings3.png'"
for p in Xbox PlayStation "Pro Controller" "Steam Deck"; do "$L" --screenshot "$W/pad-$p.png" --pad "$p" >/dev/null 2>&1; done
check "the controller badges draw (Xbox, PlayStation, Pro Controller, Steam Deck)" "test -s '$W/pad-Xbox.png' && test -s '$W/pad-PlayStation.png' && test -s '$W/pad-Pro Controller.png' && test -s '$W/pad-Steam Deck.png'"
"$L" --screenshot "$W/deck.png" --size 1280x800 >/dev/null 2>&1; check "a 1280x800 window (Steam Deck) draws" "test -s '$W/deck.png'"
XDG_CONFIG_HOME="$W/config-empty" "$L" --screenshot "$W/empty.png" >/dev/null 2>&1; check "the first start, with no games folder, draws" "test -s '$W/empty.png'"
export SDL_AUDIO_DRIVER=dummy
"$L" --screenshot "$W/s.png" > "$W/snd.txt" 2>&1; check "the soft sounds, made by the launcher, are the default" "grep -q 'sounds: soft (0 from files in -, 10 made here)' '$W/snd.txt'"
echo "sound_style = 1" >> "$W/config/nxlinux/launcher.ini"
"$L" --screenshot "$W/s.png" > "$W/snd1.txt" 2>&1; check "Kenney's sounds load from launcher/data" "grep -q \"sounds: Kenney's (9 from files\" '$W/snd1.txt'"
check "the launcher makes the folder for your own sounds" "test -d '$W/data/nxlinux/sounds'"
"$L" --export-sounds "$W/exported" > /dev/null; cp "$W/exported/move.wav" "$W/data/nxlinux/sounds/move.wav"
echo "sound_style = 2" >> "$W/config/nxlinux/launcher.ini"
"$L" --screenshot "$W/s.png" > "$W/snd2.txt" 2>&1; check "your own sounds: the ones in the folder, the soft ones for the rest" "grep -q 'sounds: your own (1 from files in $W/data/nxlinux/sounds, 9 made here)' '$W/snd2.txt'"
check "the soft sounds export as WAV files" "test -s '$W/exported/launch.wav' && test -s '$W/exported/error.wav'"
sed -i '/^sound_style/d' "$W/config/nxlinux/launcher.ini"
"$L" --screenshot "$W/p.png" --press rr > "$W/p1.txt" 2>&1; check "right twice selects the third game" "grep -q 'after the presses: game 2, top bar -1, settings 0' '$W/p1.txt'"
"$L" --screenshot "$W/p.png" --press ll > "$W/p2.txt" 2>&1; check "left at the first game stays there (the bump)" "grep -q 'after the presses: game 0,' '$W/p2.txt'"
"$L" --screenshot "$W/p.png" --press ur > "$W/p3.txt" 2>&1; check "up goes to the top bar, right to Power" "grep -q 'top bar 1, settings 0' '$W/p3.txt'"
"$L" --screenshot "$W/p.png" --press vddrd > "$W/p4.txt" 2>&1; check "View (-) opens the settings; down twice to Sound, right into it, down a row" "grep -q 'settings 1 (category 2, column 1, row 1)' '$W/p4.txt'"
"$L" --screenshot "$W/p.png" --press vb > "$W/p5.txt" 2>&1; check "B closes the settings" "grep -q 'settings 0 (category 0' '$W/p5.txt'"
"$L" --screenshot "$W/h1.png" --hover 400,350 > "$W/h1.txt" 2>&1; check "the pointer over a game highlights it" "grep -q 'hovering game 1,' '$W/h1.txt'"
"$L" --screenshot "$W/h2.png" --hover 342,62 > "$W/h2.txt" 2>&1; check "the pointer over Settings focuses it" "grep -q 'top bar 4' '$W/h2.txt'"
"$L" --screenshot "$W/h2b.png" --hover 252,62 > "$W/h2b.txt" 2>&1; check "the pointer between two icons of the top bar keeps it on the bar (no flicker back to the game)" "grep -q 'top bar [23],' '$W/h2b.txt'"
"$L" --screenshot "$W/h3.png" --settings 0 --hover 700,220 > "$W/h3.txt" 2>&1; check "the pointer over a settings row focuses it" "grep -q 'settings column 1 row 1' '$W/h3.txt'"
# a game's options: + opens them; Mode, Graphics, Mods; what is passed to nxlinux
sg=$(grep '\. Sky Garden Adventures' "$W/list.txt" | cut -d. -f1); sgi=$((sg - 1))
"$L" --screenshot "$W/o.png" --select $sgi --press m > "$W/o1.txt" 2>&1; check "+ on a game opens its options" "grep -q 'settings 2 (category 0' '$W/o1.txt'"
for c in 0 1 12 3; do "$L" --screenshot "$W/opt$c.png" --select $sgi --options $c >/dev/null 2>&1; done
check "the options screen draws, each category" "test -s '$W/opt0.png' && test -s '$W/opt1.png' && test -s '$W/opt12.png' && test -s '$W/opt3.png'"
ini="$W/config/nxlinux/games/Sky-Garden-1.2.ini"
"$L" --screenshot "$W/o.png" --select $sgi --press mra > /dev/null 2>&1; check "Run as: Native, then With your changes (saved)" "grep -q 'native = 0' '$ini'"
"$L" --screenshot "$W/o.png" --select $sgi --press mddrdda > /dev/null 2>&1; check "a mod turned on (saved)" "grep -q 'mod = HD Textures' '$ini'"
"$L" --screenshot "$W/o.png" --select $sgi --press mddrddda > /dev/null 2>&1; check "a mod unpacked inside extra folders is found and turned on" "grep -q 'mod = Wrapped Mod' '$ini'"
"$L" --screenshot "$W/o.png" --select $sgi --press mdraaa > /dev/null 2>&1; check "anisotropic filtering chosen: 8x (saved)" "grep -q 'aniso = 8' '$ini'"
"$L" --screenshot "$W/o.png" --select $sgi --press mdrdddaa > /dev/null 2>&1; check "resolution chosen: 2x (saved)" "grep -q 'res = 20' '$ini'"
echo "mod = Atmos Mod" >> "$ini"
echo "mod = Code Only" >> "$ini"
echo "fill = 1" >> "$ini"
NXL_LAUNCHER_NXLINUX="$T/fake_nxlinux.sh" "$L" --run "$sg" > /dev/null 2>&1
check "with changes: the mods' romfs (Atmosphere's layout, extra folders) go to nxlinux, in list order" "grep -q 'DM_ROMFS_OVERLAY=$W/games/Sky-Garden-1.2/mods/Atmos Mod/atmosphere/contents/0100AAAA00010000/romfs:$W/games/Sky-Garden-1.2/mods/HD Textures/romfs:$W/games/Sky-Garden-1.2/mods/Wrapped Mod/Wrapped Mod v2/01007EF00011E000/romfs' '$log'"
check "... and the anisotropic filtering" "grep -q 'NXL_ANISO=8' '$log'"
check "... and the resolution scale" "grep -q '^NXL_RES_SCALE=2.0$' '$log'"
check "... and a mod's code patches folder (exefs)" "grep -q '^NXL_EXEFS_PATCHES=$W/games/Sky-Garden-1.2/mods/Code Only/exefs$' '$log'"
check "... and the picture filling the window" "grep -q '^NXL_FILL=1$' '$log'"
sed -i 's/native = 0/native = 1/' "$ini"
NXL_LAUNCHER_NXLINUX="$T/fake_nxlinux.sh" "$L" --run "$sg" > /dev/null 2>&1
check "native: no mods and no changes go to nxlinux" "grep -q '^DM_ROMFS_OVERLAY=$' '$log' && grep -q '^NXL_ANISO=$' '$log' && grep -q '^NXL_RES_SCALE=$' '$log' && grep -q '^NXL_EXEFS_PATCHES=$' '$log' && grep -q '^NXL_FILL=$' '$log'"
# the frame rate (60 fps by nxnative's frame step): only for a main build it knows
check "a game whose main build has the frame step is listed with 60 fps" "grep -A3 'Zz Frame Step' '$W/list.txt' | grep -q '60 fps: available' && test \$(grep -c '60 fps: available' '$W/list.txt') = 1"
fini="$W/config/nxlinux/games/Zz_Frame_Step.ini"; flog="$W/data/nxlinux/logs/Zz_Frame_Step.log"
"$L" --screenshot "$W/f.png" --select 7 --press mdrdda > /dev/null 2>&1; check "Frame rate chosen: 60 (saved)" "grep -q 'fps = 60' '$fini'"
"$L" --screenshot "$W/opt-fps.png" --select 7 --options 1 > /dev/null 2>&1; check "the Graphics options with the frame rate draw" "test -s '$W/opt-fps.png'"
NXL_LAUNCHER_NXLINUX="$T/fake_nxlinux.sh" "$L" --run 8 > /dev/null 2>&1
check "native: the frame rate is not used (the game's own 30, its cuts left out)" "grep -q '^DM_FPS_TARGET=$' '$flog' && grep -q '^NXL_NO_CUTS=main$' '$flog'"
sed -i 's/native = 1/native = 0/' "$fini"
NXL_LAUNCHER_NXLINUX="$T/fake_nxlinux.sh" "$L" --run 8 > /dev/null 2>&1
check "with changes at 60: DM_FPS_TARGET=60, and the build's cuts in" "grep -q '^DM_FPS_TARGET=60$' '$flog' && grep -q '^NXL_NO_CUTS=$' '$flog'"
echo "fps = 60" >> "$ini"
NXL_LAUNCHER_NXLINUX="$T/fake_nxlinux.sh" "$L" --run "$sg" > /dev/null 2>&1
check "a game without the frame step gets no frame rate setting" "grep -q '^DM_FPS_TARGET=$' '$log'"
check "the frame rate counter is off by default" "grep -q '^NXL_FPS=$' '$log'"
check "games: FSR with medium sharpening, in a window, by default" "grep -q '^NXL_UPSCALE=fsr$' '$log' && grep -q '^NXL_SHARPNESS=0.25$' '$log' && grep -q '^NXL_FULLSCREEN=$' '$log'"
"$L" --screenshot "$W/disp.png" --settings 4 > /dev/null 2>&1; check "the Display settings draw" "test -s '$W/disp.png'"
"$L" --screenshot "$W/fc.png" --press vddddrdddda > "$W/fc.txt" 2>&1; check "Settings, Display: the frame rate counter turned on (saved)" "grep -q 'fps_counter = 1' '$W/config/nxlinux/launcher.ini'"
"$L" --screenshot "$W/fc.png" --press vddddra > /dev/null 2>&1; check "... games on the whole screen (saved)" "grep -q 'game_fullscreen = 1' '$W/config/nxlinux/launcher.ini'"
NXL_LAUNCHER_NXLINUX="$T/fake_nxlinux.sh" "$L" --run "$sg" > /dev/null 2>&1
check "... and nxlinux gets NXL_FPS=1 and NXL_FULLSCREEN=1" "grep -q '^NXL_FPS=1$' '$log' && grep -q '^NXL_FULLSCREEN=1$' '$log'"
# profiles: the first start made one named after the login; the screens; favourites; a new profile and its own saves
check "the first start makes a profile named after the login (Sam)" "grep -q 'name = Sam' '$W/config/nxlinux/profiles.ini'"
for sc in profile profiles name album news; do "$L" --screenshot "$W/scr-$sc.png" --screen $sc > /dev/null 2>&1; done
check "the profile page, the profiles window, name entry, the album and What's new draw" "test -s '$W/scr-profile.png' && test -s '$W/scr-profiles.png' && test -s '$W/scr-name.png' && test -s '$W/scr-album.png' && test -s '$W/scr-news.png'"
"$L" --screenshot "$W/m1.png" --click 70,62 > "$W/m1.txt" 2>&1
check "a click on the avatar opens the profile page itself" "grep -q 'screen 1, window 0' '$W/m1.txt'"
"$L" --screenshot "$W/m1b.png" --screen profile --click 70,62 > "$W/m1b.txt" 2>&1
check "... and on the profile page, a click on it goes back home" "grep -q 'screen 0, window 0' '$W/m1b.txt'"
"$L" --screenshot "$W/m2.png" --screen profile --hover 560,330 > "$W/m2.txt" 2>&1
check "on the profile page the pointer over Rename focuses it" "grep -q 'page row 0 button 1' '$W/m2.txt'"
"$L" --screenshot "$W/m3.png" --screen profiles --hover 640,329 > "$W/m3.txt" 2>&1
check "in the profiles window the pointer over a profile focuses it" "grep -q 'window 1 (row 0,' '$W/m3.txt'"
"$L" --screenshot "$W/m4.png" --screen profiles --click 640,393 > "$W/m4.txt" 2>&1
check "... and a click on it opens name entry" "grep -q 'window 2 ' '$W/m4.txt'"
"$L" --screenshot "$W/m5.png" --screen profiles --click 40,700 > "$W/m5.txt" 2>&1
check "a click outside the window closes it" "grep -q 'window 0 ' '$W/m5.txt'"
"$L" --screenshot "$W/fav.png" --press y > "$W/fav.txt" 2>&1
check "Y makes the selected game a favourite (saved with the profile)" "grep -q \"Deep Caves.*1\$\" '$W/config/nxlinux/profiles/1/play.ini'"
"$L" --screenshot "$W/fav2.png" --press g > "$W/fav2.txt" 2>&1
check "R shows the favourites only (one)" "grep -q 'showing 1,' '$W/fav2.txt'"
"$L" --screenshot "$W/np.png" --press xdaaddddrrra > "$W/np.txt" 2>&1
check "X, New profile, a name typed (A), Done: a second profile, in use, its page open" "grep -c '^\[profile' '$W/config/nxlinux/profiles.ini' | grep -q 2 && grep -q 'current = 2' '$W/config/nxlinux/profiles.ini' && grep -q \"screen 1, window 0.*profile 'A'\" '$W/np.txt'"
NXL_LAUNCHER_NXLINUX="$T/fake_nxlinux.sh" "$L" --run "$sg" > /dev/null 2>&1
check "... the second profile's saves are its own (save-profile-2)" "grep -q '^DM_SAVE=$W/games/Sky-Garden-1.2/save-profile-2$' '$log'"
# the loading screen: drawn, and it gives way when the game's first frame is on screen
"$L" --screenshot "$W/loading.png" --loading "$sg" > /dev/null 2>&1; check "the loading screen draws" "test -s '$W/loading.png'"
NXL_LAUNCHER_NXLINUX="$T/fake_nxlinux_loading.sh" "$L" --screenshot "$W/after.png" --select $sgi --press a > "$W/load.txt" 2>&1
check "the loading screen follows the start and steps aside at the first frame" "grep -q 'first frame is on screen after' '$W/load.txt'"
"$L" --screenshot "$W/order.png" --press l > "$W/order.txt" 2>&1
check "the game played last comes first (selected at the start)" "grep -q \"game 0,.*selected 'Sky Garden Adventures'\" '$W/order.txt' && grep -q 'Sky-Garden-1.2' '$W/config/nxlinux/profiles/2/play.ini'"
# achievements: a test list for Ocean Song, the check without a window, a start that saves (the stand-in nxlinux) and
# the pop-ups it is sent, the page, the points beside the clock
rm -rf "$W/ldata"; cp -r "$T/../../launcher/data" "$W/ldata"; cp "$T/ach_test_list.ini" "$W/ldata/achievements/0100AAAA00060000.ini"
on=$(grep '\. Ocean Song' "$W/list.txt" | cut -d. -f1)
NXL_LAUNCHER_DATA="$W/ldata" "$L" --achievements "$on" > "$W/ach0.txt" 2>&1
cat "$W/ach0.txt" | sed 's/^/# /'
check "the check: the game's list and the general ones, no save yet" "grep -q '0 save slots with test.sav' '$W/ach0.txt' && grep -q 'no save.*Shell Collector' '$W/ach0.txt' && grep -q 'not yet.*Settling In' '$W/ach0.txt'"
NXL_LAUNCHER_DATA="$W/ldata" NXL_LAUNCHER_NXLINUX="$T/fake_nxlinux_ach.py" "$L" --screenshot "$W/achrun.png" --select-title "Ocean Song" --press a > "$W/achrun.txt" 2>&1
alog="$W/data/nxlinux/logs/$(ls "$W/data/nxlinux/logs" | grep -i ocean)"
cat "$W/achrun.txt" "$alog" | grep -i -E 'achiev|pop-up|NOTIFY' | sed 's/^/# /'
check "starting the game unlocks First Steps, the save it writes unlocks Shell Collector" "grep -q 'achievement unlocked: Ocean Song: First Steps (5 points)' '$W/achrun.txt' && grep -q 'achievement unlocked: Ocean Song: Shell Collector (20 points)' '$W/achrun.txt'"
check "... their pop-ups go to the game's window (880x184 pictures down NXL_NOTIFY_FD)" "test \$(grep -c 'pop-up: NXLIMG01 880x184 647696 bytes' '$alog') = 2"
check "... kept with the profile, with their points" "grep -q 'shells.20' '$W/config/nxlinux/profiles/2/achievements.ini' && grep -q 'general.first_steps.5' '$W/config/nxlinux/profiles/2/achievements.ini'"
NXL_LAUNCHER_DATA="$W/ldata" "$L" --achievements "$on" > "$W/ach1.txt" 2>&1
check "the check after: unlocked, the kraken not yet, a flag no save has: not tracked" "grep -q 'unlocked.*Shell Collector' '$W/ach1.txt' && grep -q 'not yet.*Deep Trouble' '$W/ach1.txt' && grep -q 'NOT TRACKED.*Ghost Flag' '$W/ach1.txt'"
NXL_LAUNCHER_DATA="$W/ldata" "$L" --screenshot "$W/achpage.png" --select-title "Ocean Song" --screen achievements > /dev/null 2>&1
NXL_LAUNCHER_DATA="$W/ldata" "$L" --screenshot "$W/achpress.png" --select-title "Ocean Song" --press t > "$W/achpress.txt" 2>&1
check "the achievements page draws; LB (T) opens it" "test -s '$W/achpage.png' && grep -q 'screen 4, window 0' '$W/achpress.txt'"
# the menu over a game (the Home button): drawn and sent as a picture; a screenshot; NativeNX's Home (the game waits,
# paused); closing the game; B resumes. The stand-in game (fake_nxlinux_menu.py) presses the buttons, its log shows
# what it was sent
for st in shot home close resume; do
    printf '#!/bin/sh\nexec python3 "%s" %s "%s"\n' "$T/fake_nxlinux_menu.py" "$st" "$W/menu-$st.rgba" > "$W/fake-$st.sh"; chmod +x "$W/fake-$st.sh"
    NXL_LAUNCHER_DATA="$W/ldata" NXL_LAUNCHER_NXLINUX="$W/fake-$st.sh" "$L" --screenshot "$W/menu-$st.png" --select-title "Ocean Song" --press a > "$W/menu-$st.txt" 2>&1
    cp "$(ls -t "$W"/data/nxlinux/logs/*cean* | head -1)" "$W/menulog-$st.txt"
done
cat "$W/menulog-shot.txt" "$W/menulog-home.txt" | grep -E '^(menu|got)' | sed 's/^/# /'
check "the Home button: the menu, drawn at the window's shape (premultiplied), sent to the game" "grep -q 'the menu over the game (1280x720)' '$W/menu-shot.txt' && grep -q 'menu: NXLIMG02 1280x720' '$W/menulog-shot.txt'"
check "... Take a screenshot asks the game for one" "grep -q 'got: shot' '$W/menulog-shot.txt'"
check "... NativeNX Home: the game waits (stopped, so it does not read on), NativeNX in front; leaving closes it" "grep -q 'waits at NativeNX' '$W/menu-home.txt' && grep -q 'closing Ocean Song' '$W/menu-home.txt'"
check "... Close the game (A twice) closes it" "grep -q 'closing Ocean Song' '$W/menu-close.txt'"
check "... B closes the menu, back to the game" "grep -q 'got: menu-off' '$W/menulog-resume.txt'"
"$L" --install > "$W/inst.txt" 2>&1
check "--install adds NativeNX to the app menu, with its icon" "grep -q 'Name=NativeNX' '$W/data/applications/nativenx.desktop' && grep -q 'Icon=nativenx' '$W/data/applications/nativenx.desktop' && test -s '$W/data/icons/hicolor/512x512/apps/nativenx.png'"
[ $fail = 0 ] && echo "nxl-tests: passed" || echo "nxl-tests: failed"

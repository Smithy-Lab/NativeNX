## Windows and every Linux
- NativeNX and nxlinux now run on Windows 10 and 11: unpack the zip and start nativenx.exe. This is new: tell us how your games do.
- On Linux, one AppImage runs on any distribution from 2021 on (Ubuntu 22.04, Debian 12, Fedora 35, Arch, SteamOS 3 and newer).

## BCML mods
- Breath of the Wild mods made for BCML (.bnp) work: unpack one into the game's mods folder and turn it on. NativeNX merges it with the game's files when the game starts, as BCML would.
- A BCML mod's options show under it in Mods, each on or off.
- BCML mods that change the player's actor, animations or other parameter files (Linkle and others) no longer stop on the loading screen: the changes BCML keeps in its logs are merged in too.
- BCML mods that change DLC files (DLC armour, Master Mode, the DLC shrines and quests) work too, over the game's DLC.

## DLC and save backups
- A game's DLC: each one's files in the game's DLC folder, in a folder named by its number (DLC/1, DLC/2 ...).
- Back up a save, and restore a backup, from a game's Options. Backups are kept in the Saves backup folder.

## Sound
- Games that mix their own sound (Dead Cells, and games using FMOD or Wwise) now have sound.

## OpenGL and Vulkan games
- Games that bring their own OpenGL or Vulkan driver (Dead Cells and others) now run: their drawing goes straight to your PC's graphics driver, nothing emulated.
- The dashboard shows what each game draws with (NVN, OpenGL or Vulkan) beside its version.

## In-game menu
- The Home button (Guide on Xbox, PS on PlayStation; Shift+Tab on the keyboard) opens a menu over the game: Resume, the game's achievements, Take a screenshot, NativeNX Home, Close the game.
- NativeNX Home pauses the game and shows the dashboard; the game shows "Paused" there, and A (or the Home button) goes straight back into it. Starting another game asks to close the paused one first.

## Look
- The games sit in the middle of the screen. Clicking your picture on your profile page goes back home.
- The top bar's icons are one size, with Zacksly's Pro Controller for the controllers.

## Achievements
- Games now have achievements with points, like gamerscore. Your total shows beside the clock.
- They unlock as you play: when the game saves, NativeNX reads the save and a trophy pops up at the top right of the game, with a chime.
- Breath of the Wild has 75 (1000 points), from WispyWoods' list on Reddit: the Divine Beasts, shrines, Korok seeds, armour sets, the Great Fairies and springs, stables and villages, Hateno house and Tarrey Town, horses, cooking, the compendium and Ganon.
- Every game has general ones: time played, screenshots, a favourite.
- LB (T on the keyboard) shows the selected game's achievements: unlocked ones with their date, progress bars for the rest.

## Profiles
- Each person has a profile: a name, a picture (Change avatar: any PNG or JPEG), the time played per game.
- Each profile has its own saves. The first profile keeps the saves made before profiles.
- Your picture at the top left opens your profile page. X (P on the keyboard) shows the profiles: switch, or make a new one.
- The mouse works everywhere: point and click in the profiles window, on the profile page and in the album; a click outside a window or the right button goes back.
- The games played last come first; Y (F) makes a game a favourite, R (G) shows the favourites only.

## Album
- F12 in a game saves a screenshot. The Album in the top bar shows them; Y opens their folder, X deletes one.

## Widescreen and code mods
- Mods with code patches (an exefs folder of .pchtxt or .ips files) work, for the game version they are made for.
- Graphics, Picture shape: Fill the window, for widescreen mods such as a 21:9 one.

## Resolution scale
- Graphics, Resolution: the game's 3D drawn 1.5x to 4x larger: real detail, not a stretched picture.

## Loading screen
- While a game starts, what nxlinux is doing, a timer and its last log line; B stops it.

## 60 fps
- Graphics, Frame rate: up to 60 fps in the games nxnative's frame step knows (Breath of the Wild 1.9.0 and four more).

## Picture
- FSR 1.0 scales the game to your screen; full screen with F11; F1 shows the frame rate.
- Anisotropic filtering leaves the game's shadows alone (it drew a line on the ground at night).

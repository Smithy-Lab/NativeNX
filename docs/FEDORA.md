# nxlinux and NativeNX on Linux

The user guide (it began as the Fedora build notes): building on Fedora below, other distributions and the AppImage in
[BUILDING.md](BUILDING.md). Tested in development on x86_64 Linux with LLVM 22 and CMake 3.28. Fedora 44 ships LLVM 22.

## Packages

```sh
sudo dnf install clang lld cmake ninja-build llvm-devel python3 git zlib-ng-compat-devel libzstd-devel \
    vulkan-loader-devel vulkan-headers SDL3-devel fmt-devel boost-devel spirv-headers-devel python3-fonttools
```

- `clang` builds nxlinux and, with its ARM64 target, the guest test programs. `lld` links those.
- `llvm-devel` provides `llvm-config` and the libraries ScarletNX's recompiler generates code with.
- `vulkan-loader-devel`, `vulkan-headers`: the Vulkan graphics backend. Your GPU needs a Vulkan driver: NVIDIA's
  (RPM Fusion's `akmod-nvidia`) or Mesa's (`mesa-vulkan-drivers`: AMD, Intel, and NVK for NVIDIA). Without
  Vulkan headers nxlinux builds with the null backend, which runs games with nothing drawn.
- `python3-fonttools`: builds the button-icon font games draw in their text (nxnative's stand-in for the console's
  "Nintendo extension" font, from Liberation Sans Bold). Without it the icons show as `?`.
- `SDL3-devel`: the game's window, controllers and keyboard. `fmt-devel`, `boost-devel`, `spirv-headers-devel`: the shader recompiler (fetched
  from GitHub at configure time if missing, as is sirit, its SPIR-V emitter).
- An x86_64 CPU with AVX2 is assumed by the code the recompiler emits for your machine (any Ryzen or Intel Core from
  the last decade).

## Build and test

```sh
git clone <this repository> nxlinux
cd nxlinux
cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build
ctest --test-dir build --output-on-failure
```

The first configure clones ScarletNX and nxnative at the commits pinned in `CMakeLists.txt` (`NXL_SCARLETNX_TAG`,
`NXL_NXNATIVE_TAG`) and builds the recompiler library. ScarletNX downloads a few small dependencies of its own (zstd and
enet) while configuring.

Options:

| Option | Meaning |
|---|---|
| `-DNXL_NXNATIVE_DIR=/path/to/nxnative` | use a local nxnative checkout instead of fetching one |
| `-DCMAKE_PREFIX_PATH=/prefix` | use a ScarletNX recompiler installed with `cmake --install` (its `lib/cmake/ScarletNXRecompiler`) |
| `-DNXL_GAME=<title ID>` | build nxnative's sources against that game's profile (`dm/games/<id>`), as nxnative's host is built per game |

## Running

```sh
./build/nxlinux -v path/to/exefs
```

`-v` logs loading, linking and recompiling; `-vv` adds the recompiler's own progress. Recompiled code is kept in
`~/.cache/nxlinux/<level>` (`--cache DIR` to choose, `--no-cache` to skip); the level changes when nxlinux changes what the recompiler emits, so old code is not reused.

## Graphics

The game opens a window (SDL3) and draws with Vulkan. Useful settings:

| Variable | Meaning |
|---|---|
| `NXL_VK_LOG=1` | log translated programs, imported pools, frames written; `2` adds every image, upload and pass |
| `NXL_DUMP_FRAMES=folder` | write presented frames as PNG pictures (every `NXL_DUMP_EVERY`-th, default 60) |
| `NXL_HEADLESS=1` | no window, even with a display |
| `NXL_VK_DEVICE=n` | use the n-th Vulkan device |
| `NXL_SHADER_LOG=1` | the shader recompiler's own messages |
| `NXL_VK_VALIDATE=1` | load the Khronos validation layer if installed (`vulkan-validation-layers`) |

`[vk] [RED] ...` lines name what the backend does not do yet; `shader translation failed` lines name the program.

### OpenGL and Vulkan games

Most games draw with NVN (the Switch's own API, above). A few bring NVIDIA's OpenGL or Vulkan driver for the Switch
with them instead (Dead Cells, LÖVE and HashLink games, some ports). nxlinux does not emulate that driver: the game's
OpenGL, EGL and Vulkan calls go straight to your PC's own driver (GL and Vulkan take the same values on both, shaders
included), and the game's driver is never started. The log says which API a game uses:
`[nxl] graphics: the game draws with OpenGL, on the PC's own driver`; NativeNX shows it beside the version.

- OpenGL: the game's window is an off-screen surface of its layer's size (1920x1080 unless the game sizes it); each
  frame is read back and shown in nxlinux's window like an NVN game's, with FSR, the frame rate counter, screenshots
  and NativeNX's menu. Needs `libglvnd-egl` (installed with any GPU driver).
- Vulkan: the game presents to nxlinux's window itself (the controller and the Home button work; NativeNX's pop-ups,
  FSR and F12 screenshots are not drawn over it yet).

| Variable | Meaning |
|---|---|
| `NXL_GL_LOG=1` | OpenGL: the driver's debug messages, and the names `eglGetProcAddress` did not find |
| `NXL_GL_SIZE=WxH` | OpenGL: the game's window at another size |
| `NXL_GL_SYNC_READ=1` | OpenGL: read each frame back straight away (no one-frame delay; slower) |
| `NXL_EGL_PLATFORM=device` | OpenGL: only this kind of EGL display (`device`, `surfaceless`, `default`) |
| `NXL_EGL_LIBRARY=path` | OpenGL: another libEGL |
| `NXL_VK_LOG=1` | Vulkan: also the driver's debug messages and the names `vkGet*ProcAddr` did not find |
| `NXL_VK_VALIDATION=1` | Vulkan: the game's instance with Khronos' validation layer |
| `NXL_RUN_GPU_DRIVER=1` | start the game's own GPU driver module anyway (for diagnosis) |

## Controls

Controllers SDL knows (Xbox, PlayStation, Switch Pro, 8BitDo, ...) work when plugged in or paired, before or after the
game starts; each one is a player of its own (the first is player 1, and a reconnected controller gets its player
back). Rumble works on controllers that have it.

The face buttons go by position, as on a Switch: an Xbox controller's **B** (right) is the Switch's **A**, **A**
(bottom) is **B**, **Y** (top) is **X** and **X** (left) is **Y**. `NXL_PAD_LAYOUT=labels` maps them by their letters
instead (Xbox A = Switch A).

| Switch | Xbox controller | Keyboard |
|---|---|---|
| A, B, X, Y | B, A, Y, X (by position) | J, K, U, I |
| left stick | left stick | W A S D |
| right stick | right stick | |
| stick clicks | stick clicks | F, G |
| L, R | LB, RB | Q, E |
| ZL, ZR | LT, RT | Z, C |
| +, - | Menu (Start), View (Back) | Return, Tab |
| d-pad | d-pad | arrow keys |
| shake | | Space |

| Variable | Meaning |
|---|---|
| `NXL_PAD_LAYOUT=labels` | face buttons by their letters rather than their position |
| `NXL_DEADZONE=0.12` | the sticks' dead zone, as a fraction of full travel |
| `DM_RUMBLE=0` | no rumble; `DM_RUMBLE_GAIN=1.5` makes it stronger (up to 3) |

Steam, when running, can take an Xbox controller over for itself; if the game sees no controller, close Steam or turn off
Steam Input for it.

## Sound

The game's sound goes to your default output through SDL3 (PipeWire on Fedora), as a stream named "game" from
"nxlinux" in the volume mixer. The sound card's pace drives the game's audio, as on the Switch. Both ways a game
makes sound are taken over: the SDK's audio renderer (most games) and AudioOut, where the game mixes its own sound
and hands over the result (FMOD, Wwise, SDL's Switch driver: Dead Cells). The log has an `audio: ...` line for each.

| Variable | Meaning |
|---|---|
| `NXL_AUDIO=0` | no sound (the game still runs at the same pace) |
| `NXL_AUDIO_VOLUME=0.5` | the game's volume, 0 to 2 |
| `SDL_AUDIO_DRIVER=pulseaudio` | another way to the sound card than SDL's first choice (`pipewire`, `pulseaudio`, `alsa`) |

## Diagnosing wrong colours

`NXL_TRACE_FMT=0x3d` (any NVN format numbers, comma-separated) logs how textures of those formats are used: drawn into,
sampled, written by compute shaders, copied, cleared or uploaded, with the format of the image that holds them and the
channel mapping. It also logs the draws that touch them: the program, the targets' blend state, the textures bound with
their mappings, and the first constants and vertex data. The output holds numbers only, no game code.
`NXL_TRACE_SIZE=1280x720,320x180` narrows it to textures of those sizes (alone, it traces those sizes in any format).
The textures those draws sample (RGBA8, BGRA8 and RGB10A2) are also saved as pictures in `~/nxl_trace`
(`NXL_TRACE_DIR` picks another folder), and constant buffers up to 1 KB are logged whole.

`touch ~/nxl_census` while a scene is on screen logs a census of the next frame's draws by program (draws,
vertices x instances, indirect draws, target, vertex formats, resources); the file is removed when it is done.
`NXL_HIDE_PROG=<program>,...` skips those programs' draws. `touch ~/nxl_hide_cycle` takes a census, then hides each
program that drew into a large target in turn and saves the whole frame at half size for each as `~/nxl_hide/<n>_<program>.png`
(`000_none.png` hides nothing): the picture where something is gone names the program that draws it.
`NXL_DUMP_SPIRV=<program>,...` logs the translated shaders of programs named there, for spirv-dis.

`NXL_OCCLUSION=0` turns occlusion counting off (the samples-passed counter then reports 0).

## Diagnosing a game that stops

| Variable | Meaning |
|---|---|
| `NXL_TRACE='*CreateThread*,*lp649MapMemory*'` | log calls to matching functions (shell patterns on symbol names) with arguments and results |
| `nxlinux --xref main+0x1234 <exefs>` | list the code that refers to a global variable or its GOT slot, marking writers (nothing runs) |
| `nxlinux --dump main+0x1234 64 <exefs>` | print 64 instruction words there (nothing runs) |
| `NXL_SVC_LOG=1` | log every kernel call (SVC) as it returns; `NXL_SVC_LOG=0x04,0x06,0x29` only those numbers |

An SDK abort prints the call chain by name, the thread's last host calls and SVCs, and the code of the function that
gave up with what each of its calls goes to.

## Troubleshooting

- **`cannot reserve the guest's ... region`**: nxlinux maps the game's memory at its Switch addresses, below 512 GiB.
  Something in the process is already there, which is unusual on x86_64 Linux. Check that you are not running it under
  a tool that changes the address-space layout.
- **`the recompiler has no backend for this machine`**: LLVM was built without the X86 target. Fedora's is not.
- **Link errors mentioning `libzstd.a` or `libxml2`**: the LLVM in use was built for static linking against libraries
  that are not installed. Fedora's `llvm-devel` links LLVM as a shared library and does not hit this; an LLVM release
  tarball can. Install `libzstd-devel` and `libxml2-devel`, or point `CMAKE_PREFIX_PATH` at Fedora's LLVM.
- **Guest tests not built (`ld.lld not found`)**: install `lld`.
- **`no Vulkan instance` / `no Vulkan device`**: install your GPU's Vulkan driver (see Packages). `vulkaninfo`
  (`vulkan-tools`) shows what Vulkan sees.
- **`... has no VK_EXT_external_memory_host`**: the driver cannot import the game's memory; NVIDIA's driver, RADV and
  ANV can. The game then runs with nothing drawn.

## Movies (cutscene videos)

Games play their videos through the SDK's movie library, which on the Switch uses NVIDIA's hardware decoder. nxlinux
takes that decoder over (nxnative's `hle_movie.c`) and decodes with the system's FFmpeg, loaded when a movie starts:
H.264, VP8, VP9 and AAC through libavcodec, Vorbis with stb_vorbis. Building needs no FFmpeg.

Fedora's own FFmpeg (`libavcodec-free`) decodes H.264 only through Cisco's OpenH264 library:

    sudo dnf install libavcodec-free openh264

If dnf reports a conflict with `noopenh264`, run `sudo dnf swap noopenh264 openh264`. RPM Fusion's full FFmpeg
(`sudo dnf swap ffmpeg-free ffmpeg --allowerasing`) works too, with FFmpeg's own H.264 decoder.

When a movie starts, the log says what was found, for example `[movie] libavcodec 61.19.100: H.264 libopenh264, ...`.
Without a decoder a movie plays as black pictures and silence for its length, and the game goes on.
`NXL_LIBAVCODEC=/path/libavcodec.so.61` (with `NXL_LIBAVUTIL`) picks a particular library.

## NativeNX (the launcher)

`build/nativenx` (NativeNX) shows your games as a console-style dashboard and starts them; it needs SDL3 (as nxlinux's
window does). The first time, choose the folder that holds your games (each game a folder with `ExeFS` and `RomFS`
inside), or give it once on the command line:

    ./build/nativenx --games "$HOME/Games/Switch"

It remembers the folder (`~/.config/nxlinux/launcher.ini`).

### What a game folder holds

nxlinux runs a game from its files as you extracted them from your own console, with your own tools: it does not
read dump files (.xci, .nsp) or decrypt anything. A game folder holds:

    Breath of the Wild/
      ExeFS/      main, main.npdm, subsdk*, sdk   (with the update you want already applied)
      RomFS/      the game's files
      DLC/1/ ...  each DLC's RomFS in a folder named by its number (optional)
      control.nacp, icon_AmericanEnglish.dat     (optional: the title and icon NativeNX shows)
      mods/       your mods (below)

### BCML mods (Breath of the Wild)

Breath of the Wild mods made with BCML come as `.bnp` files. Unpack one (it is a 7z archive) into the game's mods
folder, its own folder per mod, for example `Breath of the Wild/mods/Linkle/` (with `info.json`, `logs/`,
`01007EF00011E000/romfs/` and maybe `options/` inside). In the game's Options: Mode, With your changes; Mods, the mod
On; its options (as the mod lists them) show under it, each on or off.

When the game starts, NativeNX does what BCML would: the packs the mod changes (`logs/packs.json`) are merged with the
game's own, the mod's options over the mod; the changes BCML logs for the game's parameter files instead of shipping
them (`logs/deepmerge.aamp`, and `logs/aslist.aamp` for animation lists) are merged into those files, in packs and in
packs inside packs; and the game's resource size table is written again as BCML writes it: the mod's sizes
(`logs/rstb.json`) and those of the files the merge made, worked out as BCML does, never below the game's own (BotW
fails to load a resource larger than its size there). The
result is kept in `~/.cache/nxlinux/bnp/` and made again only when a mod, an option or the game's version changes; the
game reads it over its own files like any mod. NativeNX's output names what it merged (`[NativeNX] BCML mods: ...`).
A mod's DLC files (its `01007EF00011F000/romfs/` folder, and logged changes to DLC files) are merged the same way
over the game's DLC folders, and laid over the DLC when the game reads it; with no DLC folder they are left out
(NativeNX's output says so). Not done yet: game data, save data and map merges.

### DLC and saves

A game's DLC are the numbered folders in its `DLC/` folder, each one's RomFS (`DLC/1/`, `DLC/2/` ...; the number is the
DLC's index: its title ID minus the game's, minus 0x1000). All of them are in use; the dashboard shows how many there
are. An update is used by extracting the game with it applied.

A game's Options, DLC and saves:

- **Open the DLC folder**: makes it when it is not there yet.
- **Back up the save**: a copy of your save in `Saves backup/<game>/<date and time> <version>/` in the games folder.
  Backups are never deleted by NativeNX.
- **Restore**: choose a backup, then Restore: your save now is backed up first, then the backup is put back.

From the command line: `DM_ROMFS=<game>/RomFS DM_AOC_DIR=<game>/DLC ./build/nxlinux <game>/ExeFS`.

- Controller: left / right (D-pad or stick) choose a game, A starts it, up goes to Settings and Power, + (or Start)
  opens Settings. A Nintendo-layout pad uses its right-hand A button; Xbox, PlayStation and Steam Deck pads the bottom one.
- Keyboard: arrows, Enter starts, S opens Settings, Esc goes back, F11 switches full screen. The mouse works too
  (click a game, double-click to start, the wheel scrolls).
- The battery shows only on a machine with one (a Steam Deck, a laptop). With the `SteamDeck` variable set (Game Mode)
  the launcher starts full screen; `--fullscreen` does the same anywhere.

What each game shows, from its own files when they are there:

| | from the game's files | or add yourself, in the game's folder |
|---|---|---|
| title, publisher, version | `control.nacp` (in the game folder or a `control` folder in it) | the folder's name is used |
| picture | `icon_AmericanEnglish.dat` (or another `icon_*.dat`) next to `control.nacp` | `cover.png` or `cover.jpg` |
| background | the picture, blurred | `background.png` or `background.jpg` (Settings can set one for all games) |

A game's own nxlinux settings go in `nxlinux.env` in its folder, one `NAME=value` per line (for example
`NXL_TEX_CACHE=0`). Settings some games need are applied by themselves (Breath of the Wild: `NXL_NO_CUTS=main`).
The game's saves go to `save` and `temp` in its folder, as with the commands above, and nxlinux's log to
`~/.local/share/nxlinux/logs/<game folder>.log`. If a game ends with an error, the launcher says so and names the log.

Settings (the gear, or Menu / + / Options / S) is a screen of its own: the games folder, the background and how
much it is blurred (Off, Light, Medium, Strong), full screen, the interface sounds, their volume and style, the controllers
connected. The button pictures are Kenney's (kenney.nl, CC0), in `launcher/data`.

Sound styles: **Soft** (the default) is made by the launcher itself, gentle tones; **Kenney** is Kenney's Interface
Sounds (CC0, `launcher/data/sounds`); **Your own** plays WAV files you put in `~/.local/share/nxlinux/sounds`, named
`move`, `bump`, `confirm`, `back`, `open`, `close`, `launch`, `toggle` and `error` (`.wav`); any you leave out are the
soft ones. A sound pack you download (one whose licence lets you use it but not share it) goes there, not into nxlinux.
`./build/nativenx --export-sounds DIR` writes the soft sounds as WAV files, to hear them or to start from. With the mouse, what
the pointer is over is highlighted; a click selects a game, a click on the selected one starts it.

`./build/nativenx --list` prints what it found (titles, where they came from, pictures, settings) without a window.

To start it from the app menu like any other program, run `./build/nativenx --install` once (or Settings > About >
Add NativeNX to the app menu): it adds NativeNX with its icon. `build/nxlinux-launcher` still starts it too.
On a Steam Deck, add `nativenx` to Steam as a non-Steam game to start it from Game Mode.

### Loading screen

While a game starts, NativeNX shows its picture with what nxlinux is doing, read from the game's log as it is written:
loading the game's code, translating it for this PC (a few minutes on the first start of a game or of new settings
such as 60 fps or a resolution scale; seconds afterwards), starting the game, the first frame. A timer runs and the
last log line is shown at the bottom, so you can see it is working. When the game's window shows its first frame,
NativeNX steps aside. B (Esc) stops the game.

### Profiles, favourites, album, What's new

- **Profiles.** The picture at the top left is the profile in use (the first start makes one named after your login);
  clicking it (or A on it) opens its page. X on an Xbox pad (Y on a Nintendo pad, Square on PlayStation, P on the
  keyboard) opens the profiles window: choose one, or New profile and type a name. The page shows: picture, name, time played, recently
  played games with hours, and buttons to change the picture (any PNG or JPEG; a square from its middle is kept),
  rename, switch or delete the profile. Each profile has its own saves: the first keeps the game's `save` folder (the
  saves made before profiles), others use `save-profile-<number>` beside it. Deleting a profile leaves its saves on
  disk. Kept in `~/.config/nxlinux/profiles.ini` and `~/.config/nxlinux/profiles/<number>/play.ini`.
- **Order.** The game you played last comes first on the dashboard; games never played follow by title.
- **Favourites.** Y on an Xbox pad (X on Nintendo, Triangle, F) stars the selected game; RB (R, R1, G) shows the
  favourites only, and again shows all.
- **Album.** F12 in a game saves a screenshot (PNG) to `~/.local/share/nxlinux/album/<game>/`. The album icon in the
  top bar shows them by game; A views one, X deletes it, Y opens the folder.
- **What's new** (the top bar's news icon) lists what each update brought. The controller icon shows the pads and
  their batteries; the battery also shows in the status bar.
- **Mouse.** Pointing highlights and clicking chooses in the windows and pages too; a click outside a window, or the
  right button, goes back; the wheel scrolls What's new and the album.

### The menu in a game (the Home button)

In a game started from NativeNX, the Home button (Guide on an Xbox pad, PS on a PlayStation pad; Shift+Tab on the
keyboard) opens NativeNX's menu over the game; the game sees no buttons while it is open. B or the Home button again
goes back to the game.

- **Achievements**: the game's list, as on its page (right moves into it, up and down scroll).
- **Take a screenshot**: as F12, into the Album.
- **NativeNX Home**: the game is paused (stopped, its window minimised) and NativeNX shows its dashboard, the game
  marked "Paused". A on it (or the Home button) goes straight back. Starting another game asks to close it first.
- **Close the game**: A twice. As closing its window; what the game had not saved is lost.

### Achievements

Switch games have no achievements of their own, so NativeNX has lists written for them (`launcher/data/achievements/`,
one file per game named by its program ID, plus `general.ini` for every game). They are checked against the game's
save files: while a game runs, NativeNX looks at the profile's save folder every 3 seconds, and when the game has saved
something that meets an achievement, it unlocks with a chime and a pop-up at the top right of the game's window (as on
the PS3). Each has points; the profile's total shows beside the clock, and each profile keeps its own
(`~/.config/nxlinux/profiles/<number>/achievements.ini`).

- **LB** (L1, L; T on the keyboard) on a game shows its achievements: the unlocked ones first with their dates, then the
  rest, with a progress bar for counters (shrines, Korok seeds, monsters). Hidden ones show their name once unlocked.
- **Breath of the Wild** (1.9.0 and the other versions): 75 achievements, 1000 points: WispyWoods' list from Reddit,
  the ones a save can show. They read flags of `game_data.sav` by the game's own names for them, in every save slot.
- **Every game**: First Steps, time played (1, 10, 50, 100 hours), screenshots (1, 25) and making it a favourite.
- **Check what the save shows**, without starting anything: `./build/nativenx --achievements N` (N as `--list`
  numbers the games). Each achievement is listed as unlocked, met, not yet (with its progress), or NOT TRACKED: a flag
  the save does not have (its name is wrong for this version), which is worth reporting.

A list's format is described at the top of `launcher/achieve.h`.

### A game's options (mods, graphics, native)

On a selected game, + (Menu on Xbox, Options on PlayStation, O on the keyboard) opens its options; − (View, Create, S)
opens Settings.

- **Mode**: *Native* (the default) runs the game as it is on the console, nothing below is used. *With your changes*
  uses the graphics and mod choices. The dashboard marks a game that has changes on.
- **Graphics**: anisotropic filtering at least 2x / 4x / 8x / 16x (textures at an angle, such as the ground, stay
  sharp; nxlinux's `NXL_ANISO`). It is applied to the game's own textures only, not to the ones the game draws itself
  (shadow maps, the scene's buffers): forced onto those it drew a line across Breath of the Wild's ground at night. **Frame rate**: *Up to 60 fps* for the games whose version nxnative's frame step
  knows (see "60 fps" below); other games show it as not available. **Resolution**: 1.5x to 4x the game's own
  (see "Resolution scale" below).
- **Mods**: each folder in the game's `mods` folder, on or off. A mod holds the game files it replaces or adds in
  `mods/<name>/romfs/...`, laid out as in the game's RomFS; the layout mods are often packaged in
  (`mods/<name>/atmosphere/contents/<program ID>/romfs/...` or `mods/<name>/contents/<program ID>/romfs/...`) works too.
  Where two mods change one file, the first in the list wins. A mod's code patches (its `exefs` folder: IPSwitch
  `.pchtxt` files, or IPS files named by the game's build ID, as Switch emulators take them) are applied when the game
  loads (nxlinux's `NXL_EXEFS_PATCHES`), only those made for the game's version.
  Mods for the Wii U or other versions of a game do not work: they must be made for the Switch version you have.
- **Info**: the title, version, program ID, folder, log and nxlinux settings.

The options are kept in `~/.config/nxlinux/games/<game folder>.ini`.

## 60 fps (the frame step)

nxnative's frame step lets five games run at up to 60 fps: The Legend of Zelda: Breath of the Wild 1.9.0, Tears of
the Kingdom 1.4.3, Hyrule Warriors: Age of Calamity, Paper Mario: The Thousand-Year Door and ASTRAL CHAIN (the exact
versions nxnative's `games/<program ID>/patches.py` lists; `nativenx --list` says "60 fps: available" for them).

These games move their world by a whole number of screen refreshes (1/60 s) per frame, and pick that number
themselves (2, so 30 fps, on the console). With the frame step, small stubs that nxlinux's loader puts after the game's
`main` hand the game the number nxnative's present code decides instead: 1 (60 fps) while frames are ready within
1/60 s, 2 (30 fps) when they are not, so the game's speed stays real time either way, and the step goes back to 60
when the frames are quick again. The game's own files are not changed; the stubs are made in memory at each start.

In NativeNX: the game's options, Mode *With your changes*, Graphics → Frame rate *Up to 60 fps*. By hand:

    DM_FPS_TARGET=60 ./build/nxlinux -v "$G/ExeFS"

without `NXL_NO_CUTS=main` (the frame step needs the build's registered cuts; with them left out, nxlinux says
`DM_FPS_TARGET=60 is not used`). The log says `main: frame step installed (… stub words, … sites): steps decided by
the host, up to 60 fps`, and while playing `[nvn] frame step: hold 2 → 1 vsyncs …` when it moves between 30 and 60.
To see the frame rate, press **F1** in the game's window (or start it with `NXL_FPS=1`, or turn on Settings →
Appearance → *Frame rate counter in games* in NativeNX): the top left corner shows the frames per second over the last
second and their average time in milliseconds (16.7 at 60 fps, 33.3 at 30).
`DM_FPS_TARGET=30` keeps 30 with the host's pacing; `0` (the default) leaves the game's own step.

The first start with 60 fps recompiles `main` (its code changed at the stub sites), as the very first start did.

## Screen resolution: 1080p, 1440p, 4K (FSR)

Games draw at their own size (Breath of the Wild docked: up to 1600x900). nxlinux scales each frame to the window with
AMD FidelityFX Super Resolution 1.0 (FSR): an edge-aware upscale plus sharpening, much crisper on a 1080p, 1440p or 4K
screen than the plain stretch. For the full resolution of your screen, play full screen: **F11** in the game's window,
or NativeNX Settings → Display → *Games start: Full screen* (`NXL_FULLSCREEN=1` by hand).

- `NXL_UPSCALE=fsr` (the default), `linear` (the old smooth stretch) or `nearest` (sharp pixels).
- `NXL_SHARPNESS=0` strongest … `2` light, `0.25` the default, `off` no sharpening (NativeNX: Display → FSR sharpening).
- A window smaller than the game's frame is scaled down by the smooth stretch whatever the setting.

The shaders are kept as SPIR-V in `gfx/shaders/shaders_spv.h`; `tools/build_shaders.sh` makes it again from
`gfx/shaders/*.comp` (needs glslangValidator).

## Resolution scale (1.5x to 4x)

`NXL_RES_SCALE=1.5`, `2`, `2.5`, `3` or `4` (NativeNX: the game's options, Graphics → Resolution) draws the game's 3D
with that many times more pixels in each direction: Breath of the Wild's 1600x900 becomes 2400x1350 at 1.5x, 3200x1800
at 2x. This is real extra detail, not the stretch of the finished frame (FSR above does that part, and with a scale the
frame may be larger than your screen, which then shrinks it smoothly).

How it works (as in the yuzu emulator): every picture the game draws into of at least 256x128 is made larger the
first time it is drawn into, draws into it are scaled, and the game's shaders are translated so that wherever they
read pixels by position, or ask a picture's size, they still get the game's numbers. Pictures a compute shader
writes, or that the game fills from memory, stay at 1x. `NXL_RES_SCALE_MIN=<w>x<h>` sets the smallest scaled
picture. The log says `resolution scale: target …` for the first pictures made larger.

More pixels are more work for the graphics card: 2x is four times the pixels. Some effects may look different or
wrong at a scale; set it back to 1x (the game's own) to compare.

## Widescreen (21:9 and others)

BotW draws a 16:9 picture. A widescreen mod changes the game's camera to show a wider view in that picture; NativeNX
then stretches the picture to fill your screen (the game's options, Graphics → Picture shape → Fill the window;
`NXL_FILL=1` by hand). The HUD is stretched with it, as in emulators.

For Breath of the Wild 1.9.0, Fl4sh9174's mods (https://github.com/Fl4sh9174/Switch-Emulator-Ultrawide-FPS-Mods,
file `The Legend of Zelda Breath of the Wild [01007EF00011E000][mods].zip`) include `[21.9 Ultrawide v1.9.0]`, made
for 3440x1440. Unzip the file into the game's `mods` folder: each `[...]` folder in it is a mod NativeNX lists. Turn
on `[21.9 Ultrawide v1.9.0]` in Mods and Fill the window in Graphics. A resolution scale of 2x makes up for the
stretch (3200 pixels across, stretched to 3440). The log says `a mod's code patch ...: 9 places`.

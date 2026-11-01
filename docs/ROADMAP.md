# Roadmap

What exists is the foundation: a Switch program's code loads, links, recompiles and runs on x86_64 Linux, on a kernel
that covers start-up, memory, threads and synchronization, with host code (nxnative's included) on both sides of the
call boundary. A commercial game needs much more. The steps are in the order that gets one booting soonest, with sizes
from nxnative's own source as a guide.

## 1. Files: RomFS, saves and extraction (done)

nxnative's `hle_fs.c` is built in, with Linux versions of what it took from Darwin (`compat/nxnative/linux/include`):
`clonefile` is a reflink copy (btrfs, XFS) or a plain copy, `renamex_np(RENAME_SWAP)` is `renameat2(RENAME_EXCHANGE)`,
and its extended attributes live under `user.`. nxnative's `nx_extract` (.NET) extracts on Fedora.

## 2. Booting a retail title

- Try the SDK boot sequence (`main.c`, from nxnative's `dm/host/main.c`) on a real exefs and fix what it meets.
- Port the rest of nxnative's SVC semantics (`hle_svc.c`): `MapPhysicalMemory` in the alias region (the SDK's virtual
  address memory manager), the remaining `GetInfo` ids, `CancelSynchronization`, priorities.
- The SDK-level services are built in (`hle_sdkinit.c`, `hle_sdk16.c`, `hle_sf.c`, `hle_oe.c`, `hle_misc.c`,
  `hle_time.c`, `hle_mii.c`, `hle_ro.c`, `hle_codec.c`, `hle_swkbd.c`, `hle_hid.c`, `hle_audren.c` and others). Still to
  do: NRO loading for `hle_ro.c` (`nro_load` in `compat_dm.c`), and nxnative's guest clock freeze.
- `hle_thread.c` and `hle_os.c` are nxnative's Mac thread and clock layer. nxlinux's `svc.c` / `sync.c` replace the
  former. The guest clock (freeze when in the background, deterministic mode) should be ported from the latter.
- Read the main thread's stack size and priority from the title's NPDM.

## 3. Graphics: NVN on Vulkan

**First version done** (`gfx/`). nxnative's `hle_nvn.c` records NVN commands; `gfx/` replays them on Vulkan behind the
same interface (`nvn_mtl.h`) as nxnative's Metal backend. What is there:

- The game's memory pools are imported into Vulkan (`VK_EXT_external_memory_host`), so GPU addresses stay CPU addresses.
- Shaders are translated **at run time** by yuzu's shader recompiler (`third_party/shader_recompiler`, from the citron
  fork, SPIR-V backend only), not offline: a program is translated at its first draw, by what the draw has bound (the
  NVN driver's handle tables in c2 and the types of the textures they name), and cached by program and by those types.
- Draws, indexed and indirect draws, dispatches, render passes (dynamic rendering), clears, buffer and texture copies,
  uniform / storage buffers, combined and separate textures, samplers, storage images, texture buffers, events,
  counters and syncs. The NVN driver's constant segments are rebuilt per draw as nxnative's Metal replay does.
- Textures are Vulkan images, filled from block-linear guest memory with nxnative's deswizzle code, re-uploaded when the
  CPU writes them (nxnative's write-protect tracking); ASTC is decoded on the CPU (yuzu's decoder) when the GPU lacks it.
- A window with SDL3; `NXL_DUMP_FRAMES` writes frames to files.

Tested with lavapipe (Mesa's software Vulkan) on hand-assembled Maxwell shaders: clears, a triangle with a constant and
an interpolated colour, a uniform buffer, and a CPU-written texture sampled (`tests/guest/t_draw.c`).

Still to do, roughly in order of what games will hit:

- Overlapping the CPU and the GPU: submissions are waited for at commit, which keeps things simple and correct.
- Feedback from the real game: shader translation failures, formats a GPU lacks, aliasing of textures over the same
  memory with different formats (only same-shape, same-class aliases share an image now).
- Transform feedback, conditional rendering, indirect-count multi-draws read on the GPU, sparse textures, MSAA details.
- (Done: the pipeline cache and the translated code are kept on disk, so later starts take seconds.)

ScarletNX's renderer (`src/nx/gpu/vk`) emulates the GPU from its command buffers instead; nxnative's API-level route is
what its 76 games were brought up on, so it is the one followed.

## 4. Audio, input, video

- **Audio (done):** the renderer's 48 kHz blocks (`hle_audren.c`) go to the sound card through SDL3
  (`compat/nxnative/pcmout.c`), whose callback paces the renderer as the card plays; `t_audio` checks every sample and
  the pace through SDL's disk driver. Games that open `nn::audio::AudioOut` directly (`hle_audio.c`, CoreAudio) are
  still to port.
- **Input (done, to try in games):** `hle_hid.c` with an SDL3 front end (`gfx/nxl_input.c`) in place of
  `front_mac.m`'s controller and keyboard code: players by controller, positional face buttons, dead zones, rumble
  through `hle_haptics.c`, release on focus loss. Tested with an SDL virtual Xbox controller (`t_input`). Still to do:
  motion controls from controllers with gyros, and the touch screen from the mouse.
- **Video (done):** `hle_movie.c`'s H.264 and AAC go through the system's FFmpeg (`compat/nxnative/movie_codec.c`,
  loaded at run time: without it, movies play black and silent); VP9 through the bundled libvpx. `t_movie` checks both.

## 5. Games that draw without NVN

Most games draw with NVN, Nintendo's own API, which nxlinux carries out on Vulkan (section 3). Some bring NVIDIA's
OpenGL / OpenGL ES or Vulkan driver inside the game instead (`subsdk*` with `NvGlEs2Init`, `NvEglRegClientApi`, or a
Vulkan loader). Done, the API answered natively rather than the GPU emulated:

- Which API a game draws with, from the functions its modules import: nxlinux's log, NativeNX's list and dashboard.
- OpenGL / OpenGL ES through EGL (`gfx/hle_egl.c`): every GL and EGL function bridged to the PC's driver
  (`tools/gen_gfxapi.py`, from Khronos' registries); the window a pbuffer read back into the Vulkan present.
- Vulkan (`gfx/hle_vk.c`): every function bridged; `VK_NN_vi_surface` a surface of nxlinux's window.
- The game's own GPU driver module is not started.

Next:

- OpenGL frames shared with Vulkan without the read-back (`GL_EXT_memory_object_fd`).
- Vulkan games' frames through nxlinux's present (FSR, pop-ups, screenshots), and their swapchain sized as on the console.
- Games that load precompiled shader binaries for the Switch's GPU (`glProgramBinary`) need their GLSL instead.

## 6. Tooling and packaging

- A Linux counterpart of `scripts/build_ipa_from_nsp.py`: extract, then prepare a run folder with the profile, shaders and
  cache, and launch.
- A Flatpak or AppImage once games run.

## Known limits of the current code

- Thunks do not yet pass homogeneous floating-point aggregates (structs of 2-4 floats) in vector registers.
- `host_callable` pointers handle up to 8 integer and 8 floating-point arguments with scalar or two-register results;
  anything else needs an explicit `GUEST_CALL`.
- No IPC (`SendSyncRequest`): nxnative replaces services at the `nn::` level, so games should rarely reach it.
- Guest faults are reported (address, last guest pc), not delivered to the game's exception handler.
- All kernel waits share one lock and one condition variable.
- Thread priorities and core masks are recorded but Linux schedules the threads.

# ASTC decoder

`astc.cpp` is yuzu's ASTC decoder (citron fork, `src/video_core/textures/astc.cpp`): based on FasTC by Pavel Krajcevski,
The University of North Carolina at Chapel Hill (Apache License 2.0, see the file's header), with yuzu's changes
(GPL-2.0-or-later). nxlinux's change: the block rows are decoded on a few threads of its own instead of yuzu's texture
workers, and a C entry point (`nxl_astc_decode`) is added.

#!/bin/sh
# stands in for nxlinux in the launcher's tests: prints what it was given, and ends with status 3
echo "args: $*"
echo "DM_ROMFS=$DM_ROMFS"
echo "DM_SAVE=$DM_SAVE"
echo "DM_AOC_DIR=$DM_AOC_DIR"
echo "NXL_SEGMENT=$NXL_SEGMENT"
echo "DM_ROMFS_OVERLAY=$DM_ROMFS_OVERLAY"
echo "NXL_ANISO=$NXL_ANISO"
echo "DM_FPS_TARGET=$DM_FPS_TARGET"
echo "NXL_FPS=$NXL_FPS"
echo "NXL_RES_SCALE=$NXL_RES_SCALE"
echo "NXL_EXEFS_PATCHES=$NXL_EXEFS_PATCHES"
echo "NXL_FILL=$NXL_FILL"
echo "NXL_FULLSCREEN=$NXL_FULLSCREEN"
echo "NXL_UPSCALE=$NXL_UPSCALE"
echo "NXL_SHARPNESS=$NXL_SHARPNESS"
echo "NXL_NO_CUTS=${NXL_NO_CUTS-unset}"
exit 3

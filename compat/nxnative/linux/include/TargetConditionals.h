// SPDX-License-Identifier: GPL-3.0-or-later
// Apple's platform macros: nxlinux takes nxnative's desktop (macOS) side, not iOS's
#pragma once
#define TARGET_OS_OSX 1
#define TARGET_OS_MAC 1
#define TARGET_OS_IPHONE 0
#define TARGET_OS_IOS 0
#define TARGET_OS_SIMULATOR 0

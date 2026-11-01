// SPDX-License-Identifier: GPL-3.0-or-later
// The game profile header nxnative's sources expect, for a build without a game (-DNXL_GAME=<title ID> generates the
// real one with nxnative's dm/games/game.py). The keys and their neutral values follow game.py's output.
#pragma once
#define DM_GAME_ID "0000000000000000"
#define DM_GAME_NAME "(no game profile)"
#define DM_GAME_PROGRAM_ID 0x0100000000001000ull
#define DM_GAME_MODULES {"main", "sdk"}
#define DM_GAME_NMOD 2
#define DM_GAME_ROMFS_MARKER ""
#define DM_GAME_MEM_RESERVE 0x140000000ull
#define DM_GAME_MEM_ALIAS 0xb8000000ull
#define DM_GAME_MEM_STACK 0x10000000ull
#define DM_GAME_APP_POOL 0xcd500000ull
#define DM_GAME_MEM_USED 0x4000000ull
#define DM_GAME_SYS_RESOURCE 0x2000000ull
#define DM_GAME_ARGS ""
#define DM_GAME_ENV ""
#define DM_GAME_MEM_MAP_LIMIT 0
#define DM_GAME_DISPLAY_VERSIONS {"", ""}
#define DM_GAME_PRESENT_DISPLAY 0
#define DM_GAME_CORE_PREEMPT 0
#define DM_GAME_ALLOC_BIG_ALIGN 0
#define DM_GAME_FPS_TARGET 0
#define DM_GAME_FPS_HOLD 1
#define DM_GAME_PRESENT_SNAP 0
#define DM_GAME_PSO_MTL4 0
#define DM_GAME_PAD_LAYOUT 0
#define DM_GAME_OPMODE 1
#define DM_GAME_PERF_BOOST 0
#define DM_GAME_FEEDBACK_SNAP ""
#define DM_GAME_TEX_FLUSH_UPLOAD 0
#define DM_GAME_RENDER_COND 0
#define DM_GAME_TEX_DIRTY_UPLOAD 0
#define DM_GAME_TEX_DYN_SLOTS 0
#define DM_GAME_GPU_TS_SEGMENT 0
#define DM_GAME_VCONV_RANGE 0

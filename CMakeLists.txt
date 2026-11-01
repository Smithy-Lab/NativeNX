# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.25)
cmake_policy(SET CMP0083 NEW)   # CMAKE_POSITION_INDEPENDENT_CODE also links a PIE
project(nxlinux VERSION 0.1.0 LANGUAGES C CXX ASM)

if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    message(FATAL_ERROR "nxlinux needs an x86_64 host: the recompiler generates x86_64 code")
endif()
if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "build with Clang (cmake -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++): the recompiler and the guest tests need it")
endif()
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
set(CMAKE_C_STANDARD 11)
# a position-independent executable loads high (0x55...), away from the guest's regions; some distributions' compilers
# do not make one by default
include(CheckPIESupported)
check_pie_supported()
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_C_EXTENSIONS ON)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
# ---- what differs between the systems: Linux here, Windows in cmake/windows.cmake (docs/WINDOWS.md) ----
#   NXL_PLATFORM_SOURCES  src/platform.h's (both programs)    NXL_HOST_SOURCES  src/host.h's and the trampolines (nxlinux)
#   NXL_SHIM_FLAGS        force-included into nxnative's sources and the thunk generator's preprocessor runs
#   NXL_SHIM_INCLUDES     the stand-ins for the Darwin (and on Windows POSIX) headers nxnative's sources include
#   NXL_SYS_LIBS          the system libraries          NXL_BIN_DIR, NXL_SHARE_DIR, NXL_LAUNCHER_DIR: where an install puts things
if(WIN32)
    include(cmake/windows.cmake)
else()
    include(cmake/portable.cmake)   # -DNXL_PORTABLE=ON: for any distribution with glibc 2.34 or newer
    set(NXL_PLATFORM_SOURCES src/platform_posix.c)
    set(NXL_HOST_SOURCES src/host_posix.c src/tramp.S)
    set(NXL_SHIM_FLAGS -include ${CMAKE_CURRENT_SOURCE_DIR}/compat/nxnative/linux/linux_shim.h)
    set(NXL_SHIM_INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/compat/nxnative/linux/include)
    set(NXL_SYS_LIBS m ${CMAKE_DL_LIBS})
    set(NXL_BIN_DIR bin)
    set(NXL_SHARE_DIR share/nxlinux)
    set(NXL_LAUNCHER_DIR share/nativenx)
endif()
list(TRANSFORM NXL_SHIM_INCLUDES PREPEND "-I" OUTPUT_VARIABLE NXL_SHIM_INCLUDE_FLAGS)

# ---- ScarletNX's recompiler library (GPL-3.0): an installed copy, or fetched and built here ----
set(NXL_SCARLETNX_TAG "b5c617592fe81dfd1afb0642d9ebc4b82f16d7bf" CACHE STRING "ScarletNX commit to build the recompiler from")
find_package(ScarletNXRecompiler QUIET)
if(ScarletNXRecompiler_FOUND)
    message(STATUS "ScarletNX recompiler: an installed copy (${ScarletNXRecompiler_DIR}); it needs nxlinux's patches, tools/patch_scarletnx.py")
else()
    message(STATUS "ScarletNX recompiler: building it from ${NXL_SCARLETNX_TAG}")
    include(FetchContent)
    set(SNX_BUILD_NX OFF CACHE BOOL "" FORCE)
    set(SNX_BUILD_GNUABI OFF CACHE BOOL "" FORCE)
    set(SNX_BUILD_3DS OFF CACHE BOOL "" FORCE)
    set(SNX_BUILD_VITA OFF CACHE BOOL "" FORCE)
    set(SNX_BUILD_AGB OFF CACHE BOOL "" FORCE)
    set(SNX_BUILD_GUI OFF CACHE BOOL "" FORCE)
    set(SNX_TOOLS_ONLY ON CACHE BOOL "" FORCE)
    set(SNX_DEPLOY OFF CACHE BOOL "" FORCE)
    # nxlinux's changes to it (tools/patch_scarletnx.py: a narrower store-exclusive succeeds, as on the CPU)
    FetchContent_Declare(scarletnx GIT_REPOSITORY https://github.com/scarletnx/ScarletNX.git GIT_TAG ${NXL_SCARLETNX_TAG}
                         GIT_SHALLOW FALSE EXCLUDE_FROM_ALL
                         PATCH_COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/patch_scarletnx.py .)
    FetchContent_MakeAvailable(scarletnx)
    if(NOT TARGET ScarletNX::recompiler)
        add_library(ScarletNX::recompiler ALIAS scarletnx-recompiler)
    endif()
endif()

# the recompiler's include directories, for the thunk generator's preprocessor runs
set(NXL_SNX_INCLUDES "-I$<JOIN:$<TARGET_PROPERTY:ScarletNX::recompiler,INTERFACE_INCLUDE_DIRECTORIES>,$<SEMICOLON>-I>")

# ---- host implementations: each source compiles together with the thunks tools/gen_thunks.py writes for it ----
set(NXL_HLE_SOURCES src/hle_libc.c src/hle_selftest.c)
set(NXL_HLE_GENERATED)
foreach(src ${NXL_HLE_SOURCES})
    get_filename_component(stem ${src} NAME_WE)
    set(out ${CMAKE_CURRENT_BINARY_DIR}/gen/${stem}.thunks.c)
    add_custom_command(OUTPUT ${out}
                       COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/gen
                       COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_thunks.py ${CMAKE_CURRENT_SOURCE_DIR}/${src} ${out}
                               -- ${CMAKE_C_COMPILER} ${NXL_GEN_FLAGS} -I${CMAKE_CURRENT_SOURCE_DIR}/src ${NXL_SNX_INCLUDES}
                       DEPENDS ${src} tools/gen_thunks.py src/hle.h src/hle_abi.h
                       COMMAND_EXPAND_LISTS
                       COMMENT "Generating the guest-call thunks of ${src}")
    list(APPEND NXL_HLE_GENERATED ${out})
endforeach()

add_library(nxl_lz4 STATIC third_party/lz4/lz4.c)
target_include_directories(nxl_lz4 PUBLIC third_party/lz4)

# ---- nxnative's host sources (MIT), built in unchanged through compat/nxnative ----
# nxnative's own dm.h and game profile header are used as they are; compat/nxnative/hle.h replaces its hle.h, and
# compat/nxnative/compat.c implements the parts of nxnative's host they call that nxlinux provides differently.
set(NXL_NXNATIVE_TAG "583ae42c5964dd17658b14e34a74475a75098792" CACHE STRING "nxnative commit whose host sources are built in")
set(NXL_NXNATIVE_DIR "" CACHE PATH "a local nxnative checkout to use instead of fetching one")
if(NOT NXL_NXNATIVE_DIR)
    include(FetchContent)
    FetchContent_Declare(nxnative GIT_REPOSITORY https://github.com/seedds/nxnative.git GIT_TAG ${NXL_NXNATIVE_TAG} GIT_SHALLOW FALSE)
    FetchContent_GetProperties(nxnative)
    if(NOT nxnative_POPULATED)
        FetchContent_Populate(nxnative)
    endif()
    set(NXL_NXNATIVE_DIR ${nxnative_SOURCE_DIR})
endif()
# the service files ported so far (each needs only what compat.c provides)
set(NXL_NXNATIVE_HLE hle_offline.c hle_friends.c hle_account.c hle_net.c hle_sdkinit.c hle_nvdrv.c hle_sf.c hle_codec.c hle_ro.c hle_mii.c hle_misc.c hle_swkbd.c hle_oe.c hle_sdk16.c hle_audren.c hle_hid.c hle_time.c hle_fdstat.c hle_touch.c hle_haptics.c hle_guestthread.c hle_fs.c hle_movie.c ../gfx/hle_nv.c ../gfx/hle_vi.c ../gfx/nvn/hle_nvn.c)
set(NXL_GAME "" CACHE STRING "title ID of an nxnative game profile (dm/games/<id>) to build for; empty: no game")
if(NXL_GAME)
    set(NXL_PROFILE_DIR ${CMAKE_CURRENT_BINARY_DIR}/profile)
    add_custom_command(OUTPUT ${NXL_PROFILE_DIR}/game_profile.h
                       COMMAND ${CMAKE_COMMAND} -E make_directory ${NXL_PROFILE_DIR}
                       COMMAND Python3::Interpreter ${NXL_NXNATIVE_DIR}/dm/games/game.py header ${NXL_GAME} ${NXL_PROFILE_DIR}/game_profile.h
                       WORKING_DIRECTORY ${NXL_NXNATIVE_DIR}
                       DEPENDS ${NXL_NXNATIVE_DIR}/dm/games/${NXL_GAME}/game.tsv
                       COMMENT "Generating the game profile header of ${NXL_GAME}")
    set(NXL_PROFILE_DEP ${NXL_PROFILE_DIR}/game_profile.h)
else()
    set(NXL_PROFILE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/compat/nxnative/default_profile)
    set(NXL_PROFILE_DEP)
endif()
# how nxnative's sources compile here: Linux stand-ins for the Darwin headers and functions they use first, then the
# compat hle.h before nxnative's own, nxnative's dm.h, the game profile, and nxlinux's hle_abi.h
set(C ${CMAKE_CURRENT_SOURCE_DIR}/compat/nxnative)
set(NXL_NXNATIVE_CFLAGS ${NXL_GEN_FLAGS} ${NXL_SHIM_FLAGS} ${NXL_SHIM_INCLUDE_FLAGS} -I${C} -I${C}/gfx -I${NXL_NXNATIVE_DIR}/dm/host
                        -I${NXL_NXNATIVE_DIR}/dm/gfx/nvn -I${NXL_NXNATIVE_DIR}/dm/gfx/nvn/include -I${NXL_NXNATIVE_DIR}/dm/third_party/stb_vorbis -I${NXL_PROFILE_DIR} -I${CMAKE_CURRENT_SOURCE_DIR}/src -Dguest_sym=dm_compat_guest_sym -Dmodule_of=dm_compat_module_of)
set(NXL_NXNATIVE_GENERATED)
foreach(f ${NXL_NXNATIVE_HLE})
    get_filename_component(stem ${f} NAME_WE)
    # compat/nxnative/patches/<stem>.py: text changes a source needs (tools/gen_thunks.py --patch)
    set(patch_arg)
    set(patch_dep)
    if(EXISTS ${C}/patches/${stem}.py)
        set(patch_arg --patch ${C}/patches/${stem}.py)
        set(patch_dep ${C}/patches/${stem}.py)
    endif()
    set(out ${CMAKE_CURRENT_BINARY_DIR}/gen/nxnative_${stem}.thunks.c)
    add_custom_command(OUTPUT ${out}
                       COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/gen
                       COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_thunks.py ${NXL_NXNATIVE_DIR}/dm/host/${f} ${out}
                               ${patch_arg} -- ${CMAKE_C_COMPILER} ${NXL_NXNATIVE_CFLAGS} ${NXL_SNX_INCLUDES}
                       DEPENDS ${NXL_NXNATIVE_DIR}/dm/host/${f} tools/gen_thunks.py compat/nxnative/hle.h src/hle_abi.h
                               compat/nxnative/linux/linux_shim.h ${NXL_PROFILE_DEP} ${patch_dep}
                       COMMAND_EXPAND_LISTS
                       COMMENT "Generating the guest-call thunks of nxnative's ${f}")
    list(APPEND NXL_NXNATIVE_GENERATED ${out})
endforeach()
# nxlinux's own service files written as nxnative's are (where nxnative's own is for the Mac): AudioOut on pcmout.c
foreach(f hle_audout.c)
    get_filename_component(stem ${f} NAME_WE)
    set(out ${CMAKE_CURRENT_BINARY_DIR}/gen/compat_${stem}.thunks.c)
    add_custom_command(OUTPUT ${out}
                       COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/gen
                       COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_thunks.py ${C}/${f} ${out}
                               -- ${CMAKE_C_COMPILER} ${NXL_NXNATIVE_CFLAGS} ${NXL_SNX_INCLUDES}
                       DEPENDS ${C}/${f} tools/gen_thunks.py compat/nxnative/hle.h src/hle_abi.h compat/nxnative/linux/linux_shim.h ${NXL_PROFILE_DEP}
                       COMMAND_EXPAND_LISTS
                       COMMENT "Generating the guest-call thunks of compat/nxnative/${f}")
    list(APPEND NXL_NXNATIVE_GENERATED ${out})
endforeach()
# NVN's entry point names, from nxnative's generated assembly (only the names are used)
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/gen/nvn_names.c
                   COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/gen
                   COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/tools/nvn_names.py ${NXL_NXNATIVE_DIR}/dm/gfx/nvn/nvn_thunks.S ${CMAKE_CURRENT_BINARY_DIR}/gen/nvn_names.c
                   DEPENDS ${NXL_NXNATIVE_DIR}/dm/gfx/nvn/nvn_thunks.S tools/nvn_names.py
                   COMMENT "Reading NVN's entry point names")
# nxnative's registered cuts of particular module builds (sdk_patches.py, games/*/patches.py), applied by the loader
file(GLOB NXL_PATCH_FILES ${NXL_NXNATIVE_DIR}/dm/games/*/patches.py)
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/gen/nxl_cuts.c
                   COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/gen
                   COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_cuts.py ${NXL_NXNATIVE_DIR}/dm ${CMAKE_CURRENT_BINARY_DIR}/gen/nxl_cuts.c
                   DEPENDS ${NXL_NXNATIVE_DIR}/dm/tools/sdk_patches.py ${NXL_PATCH_FILES} tools/gen_cuts.py
                   COMMENT "Reading nxnative's registered per-build cuts")
# nxnative's frame step stubs (FRAME_STEP of games/*/patches.py): 60 fps in the games that have them, installed by the loader
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/gen/nxl_fstep.c
                   COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/gen
                   COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_fstep.py ${NXL_NXNATIVE_DIR}/dm ${CMAKE_CURRENT_BINARY_DIR}/gen/nxl_fstep.c
                   DEPENDS ${NXL_PATCH_FILES} tools/gen_fstep.py
                   COMMENT "Reading nxnative's frame step stubs")
# nxnative sources with no HLE declarations, compiled where they are (their relative includes find nxnative's headers)
set(NXL_NXNATIVE_PLAIN ${NXL_NXNATIVE_DIR}/dm/gfx/nvn/nvn_tex.c ${NXL_NXNATIVE_DIR}/dm/gfx/nvn/nvn_dirty.c
                       ${NXL_NXNATIVE_DIR}/dm/gfx/tex/nvn_deswizzle.c)
# ---- graphics: the Vulkan backend (gfx/), or the null one (compat/nxnative/nvn_null.c) without Vulkan ----
include(cmake/graphics.cmake)
include(cmake/fonts.cmake)
# OpenGL and Vulkan games: their calls bridged to the PC's driver (gfx/hle_egl.c, gfx/hle_vk.c; tools/gen_gfxapi.py writes
# a bridge for each function in gfx/api/*.sigs, the ones those files do themselves left to them)
if(NXL_VULKAN)
    foreach(src gfx/hle_egl.c gfx/hle_vk.c)
        get_filename_component(stem ${src} NAME_WE)
        set(out ${CMAKE_CURRENT_BINARY_DIR}/gen/${stem}.thunks.c)
        add_custom_command(OUTPUT ${out}
                           COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/gen
                           COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_thunks.py ${CMAKE_CURRENT_SOURCE_DIR}/${src} ${out}
                                   -- ${CMAKE_C_COMPILER} ${NXL_GEN_FLAGS} -I${CMAKE_CURRENT_SOURCE_DIR}/src -I${Vulkan_INCLUDE_DIRS} ${NXL_SNX_INCLUDES}
                           DEPENDS ${src} tools/gen_thunks.py src/hle.h src/hle_abi.h
                           COMMAND_EXPAND_LISTS
                           COMMENT "Generating the guest-call thunks of ${src}")
        list(APPEND NXL_HLE_GENERATED ${out})
    endforeach()
    foreach(api gl egl vk)
        set(hand ${CMAKE_CURRENT_SOURCE_DIR}/gfx/hle_egl.c)
        if(api STREQUAL "vk")
            set(hand ${CMAKE_CURRENT_SOURCE_DIR}/gfx/hle_vk.c)
        endif()
        set(out ${CMAKE_CURRENT_BINARY_DIR}/gen/${api}.bridges.c)
        add_custom_command(OUTPUT ${out}
                           COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/gen
                           COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_gfxapi.py thunks ${api} ${CMAKE_CURRENT_SOURCE_DIR}/gfx/api/${api}.sigs ${out} ${hand}
                           DEPENDS gfx/api/${api}.sigs tools/gen_gfxapi.py ${hand}
                           COMMENT "Generating the ${api} bridges to the PC's driver")
        list(APPEND NXL_HLE_GENERATED ${out})
    endforeach()
endif()
add_library(nxl_nxnative OBJECT ${NXL_NXNATIVE_GENERATED} compat/nxnative/compat.c compat/nxnative/compat_dm.c compat/nxnative/pcmout.c compat/nxnative/movie_codec.c ${NXL_NXNATIVE_HOST_SOURCES}
            ${NXL_GFX_BACKEND_SOURCES} ${CMAKE_CURRENT_BINARY_DIR}/gen/nvn_names.c ${CMAKE_CURRENT_BINARY_DIR}/gen/nxl_cuts.c ${CMAKE_CURRENT_BINARY_DIR}/gen/nxl_fstep.c ${NXL_NXNATIVE_PLAIN})
# order matters: compat/nxnative's hle.h must be found before nxnative's own
target_include_directories(nxl_nxnative PRIVATE ${NXL_SHIM_INCLUDES} compat/nxnative compat/nxnative/gfx ${NXL_NXNATIVE_DIR}/dm/host
                           ${NXL_NXNATIVE_DIR}/dm/gfx/nvn ${NXL_NXNATIVE_DIR}/dm/gfx/nvn/include ${NXL_NXNATIVE_DIR}/dm/third_party/stb_vorbis ${NXL_PROFILE_DIR} src)
set_source_files_properties(${NXL_NXNATIVE_GENERATED} compat/nxnative/compat_dm.c ${NXL_GFX_BACKEND_C} ${NXL_NXNATIVE_PLAIN} PROPERTIES COMPILE_OPTIONS "-include;${C}/linux/linux_shim.h")
target_link_libraries(nxl_nxnative PRIVATE ScarletNX::recompiler ${NXL_GFX_LIBS})
if(NXL_VULKAN)
    target_include_directories(nxl_nxnative PRIVATE gfx ${NXL_NXNATIVE_DIR}/dm/gfx third_party/fsr third_party/stb)
    target_compile_definitions(nxl_nxnative PRIVATE ${NXL_GFX_DEFS})
endif()
target_compile_options(nxl_nxnative PRIVATE -Wall -Wno-unused-function -Wno-unused-variable -Wno-format -Wno-frame-address ${NXL_NXNATIVE_EXTRA_FLAGS})   # nxnative's sources are built unchanged
# nxnative's code calls the functions guest_sym returns: give it native stubs for them (compat.c dm_compat_guest_sym)
set_source_files_properties(${NXL_NXNATIVE_GENERATED} compat/nxnative/compat_dm.c PROPERTIES COMPILE_DEFINITIONS "guest_sym=dm_compat_guest_sym;module_of=dm_compat_module_of")

add_executable(nxlinux src/main.c ${NXL_PLATFORM_SOURCES} ${NXL_HOST_SOURCES} src/memory.c src/loader.c src/fstep.c src/exefs_patch.c src/hle.c src/cpu.c src/svc.c src/sync.c src/profile.c src/xref.c ${NXL_PORTABLE_SOURCES} ${NXL_HLE_GENERATED}
               $<TARGET_OBJECTS:nxl_nxnative>)
target_include_directories(nxlinux PRIVATE src)
target_compile_definitions(nxlinux PRIVATE NXL_DEFAULT_GAMES_DIR="${NXL_NXNATIVE_DIR}/dm/games")   # nxnative's game profiles
target_compile_options(nxlinux PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Wno-missing-field-initializers)
target_link_libraries(nxlinux PRIVATE ScarletNX::recompiler nxl_lz4 Threads::Threads ${NXL_SYS_LIBS} ${NXL_GFX_LIBS})
set_target_properties(nxlinux PROPERTIES LINKER_LANGUAGE CXX)   # the recompiler links LLVM, which is C++
# names in crash reports' host backtraces; the static libraries' symbols (LLVM's, the recompiler's) not exported: a driver
# the program loads that links its own LLVM (Mesa's OpenGL and Vulkan drivers) would otherwise call into this one's
if(NOT WIN32)
    target_link_options(nxlinux PRIVATE -rdynamic -Wl,--exclude-libs,ALL)
endif()
find_package(Threads REQUIRED)
# an install (cmake --install): bin/ the programs; share/nxlinux/ nxnative's game profiles and the button font;
# share/nativenx/ NativeNX's data (each program finds them from where it is: src/platform.h)
install(TARGETS nxlinux RUNTIME DESTINATION ${NXL_BIN_DIR})
install(DIRECTORY ${NXL_NXNATIVE_DIR}/dm/games/ DESTINATION ${NXL_SHARE_DIR}/games FILES_MATCHING PATTERN "game.tsv")
if(NXL_EXT_FONT)
    install(FILES ${NXL_EXT_FONT} DESTINATION ${NXL_SHARE_DIR} OPTIONAL)
endif()

# NativeNX, the dashboard launcher (launcher/): with SDL3
find_package(SDL3 CONFIG QUIET)
if(SDL3_FOUND)
    add_executable(nativenx launcher/main.c launcher/ui_draw.c launcher/ui_assets.c launcher/input.c launcher/launch.c launcher/achievements.c launcher/settings.c launcher/dashboard.c launcher/profiles_ui.c launcher/news_album.c launcher/home_menu.c launcher/library.c launcher/bnp.c launcher/aamp.c launcher/gfxapi.c launcher/profile.c launcher/achieve.c launcher/paths.c ${NXL_PLATFORM_SOURCES} ${NXL_PORTABLE_SOURCES} ${CMAKE_CURRENT_BINARY_DIR}/gen/nxl_fstep.c)
    target_include_directories(nativenx PRIVATE launcher src third_party/stb)
    target_compile_options(nativenx PRIVATE -Wall -Wno-unused-function)
    if(WIN32)   # a windowed program (no console), with its icon and manifest (cmake/windows.cmake)
        set_target_properties(nativenx PROPERTIES WIN32_EXECUTABLE ON)
        target_sources(nativenx PRIVATE packaging/windows/nativenx.rc)
    else()
        # the launcher's first name still starts it
        add_custom_command(TARGET nativenx POST_BUILD COMMAND ${CMAKE_COMMAND} -E create_symlink nativenx ${CMAKE_CURRENT_BINARY_DIR}/nxlinux-launcher VERBATIM)
        target_compile_definitions(nativenx PRIVATE "NXL_LAUNCHER_DATA_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/launcher/data\"")
    endif()
    execute_process(COMMAND git -C ${CMAKE_CURRENT_SOURCE_DIR} log -1 --format=%h\ \(%cs\) OUTPUT_VARIABLE NXL_GIT_VERSION OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(NXL_GIT_VERSION)
        target_compile_definitions(nativenx PRIVATE "NXL_VERSION=\"${NXL_GIT_VERSION}\"")
    endif()
    target_link_libraries(nativenx PRIVATE SDL3::SDL3 nxl_lz4 ${NXL_SYS_LIBS})
    install(TARGETS nativenx RUNTIME DESTINATION ${NXL_BIN_DIR})
    install(DIRECTORY launcher/data/ DESTINATION ${NXL_LAUNCHER_DIR})
else()
    message(WARNING "SDL3 not found (sudo dnf install SDL3-devel): NativeNX (the launcher) is not built")
endif()
if(NOT WIN32)   # (the guest tests' packer: Linux only, as those tests are)
    add_executable(elf2nso tools/elf2nso.c)
    target_link_libraries(elf2nso PRIVATE nxl_lz4)
endif()

# ---- tests: guest programs compiled for the Switch's CPU, packed as NSOs, run by nxlinux ----
include(CTest)
if(BUILD_TESTING AND WIN32)
    include(cmake/windows_tests.cmake)   # the tests that run on Windows (and under Wine)
elseif(BUILD_TESTING)
    # the movie decoders on small clips (tests/data), with the system's FFmpeg if there is one, black and silent without
    # the launcher's achievements: conditions against a made-up save, the shipped lists, a profile's unlocks
    add_executable(t_achieve tests/launcher/t_achieve.c launcher/achieve.c launcher/paths.c src/platform_posix.c)
    target_compile_options(t_achieve PRIVATE -Wall)
    target_include_directories(t_achieve PRIVATE src)
    add_test(NAME t_achieve COMMAND t_achieve ${CMAKE_CURRENT_SOURCE_DIR}/launcher/data)
    # BCML mods (launcher/bnp.c): a made-up game and mod in BotW's and BCML's layouts merged
    # the parsers' fuzzer (docs/DEVELOPING.md): built with -DNXL_FUZZ=ON, with the sanitizers
    option(NXL_FUZZ "Build the parsers' fuzzer (tests/fuzz), with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
    if(NXL_FUZZ)
        add_executable(fuzz_parsers tests/fuzz/fuzz_parsers.c launcher/bnp.c launcher/aamp.c)
        target_include_directories(fuzz_parsers PRIVATE src)
        target_compile_options(fuzz_parsers PRIVATE -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined)
        target_link_options(fuzz_parsers PRIVATE -fsanitize=address,undefined)
        target_link_libraries(fuzz_parsers PRIVATE m)
    endif()
    add_executable(t_bnp tests/launcher/t_bnp.c launcher/bnp.c launcher/aamp.c)
    target_link_libraries(t_bnp PRIVATE m)
    target_compile_options(t_bnp PRIVATE -Wall)
    add_test(NAME t_bnp COMMAND t_bnp ${CMAKE_CURRENT_BINARY_DIR}/t_bnp_work)
    set_tests_properties(t_bnp PROPERTIES PASS_REGULAR_EXPRESSION "nxl-tests: passed" FAIL_REGULAR_EXPRESSION "not ok")
    set_tests_properties(t_achieve PROPERTIES PASS_REGULAR_EXPRESSION "all passed" FAIL_REGULAR_EXPRESSION "not ok")
    if(TARGET nativenx)   # the launcher: library, starting a game (a stand-in nxlinux), its screens drawn offscreen
        add_test(NAME t_launcher COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/launcher/t_launcher.sh $<TARGET_FILE:nativenx>
                 ${CMAKE_CURRENT_SOURCE_DIR}/tests/launcher ${CMAKE_CURRENT_BINARY_DIR}/t_launcher)
        set_tests_properties(t_launcher PROPERTIES PASS_REGULAR_EXPRESSION "nxl-tests: passed" FAIL_REGULAR_EXPRESSION "not ok" TIMEOUT 300)
    endif()
    add_executable(t_movie tests/gfx/t_movie.c compat/nxnative/movie_codec.c)
    target_include_directories(t_movie PRIVATE compat/nxnative ${NXL_NXNATIVE_DIR}/dm/host)
    target_link_libraries(t_movie PRIVATE m ${CMAKE_DL_LIBS} Threads::Threads)
    add_test(NAME t_movie COMMAND t_movie ${CMAKE_CURRENT_SOURCE_DIR}/tests/data)
    add_test(NAME t_movie_no_ffmpeg COMMAND ${CMAKE_COMMAND} -E env NXL_LIBAVCODEC=/nonexistent/libavcodec.so $<TARGET_FILE:t_movie> ${CMAKE_CURRENT_SOURCE_DIR}/tests/data)
    set_tests_properties(t_movie t_movie_no_ffmpeg PROPERTIES PASS_REGULAR_EXPRESSION "nxl-tests: passed" FAIL_REGULAR_EXPRESSION "not ok" TIMEOUT 60)
    # the frame step stubs as the loader moves them after an image, against nxnative's stubs assembled there
    add_executable(t_fstep tests/fstep/t_fstep.c src/fstep.c ${CMAKE_CURRENT_BINARY_DIR}/gen/nxl_fstep.c)
    add_test(NAME t_fstep COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/tests/fstep/t_fstep.py $<TARGET_FILE:t_fstep>
             ${NXL_NXNATIVE_DIR}/dm ${CMAKE_CURRENT_SOURCE_DIR}/tools)
    set_tests_properties(t_fstep PROPERTIES TIMEOUT 60)
    # mods' code patches: IPSwitch text patches and IPS files applied to a made-up module image
    add_executable(t_exefs tests/exefs/t_exefs.c src/exefs_patch.c)
    target_include_directories(t_exefs PRIVATE src $<TARGET_PROPERTY:ScarletNX::recompiler,INTERFACE_INCLUDE_DIRECTORIES>)
    add_test(NAME t_exefs COMMAND t_exefs ${CMAKE_CURRENT_BINARY_DIR}/t_exefs_files)
    set_tests_properties(t_exefs PROPERTIES PASS_REGULAR_EXPRESSION "nxl-tests: passed" FAIL_REGULAR_EXPRESSION "not ok")
    find_program(NXL_LLD NAMES ld.lld HINTS ${LLVM_TOOLS_BINARY_DIR} "${CMAKE_C_COMPILER}/.." PATHS /usr/bin)
    if(NOT NXL_LLD)
        get_filename_component(cc_dir ${CMAKE_C_COMPILER} DIRECTORY)
        find_program(NXL_LLD NAMES ld.lld HINTS ${cc_dir})
    endif()
    if(NOT NXL_LLD)
        message(WARNING "ld.lld not found: the guest tests are not built (install lld)")
    else()
        set(G ${CMAKE_CURRENT_SOURCE_DIR}/tests/guest)
        set(GB ${CMAKE_CURRENT_BINARY_DIR}/guest)
        set(GUEST_CFLAGS --target=aarch64-none-elf -mcpu=cortex-a57 -O2 -ffreestanding -fPIC -fsemantic-interposition
                         -I${NXL_NXNATIVE_DIR}/dm/gfx/nvn/include
                         -fno-stack-protector -fno-builtin-printf -nostdlib -Wall -Wno-unused-function)
        # guest_program(name nso_name sources...): crt0 + sources -> ELF -> NSO
        function(guest_program name)
            set(objs)
            foreach(s crt0.S ${ARGN})
                get_filename_component(stem ${s} NAME_WE)
                set(o ${GB}/${name}.${stem}.o)
                add_custom_command(OUTPUT ${o} COMMAND ${CMAKE_COMMAND} -E make_directory ${GB}
                                   COMMAND ${CMAKE_C_COMPILER} ${GUEST_CFLAGS} -c ${G}/${s} -o ${o}
                                   DEPENDS ${G}/${s} ${G}/nx.h)
                list(APPEND objs ${o})
            endforeach()
            add_custom_command(OUTPUT ${GB}/${name}.elf
                               COMMAND ${NXL_LLD} -shared -T ${G}/guest.ld --hash-style=sysv -z max-page-size=0x1000 -z noexecstack ${objs} -o ${GB}/${name}.elf
                               DEPENDS ${objs} ${G}/guest.ld)
            add_custom_command(OUTPUT ${GB}/${name}.nso ${GB}/${name}.u.nso
                               COMMAND elf2nso ${GB}/${name}.elf ${GB}/${name}.nso
                               COMMAND elf2nso -u ${GB}/${name}.elf ${GB}/${name}.u.nso
                               DEPENDS ${GB}/${name}.elf elf2nso)
            add_custom_target(guest_${name} ALL DEPENDS ${GB}/${name}.nso)
        endfunction()
        guest_program(t_basic t_basic.c)
        guest_program(t_threads t_threads.c)
        guest_program(t_compute t_compute.c)
        guest_program(t_multi_main t_multi_main.c)
        guest_program(t_multi_lib t_multi_lib.c)
        guest_program(t_missing t_missing.c)
        guest_program(t_nxnative t_nxnative.c)
        guest_program(t_fs t_fs.c)
        guest_program(t_audout t_audout.c)
        guest_program(t_dlc t_dlc.c)
        guest_program(t_nvn t_nvn.c)
        guest_program(t_crash t_crash.c)
        guest_program(t_abort t_abort.c)
        guest_program(t_xref t_xref.c)
        guest_program(t_vtable t_vtable.c)
        guest_program(t_spin t_spin.c)
        guest_program(t_lockwait t_lockwait.c)
        guest_program(t_excl t_excl.c)
        guest_program(t_profile t_profile.c)
        guest_program(t_draw t_draw.c)
        if(NXL_VULKAN)
            guest_program(t_gl t_gl.c)
            guest_program(t_gldrv t_gldrv.c)
            set(GUEST_CFLAGS ${GUEST_CFLAGS} -idirafter ${Vulkan_INCLUDE_DIRS})   # Vulkan's headers (plain C), after the compiler's own: the folder may be /usr/include
            guest_program(t_vk t_vk.c)
        endif()

        set(pass "nxl-tests: [0-9]+ passed, 0 failed")
        # drawing, with the Vulkan backend and a Vulkan device (NXL_TEST_VK_ICD: an ICD json to use, such as lavapipe's)
        if(NXL_VULKAN)
            set(NXL_TEST_VK_ICD "" CACHE FILEPATH "the Vulkan driver (ICD json) the drawing tests use; empty: the system's")
            add_test(NAME t_draw COMMAND nxlinux --no-cache ${GB}/t_draw.nso)
            # the same drawing at the resolution scale (NXL_RES_SCALE): targets of 128x128 or more scaled 2x and 1.5x, every
            # target 2x; and a control: with the shaders' rescaling pass left out, the texel-fetch and size checks must fail
            add_test(NAME t_draw_scale2 COMMAND ${CMAKE_COMMAND} -E env NXL_RES_SCALE=2 NXL_RES_SCALE_MIN=128 $<TARGET_FILE:nxlinux> --no-cache ${GB}/t_draw.nso)
            add_test(NAME t_draw_scale15 COMMAND ${CMAKE_COMMAND} -E env NXL_RES_SCALE=1.5 NXL_RES_SCALE_MIN=128 $<TARGET_FILE:nxlinux> --no-cache ${GB}/t_draw.nso)
            add_test(NAME t_draw_scale_all COMMAND ${CMAKE_COMMAND} -E env NXL_RES_SCALE=2 NXL_RES_SCALE_MIN=1 $<TARGET_FILE:nxlinux> --no-cache ${GB}/t_draw.nso)
            add_test(NAME t_draw_scale_control COMMAND ${CMAKE_COMMAND} -E env NXL_RES_SCALE=2 NXL_RES_SCALE_MIN=128 NXL_TEST_NO_RESCALING_PASS=1 $<TARGET_FILE:nxlinux> --no-cache ${GB}/t_draw.nso)
            # an OpenGL game's calls bridged to the PC's driver (gfx/hle_egl.c): EGL and OpenGL ES on Mesa's software
            # driver where there is one (NXL_TEST_EGL_LIBRARY: a libEGL to use; empty: the system's)
            set(NXL_TEST_EGL_LIBRARY "" CACHE FILEPATH "the libEGL the OpenGL test uses; empty: the system's")
            add_test(NAME t_gl COMMAND nxlinux --no-cache ${GB}/t_gl.nso)
            set_tests_properties(t_gl PROPERTIES PASS_REGULAR_EXPRESSION "${pass}" FAIL_REGULAR_EXPRESSION "not ok|\\[RED\\] fatal" TIMEOUT 120
                                 ENVIRONMENT "NXL_HEADLESS=1;NXL_EGL_LIBRARY=${NXL_TEST_EGL_LIBRARY};NXL_DUMP_FRAMES=${CMAKE_CURRENT_BINARY_DIR}/t_gl_frames;NXL_DUMP_EVERY=1;LIBGL_ALWAYS_SOFTWARE=1;VK_ICD_FILENAMES=${NXL_TEST_VK_ICD}")
            # the same with a stand-in for the game's own GPU driver beside it, whose entry points must not be called
            add_test(NAME t_gl_driver COMMAND nxlinux --no-cache ${GB}/t_gl.nso ${GB}/t_gldrv.nso)
            get_test_property(t_gl ENVIRONMENT t_gl_env)
            set_tests_properties(t_gl_driver PROPERTIES PASS_REGULAR_EXPRESSION "${pass}" FAIL_REGULAR_EXPRESSION "not ok|\\[RED\\] fatal" TIMEOUT 120
                                 ENVIRONMENT "${t_gl_env}")
            if(TARGET nativenx)   # NativeNX's graphics check on these programs
                add_test(NAME t_gfxapi COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/launcher/t_gfxapi.sh $<TARGET_FILE:nativenx> ${GB} ${CMAKE_CURRENT_BINARY_DIR}/t_gfxapi)
                set_tests_properties(t_gfxapi PROPERTIES PASS_REGULAR_EXPRESSION "nxl-tests: passed" FAIL_REGULAR_EXPRESSION "not ok")
            endif()
            # a Vulkan game's calls bridged to the PC's driver (gfx/hle_vk.c), on the drawing tests' driver, headless
            add_test(NAME t_vk COMMAND nxlinux --no-cache ${GB}/t_vk.nso)
            set_tests_properties(t_vk PROPERTIES PASS_REGULAR_EXPRESSION "${pass}" FAIL_REGULAR_EXPRESSION "not ok|\\[RED\\] fatal" TIMEOUT 120
                                 ENVIRONMENT "NXL_HEADLESS=1;VK_ICD_FILENAMES=${NXL_TEST_VK_ICD}")
            set(NXL_DRAW_TESTS t_draw t_draw_scale2 t_draw_scale15 t_draw_scale_all t_draw_scale_control)
            set_tests_properties(${NXL_DRAW_TESTS} PROPERTIES PASS_REGULAR_EXPRESSION "${pass}" FAIL_REGULAR_EXPRESSION "\\[RED\\] fatal" TIMEOUT 300)
            set_tests_properties(t_draw t_draw_scale2 t_draw_scale15 t_draw_scale_all PROPERTIES FAIL_REGULAR_EXPRESSION "not ok|\\[RED\\] fatal")
            set_tests_properties(t_draw_scale_control PROPERTIES WILL_FAIL TRUE)
            if(NXL_TEST_VK_ICD)
                set_tests_properties(${NXL_DRAW_TESTS} PROPERTIES ENVIRONMENT "VK_ICD_FILENAMES=${NXL_TEST_VK_ICD};NXL_HEADLESS=1")
            else()
                set_tests_properties(${NXL_DRAW_TESTS} PROPERTIES ENVIRONMENT "NXL_HEADLESS=1")
            endif()
            # frames shown in a window: SDL's offscreen video driver over VK_EXT_headless_surface (the test driver has it)
            if(NXL_TEST_VK_ICD AND SDL3_FOUND)
                add_test(NAME t_window COMMAND nxlinux --no-cache ${GB}/t_nvn.nso)
                add_test(NAME t_upscale COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/gfx/t_upscale.sh $<TARGET_FILE:nxlinux> ${GB}/t_nvn.nso ${CMAKE_CURRENT_BINARY_DIR}/t_upscale)
                set_tests_properties(t_upscale PROPERTIES PASS_REGULAR_EXPRESSION "nxl-tests: passed" FAIL_REGULAR_EXPRESSION "not ok" TIMEOUT 120
                                     ENVIRONMENT "VK_ICD_FILENAMES=${NXL_TEST_VK_ICD}")
                add_test(NAME t_window_shot COMMAND sh -c "rm -rf '${CMAKE_CURRENT_BINARY_DIR}/t_album' && NXL_TEST_SHOT=3 NXL_ALBUM_DIR='${CMAKE_CURRENT_BINARY_DIR}/t_album/game' $<TARGET_FILE:nxlinux> --no-cache ${GB}/t_nvn.nso && ls '${CMAKE_CURRENT_BINARY_DIR}/t_album/game/'*.png && echo 'screenshot saved'")   # F12: a screenshot into the album
                set_tests_properties(t_window_shot PROPERTIES PASS_REGULAR_EXPRESSION "screenshot saved" TIMEOUT 120
                                     ENVIRONMENT "VK_ICD_FILENAMES=${NXL_TEST_VK_ICD};NXL_HEADLESS=0;SDL_VIDEO_DRIVER=offscreen")
                # an achievement pop-up (NativeNX's picture) drawn over a blue image by the window's overlay, the pixels checked
                add_test(NAME t_window_overlay COMMAND sh -c "python3 -c \"import struct,sys; open(sys.argv[1],'wb').write(b'NXLIMG01'+struct.pack('<II',880,184)+bytes([200,40,40,255])*(880*184))\" '${CMAKE_CURRENT_BINARY_DIR}/t_overlay.rgba' && NXL_TEST_OVERLAY='${CMAKE_CURRENT_BINARY_DIR}/t_overlay.rgba' $<TARGET_FILE:nxlinux> --no-cache ${GB}/t_nvn.nso")
                set_tests_properties(t_window_overlay PROPERTIES PASS_REGULAR_EXPRESSION "nxl-overlay-test: done" FAIL_REGULAR_EXPRESSION "not ok|\\[RED\\] fatal" TIMEOUT 120
                                     ENVIRONMENT "VK_ICD_FILENAMES=${NXL_TEST_VK_ICD};NXL_HEADLESS=0;SDL_VIDEO_DRIVER=offscreen")
                add_test(NAME t_window_fps COMMAND ${CMAKE_COMMAND} -E env NXL_FPS=1 $<TARGET_FILE:nxlinux> --no-cache ${GB}/t_nvn.nso)   # with the frame rate counter
                set_tests_properties(t_window t_window_fps PROPERTIES PASS_REGULAR_EXPRESSION "first frame shown in the window" FAIL_REGULAR_EXPRESSION "not ok|\\[RED\\] fatal|\\[vk\\] \\[RED\\]" TIMEOUT 120
                                     ENVIRONMENT "VK_ICD_FILENAMES=${NXL_TEST_VK_ICD};NXL_HEADLESS=0;SDL_VIDEO_DRIVER=offscreen")
            endif()
            add_executable(t_shader tests/gfx/t_shader.c gfx/nxl_shader.cpp)
            target_link_libraries(t_shader PRIVATE shader_recompiler)
            add_test(NAME t_shader COMMAND t_shader)
            set_tests_properties(t_shader PROPERTIES PASS_REGULAR_EXPRESSION "nxl-tests: passed" FAIL_REGULAR_EXPRESSION "not ok")
            if(SDL3_FOUND)
                add_executable(t_input tests/gfx/t_input.c gfx/nxl_input.c)
                target_include_directories(t_input PRIVATE gfx ${NXL_NXNATIVE_DIR}/dm/host)
                target_link_libraries(t_input PRIVATE SDL3::SDL3 m)
                add_test(NAME t_input COMMAND t_input)
                add_executable(t_audio tests/gfx/t_audio.c compat/nxnative/pcmout.c)
                target_compile_definitions(t_audio PRIVATE NXL_HAVE_SDL3=1)
                target_link_libraries(t_audio PRIVATE SDL3::SDL3 m)
                add_test(NAME t_audio COMMAND t_audio ${CMAKE_CURRENT_BINARY_DIR})
                set_tests_properties(t_audio PROPERTIES PASS_REGULAR_EXPRESSION "nxl-tests: passed" FAIL_REGULAR_EXPRESSION "not ok" TIMEOUT 60)
                set_tests_properties(t_input PROPERTIES PASS_REGULAR_EXPRESSION "nxl-tests: passed" FAIL_REGULAR_EXPRESSION "not ok" TIMEOUT 60)
            endif()
        endif()
        set(fail_re "not ok|\\[RED\\] fatal")
        foreach(t t_basic t_threads t_compute t_nxnative t_nvn)
            add_test(NAME ${t} COMMAND nxlinux --no-cache ${GB}/${t}.nso)
            set_tests_properties(${t} PROPERTIES PASS_REGULAR_EXPRESSION "${pass}" FAIL_REGULAR_EXPRESSION "${fail_re}" TIMEOUT 300)
        endforeach()
        # the condition variable test again with a pause where a wrong order would lose a wake-up (sync.c NXL_TEST_CV_GAP)
        add_test(NAME t_threads_cv_gap COMMAND ${CMAKE_COMMAND} -E env NXL_TEST_CV_GAP=50 $<TARGET_FILE:nxlinux> --no-cache ${GB}/t_threads.nso)
        set_tests_properties(t_threads_cv_gap PROPERTIES PASS_REGULAR_EXPRESSION "${pass}" FAIL_REGULAR_EXPRESSION "${fail_re}" TIMEOUT 300)
        add_test(NAME t_basic_uncompressed COMMAND nxlinux --no-cache ${GB}/t_basic.u.nso)
        add_test(NAME t_multi_module COMMAND nxlinux --no-cache ${GB}/t_multi_main.nso ${GB}/t_multi_lib.nso)
        add_test(NAME t_trace COMMAND ${CMAKE_COMMAND} -E env NXL_TRACE=lib_scale,main_offset $<TARGET_FILE:nxlinux> --no-cache ${GB}/t_multi_main.nso ${GB}/t_multi_lib.nso)
        set_tests_properties(t_trace PROPERTIES PASS_REGULAR_EXPRESSION "> lib_scale\\([^\n]*\n[^\n]*> main_offset[^\n]*\n[^\n]*< main_offset = 0x73\n[^\n]*< lib_scale = 0x73")
        add_test(NAME t_missing_import COMMAND nxlinux --no-cache ${GB}/t_missing.nso)
        add_test(NAME t_code_cache_fill COMMAND nxlinux --cache ${CMAKE_CURRENT_BINARY_DIR}/test-cache ${GB}/t_compute.nso)
        add_test(NAME t_code_cache_reuse COMMAND nxlinux --cache ${CMAKE_CURRENT_BINARY_DIR}/test-cache ${GB}/t_compute.nso)
        add_test(NAME t_exefs_folder COMMAND ${CMAKE_COMMAND} -E env sh -c
                 "rm -rf ${GB}/exefs && mkdir -p ${GB}/exefs && cp ${GB}/t_multi_main.nso ${GB}/exefs/main && cp ${GB}/t_multi_lib.nso ${GB}/exefs/subsdk0 && exec $<TARGET_FILE:nxlinux> --no-cache ${GB}/exefs")
        set_tests_properties(t_basic_uncompressed t_multi_module t_code_cache_fill t_code_cache_reuse t_exefs_folder PROPERTIES
                             PASS_REGULAR_EXPRESSION "${pass}" FAIL_REGULAR_EXPRESSION "${fail_re}" TIMEOUT 300)
        set_tests_properties(t_code_cache_reuse PROPERTIES DEPENDS t_code_cache_fill)
        # nn::fs: a RomFS folder to read, a save folder to commit to, and afterwards the committed file checked on disk
        set(FSDIR ${CMAKE_CURRENT_BINARY_DIR}/test-fs)
        add_test(NAME t_fs COMMAND ${CMAKE_COMMAND} -E env sh -c
                 "rm -rf ${FSDIR} && mkdir -p ${FSDIR}/romfs/data ${FSDIR}/save ${FSDIR}/temp && printf 'hello from the romfs\\n' > ${FSDIR}/romfs/data/hello.txt && DM_ROMFS=${FSDIR}/romfs DM_SAVE=${FSDIR}/save DM_TEMP=${FSDIR}/temp $<TARGET_FILE:nxlinux> --no-cache ${GB}/t_fs.nso && grep -q 'WORLD 3-2 CLEAR!' ${FSDIR}/save/progress.bin && echo 'committed save is on disk'")
        add_test(NAME t_audout COMMAND ${CMAKE_COMMAND} -E env NXL_AUDIO=0 $<TARGET_FILE:nxlinux> --no-cache ${GB}/t_audout.nso)
        set_tests_properties(t_audout PROPERTIES PASS_REGULAR_EXPRESSION "nxl-tests: [0-9]+ passed, 0 failed" FAIL_REGULAR_EXPRESSION "not ok" TIMEOUT 60)
        # a game's DLC (DM_AOC_DIR/<index>/, each one's RomFS extracted), with mods' DLC files over it (DM_AOC_OVERLAY)
        set(DLCW ${CMAKE_CURRENT_BINARY_DIR}/t_dlc_files)
        add_test(NAME t_dlc_folder COMMAND ${CMAKE_COMMAND} -E env sh -c
                 "rm -rf ${DLCW} && mkdir -p ${DLCW}/romfs/data ${DLCW}/romfs/zzz ${DLCW}/aoc/2/dlc ${DLCW}/aocmod/dlc ${DLCW}/save ${DLCW}/temp && printf 'HELLO from the patch\\n' > ${DLCW}/romfs/data/hello.txt && printf 'x\\n' > ${DLCW}/romfs/data/other.txt && printf 'new in 2.0.0\\n' > ${DLCW}/romfs/zzz/new.txt && printf 'dlc two\\n' > ${DLCW}/aoc/2/dlc/item.txt && printf 'kept\\n' > ${DLCW}/aoc/2/dlc/kept.txt && printf 'mod item\\n' > ${DLCW}/aocmod/dlc/item.txt && printf 'mod extra\\n' > ${DLCW}/aocmod/dlc/extra.txt && DM_ROMFS=${DLCW}/romfs DM_AOC_DIR=${DLCW}/aoc DM_AOC_OVERLAY=${DLCW}/aocmod DM_SAVE=${DLCW}/save DM_TEMP=${DLCW}/temp $<TARGET_FILE:nxlinux> --no-cache ${GB}/t_dlc.nso")
        set_tests_properties(t_dlc_folder PROPERTIES PASS_REGULAR_EXPRESSION "nxl-tests: [0-9]+ passed, 0 failed" FAIL_REGULAR_EXPRESSION "not ok|FATAL")
        set_tests_properties(t_fs PROPERTIES PASS_REGULAR_EXPRESSION "nxl-tests: [0-9]+ passed, 0 failed[^\n]*\n.*committed save is on disk"
                             FAIL_REGULAR_EXPRESSION "${fail_re}" TIMEOUT 300)
        # the program dies by the signal (CTest fails any such test), so a shell runs it and the report is what is judged
        add_test(NAME t_crash_report COMMAND sh -c "$<TARGET_FILE:nxlinux> --no-cache ${GB}/t_crash.nso 2>&1; true")
        set_tests_properties(t_crash_report PROPERTIES PASS_REGULAR_EXPRESSION "segmentation fault at address 0xc0[^\n]*\n  in [^\n]*crash_here\\+[^\n]*\n  lr [^\n]*crash_caller\\+")
        add_test(NAME t_excl COMMAND nxlinux --no-cache ${GB}/t_excl.nso)
        set_tests_properties(t_excl PROPERTIES PASS_REGULAR_EXPRESSION "ok - ldxr w / stxr w" FAIL_REGULAR_EXPRESSION "not ok" TIMEOUT 30)
        add_test(NAME t_lockwait COMMAND nxlinux -v --no-cache ${GB}/t_lockwait.nso)
        set_tests_properties(t_lockwait PROPERTIES PASS_REGULAR_EXPRESSION "waiting for the lock at 0x[0-9a-f]+, held by thread 8[0-9]\n      from t_lockwait.nso\\+0x" FAIL_REGULAR_EXPRESSION "not ok" TIMEOUT 60)
        add_test(NAME t_spin_sample COMMAND nxlinux -v --no-cache ${GB}/t_spin.nso)
        set_tests_properties(t_spin_sample PROPERTIES PASS_REGULAR_EXPRESSION "running guest code[^\n]*\n +in [^\n]*spin_here\\+[^\n]*\n +from t_spin.nso\\+0x" TIMEOUT 60)
        add_test(NAME t_vtable COMMAND nxlinux -v --no-cache ${GB}/t_vtable.nso)
        set_tests_properties(t_vtable PROPERTIES PASS_REGULAR_EXPRESSION "ok - functions reached only through a table" FAIL_REGULAR_EXPRESSION "not ok|JIT: the first function|\\[RED\\] fatal")
        add_test(NAME t_xref COMMAND nxlinux --no-cache --xref t_xref.nso+0x2110 ${GB}/t_xref.nso)
        set_tests_properties(t_xref PROPERTIES PASS_REGULAR_EXPRESSION "xref_setter\\+0xc\\): load the slot  <-- WRITES[^\n]*\n[^\n]*\n[^\n]*\n  [^\n]*xref_reader\\+0x4\\): load the slot\n")
        add_test(NAME t_abort_report COMMAND sh -c "$<TARGET_FILE:nxlinux> --no-cache ${GB}/t_abort.nso 2>&1; true")
        set_tests_properties(t_abort_report PROPERTIES PASS_REGULAR_EXPRESSION "call chain:\n  at [^\n]*abort_inner\\+[^\n]*\n  lr [^\n]*abort_outer\\+[^\n]*\n  #0 [^\n]*test_main\\+")
        # GetInfo from the title's NPDM and its game profile: an exefs with a main.npdm, a profile in a games folder
        set(PD ${CMAKE_CURRENT_BINARY_DIR}/test-profile)
        add_test(NAME t_profile COMMAND ${CMAKE_COMMAND} -E env sh -c
                 "rm -rf ${PD} && mkdir -p ${PD}/exefs ${PD}/games/0100ABCD00001000 && cp ${GB}/t_profile.nso ${PD}/exefs/main && ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/make_npdm.py ${PD}/exefs/main.npdm 0100abcd00001000 0 0x200000 44 && printf 'title_id\\t0100ABCD00001000\\nmem_alias\\t0xB8000000\\nmem_stack\\t0x10000000\\nmem_reserve\\t0x140000000\\napp_pool\\t0xCD500000\\n' > ${PD}/games/0100ABCD00001000/game.tsv && NXL_GAMES_DIR=${PD}/games exec $<TARGET_FILE:nxlinux> --no-cache ${PD}/exefs")
        set_tests_properties(t_profile PROPERTIES PASS_REGULAR_EXPRESSION "alias b8000000 stack 10000000 sysres 2000000 program 100abcd00001000 pool cd500000 coremask 7\n(.|\n)*nxl-tests: [0-9]+ passed, 0 failed"
                             FAIL_REGULAR_EXPRESSION "${fail_re}" TIMEOUT 300)
        set_tests_properties(t_missing_import PROPERTIES PASS_REGULAR_EXPRESSION "nn_not_implemented_anywhere, which no module defines")
    endif()
endif()

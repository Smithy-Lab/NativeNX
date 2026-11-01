# SPDX-License-Identifier: GPL-3.0-or-later
# The tests that run on Windows, or on Linux under Wine (cmake -DCMAKE_CROSSCOMPILING_EMULATOR=wine): the parts with no
# shell scripts around them. tests/launcher/t_launcher_win.sh runs NativeNX's own under Wine.
function(nxl_win_test name)   # a test program, with the manifest (UTF-8 names) the programs have
    add_executable(${name} ${ARGN} packaging/windows/manifest.rc)
    target_compile_options(${name} PRIVATE -Wall)
endfunction()
nxl_win_test(t_bnp tests/launcher/t_bnp.c launcher/bnp.c launcher/aamp.c)
add_test(NAME t_bnp COMMAND t_bnp ${CMAKE_CURRENT_BINARY_DIR}/t_bnp_work)
nxl_win_test(t_achieve tests/launcher/t_achieve.c launcher/achieve.c launcher/paths.c)
target_include_directories(t_achieve PRIVATE src)
add_test(NAME t_achieve COMMAND t_achieve ${CMAKE_CURRENT_SOURCE_DIR}/launcher/data)
nxl_win_test(t_exefs tests/exefs/t_exefs.c src/exefs_patch.c)
target_include_directories(t_exefs PRIVATE src $<TARGET_PROPERTY:ScarletNX::recompiler,INTERFACE_INCLUDE_DIRECTORIES>)
add_test(NAME t_exefs COMMAND t_exefs ${CMAKE_CURRENT_BINARY_DIR}/t_exefs_files)
nxl_win_test(fake_nxlinux tests/launcher/fake_nxlinux.c)   # (tests/launcher/t_launcher_win.sh)
target_include_directories(fake_nxlinux PRIVATE src)
set_tests_properties(t_bnp t_exefs PROPERTIES PASS_REGULAR_EXPRESSION "nxl-tests: passed" FAIL_REGULAR_EXPRESSION "not ok")
set_tests_properties(t_achieve PROPERTIES PASS_REGULAR_EXPRESSION "all passed" FAIL_REGULAR_EXPRESSION "not ok")

# The guest test programs (tests/guest), run by nxlinux.exe: they are compiled for the Switch's CPU by a Linux build,
# whose guest/ folder NXL_GUEST_DIR names (the same files on both systems). Those that need files made first run through
# a shell, so only where the build runs on Linux (with Wine as CMAKE_CROSSCOMPILING_EMULATOR)
set(NXL_GUEST_DIR "" CACHE PATH "a Linux build's guest/ folder: the guest test programs, for nxlinux.exe to run")
if(NXL_GUEST_DIR)
    set(GB ${NXL_GUEST_DIR})
    set(pass "nxl-tests: [0-9]+ passed, 0 failed")
    set(fail_re "not ok|\\[RED\\] fatal")
    foreach(t t_basic t_threads t_compute t_nxnative t_nvn)
        add_test(NAME ${t} COMMAND nxlinux --no-cache ${GB}/${t}.nso)
    endforeach()
    add_test(NAME t_threads_cv_gap COMMAND ${CMAKE_COMMAND} -E env NXL_TEST_CV_GAP=50 ${CMAKE_CROSSCOMPILING_EMULATOR} $<TARGET_FILE:nxlinux> --no-cache ${GB}/t_threads.nso)
    add_test(NAME t_basic_uncompressed COMMAND nxlinux --no-cache ${GB}/t_basic.u.nso)
    add_test(NAME t_multi_module COMMAND nxlinux --no-cache ${GB}/t_multi_main.nso ${GB}/t_multi_lib.nso)
    add_test(NAME t_code_cache_fill COMMAND nxlinux --cache ${CMAKE_CURRENT_BINARY_DIR}/test-cache ${GB}/t_compute.nso)
    add_test(NAME t_code_cache_reuse COMMAND nxlinux --cache ${CMAKE_CURRENT_BINARY_DIR}/test-cache ${GB}/t_compute.nso)
    add_test(NAME t_audout COMMAND ${CMAKE_COMMAND} -E env NXL_AUDIO=0 ${CMAKE_CROSSCOMPILING_EMULATOR} $<TARGET_FILE:nxlinux> --no-cache ${GB}/t_audout.nso)
    set_tests_properties(t_basic t_threads t_compute t_nxnative t_nvn t_threads_cv_gap t_basic_uncompressed t_multi_module
                         t_code_cache_fill t_code_cache_reuse t_audout PROPERTIES
                         PASS_REGULAR_EXPRESSION "${pass}" FAIL_REGULAR_EXPRESSION "${fail_re}" TIMEOUT 300)
    set_tests_properties(t_code_cache_reuse PROPERTIES DEPENDS t_code_cache_fill)
    add_test(NAME t_missing_import COMMAND nxlinux --no-cache ${GB}/t_missing.nso)
    set_tests_properties(t_missing_import PROPERTIES PASS_REGULAR_EXPRESSION "nn_not_implemented_anywhere, which no module defines")
    add_test(NAME t_excl COMMAND nxlinux --no-cache ${GB}/t_excl.nso)
    set_tests_properties(t_excl PROPERTIES PASS_REGULAR_EXPRESSION "ok - ldxr w / stxr w" FAIL_REGULAR_EXPRESSION "not ok" TIMEOUT 60)
    add_test(NAME t_lockwait COMMAND nxlinux -v --no-cache ${GB}/t_lockwait.nso)
    set_tests_properties(t_lockwait PROPERTIES PASS_REGULAR_EXPRESSION "waiting for the lock at 0x[0-9a-f]+, held by thread 8[0-9]" FAIL_REGULAR_EXPRESSION "not ok" TIMEOUT 60)
    add_test(NAME t_vtable COMMAND nxlinux -v --no-cache ${GB}/t_vtable.nso)
    set_tests_properties(t_vtable PROPERTIES PASS_REGULAR_EXPRESSION "ok - functions reached only through a table" FAIL_REGULAR_EXPRESSION "not ok|JIT: the first function|\\[RED\\] fatal")
    add_test(NAME t_xref COMMAND nxlinux --no-cache --xref t_xref.nso+0x2110 ${GB}/t_xref.nso)
    set_tests_properties(t_xref PROPERTIES PASS_REGULAR_EXPRESSION "xref_setter\\+0xc\\): load the slot  <-- WRITES")
    # drawing with the Vulkan backend (under Wine: its Vulkan over the Linux driver NXL_TEST_VK_ICD names, lavapipe's)
    if(NXL_VULKAN)
        set(NXL_TEST_VK_ICD "" CACHE FILEPATH "the Vulkan driver (ICD json) the drawing tests use; empty: the system's")
        add_test(NAME t_draw COMMAND nxlinux --no-cache ${GB}/t_draw.nso)
        add_test(NAME t_draw_scale2 COMMAND ${CMAKE_COMMAND} -E env NXL_RES_SCALE=2 NXL_RES_SCALE_MIN=128 ${CMAKE_CROSSCOMPILING_EMULATOR} $<TARGET_FILE:nxlinux> --no-cache ${GB}/t_draw.nso)
        set_tests_properties(t_draw t_draw_scale2 PROPERTIES PASS_REGULAR_EXPRESSION "${pass}" FAIL_REGULAR_EXPRESSION "${fail_re}" TIMEOUT 300
                             ENVIRONMENT "NXL_HEADLESS=1;VK_ICD_FILENAMES=${NXL_TEST_VK_ICD}")
        # (not here: a window, and t_vk's surface. On Linux they draw to VK_EXT_headless_surface through SDL's offscreen
        # driver; on Windows SDL wants VK_KHR_win32_surface, which Wine passes to the Linux driver's X11 surface, and the
        # test driver has none)
    endif()
    if(CMAKE_HOST_UNIX AND CMAKE_CROSSCOMPILING_EMULATOR)
        string(REPLACE ";" " " EMU "${CMAKE_CROSSCOMPILING_EMULATOR}")   # (a list: into one command line for sh)
        set(NX "${EMU} $<TARGET_FILE:nxlinux>")
        # the crash and abort reports (the program ends with the exception; the report is what is judged)
        add_test(NAME t_crash_report COMMAND sh -c "${NX} --no-cache ${GB}/t_crash.nso 2>&1; true")
        set_tests_properties(t_crash_report PROPERTIES PASS_REGULAR_EXPRESSION "segmentation fault[^\n]* at address 0+c0[^\n]*\n  in [^\n]*crash_here\\+[^\n]*\n  lr [^\n]*crash_caller\\+")
        add_test(NAME t_abort_report COMMAND sh -c "${NX} --no-cache ${GB}/t_abort.nso 2>&1; true")
        set_tests_properties(t_abort_report PROPERTIES PASS_REGULAR_EXPRESSION "call chain:\n  at [^\n]*abort_inner\\+[^\n]*\n  lr [^\n]*abort_outer\\+")
        # nn::fs: a RomFS to read, a save to commit to (copied and swapped on Windows), the committed file checked
        set(FSDIR ${CMAKE_CURRENT_BINARY_DIR}/test-fs)
        add_test(NAME t_fs COMMAND sh -c
                 "rm -rf ${FSDIR} && mkdir -p ${FSDIR}/romfs/data ${FSDIR}/save ${FSDIR}/temp && printf 'hello from the romfs\\n' > ${FSDIR}/romfs/data/hello.txt && DM_ROMFS=${FSDIR}/romfs DM_SAVE=${FSDIR}/save DM_TEMP=${FSDIR}/temp ${NX} --no-cache ${GB}/t_fs.nso && grep -q 'WORLD 3-2 CLEAR!' ${FSDIR}/save/progress.bin && echo 'committed save is on disk'")
        # a game's DLC from its extracted folders (DM_AOC_DIR/<index>/), mods' DLC files over it
        set(DLCW ${CMAKE_CURRENT_BINARY_DIR}/t_dlc_files)
        add_test(NAME t_dlc_folder COMMAND sh -c
                 "rm -rf ${DLCW} && mkdir -p ${DLCW}/romfs/data ${DLCW}/romfs/zzz ${DLCW}/aoc/2/dlc ${DLCW}/aocmod/dlc ${DLCW}/save ${DLCW}/temp && printf 'HELLO from the patch\\n' > ${DLCW}/romfs/data/hello.txt && printf 'x\\n' > ${DLCW}/romfs/data/other.txt && printf 'new in 2.0.0\\n' > ${DLCW}/romfs/zzz/new.txt && printf 'dlc two\\n' > ${DLCW}/aoc/2/dlc/item.txt && printf 'kept\\n' > ${DLCW}/aoc/2/dlc/kept.txt && printf 'mod item\\n' > ${DLCW}/aocmod/dlc/item.txt && printf 'mod extra\\n' > ${DLCW}/aocmod/dlc/extra.txt && DM_ROMFS=${DLCW}/romfs DM_AOC_DIR=${DLCW}/aoc DM_AOC_OVERLAY=${DLCW}/aocmod DM_SAVE=${DLCW}/save DM_TEMP=${DLCW}/temp ${NX} --no-cache ${GB}/t_dlc.nso")
        set_tests_properties(t_dlc_folder PROPERTIES PASS_REGULAR_EXPRESSION "nxl-tests: [0-9]+ passed, 0 failed" FAIL_REGULAR_EXPRESSION "not ok|FATAL")
        set_tests_properties(t_fs PROPERTIES PASS_REGULAR_EXPRESSION "nxl-tests: [0-9]+ passed, 0 failed[^\n]*\n.*committed save is on disk" FAIL_REGULAR_EXPRESSION "${fail_re}" TIMEOUT 300)
    endif()
endif()
if(CMAKE_HOST_UNIX AND CMAKE_CROSSCOMPILING_EMULATOR AND TARGET nativenx)   # NativeNX itself, under Wine
    string(REPLACE ";" " " EMU "${CMAKE_CROSSCOMPILING_EMULATOR}")
    add_test(NAME t_launcher_win COMMAND ${CMAKE_COMMAND} -E env "WINE=${EMU}" sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/launcher/t_launcher_win.sh
             ${CMAKE_CURRENT_BINARY_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/tests/launcher ${CMAKE_CURRENT_BINARY_DIR}/t_launcher_win)
    set_tests_properties(t_launcher_win PROPERTIES PASS_REGULAR_EXPRESSION "nxl-tests: passed" FAIL_REGULAR_EXPRESSION "not ok" TIMEOUT 300)
endif()

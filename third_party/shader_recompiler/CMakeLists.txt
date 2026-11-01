# SPDX-License-Identifier: GPL-3.0-or-later
# yuzu's Maxwell shader recompiler (GPL-2.0-or-later, from the citron fork), SPIR-V backend only: Maxwell machine code to
# SPIR-V at run time. common/ holds the few yuzu headers it needs, with logging and settings replaced (see LICENSE.md)
file(GLOB_RECURSE NXL_SHREC_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shader_recompiler/*.cpp)
add_library(shader_recompiler STATIC ${NXL_SHREC_SOURCES})
target_include_directories(shader_recompiler PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(shader_recompiler PUBLIC cxx_std_23)
target_link_libraries(shader_recompiler PUBLIC fmt::fmt sirit Boost::headers)
set_target_properties(shader_recompiler PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_options(shader_recompiler PRIVATE -w $<$<CXX_COMPILER_ID:Clang>:-fbracket-depth=1024>)

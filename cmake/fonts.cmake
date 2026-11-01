# The button icons games draw in their text (Press (A) to start): nxnative's stand-in for the console's "Nintendo
# extension" shared font, generated with fontTools from a bold system font into the build folder, where nxlinux looks
# for it (DM_SHARED_FONT_EXT overrides). Without fontTools or a bold font the icons show as '?'.
execute_process(COMMAND ${Python3_EXECUTABLE} -c "import fontTools" RESULT_VARIABLE NXL_NO_FONTTOOLS OUTPUT_QUIET ERROR_QUIET)
find_file(NXL_ICON_LETTER_FONT NAMES LiberationSans-Bold.ttf DejaVuSans-Bold.ttf NotoSans-Bold.ttf arialbd.ttf
          PATHS /usr/share/fonts/liberation-sans-fonts /usr/share/fonts/liberation-sans /usr/share/fonts/truetype/liberation
                /usr/share/fonts/dejavu-sans-fonts /usr/share/fonts/dejavu /usr/share/fonts/truetype/dejavu
                /usr/share/fonts/google-noto /usr/share/fonts/noto /usr/share/fonts/truetype/noto
                "$ENV{WINDIR}/Fonts" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)   # (the build machine's fonts, when cross-compiling too)
if(NXL_NO_FONTTOOLS)
    message(WARNING "fontTools not found (sudo dnf install python3-fonttools): button icons in game text will show as '?'")
elseif(NOT NXL_ICON_LETTER_FONT)
    message(WARNING "no bold TrueType font for the button icons (sudo dnf install liberation-sans-fonts): they will show as '?'")
else()
    set(NXL_EXT_FONT ${CMAKE_CURRENT_BINARY_DIR}/nintendo_ext.ttf)
    add_custom_command(OUTPUT ${NXL_EXT_FONT}
                       COMMAND ${Python3_EXECUTABLE} ${NXL_NXNATIVE_DIR}/dm/tools/make_ext_font.py ${NXL_EXT_FONT} ${NXL_ICON_LETTER_FONT}
                       DEPENDS ${NXL_NXNATIVE_DIR}/dm/tools/make_ext_font.py COMMENT "Button icon font (nintendo_ext.ttf)" VERBATIM)
    add_custom_target(nxl_ext_font ALL DEPENDS ${NXL_EXT_FONT})
    message(STATUS "button icons: from ${NXL_ICON_LETTER_FONT}")
endif()

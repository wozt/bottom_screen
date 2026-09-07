# The shared C, as one list.
#
# Every emulator that carries the bridge compiles the same files, and
# they were listed separately in each of the three build systems. Adding
# a file then meant remembering three places -- which failed twice:
# libswresample was added to one and missed in the others, and bs_ws.c
# broke all three at once without anyone noticing, because the standalone
# Makefile still built.
#
# So the list lives here, beside the sources it names.
#
#   include("${BOTTOM_SCREEN_DIR}/bottom_screen.cmake")
#   bottom_screen_sources(BS_SOURCES)          # the C files
#   bottom_screen_attach(my_target)            # includes, libraries, define

find_package(PkgConfig REQUIRED)
pkg_check_modules(BOTTOMSCREEN_FFMPEG REQUIRED
                  libavcodec libavutil libswscale libswresample)

# The web page is compiled in, so there is no file to install and no
# path to get wrong relative to wherever an emulator was launched from.
# Generated into the build tree rather than committed, because a
# generated file in the repository is one that can quietly go stale.
set(BOTTOM_SCREEN_GENERATED "${CMAKE_CURRENT_BINARY_DIR}/bottom_screen_generated")
file(MAKE_DIRECTORY "${BOTTOM_SCREEN_GENERATED}")

add_custom_command(
    OUTPUT "${BOTTOM_SCREEN_GENERATED}/web_page.h"
    COMMAND ${CMAKE_COMMAND} -E env python3
            "${BOTTOM_SCREEN_DIR}/scripts/embed_page.py"
            "${BOTTOM_SCREEN_DIR}/web/index.html"
            "${BOTTOM_SCREEN_GENERATED}/web_page.h"
    DEPENDS "${BOTTOM_SCREEN_DIR}/web/index.html"
            "${BOTTOM_SCREEN_DIR}/scripts/embed_page.py"
    COMMENT "bottom_screen: embedding the web page")

add_custom_target(bottom_screen_web_page
                  DEPENDS "${BOTTOM_SCREEN_GENERATED}/web_page.h")

function(bottom_screen_sources out)
    set(${out}
        "${BOTTOM_SCREEN_DIR}/bs_server.c"
        "${BOTTOM_SCREEN_DIR}/bs_encoder.c"
        "${BOTTOM_SCREEN_DIR}/bs_audio.c"
        "${BOTTOM_SCREEN_DIR}/bs_net.c"
        "${BOTTOM_SCREEN_DIR}/bs_mailbox.c"
        "${BOTTOM_SCREEN_DIR}/bs_ws.c"
        PARENT_SCOPE)
endfunction()

function(bottom_screen_attach target)
    target_include_directories(${target} PRIVATE
        "${BOTTOM_SCREEN_DIR}"
        "${BOTTOM_SCREEN_GENERATED}"
        ${BOTTOMSCREEN_FFMPEG_INCLUDE_DIRS})
    target_link_libraries(${target} PRIVATE
        ${BOTTOMSCREEN_FFMPEG_LIBRARIES} pthread m)
    target_compile_definitions(${target} PRIVATE BOTTOM_SCREEN_ENABLED)
    add_dependencies(${target} bottom_screen_web_page)
endfunction()

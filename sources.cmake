# Shared engine of the app (CMakeLists.txt) and of the command line tool (agent/CMakeLists.txt):
# every source but main.c. One list, so a new file reaches both programs.
#
# What differs between the systems: the window (app_window.c: Windows driver, app_window_sdl.c:
# SDL on macOS / Linux), the serial ports (serial_win.c / serial_posix.c) and the rest of the
# system calls (platform_win.c / platform_posix.c, see Inc/platform.h).

set(APP_DIR ${CMAKE_CURRENT_LIST_DIR})

set(APP_SOURCES
    ${APP_DIR}/Src/screens.c
    ${APP_DIR}/Src/serial_common.c
    ${APP_DIR}/Src/plotter.c
    ${APP_DIR}/Src/line_parser.c
    ${APP_DIR}/Src/profile.c
    ${APP_DIR}/Src/plot_export.c
    ${APP_DIR}/Src/test_data.c
    ${APP_DIR}/Src/test_bench.c
    ${APP_DIR}/Src/compare.c
    ${APP_DIR}/Src/settings.c
    ${APP_DIR}/Src/cmd_panel.c
    ${APP_DIR}/Src/lang.c
    ${APP_DIR}/Src/ui_widgets.c
)

if(WIN32)
    set(APP_PLATFORM_SOURCES ${APP_DIR}/Src/platform_win.c)
    list(APPEND APP_SOURCES ${APP_DIR}/Src/serial_win.c ${APP_DIR}/Src/app_window.c ${APP_PLATFORM_SOURCES})
    set(APP_LIBRARIES lvgl gdi32 user32 setupapi comdlg32 ole32 dwmapi shell32)
else()
    # macOS / Linux: the window is SDL2's (LVGL's SDL driver; LVGL also finds SDL2 for itself)
    set(APP_PLATFORM_SOURCES ${APP_DIR}/Src/platform_posix.c)
    list(APPEND APP_SOURCES ${APP_DIR}/Src/serial_posix.c ${APP_DIR}/Src/app_window_sdl.c ${APP_PLATFORM_SOURCES})

    find_package(Threads REQUIRED)
    find_package(SDL2 QUIET)

    if(TARGET SDL2::SDL2)
        set(APP_SDL2 SDL2::SDL2)
    else()
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(APP_SDL2_PKG REQUIRED IMPORTED_TARGET sdl2)
        set(APP_SDL2 PkgConfig::APP_SDL2_PKG)
    endif()

    set(APP_LIBRARIES lvgl ${APP_SDL2} Threads::Threads m)
endif()

set(APP_INCLUDE_DIRS ${APP_DIR} ${APP_DIR}/Inc)

# The project's own files only (not LVGL)
set(APP_WARNINGS -Wall -Wextra)

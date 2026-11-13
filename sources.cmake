# Shared engine of the app (CMakeLists.txt) and of the command line tool (agent/CMakeLists.txt):
# every source but main.c. One list, so a new file reaches both programs.

set(APP_DIR ${CMAKE_CURRENT_LIST_DIR})

set(APP_SOURCES
    ${APP_DIR}/Src/screens.c
    ${APP_DIR}/Src/serial.c
    ${APP_DIR}/Src/plotter.c
    ${APP_DIR}/Src/line_parser.c
    ${APP_DIR}/Src/plot_export.c
    ${APP_DIR}/Src/test_data.c
    ${APP_DIR}/Src/test_bench.c
    ${APP_DIR}/Src/compare.c
    ${APP_DIR}/Src/settings.c
    ${APP_DIR}/Src/app_window.c
    ${APP_DIR}/Src/cmd_panel.c
    ${APP_DIR}/Src/lang.c
    ${APP_DIR}/Src/ui_widgets.c
)

set(APP_INCLUDE_DIRS ${APP_DIR} ${APP_DIR}/Inc)

set(APP_LIBRARIES lvgl gdi32 user32 setupapi comdlg32 ole32 dwmapi shell32)

# The project's own files only (not LVGL)
set(APP_WARNINGS -Wall -Wextra)

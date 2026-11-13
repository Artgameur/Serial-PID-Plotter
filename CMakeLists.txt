cmake_minimum_required(VERSION 3.20)

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

project(serial_dashboard LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_subdirectory(lvgl)

include(sources.cmake)

add_executable(serial_dashboard WIN32 main.c ${APP_SOURCES})

target_include_directories(serial_dashboard PRIVATE ${APP_INCLUDE_DIRS})
target_compile_options(serial_dashboard PRIVATE ${APP_WARNINGS})

# Program icon (Assets/app.ico, found by the resource compiler through the include directories);
# without the file, the default icon is used
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/Assets/app.ico)
    enable_language(RC)
    target_sources(serial_dashboard PRIVATE Assets/app.rc)
    # Rebuilt when the icon changes
    set_source_files_properties(Assets/app.rc PROPERTIES OBJECT_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/Assets/app.ico)
endif()

target_link_libraries(serial_dashboard PRIVATE ${APP_LIBRARIES})

# Tests of the engine without the UI (console program): run serial_dashboard_tests.exe or ctest.
# SERIAL_DASHBOARD_CLI: the command line tool's direct CSV write (plotExportCliWrite).
enable_testing()
add_executable(serial_dashboard_tests
    tests/tests.c
    ${APP_DIR}/Src/line_parser.c
    ${APP_DIR}/Src/test_data.c
    ${APP_DIR}/Src/plot_export.c
    ${APP_DIR}/Src/app_window.c
    ${APP_DIR}/Src/settings.c
    ${APP_DIR}/Src/lang.c
)
target_compile_definitions(serial_dashboard_tests PRIVATE SERIAL_DASHBOARD_CLI)
target_include_directories(serial_dashboard_tests PRIVATE ${APP_INCLUDE_DIRS})
target_compile_options(serial_dashboard_tests PRIVATE ${APP_WARNINGS})
target_link_libraries(serial_dashboard_tests PRIVATE ${APP_LIBRARIES})
add_test(NAME engine COMMAND serial_dashboard_tests)

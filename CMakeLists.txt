cmake_minimum_required(VERSION 3.20)

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

project(serial_dashboard LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Strict -std=c11 hides the POSIX calls (glibc): LVGL's pthread code and ours need them
if(NOT WIN32)
    add_compile_definitions(_DEFAULT_SOURCE)
endif()

add_subdirectory(lvgl)

include(sources.cmake)

# WIN32: a Windows program (no console window); ignored on macOS / Linux
add_executable(serial_dashboard WIN32 main.c ${APP_SOURCES})

target_include_directories(serial_dashboard PRIVATE ${APP_INCLUDE_DIRS})
target_compile_options(serial_dashboard PRIVATE ${APP_WARNINGS})

# Program icon (Assets/app.ico, found by the resource compiler through the include directories);
# without the file, the default icon is used
if(WIN32 AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/Assets/app.ico)
    enable_language(RC)
    target_sources(serial_dashboard PRIVATE Assets/app.rc)
    # Rebuilt when the icon changes
    set_source_files_properties(Assets/app.rc PROPERTIES OBJECT_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/Assets/app.ico)
endif()

target_link_libraries(serial_dashboard PRIVATE ${APP_LIBRARIES})

# Profiling build (Inc/profile.h): -DSD_PROFILE=ON, in its own build folder. Off: no profiling code.
# Windows only for now (it measures with Windows calls).
option(SD_PROFILE "Time spent per stage, written to a CSV file once per second" OFF)
if(SD_PROFILE)
    if(NOT WIN32)
        message(FATAL_ERROR "SD_PROFILE: the profiling build is Windows only for now")
    endif()
    target_compile_definitions(serial_dashboard PRIVATE SD_PROFILE)
endif()

# Tests of the engine without the UI (console program): run serial_dashboard_tests or ctest.
# SERIAL_DASHBOARD_CLI: the command line tool's direct CSV write (plotExportCliWrite).
enable_testing()
add_executable(serial_dashboard_tests
    tests/tests.c
    ${APP_DIR}/Src/line_parser.c
    ${APP_DIR}/Src/test_data.c
    ${APP_DIR}/Src/plot_export.c
    ${APP_DIR}/Src/settings.c
    ${APP_DIR}/Src/lang.c
    ${APP_PLATFORM_SOURCES}
)
target_compile_definitions(serial_dashboard_tests PRIVATE SERIAL_DASHBOARD_CLI)
target_include_directories(serial_dashboard_tests PRIVATE ${APP_INCLUDE_DIRS})
target_compile_options(serial_dashboard_tests PRIVATE ${APP_WARNINGS})
target_link_libraries(serial_dashboard_tests PRIVATE ${APP_LIBRARIES})
add_test(NAME engine COMMAND serial_dashboard_tests)

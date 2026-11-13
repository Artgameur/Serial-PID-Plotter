# Command line tool for AI agents: imports test CSV files and writes pictures of the Compare graphs.
#
# Its own CMake project, on purpose: the app's CMakeLists.txt does not know about it, so nothing here
# can affect the serial_dashboard build. It compiles the app's sources with SERIAL_DASHBOARD_CLI.
# Build it with build_cli.bat.

cmake_minimum_required(VERSION 3.20)

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

project(serial_dashboard_cli LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# The app's source list (sets APP_DIR, APP_SOURCES, ...)
include(${CMAKE_CURRENT_SOURCE_DIR}/../sources.cmake)

# The lv_conf.h of the app, not of this folder
set(LV_BUILD_CONF_DIR ${APP_DIR} CACHE PATH "" FORCE)

add_subdirectory(${APP_DIR}/lvgl ${CMAKE_BINARY_DIR}/lvgl)

# All of the app but main.c: the Compare tab needs little of the rest at run time, but links with it
add_executable(serial_dashboard_cli
    serial_dashboard_cli.c
    cli_session.c
    ${APP_SOURCES}
)

target_compile_definitions(serial_dashboard_cli PRIVATE SERIAL_DASHBOARD_CLI)
target_include_directories(serial_dashboard_cli PRIVATE ${APP_INCLUDE_DIRS})
target_compile_options(serial_dashboard_cli PRIVATE ${APP_WARNINGS})
target_link_libraries(serial_dashboard_cli PRIVATE ${APP_LIBRARIES})

# One file to hand over: no DLL of the compiler needed
target_link_options(serial_dashboard_cli PRIVATE -static)

cmake_minimum_required(VERSION 3.20)

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

project(serial_dashboard LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_subdirectory(lvgl)

add_executable(serial_dashboard WIN32
    main.c
    Src/lv_port_disp.c
    Src/lv_port_indev.c
    Src/screens.c
    Src/Serial_Reception.c
    Src/plotter.c
    Src/plot_export.c
    Src/test_data.c
    Src/test_bench.c
    Src/compare.c
    Src/settings.c
    Src/app_window.c
    Src/cmd_panel.c
    Src/lang.c
    Src/ui_widgets.c
    Assets/frame_0001.c
    Assets/frame_0002.c
    Assets/frame_0003.c
    Assets/frame_0004.c
    Assets/robot_arm_img.c
)

target_include_directories(serial_dashboard PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/Inc
)

# Program icon (Assets/app.ico, found by the resource compiler through the include directories);
# without the file, the default icon is used
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/Assets/app.ico)
    enable_language(RC)
    target_sources(serial_dashboard PRIVATE Assets/app.rc)
    # Rebuilt when the icon changes
    set_source_files_properties(Assets/app.rc PROPERTIES OBJECT_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/Assets/app.ico)
endif()

target_link_libraries(serial_dashboard PRIVATE lvgl gdi32 user32 setupapi comdlg32 ole32 dwmapi shell32)

set_target_properties(serial_dashboard PROPERTIES
    CMAKE_C_STANDARD 11
    CMAKE_C_STANDARD_REQUIRED ON
)

cmake_minimum_required(VERSION 3.21)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64")
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

project(xdBot2 VERSION 1.0.0)
 
add_library(${PROJECT_NAME} SHARED
    src/main.cpp
    src/global.cpp
    src/macro.cpp
    src/keybinds.cpp
    src/utils/utils.cpp
    src/gdr/gdr.cpp
    src/renderer/renderer.cpp
    src/renderer/native_ffmpeg.cpp

    src/hacks/other.cpp
    src/hacks/tps_bypass.cpp
    src/hacks/frame_stepper.cpp
    src/hacks/layout_mode.cpp
    src/hacks/show_trajectory.cpp
    src/hacks/coin_finder.cpp
    src/hacks/clickbot.cpp
    src/hacks/autoclicker.cpp

    src/ui/macro_editor.cpp
    src/ui/button_edit_layer.cpp
    src/ui/game_ui.cpp
    src/ui/record_layer.cpp
    src/ui/render_settings_layer.cpp
    src/ui/load_macro_layer.cpp
    src/ui/clickbot_layer.cpp
    src/ui/button_setting.cpp

    src/practice_fixes/input.cpp
    src/practice_fixes/player.cpp
    src/practice_fixes/play_layer.cpp
)

# native_ffmpeg.cpp tiene que incluir <winsock2.h> antes que <windows.h>. Con PCH activo
# (Geode lo prende cuando usas sccache + Clang, como en CI), el PCH de Geode.hpp se
# inyecta antes de la línea 1 y mete <windows.h> -> <winsock.h> primero, lo que choca
# con winsock2 (redefinición de sockaddr, fd_set, IPPROTO_*...). Este archivo va sin PCH.
set_source_files_properties(src/renderer/native_ffmpeg.cpp PROPERTIES
    SKIP_PRECOMPILE_HEADERS ON
    SKIP_UNITY_BUILD_INCLUSION ON
)

if (NOT DEFINED ENV{GEODE_SDK})
    message(FATAL_ERROR "Unable to find Geode SDK! Please define GEODE_SDK environment variable to point to Geode")
else()
    message(STATUS "Found Geode: $ENV{GEODE_SDK}")
endif()

add_subdirectory($ENV{GEODE_SDK} ${CMAKE_CURRENT_BINARY_DIR}/geode)

setup_geode_mod(${PROJECT_NAME})
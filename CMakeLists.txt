cmake_minimum_required(VERSION 3.16)
project(ScreenCast VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# Find Qt6
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network Multimedia)

# Find FFmpeg
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED
    libavcodec
    libavformat
    libavutil
    libswscale
)

# Include directories
include_directories(${FFMPEG_INCLUDE_DIRS})
link_directories(${FFMPEG_LIBRARY_DIRS})

# Unified App
qt_add_executable(screencast
    main.cpp
    LauncherWindow.h
    server_gui/MainWindow.cpp
    server_gui/MainWindow.h
    server_gui/ServerComponents.cpp
    server_gui/ServerComponents.h
    client_gui/MainWindow.cpp
    client_gui/MainWindow.h
    client_gui/ClientComponents.cpp
    client_gui/ClientComponents.h
    common/H265Codec.cpp
    common/H265Codec.h
    common/protocol.h
)
target_include_directories(screencast PRIVATE 
    ${CMAKE_CURRENT_SOURCE_DIR}/common
    ${CMAKE_CURRENT_SOURCE_DIR}/server_gui
    ${CMAKE_CURRENT_SOURCE_DIR}/client_gui
    ${FFMPEG_INCLUDE_DIRS}
)
target_link_libraries(screencast PRIVATE 
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network Qt6::Multimedia
    vulkan X11 Xtst asound
    ${FFMPEG_LIBRARIES}
)

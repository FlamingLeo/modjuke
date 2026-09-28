cmake_minimum_required(VERSION 3.22)
project(modjuke VERSION 1.0.0 LANGUAGES CXX)

# Targets Qt 6.4.2 (Linux Mint 22 / Ubuntu 24.04); builds against newer too.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Widgets Multimedia DBus)

qt_standard_project_setup()

add_library(modjuke_core STATIC
    src/reveal.cpp
    src/openmptapi.cpp
    src/config.cpp
    src/stores.cpp
    src/shuffles.cpp
    src/library.cpp
    src/theme.cpp
    src/themeeditor.cpp
    resources/arrows.qrc
    src/engine.cpp
    src/analyzer.cpp
    src/widgets.cpp
    src/queue.cpp
    src/dialogs.cpp
    src/mainwindow.cpp
)
target_include_directories(modjuke_core PUBLIC src)
target_link_libraries(modjuke_core PUBLIC
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Multimedia Qt6::DBus)

add_executable(modjuke src/main.cpp resources/app.qrc)
target_link_libraries(modjuke PRIVATE modjuke_core)

include(GNUInstallDirs)

# Use the absolute installed icon path in the desktop entry. This avoids icon
# theme cache/lookup differences in desktop environments such as Cinnamon.
set(MODJUKE_ICON_PATH "${CMAKE_INSTALL_FULL_DATADIR}/icons/hicolor/256x256/apps/modjuke.png")
configure_file(resources/modjuke.desktop.in "${CMAKE_CURRENT_BINARY_DIR}/modjuke.desktop" @ONLY)

install(TARGETS modjuke RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/modjuke.desktop" DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES resources/modjuke.png DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/256x256/apps)
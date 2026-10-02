cmake_minimum_required(VERSION 3.22)
project(modjuke VERSION 1.1.0 LANGUAGES CXX)

# Targets Qt 6.4.2 (Linux Mint 22 / Ubuntu 24.04); builds against newer too,
# and on Windows and macOS (packaging/ and .github/workflows/build.yml).
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

set(MODJUKE_QT_COMPONENTS Core Gui Widgets Multimedia)
if(UNIX AND NOT APPLE)
    list(APPEND MODJUKE_QT_COMPONENTS DBus)   # "Show in folder" via the file manager
endif()
find_package(Qt6 6.4 REQUIRED COMPONENTS ${MODJUKE_QT_COMPONENTS})

qt_standard_project_setup()

add_library(modjuke_core STATIC
    src/reveal.cpp
    src/openmptapi.cpp
    src/config.cpp
    src/jsonfile.cpp
    src/analysiscache.cpp
    src/playliststore.cpp
    src/statsstore.cpp
    src/ignorestore.cpp
    src/shuffles.cpp
    src/library.cpp
    src/playbackqueue.cpp
    src/queuebuilder.cpp
    src/queueplayer.cpp
    src/settingskeeper.cpp
    src/listeningstats.cpp
    src/infopanel.cpp
    src/transportbar.cpp
    src/trackerpage.cpp
    src/queuepanel.cpp
    src/songactions.cpp
    src/musiclibrary.cpp
    src/theme.cpp
    src/themeeditor.cpp
    resources/arrows.qrc
    src/audiooutput.cpp
    src/trackerdata.cpp
    src/engine.cpp
    src/analyzer.cpp
    src/widgets.cpp
    src/queue.cpp
    src/filterdialog.cpp
    src/playlistsdialog.cpp
    src/settingsdialog.cpp
    src/infodialogs.cpp
    src/mainwindow.cpp
)
target_include_directories(modjuke_core PUBLIC src)
# documentation screenshots (MODJUKE_SHOTS=dir, see src/shotdriver.cpp)
option(MODJUKE_SHOT_DRIVER "Build the screenshot driver into the player" OFF)
if(MODJUKE_SHOT_DRIVER)
    target_sources(modjuke_core PRIVATE src/shotdriver.cpp)
    target_compile_definitions(modjuke_core PUBLIC MODJUKE_SHOT_DRIVER)
endif()
# the one place the version is set: --version, About, the Windows and macOS file info
target_compile_definitions(modjuke_core PUBLIC MODJUKE_VERSION="${PROJECT_VERSION}")
target_link_libraries(modjuke_core PUBLIC
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Multimedia)
if(UNIX AND NOT APPLE)
    target_link_libraries(modjuke_core PUBLIC Qt6::DBus)
endif()
if(MSVC)
    # the sources are UTF-8 (casefold.h has non-ASCII literals)
    target_compile_options(modjuke_core PUBLIC /utf-8)
endif()

set(MODJUKE_PLATFORM_SOURCES)
if(WIN32)
    # icon and version information (Explorer > Properties > Details)
    configure_file(resources/modjuke.rc.in "${CMAKE_CURRENT_BINARY_DIR}/modjuke.rc" @ONLY)
    list(APPEND MODJUKE_PLATFORM_SOURCES "${CMAKE_CURRENT_BINARY_DIR}/modjuke.rc")
elseif(APPLE)
    # modjuke.icns from modjuke.png with Apple's tools
    set(MODJUKE_ICONSET "${CMAKE_CURRENT_BINARY_DIR}/modjuke.iconset")
    set(MODJUKE_ICNS "${CMAKE_CURRENT_BINARY_DIR}/modjuke.icns")
    set(MODJUKE_PNG "${CMAKE_CURRENT_SOURCE_DIR}/resources/modjuke.png")
    add_custom_command(OUTPUT "${MODJUKE_ICNS}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${MODJUKE_ICONSET}"
        COMMAND sips -z 16 16 "${MODJUKE_PNG}" --out "${MODJUKE_ICONSET}/icon_16x16.png" > /dev/null
        COMMAND sips -z 32 32 "${MODJUKE_PNG}" --out "${MODJUKE_ICONSET}/icon_16x16@2x.png" > /dev/null
        COMMAND sips -z 32 32 "${MODJUKE_PNG}" --out "${MODJUKE_ICONSET}/icon_32x32.png" > /dev/null
        COMMAND sips -z 64 64 "${MODJUKE_PNG}" --out "${MODJUKE_ICONSET}/icon_32x32@2x.png" > /dev/null
        COMMAND sips -z 128 128 "${MODJUKE_PNG}" --out "${MODJUKE_ICONSET}/icon_128x128.png" > /dev/null
        COMMAND sips -z 256 256 "${MODJUKE_PNG}" --out "${MODJUKE_ICONSET}/icon_128x128@2x.png" > /dev/null
        COMMAND sips -z 256 256 "${MODJUKE_PNG}" --out "${MODJUKE_ICONSET}/icon_256x256.png" > /dev/null
        COMMAND iconutil -c icns -o "${MODJUKE_ICNS}" "${MODJUKE_ICONSET}"
        DEPENDS "${MODJUKE_PNG}"
        VERBATIM)
    set_source_files_properties("${MODJUKE_ICNS}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    list(APPEND MODJUKE_PLATFORM_SOURCES "${MODJUKE_ICNS}")
endif()

# WIN32: no console window on Windows; MACOSX_BUNDLE: modjuke.app on macOS.
# Both are ignored on the other systems.
add_executable(modjuke WIN32 MACOSX_BUNDLE src/main.cpp resources/app.qrc ${MODJUKE_PLATFORM_SOURCES})
target_link_libraries(modjuke PRIVATE modjuke_core)
set_target_properties(modjuke PROPERTIES
    MACOSX_BUNDLE_BUNDLE_NAME modjuke
    MACOSX_BUNDLE_GUI_IDENTIFIER io.github.flamingleo.modjuke
    MACOSX_BUNDLE_ICON_FILE modjuke.icns
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION})

# The install rules are for Linux (desktop entry and icon). Windows and macOS
# packages are made by the scripts in packaging/.
if(UNIX AND NOT APPLE)
    include(GNUInstallDirs)

    # Use the absolute installed icon path in the desktop entry. This avoids icon
    # theme cache/lookup differences in desktop environments such as Cinnamon.
    set(MODJUKE_ICON_PATH "${CMAKE_INSTALL_FULL_DATADIR}/icons/hicolor/256x256/apps/modjuke.png")
    # The same for the program: ~/.local/bin is only on the menu's PATH if it
    # existed when the session started, so a bare "modjuke" may not be found.
    set(MODJUKE_EXEC_PATH "${CMAKE_INSTALL_FULL_BINDIR}/modjuke")
    configure_file(resources/modjuke.desktop.in "${CMAKE_CURRENT_BINARY_DIR}/modjuke.desktop" @ONLY)

    install(TARGETS modjuke RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/modjuke.desktop" DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(FILES resources/modjuke.png DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/256x256/apps)
endif()

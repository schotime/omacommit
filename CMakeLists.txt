cmake_minimum_required(VERSION 3.16)
project(og VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
# An optimised build unless asked otherwise: half the size of a plain one.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(Qt6 REQUIRED COMPONENTS Widgets)

add_executable(og
    src/main.cpp
    src/Agent.cpp
    src/Style.cpp
    src/GitRepo.cpp
    src/Theme.cpp
    src/MessageEdit.cpp
    src/DiffView.cpp
    src/ImageCompare.cpp
    src/Syntax.cpp
    src/CommitWindow.cpp
    src/LogGraph.cpp
    src/LogWindow.cpp
    src/ResolveWindow.cpp
    src/OgWindow.cpp
    src/PageTabs.cpp
)
# The app icon, compiled in for the window icon (installed below for launchers).
qt_add_resources(og icons PREFIX "/" FILES icons/omarchy-commit.svg)
target_link_libraries(og PRIVATE Qt6::Widgets)
# Syntax highlighting, when KDE's library is there (Arch: syntax-highlighting);
# without it code is shown as plain text.
find_package(KF6SyntaxHighlighting QUIET)
if(KF6SyntaxHighlighting_FOUND)
    target_link_libraries(og PRIVATE KF6::SyntaxHighlighting)
    target_compile_definitions(og PRIVATE OG_SYNTAX)
    message(STATUS "Syntax highlighting: on (KF6SyntaxHighlighting)")
else()
    message(STATUS "Syntax highlighting: off (install KF6SyntaxHighlighting to turn it on)")
endif()
# The desktop's file chooser (XDG portal, over D-Bus): Linux only.
if(UNIX AND NOT APPLE)
    find_package(Qt6 REQUIRED COMPONENTS DBus)
    target_sources(og PRIVATE src/Portal.cpp)
    target_link_libraries(og PRIVATE Qt6::DBus)
    target_compile_definitions(og PRIVATE OG_PORTAL)
endif()
target_compile_definitions(og PRIVATE OG_VERSION="${PROJECT_VERSION}")

include(GNUInstallDirs)
install(TARGETS og RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES omarchy-commit.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES icons/omarchy-commit.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)

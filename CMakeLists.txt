cmake_minimum_required(VERSION 3.16)
project(og VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
# An optimised build unless asked otherwise: half the size of a plain one.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(OG_STANDALONE "Build a single-file Windows executable (static Qt / MinGW)" OFF)
if(OG_STANDALONE)
    if(NOT WIN32 OR NOT MINGW)
        message(FATAL_ERROR "OG_STANDALONE requires Windows with MinGW and static Qt")
    endif()
    # Qt's static MSYS2 build uses system codecs. Prefer their static archives
    # during dependency discovery so no UCRT64 DLLs leak into the executable.
    get_filename_component(_og_toolchain_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    get_filename_component(_og_static_lib_dir "${_og_toolchain_bin}/../lib" ABSOLUTE)
    foreach(_og_pair IN ITEMS
            "ZLIB_LIBRARY_RELEASE=libz.a"
            "PCRE2_LIBRARY_RELEASE=libpcre2-16.a"
            "PCRE2_LIBRARY_DEBUG=libpcre2-16.a"
            "PNG_LIBRARY_RELEASE=libpng16.a"
            "FREETYPE_LIBRARY_RELEASE=libfreetype.a"
            "JPEG_LIBRARY_RELEASE=libjpeg.a"
            "TIFF_LIBRARY_RELEASE=libtiff.a"
            "WebP_LIBRARY=libwebp.a"
            "WebP_demux_LIBRARY=libwebpdemux.a"
            "WebP_mux_LIBRARY=libwebpmux.a")
        string(REPLACE "=" ";" _og_parts "${_og_pair}")
        list(GET _og_parts 0 _og_var)
        list(GET _og_parts 1 _og_archive)
        if(NOT EXISTS "${_og_static_lib_dir}/${_og_archive}")
            message(FATAL_ERROR "Missing static library: ${_og_static_lib_dir}/${_og_archive}")
        endif()
        set(${_og_var} "${_og_static_lib_dir}/${_og_archive}" CACHE FILEPATH "Static library for og" FORCE)
    endforeach()
    # This must follow Qt's transitive libraries, not just our direct Qt target.
    # CMake emits the standard libraries at the very end of the link line.
    set(CMAKE_CXX_STANDARD_LIBRARIES
        "${CMAKE_CXX_STANDARD_LIBRARIES} ${_og_static_lib_dir}/libharfbuzz.a -Wl,--end-group")
endif()

find_package(Qt6 REQUIRED COMPONENTS Widgets)
if(OG_STANDALONE)
    get_target_property(_og_qt_type Qt6::Core TYPE)
    if(NOT _og_qt_type STREQUAL "STATIC_LIBRARY")
        message(FATAL_ERROR "OG_STANDALONE requires static Qt; set CMAKE_PREFIX_PATH to the static Qt prefix")
    endif()
endif()

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
if(WIN32)
    set_target_properties(og PROPERTIES WIN32_EXECUTABLE TRUE)
endif()
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
# MSYS2's static Qt brings in image plugins and codecs with circular static
# dependencies. Group the archives and force the static libb2 before any Qt
# import library can pull in its DLL version.
if(OG_STANDALONE)
    target_link_options(og PRIVATE -static -static-libgcc -static-libstdc++
        -Wl,--whole-archive "${_og_static_lib_dir}/libb2.a" -Wl,--no-whole-archive
        -Wl,--start-group)
    target_link_libraries(og PRIVATE
        "${_og_static_lib_dir}/libfreetype.a"
        "${_og_static_lib_dir}/libtiff.a"
        "${_og_static_lib_dir}/libbrotlidec.a"
        "${_og_static_lib_dir}/libbrotlicommon.a"
        "${_og_static_lib_dir}/libbz2.a"
        "${_og_static_lib_dir}/libjbig.a"
        "${_og_static_lib_dir}/libjpeg.a"
        "${_og_static_lib_dir}/libdeflate.a"
        "${_og_static_lib_dir}/libLerc.a"
        "${_og_static_lib_dir}/libzstd.a"
        "${_og_static_lib_dir}/liblzma.a"
        "${_og_static_lib_dir}/libgraphite2.a"
        rpcrt4
    )
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
if(UNIX AND NOT APPLE)
    install(FILES omarchy-commit.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(FILES icons/omarchy-commit.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
endif()

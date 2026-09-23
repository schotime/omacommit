cmake_minimum_required(VERSION 3.16)
project(og VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)

add_executable(og
    src/main.cpp
    src/GitRepo.cpp
    src/Theme.cpp
    src/MessageEdit.cpp
    src/DiffView.cpp
    src/CommitWindow.cpp
    src/LogGraph.cpp
    src/LogWindow.cpp
    src/ResolveWindow.cpp
)
target_link_libraries(og PRIVATE Qt6::Widgets)
target_compile_definitions(og PRIVATE OG_VERSION="${PROJECT_VERSION}")

include(GNUInstallDirs)
install(TARGETS og RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES omarchy-commit.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)

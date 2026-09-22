cmake_minimum_required(VERSION 3.16)
project(git-commit-ui VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)

add_executable(git-commit-ui
    src/main.cpp
    src/GitRepo.cpp
    src/Theme.cpp
    src/MessageEdit.cpp
    src/DiffView.cpp
    src/CommitWindow.cpp
)
target_link_libraries(git-commit-ui PRIVATE Qt6::Widgets)

include(GNUInstallDirs)
install(TARGETS git-commit-ui RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES omarchy-commit.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)

cmake_minimum_required(VERSION 3.25)
project(ResoluteQuantNative VERSION 0.1.0 LANGUAGES CXX)

if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "This foundation targets MSVC x64 only. Use the 'release' preset from tools/build.ps1.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
add_compile_options(/W4 /permissive- /utf-8 /Zc:__cplusplus)

# Only LGPL modules, linked dynamically. See docs/architecture/task-0-native-foundation.md.
find_package(Qt6 6.12 REQUIRED COMPONENTS Core Gui Widgets Test)
qt_standard_project_setup()

qt_add_executable(ResoluteQuant WIN32
  src/main.cpp
  src/app/MainWindow.cpp
  src/app/MainWindow.h
  src/app/Theme.cpp
  src/app/Theme.h
)
target_include_directories(ResoluteQuant PRIVATE src)
target_link_libraries(ResoluteQuant PRIVATE Qt6::Widgets)
qt_add_resources(ResoluteQuant "assets"
  PREFIX "/"
  BASE "${CMAKE_CURRENT_SOURCE_DIR}/.."
  FILES "${CMAKE_CURRENT_SOURCE_DIR}/../assets/img/favicon.png"
)

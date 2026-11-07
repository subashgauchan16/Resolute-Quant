cmake_minimum_required(VERSION 3.25)
project(ResoluteQuantNative VERSION 0.1.0 LANGUAGES CXX)

if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "This foundation targets MSVC x64 only. Use the 'release' preset from tools/build.ps1.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
add_compile_options(/W4 /permissive- /utf-8 /Zc:__cplusplus)
add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)

# The private Python created by tools/setup_runtime.ps1. No system Python is ever used.
set(RQ_PYTHON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/runtime/python")
if(NOT EXISTS "${RQ_PYTHON_DIR}/libs/python313.lib")
  message(FATAL_ERROR "The private Python is missing. Run native\\tools\\setup_runtime.ps1 first.")
endif()

# Only LGPL modules, linked dynamically. See docs/architecture/task-0-native-foundation.md.
find_package(Qt6 6.12 REQUIRED COMPONENTS Core Gui Widgets Test)
qt_standard_project_setup()

include(FetchContent)
set(PYBIND11_NOPYTHON ON)
FetchContent_Declare(pybind11
  URL "https://github.com/pybind/pybind11/archive/refs/tags/v3.1.0.tar.gz"
  URL_HASH SHA256=EF712655692A2E9BF7BB7874C022564A45F91D847DDEE987E720CD9E28849665
  DOWNLOAD_EXTRACT_TIMESTAMP FALSE
)
FetchContent_MakeAvailable(pybind11)

# The only target that links Python. The app loads it at runtime.
add_library(rq_python_host SHARED src/bridge/PythonHost.cpp src/bridge/PythonHost.h)
target_compile_definitions(rq_python_host PRIVATE RQ_PYTHON_HOST_BUILD)
target_include_directories(rq_python_host PRIVATE src "${RQ_PYTHON_DIR}/include")
target_link_directories(rq_python_host PRIVATE "${RQ_PYTHON_DIR}/libs")
target_link_libraries(rq_python_host PRIVATE pybind11::headers)
set_target_properties(rq_python_host PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

# Everything except main(), shared with the tests.
qt_add_library(rq_core STATIC
  src/app/BrandMark.cpp
  src/app/BrandMark.h
  src/app/CandleChart.cpp
  src/app/CandleChart.h
  src/app/ErrorDisplay.cpp
  src/app/ErrorDisplay.h
  src/app/Format.cpp
  src/app/Format.h
  src/app/HealthPanel.cpp
  src/app/HealthPanel.h
  src/app/HistoryPanel.cpp
  src/app/HistoryPanel.h
  src/app/Logging.cpp
  src/app/Logging.h
  src/app/MainWindow.cpp
  src/app/MainWindow.h
  src/app/Palette.h
  src/app/TerminalPage.cpp
  src/app/TerminalPage.h
  src/app/TerminalWidgets.cpp
  src/app/TerminalWidgets.h
  src/app/Theme.cpp
  src/app/Theme.h
  src/bridge/PythonEngine.cpp
  src/bridge/PythonEngine.h
  src/bridge/PythonHost.h
  src/bridge/PythonResult.h
)
target_include_directories(rq_core PUBLIC src)
target_link_libraries(rq_core PUBLIC Qt6::Widgets)
# The terminal's own fonts and logo. The logos are PNG copies of assets/img, because this Qt has no WebP plugin.
qt_add_resources(rq_core "native_assets"
  PREFIX "/native"
  FILES
    assets/fonts/IBMPlexMono-Regular.ttf
    assets/fonts/IBMPlexMono-Medium.ttf
    assets/fonts/IBMPlexMono-SemiBold.ttf
    assets/fonts/IBMPlexSansCondensed-Medium.ttf
    assets/fonts/IBMPlexSansCondensed-SemiBold.ttf
    assets/img/rq-mark.png
    assets/img/resolute-quant-logo.png
)

qt_add_executable(ResoluteQuant WIN32 src/main.cpp)
target_link_libraries(ResoluteQuant PRIVATE rq_core)
add_dependencies(ResoluteQuant rq_python_host)
qt_add_resources(ResoluteQuant "assets"
  PREFIX "/"
  BASE "${CMAKE_CURRENT_SOURCE_DIR}/.."
  FILES "${CMAKE_CURRENT_SOURCE_DIR}/../assets/img/favicon.png"
)

# Keep the Python sources beside the exe, and point the build-tree exe at the development runtime.
# tools/deploy.ps1 does not copy this file, so a deployed app can only use its own runtime.
file(WRITE "${CMAKE_BINARY_DIR}/dev_runtime_path.txt" "${RQ_PYTHON_DIR}")
add_custom_target(rq_python_files ALL
  COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/python/rq_engine" "${CMAKE_BINARY_DIR}/python/rq_engine"
  COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_CURRENT_SOURCE_DIR}/../history.py" "${CMAKE_CURRENT_SOURCE_DIR}/../crypto_feed.py" "${CMAKE_BINARY_DIR}/python/"
  COMMENT "Copying the Python sources, including the modules shared with the browser terminal"
)
add_dependencies(ResoluteQuant rq_python_files)

enable_testing()
function(rq_add_test name)
  qt_add_executable(${name} tests/${name}.cpp)
  target_link_libraries(${name} PRIVATE rq_core Qt6::Test)
  set_target_properties(${name} PROPERTIES WIN32_EXECUTABLE OFF)
  add_dependencies(${name} ResoluteQuant)
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES
    TIMEOUT 180
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT6_INSTALL_PREFIX}/bin"
  )
endfunction()
rq_add_test(test_bridge)
rq_add_test(test_ui)
rq_add_test(test_history_ui)
rq_add_test(test_terminal_ui)

# Contacts real providers, so it is built but deliberately not registered with ctest.
qt_add_executable(live_history_check tests/live_history_check.cpp)
target_link_libraries(live_history_check PRIVATE rq_core Qt6::Test)
set_target_properties(live_history_check PROPERTIES WIN32_EXECUTABLE OFF)
add_dependencies(live_history_check ResoluteQuant)
qt_add_executable(live_feed_check tests/live_feed_check.cpp)
target_link_libraries(live_feed_check PRIVATE rq_core Qt6::Test psapi)
set_target_properties(live_feed_check PROPERTIES WIN32_EXECUTABLE OFF)
add_dependencies(live_feed_check ResoluteQuant)

# Runs the Python history code directly on the private Python, against a fake provider.
add_test(NAME python_tests COMMAND "${RQ_PYTHON_DIR}/python.exe" -I "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_market_history.py")
add_test(NAME python_live_tests COMMAND "${RQ_PYTHON_DIR}/python.exe" -I "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_live.py")

set(RQ_POWERSHELL powershell -NoProfile -ExecutionPolicy Bypass -File)
add_test(NAME process_tests COMMAND ${RQ_POWERSHELL} "${CMAKE_CURRENT_SOURCE_DIR}/tests/process_tests.ps1"
  -Exe "$<TARGET_FILE:ResoluteQuant>" -QtBin "${QT6_INSTALL_PREFIX}/bin")
add_test(NAME clean_folder_test COMMAND ${RQ_POWERSHELL} "${CMAKE_CURRENT_SOURCE_DIR}/tests/clean_folder_test.ps1"
  -Deploy "${CMAKE_CURRENT_SOURCE_DIR}/tools/deploy.ps1")
set_tests_properties(process_tests clean_folder_test PROPERTIES TIMEOUT 300 RUN_SERIAL TRUE)

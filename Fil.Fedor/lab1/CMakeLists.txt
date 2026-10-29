cmake_minimum_required(VERSION 3.16)
project(lab1_disk_monitor LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "This lab requires Linux: inotify, /proc, signalfd and pidfd.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
option(BUILD_TESTING "Build and register tests" OFF)

add_compile_options(-Wall -Wextra -Werror)

add_library(monitor_core STATIC src/config.cpp src/daemon.cpp src/watcher.cpp)
target_include_directories(monitor_core PUBLIC include)
add_executable(lab1 src/main.cpp)
target_link_libraries(lab1 PRIVATE monitor_core)

include(CTest)
if(BUILD_TESTING)
    add_executable(lab1_test tests/test.cpp)
    target_link_libraries(lab1_test PRIVATE monitor_core)
    add_test(NAME lab1_test COMMAND lab1_test)
endif()

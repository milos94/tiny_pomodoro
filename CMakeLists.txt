cmake_minimum_required(VERSION 3.20)

set(CMAKE_CXX_COMPILER "/usr/bin/clang++-20")

project(tiny_pomodoro CXX)

# C++ Standard and libc++
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile commands for clangd and clang-tidy
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Find nappgui dependency
find_package(nappgui REQUIRED)

# Create executable
add_executable(tiny_pomodoro
    src/main.cpp
)

# Link nappgui - use the gui target as the main entry point
target_link_libraries(tiny_pomodoro PRIVATE nappgui::gui)

# Add nappgui include directories (from the find_package)
target_include_directories(tiny_pomodoro PRIVATE ${NAPPGUI_INCLUDE_PATH})

# Build type configurations
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Release build
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_options(tiny_pomodoro PRIVATE -O3)
endif()

# Debug build
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(tiny_pomodoro PRIVATE -g -O0)
endif()

# Debug with Address Sanitizer
if(CMAKE_BUILD_TYPE STREQUAL "DebugASAN")
    target_compile_options(tiny_pomodoro PRIVATE -g -O0 -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(tiny_pomodoro PRIVATE -fsanitize=address)
endif()

# Debug with Undefined Behavior Sanitizer
if(CMAKE_BUILD_TYPE STREQUAL "DebugUBSAN")
    target_compile_options(tiny_pomodoro PRIVATE -g -O0 -fsanitize=undefined -fno-omit-frame-pointer)
    target_link_options(tiny_pomodoro PRIVATE -fsanitize=undefined)
endif()

# Tiny build - minimal executable size
if(CMAKE_BUILD_TYPE STREQUAL "Tiny")
    target_compile_options(tiny_pomodoro PRIVATE -Oz -ffunction-sections -fdata-sections -fno-rtti -fno-exceptions)
    target_link_options(tiny_pomodoro PRIVATE -Wl,--gc-sections -Wl,-s)
endif()

# Common flags for all builds
target_compile_options(tiny_pomodoro PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wconversion
    -stdlib=libc++
)

# we need to also tell the linker to use the libc++
target_link_options(tiny_pomodoro PRIVATE 
    -lc++)
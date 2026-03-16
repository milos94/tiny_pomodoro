cmake_minimum_required(VERSION 3.20)

set(CMAKE_CXX_COMPILER "/usr/bin/clang++-20")

project(tiny_pomodoro CXX)

# C++ Standard and libc++
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile commands for clangd and clang-tidy
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Find FLTK (used by the FLTK build only)
find_package(FLTK REQUIRED)

# Create two executables: one using FLTK, one using only raw/native APIs
add_executable(tiny_pomodoro_fltk
    src/main_fltk.cpp
)

target_link_libraries(tiny_pomodoro_fltk PRIVATE
    fltk
    X11 Xext Xinerama Xcursor Xrender Xfixes fontconfig
    pangocairo-1.0 pango-1.0 gobject-2.0 glib-2.0 harfbuzz
    cairo gtk-3 gdk-3 atk-1.0 cairo-gobject gdk_pixbuf-2.0 gio-2.0
    wayland-cursor wayland-client xkbcommon dbus-1
    z m pthread dl
)

# Minimal/raw build (no FLTK)
add_executable(tiny_pomodoro_raw
    src/main_raw.cpp
)

target_link_libraries(tiny_pomodoro_raw PRIVATE
    X11 Xext Xinerama Xcursor Xrender Xfixes fontconfig
    z m pthread dl
)

# Build type configurations
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Helper: list of pomodoro targets to apply common flags to
set(POMODORO_TARGETS tiny_pomodoro_fltk tiny_pomodoro_raw)

# Release build
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    foreach(tgt ${POMODORO_TARGETS})
        target_compile_options(${tgt} PRIVATE -O3)
    endforeach()
endif()

# Debug build
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    foreach(tgt ${POMODORO_TARGETS})
        target_compile_options(${tgt} PRIVATE -g -O0)
    endforeach()
endif()

# Debug with Address Sanitizer
if(CMAKE_BUILD_TYPE STREQUAL "DebugASAN")
    foreach(tgt ${POMODORO_TARGETS})
        target_compile_options(${tgt} PRIVATE -g -O0 -fsanitize=address -fno-omit-frame-pointer)
        target_link_options(${tgt} PRIVATE -fsanitize=address)
    endforeach()
endif()

# Debug with Undefined Behavior Sanitizer
if(CMAKE_BUILD_TYPE STREQUAL "DebugUBSAN")
    foreach(tgt ${POMODORO_TARGETS})
        target_compile_options(${tgt} PRIVATE -g -O0 -fsanitize=undefined -fno-omit-frame-pointer)
        target_link_options(${tgt} PRIVATE -fsanitize=undefined)
    endforeach()
endif()

# Tiny build - minimal executable size
if(CMAKE_BUILD_TYPE STREQUAL "Tiny")
    foreach(tgt ${POMODORO_TARGETS})
        target_compile_options(${tgt} PRIVATE -Oz -ffunction-sections -fdata-sections -fno-rtti -fno-exceptions)
        target_link_options(${tgt} PRIVATE -Wl,--gc-sections -Wl,-s)
    endforeach()
endif()

# Common flags for all builds
foreach(tgt ${POMODORO_TARGETS})
    target_compile_options(${tgt} PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -stdlib=libc++
    )

    # we need to also tell the linker to use the libc++
    target_link_options(${tgt} PRIVATE -lc++)
endforeach()
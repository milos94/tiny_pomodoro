cmake_minimum_required(VERSION 3.20)

set(CMAKE_CXX_COMPILER "/usr/bin/clang++-20")

project(tiny_pomodoro CXX)

# XDG/FHS install paths
include(GNUInstallDirs)

# C++ Standard and libc++
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile commands for clangd and clang-tidy
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Find FLTK (used by the FLTK build only)
find_package(FLTK REQUIRED CONFIG)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK3 REQUIRED gtk+-3.0)
pkg_check_modules(WAYLAND REQUIRED wayland-client wayland-cursor)
pkg_check_modules(XKBCOMMON REQUIRED xkbcommon)
pkg_check_modules(DBUS REQUIRED dbus-1)

# Create two executables: one using FLTK, one using only raw/native APIs
add_executable(tiny_pomodoro
    src/main.cpp
)

target_include_directories(tiny_pomodoro PRIVATE
    ${GTK3_INCLUDE_DIRS}
    ${WAYLAND_INCLUDE_DIRS}
    ${XKBCOMMON_INCLUDE_DIRS}
    ${DBUS_INCLUDE_DIRS}
)

target_link_libraries(tiny_pomodoro PRIVATE
    fltk
    X11 Xext Xinerama Xcursor Xrender Xfixes fontconfig
    ${GTK3_LIBRARIES}
    ${WAYLAND_LIBRARIES}
    ${XKBCOMMON_LIBRARIES}
    ${DBUS_LIBRARIES}
    asound
    z m pthread dl
)

target_link_directories(tiny_pomodoro PRIVATE
    ${GTK3_LIBRARY_DIRS}
    ${WAYLAND_LIBRARY_DIRS}
    ${XKBCOMMON_LIBRARY_DIRS}
    ${DBUS_LIBRARY_DIRS}
)

# Minimal/raw build (no FLTK)
add_executable(tiny_pomodoro_pro
    src/main_pro.cpp
)

target_link_libraries(tiny_pomodoro_pro PRIVATE
    X11 Xext Xinerama Xcursor Xrender Xfixes fontconfig
    asound
    z m pthread dl
)

# Terminal-only build (STL + POSIX, no GUI, no ALSA)
add_executable(tiny_pomodoro_pro_plus
    src/main_pro_plus.cpp
)

target_link_libraries(tiny_pomodoro_pro_plus PRIVATE
    pthread
)

# Assembly build (pure Linux syscalls, no libc)
enable_language(ASM_NASM)
set(CMAKE_ASM_NASM_FLAGS "-f elf64")
set(CMAKE_ASM_NASM_LINK_EXECUTABLE "ld -o <TARGET> <OBJECTS> -nostdlib -static")
add_executable(tiny_pomodoro_pro_plus_ultra
    src/main_pro_plus_ultra.asm
)

# Minimal assembly build: no UI, no input, no state — just timed bells.
add_executable(tiny_pomodoro_2
    src/main_2.asm
)

# Build type configurations
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Tiny)
endif()

# Helper: list of pomodoro targets to apply common flags to
set(POMODORO_TARGETS tiny_pomodoro tiny_pomodoro_pro tiny_pomodoro_pro_plus)

# Per-build-type output directory
string(TOLOWER "${CMAKE_BUILD_TYPE}" BUILD_TYPE_LOWER)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/build/${BUILD_TYPE_LOWER}")

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

# Sound path baked in at compile time so the installed binaries find the sound
# file at its XDG location.  Falls back to the synthesised beep if the file is
# absent (e.g. when running directly from the build tree during development).
target_compile_definitions(tiny_pomodoro PRIVATE
    SOUND_PATH="${CMAKE_INSTALL_FULL_DATADIR}/sounds/tiny_pomodoro/beep.wav"
)
target_compile_definitions(tiny_pomodoro_pro PRIVATE
    SOUND_PATH="${CMAKE_INSTALL_FULL_DATADIR}/sounds/tiny_pomodoro/beep.wav"
)

# ── Install rules ────────────────────────────────────────────────────────────

# Binaries
install(TARGETS
    tiny_pomodoro
    tiny_pomodoro_pro
    tiny_pomodoro_pro_plus
    tiny_pomodoro_pro_plus_ultra
    tiny_pomodoro_2
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# Shared sound
install(FILES assets/beep.wav
    DESTINATION ${CMAKE_INSTALL_DATADIR}/sounds/tiny_pomodoro
)

# .desktop launcher files
install(FILES
    assets/tiny_pomodoro.desktop
    assets/tiny_pomodoro_pro.desktop
    DESTINATION ${CMAKE_INSTALL_DATADIR}/applications
)

# Icons
install(FILES
    assets/tiny_pomodoro.png
    assets/tiny_pomodoro_pro.png
    DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/512x512/apps
)
if(CMAKE_BUILD_TYPE STREQUAL "Tiny")
    set(CMAKE_ASM_NASM_FLAGS "-f elf64 -Ox")
    set(CMAKE_ASM_NASM_LINK_EXECUTABLE "ld -o <TARGET> <OBJECTS> -nostdlib -static -s")
    foreach(tgt ${POMODORO_TARGETS})
        target_compile_options(${tgt} PRIVATE
            -Oz
            -ffunction-sections
            -fdata-sections
            -fno-rtti
            -fno-exceptions
            -flto=thin
            -fvisibility=hidden
            -fno-unwind-tables
            -fno-asynchronous-unwind-tables
            -fmerge-all-constants
        )
        target_link_options(${tgt} PRIVATE
            -fuse-ld=lld
            -flto=thin
            -Wl,--gc-sections
            -Wl,-s
            -Wl,--as-needed
            -Wl,--icf=all
        )
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
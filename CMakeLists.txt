cmake_minimum_required(VERSION 3.20)

set(CMAKE_CXX_COMPILER "/usr/bin/clang++")
# FLTK compiles C sources too; they must go through clang as well, or -flto=thin
# is handed to gcc, which does not understand it.
set(CMAKE_C_COMPILER "/usr/bin/clang")

project(tiny_pomodoro C CXX)

# XDG/FHS install paths
include(GNUInstallDirs)

# C++ Standard and libc++
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile commands for clangd and clang-tidy
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# The PRE-BUILT FLTK, imported by hand rather than via find_package(FLTK).
#
# find_package would import a target literally named "fltk", which collides with
# the target of the same name that FLTK's own CMakeLists creates when we build it
# from source below.  We want both in one build tree so the two binaries can be
# compared, so the pre-built one gets an explicit, differently-named target.
find_library(FLTK_PREBUILT_LIBRARY NAMES fltk PATHS /usr/local/lib REQUIRED)
find_path(FLTK_PREBUILT_INCLUDE_DIR NAMES FL/Fl.H PATHS /usr/local/include REQUIRED)

add_library(fltk_prebuilt STATIC IMPORTED)
set_target_properties(fltk_prebuilt PROPERTIES
    IMPORTED_LOCATION "${FLTK_PREBUILT_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${FLTK_PREBUILT_INCLUDE_DIR}"
)

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
    fltk_prebuilt
    X11 Xext Xinerama Xcursor Xrender Xfixes Xft fontconfig
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

# The size flags, named once so the app and any library we build from source
# cannot drift apart.  They must agree on -flto=thin in particular, or the link
# fails.  Split by language: -fno-rtti / -fno-exceptions / -stdlib are C++ only.
set(TINY_CXX_FLAGS
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
set(TINY_C_FLAGS
    -Oz
    -ffunction-sections
    -fdata-sections
    -flto=thin
    -fvisibility=hidden
    -fno-unwind-tables
    -fno-asynchronous-unwind-tables
    -fmerge-all-constants
)
set(TINY_LINK_FLAGS
    -fuse-ld=lld
    -flto=thin
    -Wl,--gc-sections
    -Wl,-s
    -Wl,--as-needed
    -Wl,--icf=all
)

# ── FLTK from source ─────────────────────────────────────────────────────────
#
# tiny_pomodoro links the *pre-built* /usr/local/lib/libfltk.a: opaque native
# code, compiled -O3 against libstdc++.  LTO cannot see into it, and because the
# app uses libc++ the executable ends up linking *two* C++ runtimes.
#
# tiny_pomodoro_src is the same source file, but FLTK is compiled as part of
# this build, with our flags.  Its object files are then LLVM bitcode, so LTO,
# --gc-sections and --icf can finally reach inside the library.
set(FLTK_SOURCE_DIR "${CMAKE_SOURCE_DIR}/../fltk"
    CACHE PATH "FLTK source checkout, compiled together with the app")

if(EXISTS "${FLTK_SOURCE_DIR}/CMakeLists.txt")
    # Mirror the configuration the pre-built libfltk.a was made with, so the two
    # differ only in the flags — not in which FLTK they are.
    set(FLTK_BUILD_FLUID         OFF CACHE BOOL "" FORCE)
    set(FLTK_BUILD_FLTK_OPTIONS  OFF CACHE BOOL "" FORCE)
    set(FLTK_BUILD_TEST          OFF CACHE BOOL "" FORCE)
    set(FLTK_BUILD_EXAMPLES      OFF CACHE BOOL "" FORCE)
    set(FLTK_BUILD_FORMS         OFF CACHE BOOL "" FORCE)
    set(FLTK_BUILD_SHARED_LIBS   OFF CACHE BOOL "" FORCE)
    set(FLTK_BUILD_HTML_DOCS     OFF CACHE BOOL "" FORCE)
    set(FLTK_BUILD_PDF_DOCS      OFF CACHE BOOL "" FORCE)
    set(FLTK_BACKEND_WAYLAND     OFF CACHE BOOL "" FORCE)
    set(FLTK_USE_PANGO           OFF CACHE BOOL "" FORCE)
    set(FLTK_BUILD_GL            ON  CACHE BOOL "" FORCE)

    add_subdirectory("${FLTK_SOURCE_DIR}" fltk_src EXCLUDE_FROM_ALL)

    add_executable(tiny_pomodoro_src src/main.cpp)
    target_link_libraries(tiny_pomodoro_src PRIVATE
        fltk::fltk
        asound
    )
    target_compile_definitions(tiny_pomodoro_src PRIVATE
        SOUND_PATH="${CMAKE_INSTALL_FULL_DATADIR}/sounds/tiny_pomodoro/beep.wav"
    )

    # Link with libc++ too.  tiny_pomodoro cannot do this: the pre-built FLTK is
    # a libstdc++ build, so that binary has to drag in *both* runtimes.  Since we
    # compile FLTK ourselves here, one runtime is enough.
    target_link_options(tiny_pomodoro_src PRIVATE -stdlib=libc++)

    # The app itself gets the usual treatment.
    list(APPEND POMODORO_TARGETS tiny_pomodoro_src)

    # ...and so does FLTK.  Per-language, because FLTK also compiles C.
    foreach(fltk_tgt fltk fltk_images fltk_gl fltk_jpeg fltk_png fltk_z)
        if(TARGET ${fltk_tgt})
            # libc++ unconditionally: this is an ABI decision, not an optimisation.
            # The app is built with libc++, so FLTK must be too, in every build
            # type — otherwise the two disagree about std::string and the link
            # fails with undefined std::__throw_* / _Rb_tree_* symbols.
            target_compile_options(${fltk_tgt} PRIVATE
                "$<$<COMPILE_LANGUAGE:CXX>:-stdlib=libc++>"
            )
            if(CMAKE_BUILD_TYPE STREQUAL "Tiny")
                target_compile_options(${fltk_tgt} PRIVATE
                    "$<$<COMPILE_LANGUAGE:CXX>:${TINY_CXX_FLAGS}>"
                    "$<$<COMPILE_LANGUAGE:C>:${TINY_C_FLAGS}>"
                )
            endif()
        endif()
    endforeach()
endif()

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

if(TARGET tiny_pomodoro_src)
    install(TARGETS tiny_pomodoro_src RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

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
        target_compile_options(${tgt} PRIVATE ${TINY_CXX_FLAGS})
        target_link_options(${tgt} PRIVATE ${TINY_LINK_FLAGS})
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
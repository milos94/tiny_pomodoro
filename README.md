# Tiny Pomodoro

A minimal pomodoro timer application built with a focus on **executable size optimization**.
**Note:** the code and the readme have been AI generated.

## Purpose

This project is a functional pomodoro timer application that demonstrates size optimization techniques for compiled C++ applications. It's designed as a reference implementation for a technical talk on binary size reduction strategies.

**Default Configuration:**
- Focus Time: 25 minutes
- Short Break: 5 minutes
- Long Break: 15 minutes
- Rounds Until Long Break: 4

## Tech Stack

- **Language**: C++23 and x86-64 Assembly (NASM)
- **Compiler**: clang++-20, NASM
- **Build System**: CMake 3.20+
- **GUI**:
  - `tiny_pomodoro` — FLTK-based GUI, linked against the **pre-built** `/usr/local/lib/libfltk.a`
  - `tiny_pomodoro_src` — **the same `main.cpp`**, but FLTK is compiled from source as part of
    this build, so the size flags apply to the library too (see below)
  - `tiny_pomodoro_pro` — raw X11 (no extra GUI framework)
  - `tiny_pomodoro_pro_plus` — terminal UI (ANSI escapes, no X11/FLTK/ALSA)
  - `tiny_pomodoro_pro_plus_ultra` — terminal UI in pure x86-64 assembly (raw Linux syscalls, no libc)
- **Audio**: ALSA (direct PCM; synthesised beep fallback if WAV not found); terminal bell via `/dev/tty` for terminal builds

## Features

- Automatic session progression: Focus → Short Break → (every 4th) Long Break → repeat
- Adjustable durations per session type via +/- buttons (GUI) or keyboard shortcuts (terminal)
- Sound notification at session end (custom WAV or synthesised beep; bell via `/dev/tty` for terminal builds)
- Keyboard shortcuts: `q` / `Esc` to quit; `s` start/pause, `r` reset, `f/F` `b/B` `l/L` adjust durations (terminal)
- `tiny_pomodoro_pro_plus_ultra`: pure x86-64 NASM, no libc, no runtime — only Linux syscalls

## Prerequisites

```bash
sudo apt install \
  clang++-20 lld libc++-20-dev libc++abi-20-dev \
  cmake libfltk1.3-dev \
  libgtk-3-dev libpango1.0-dev libcairo2-dev libglib2.0-dev \
  libharfbuzz-dev libatk1.0-dev libgdk-pixbuf-2.0-dev \
  libwayland-dev libxkbcommon-dev libdbus-1-dev \
  libasound2-dev \
  nasm
```

## Building

### Build all configurations at once

```bash
./build_all.sh
```

This configures and builds all five build types in parallel. Binaries are placed in `build/<type>/`.

### Build a single configuration

```bash
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release
```

### Build types

| Type | Flags | Use |
|------|-------|-----|
| `Tiny` | `-Oz -flto=thin -fvisibility=hidden -fno-exceptions -fno-rtti` + LLD ICF | Smallest binary **(default)** |
| `Release` | `-O3` | Fast runtime |
| `Debug` | `-g -O0` | Debugging |
| `DebugASAN` | `-g -O0 -fsanitize=address` | Memory error detection |
| `DebugUBSAN` | `-g -O0 -fsanitize=undefined` | UB detection |

### Output layout

```
build/
  tiny/
    tiny_pomodoro                  ← FLTK build
    tiny_pomodoro_pro              ← raw X11 build
    tiny_pomodoro_pro_plus         ← terminal-only C++ build
    tiny_pomodoro_pro_plus_ultra   ← terminal-only assembly build
    sounds/beep.wav                ← copied from assets/ by build_all.sh
  release/
    tiny_pomodoro
    tiny_pomodoro_pro
    tiny_pomodoro_pro_plus
    tiny_pomodoro_pro_plus_ultra
    sounds/beep.wav
  debug/   debugasan/   debugubsan/   (same structure)
```

## Installation

Requires `sudo`. Installs binaries, sound, icons and `.desktop` launcher files to
the standard XDG/FHS locations under `/usr/local` (override with `PREFIX=...`).

```bash
sudo make install-tiny       # install the Tiny (smallest) build
sudo make install-release    # install the Release (fastest) build
```

Or equivalently:

```bash
sudo cmake --install build/tiny --prefix /usr/local
```

What gets installed:

| Asset | Destination |
|-------|-------------|
| Binaries | `/usr/local/bin/` |
| Sound | `/usr/local/share/sounds/tiny_pomodoro/beep.wav` |
| Icons | `/usr/local/share/icons/hicolor/512x512/apps/` |
| Launchers | `/usr/local/share/applications/` |

## Custom Sound

Place a PCM WAV file named `beep.wav` in `assets/` before running `build_all.sh`.
It will be copied automatically to `build/<type>/sounds/beep.wav` (used when running
from the build tree) and installed to `/usr/local/share/sounds/tiny_pomodoro/beep.wav`
by `make install-*`. If the file is absent or invalid, a synthesised 880 Hz beep
is played instead.

## Building FLTK from source (`tiny_pomodoro_src`)

`tiny_pomodoro` links the pre-built `/usr/local/lib/libfltk.a`. That archive was
compiled separately with `-O3`, exceptions and RTTI on, and against libstdc++ —
so **none of the size flags below apply to it**. They apply only to `src/main.cpp`.

`tiny_pomodoro_src` builds the same `main.cpp`, but pulls FLTK in with
`add_subdirectory` so the flags reach the library's own sources. Point
`FLTK_SOURCE_DIR` at an FLTK checkout (defaults to `../fltk`):

```bash
cmake -S . -B build/tiny -DCMAKE_BUILD_TYPE=Tiny -DFLTK_SOURCE_DIR=/path/to/fltk
```

If the directory does not exist, the target is silently skipped and everything
else builds as before.

| build | pre-built FLTK | FLTK from source |
|-------|---------------:|-----------------:|
| `Release` (`-O3`, no size flags) | 1,112,096 B | 1,115,616 B |
| `Tiny` (all 15 flags) | 850,536 B | **327,552 B** |

Compiling the library from source is *not itself* an optimization — in `Release`
it is marginally bigger. It is what gives the optimizations something to work on.
It also lets FLTK be built against libc++, so the binary stops needing *two* C++
runtimes (`libstdc++` **and** `libc++abi`).

Cost: FLTK takes minutes to compile, so every build type now rebuilds it.

## Size Optimization Techniques Demonstrated

- `-Oz` — clang's maximum size optimization level
- `-flto=thin` — thin link-time optimization across translation units
- `-fvisibility=hidden` — eliminates exported symbols
- `-fno-exceptions -fno-rtti` — removes exception and type-info overhead
- `-fno-unwind-tables -fno-asynchronous-unwind-tables` — removes unwind metadata
- `-fmerge-all-constants` — deduplicates constants across TUs
- `-ffunction-sections -fdata-sections` + `-Wl,--gc-sections` — dead section elimination
- `-Wl,--icf=all` — identical code folding (requires LLD)
- `-Wl,--as-needed` — strips unused library references
- `-Wl,-s` — strips the symbol table
- `nasm -Ox` — NASM multi-pass optimizer for shortest instruction encodings (assembly target)
- `ld -nostdlib -static -s` — links assembly target with no libc and strips all symbols

## Notes

Targets Linux only. No cross-platform compatibility is intended.

## Attribution

<a href="https://www.flaticon.com/free-icons/pomodoro-technique" title="pomodoro technique icons">Pomodoro technique icons created by Freepik - Flaticon</a>

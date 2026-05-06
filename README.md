# Tiny Pomodoro

A minimal pomodoro timer application built with a focus on **executable size optimization**.

## Purpose

This project is a functional pomodoro timer application that demonstrates size optimization techniques for compiled C++ applications. It's designed as a reference implementation for a technical talk on binary size reduction strategies.

**Default Configuration:**
- Focus Time: 25 minutes
- Short Break: 5 minutes
- Long Break: 15 minutes
- Rounds Until Long Break: 4

## Tech Stack

- **Language**: C++23
- **Compiler**: clang++-20
- **Build System**: CMake 3.20+
- **GUI**:
  - `tiny_pomodoro` — FLTK-based GUI
  - `tiny_pomodoro_pro` — raw X11 (no extra GUI framework)
- **Audio**: ALSA (direct PCM; synthesised beep fallback if WAV not found)

## Features

- Automatic session progression: Focus → Short Break → (every 4th) Long Break → repeat
- Adjustable durations per session type via +/- buttons
- Sound notification at session end (custom WAV or synthesised beep)
- Keyboard shortcuts: `q` / `Esc` to quit

## Prerequisites

```bash
sudo apt install \
  clang++-20 lld libc++-20-dev libc++abi-20-dev \
  cmake libfltk1.3-dev \
  libgtk-3-dev libpango1.0-dev libcairo2-dev libglib2.0-dev \
  libharfbuzz-dev libatk1.0-dev libgdk-pixbuf-2.0-dev \
  libwayland-dev libxkbcommon-dev libdbus-1-dev \
  libasound2-dev
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
    tiny_pomodoro          ← FLTK build
    tiny_pomodoro_pro      ← raw X11 build
    sounds/beep.wav        ← copied by build_all.sh
  release/
    tiny_pomodoro
    tiny_pomodoro_pro
    sounds/beep.wav
  debug/   debugasan/   debugubsan/   (same structure)
```

## Custom Sound

Place a PCM WAV file named `beep.wav` in the project root before running `build_all.sh`. It will be copied automatically to `build/<type>/sounds/beep.wav`. If the file is absent or invalid, a synthesised 880 Hz beep is played instead.

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

## Notes

Targets Linux only. No cross-platform compatibility is intended.

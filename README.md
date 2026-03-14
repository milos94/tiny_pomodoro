# Tiny Pomodoro

A minimal pomodoro timer application built with a focus on **executable size optimization**.

## Purpose

This project demonstrates size optimization techniques for compiled C++ applications. The goal is to build the smallest possible functional pomodoro timer while maintaining clean, readable code. This is a personal project created as a reference implementation for a technical talk on binary size reduction strategies.

## Tech Stack

- **Language**: C++23
- **Compiler**: clang++-20
- **GUI Framework**: NAppGUI (https://nappgui.com/en/start/quick.html#h3)
- **Build System**: CMake 3.20+

## Building

### Prerequisites

- clang++-20
- libc++ development libraries
- CMake 3.20 or newer
- NAppGUI development libraries installed

### Build Instructions

1. **Create a build directory**:
   ```bash
   mkdir build
   cd build
   ```

2. **Configure and build** (Release optimization):
   ```bash
   cmake -DCMAKE_BUILD_TYPE=Release ..
   cmake --build .
   ```

3. **For the smallest possible binary** (uses size optimization flags):
   ```bash
   cmake -DCMAKE_BUILD_TYPE=Tiny ..
   cmake --build .
   ```

4. **Run the application**:
   ```bash
   ./tiny_pomodoro
   ```

### Build Types

- **Release**: Standard `-O3` optimization
- **Debug**: No optimization, debug symbols included
- **DebugASAN**: Debug build with Address Sanitizer
- **DebugUBSAN**: Debug build with Undefined Behavior Sanitizer
- **Tiny**: Aggressive size optimization using:
  - `-Oz` (clang's maximum size optimization)
  - `-ffunction-sections -fdata-sections`
  - Linker garbage collection (`-Wl,--gc-sections`)
  - Symbol stripping (`-Wl,-s`)

## Notes

This project is optimized for Linux with GTK3. It does not aim for cross-platform compatibility or supporting varied development environments. The focus is on demonstrating practical binary size reduction techniques for educational purposes.

## Size Optimization Techniques Demonstrated

- Compiler-level size optimization flags
- Linker-time section garbage collection
- Symbol stripping
- Function and data section splitting

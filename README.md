# Serial Dashboard

Windows desktop app (C, LVGL) for devices that send numbers over a serial / USB VCOM port:

- **Serial** tab: up to 4 connections at once, terminal (text / hex / decimal), auto reconnect.
- **Plot** tab: live plots of the received values (up to 8 plot windows, 32 variables), zoom,
  cursors, bars, command buttons, CSV export.
- **Compare** tab: records tests started by a command, stopped by a condition, a timeout or a
  plateau; chains of tests; compares recorded tests and tests opened from CSV files.

Settings are kept in `%APPDATA%\SerialDashboard\settings.ini` (profiles in `profiles\`).

`agent/` builds `serial_dashboard_cli.exe`, a command line version of the same engine for
scripts and AI agents (see `agent/AGENT.md`).

## Prerequisites

- CMake 3.20+
- GCC / MinGW toolchain with Make (`C:\w64devkit`)
- Windows 10 or newer
- LVGL in `lvgl/`: a git submodule pinned to LVGL v9.6.0. Clone with
  `git clone --recurse-submodules`, or run `git submodule update --init` after a plain clone.

## Build

Use the Windows-native MinGW toolchain and avoid the STM32 cross-toolchain that is installed on this machine.

In VS Code, use the CMake Tools extension with the `windows-gcc` preset (Configure, then Build).

Or from a terminal:

```bat
set PATH=C:\w64devkit\bin;%PATH%
cmake -S . -B cmake-build -G "MinGW Makefiles" -DCMAKE_C_COMPILER=C:\w64devkit\bin\gcc.exe -DCMAKE_MAKE_PROGRAM=C:\w64devkit\bin\make.exe
cmake --build cmake-build --target serial_dashboard
```

The executable is created under:

```text
cmake-build\serial_dashboard.exe
```

The command line tool: `agent\build_cli.bat` (output in `agent\dist`).

## Tests

`serial_dashboard_tests` (built with the app) tests the engine without the UI: conditions, CSV
export / import, plateau detection, serial line parser, settings file.

```bat
cmake --build cmake-build --target serial_dashboard_tests
cmake-build\serial_dashboard_tests.exe
```

(or `ctest --test-dir cmake-build`). Exit code 0 when all pass.

## Release and profiling builds

Presets: `windows-gcc` (Debug, `cmake-build`), `windows-gcc-release` (`cmake-build-release`) and
`windows-gcc-profile` (Release with profiling, `cmake-build-profile`). Use Release for daily use
and for performance comparisons.

The profiling build (CMake option `SD_PROFILE`, off otherwise) writes, once per second, the time
spent per stage (serial acquisition, queue, parsing, recording, chart preparation, LVGL layout and
rendering...), CPU, memory and per-port counters to a CSV file. It can also simulate serial ports.
The environment variables are listed in `Inc/profile.h`.

## Source layout

- `main.c`: window and LVGL setup, main loop.
- `sources.cmake`: the engine's source list, shared by the app and by `agent/`.
- `Src/`, `Inc/`: the engine (one `.c` / `.h` pair per module). System calls go through
  `platform.h` (`platform_win.c`); the window is `app_window.c`, the serial ports `serial_win.c`
  with `serial_common.c` (receive queue).

# Serial PID Plotter

Windows desktop app (C, LVGL; macOS / Linux versions not tested yet) for devices that send numbers over a serial / USB VCOM port:

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
cmake-build\Serial_PID_Plotter.exe
```

(The CMake target keeps the name `serial_dashboard`. The settings stay in the `SerialDashboard`
folder, so settings from older versions are kept.)

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

## macOS and Linux (not tested)

The app also builds there, with SDL2 for the window (LVGL's SDL driver). **Not tested yet:**
on Linux it was only built and run in WSL (Ubuntu): the tests pass and the window opens, but it
has not been used on a Linux desktop or with a serial device. macOS has never been built: expect
fixes on the first build.

```sh
# macOS (Homebrew)
brew install cmake sdl2 python
# Debian / Ubuntu
sudo apt install build-essential cmake python3 libsdl2-dev

git clone --recurse-submodules <repo> && cd "Serial Dashboard"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target serial_dashboard serial_dashboard_tests -j
./build/serial_dashboard_tests
./build/Serial_PID_Plotter
```

- Serial ports: macOS `/dev/cu.*`, Linux `/dev/ttyUSB*`, `ttyACM*`, `ttyAMA*` and real `ttyS*`.
  On Linux the user must be allowed to open them (group `dialout` or `uucp`).
- File and folder dialogs: macOS `osascript`, Linux `zenity` (or `kdialog`); notifications:
  `osascript`, `notify-send`.
- Settings: `~/Library/Application Support/SerialDashboard` (macOS), `~/.config/SerialDashboard`
  (Linux).
- Not on macOS / Linux yet: the profiling build (`SD_PROFILE`), the command line tool (`agent/`),
  the window icon in the theme's colors, the title bar colors, non-standard baud rates on Linux,
  1.5 stop bits, mark / space parity on macOS.

## License

MIT (see `LICENSE`): free to use, modify and publish, as long as the copyright notice is kept.
LVGL (`lvgl/`) has its own MIT license.

## Source layout

- `main.c`: window and LVGL setup, main loop.
- `sources.cmake`: the engine's source list, shared by the app and by `agent/`.
- `Src/`, `Inc/`: the engine (one `.c` / `.h` pair per module). What differs between the systems:
  `platform.h` (`platform_win.c` / `platform_posix.c`), the window (`app_window.c` Windows,
  `app_window_sdl.c` macOS / Linux) and the serial ports (`serial_win.c` / `serial_posix.c`, both
  with `serial_common.c`).

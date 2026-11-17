# Serial PID Plotter

Desktop app for devices that send numbers over a serial / USB VCOM port: an Arduino, an STM32,
a PID controller, a sensor board... It shows what the device sends, plots it live, and records
tests (for example a step of a PID setpoint) to compare them side by side and save them for a
report.

Windows 10 / 11, written in C with [LVGL](https://lvgl.io). macOS and Linux versions exist but are
**not tested yet** (see [macOS and Linux](#macos-and-linux-not-tested)).

- [Quick start](#quick-start)
- [Serial tab](#serial-tab)
- [Plot tab](#plot-tab)
- [Commands window](#commands-window)
- [Compare tab: recording tests](#compare-tab-recording-tests)
- [Chains of tests](#chains-of-tests)
- [Test bench: auto save and report pictures](#test-bench-auto-save-and-report-pictures)
- [Settings, profiles and display](#settings-profiles-and-display)
- [Files](#files)
- [Command line tool](#command-line-tool)
- [Limits](#limits)
- [Building](#prerequisites)

## Quick start

1. Download `Serial_PID_Plotter.exe` (Windows x64) from the
   [Releases](https://github.com/Artgameur/Serial-PID-Plotter/releases) page (the zip also has the
   command line tool), or take it from [`bin/`](bin/) in this repo. Run it: nothing to install.
   Windows may warn "Windows protected your PC" the first time (the exe is not code-signed): click
   **More info**, then **Run anyway**.
2. **Serial** tab: choose the port and the baud rate, then **Connect**.
3. Make the device print one line of numbers per sample, for example:

   ```text
   17.08 17.50 -2.38
   Temp:17.08, SP:17.50, Out:-2.38
   ```

   Each number becomes a variable (`Var 1`, `Var 2`, ...). Text like `Temp:` is skipped.
4. **Plot** tab: the variables are plotted live. The gear button names them, gives them a unit,
   a color, a gain and an offset.
5. To compare tests: add a command (ex: `SP:20`) in the **Commands** window and check its film
   button. Each time it is sent, the **Compare** tab records a test until it settles.

The **Help** button (top right) explains each tab in the app.

## Serial tab

- **Up to 4 connections at once**, each with its own port and settings. They share the 32
  variables: with 2 connections, C1 plots Var 1-16 and C2 Var 17-32.
- **Port list** with the device's description (ex: "USB Serial Device"), refreshed with one
  click. The status LED is green when connected, amber when waiting to reconnect.
- **Baud rate**: presets from 9600 to 2 000 000, or any value typed.
- **Data format**: 5 to 8 data bits; parity none, odd, even, mark or space; 1, 1.5 or 2 stop bits.
- **Auto reconnect**: reopens the port when the device comes back (unplugged, reset, or not
  there yet when the app starts).
- **Line format**:
  - separator between the values: Auto, space, comma, semicolon, colon or tab;
  - variables per line: Auto (as many as received) or a fixed number;
  - line ending added to what you send: none, LF, CR or CR+LF.
- **Terminal**: what the device sends, shown as text, hex or decimal bytes. Type a line at the
  bottom and press Enter to send it, to any connection. Clear button.
- **Messages in the terminal**: connected, disconnected, waiting to reconnect, send failed, and
  warnings when data could be lost (see [Receiving data](#receiving-data)).

### Receiving data

- Each received line is split into numbers. Text fields (labels like `Temp:` or `SP=`) are skipped,
  so `T:17.1 SP:20` gives 2 variables.
- **Echo of a sent command**: some devices echo the command without a line break, merging it with
  the next data line (`SP:17` + `13.07 ...` = `SP:1713.07 ...`, a false spike). The app removes the
  echo from lines received within 2 s of sending.
- **Replies are not data**: a line with a different number of text fields than the data lines (ex:
  the device's reply to a command) is not plotted, unless that new format repeats 3 times (the
  device changed what it sends).
- **Nothing is dropped**: a reader thread per port stamps each chunk with its arrival time and puts
  it in a 4 MB queue. If the app ever falls behind, it pauses reading (the data waits in the
  device's driver) instead of losing it, and says so in the terminal. Lost data reported by
  Windows and framing / parity errors are shown too.

## Plot tab

- **Up to 8 plot windows**, each showing the variables chosen for it. Windows can be moved,
  resized, and arranged side by side with **Tile**.
- **Length**: 100 to 10 000 points shown. The last 10 000 samples are kept, whatever the length.
- **X axis**: sample number, seconds before the latest sample, or clock time.
- **Y scale**: automatic (follows the data) or fixed min / max, per plot.
- **Zoom**: drag along the X or Y axis, or draw a zoom box. Right-click a plot to reset its zoom;
  the reset button resets all of them. The grid follows the zoom.
- **Cursors**: V1 / V2 (vertical) and H1 / H2 (horizontal) to measure times and values.
- **Bars**: a bar graph of the current values beside the chart (or alone). The split between the
  chart and the bars is dragged.
- **Marks**: a line on the plot each time a command is sent, labelled with the command.
- **Pause / resume**, **clear**, and **export to CSV** (every variable, with gain and offset
  applied).
- **Variable settings** (gear button), for each of the 32 variables:
  - name and unit (presets like °C, V, mA, rpm, or typed);
  - color (16 presets) and line thickness;
  - plotted value = received value x **gain** + **offset** (gain `2.5`, or `/4` to divide);
  - which plots show it.

## Commands window

Keeps the commands you send often, up to 32. Each one is sent with one click.

- **Command alone** (ex: `RESET`), or **command + separator + value** (ex: `SP` `:` `20` sends
  `SP:20`).
- **Several values** (`10,15,20`): the value is chosen in a dropdown on the command's row.
- **Text field**: the value is typed on the row before sending.
- **Connection**: each command is sent on its own connection (C1 to C4).
- **Film button**: sending this command starts a test in the Compare tab.
- Commands are reordered by dragging, edited and deleted. The window can be hidden.

## Compare tab: recording tests

Records tests to compare them: for example the response of a temperature controller to a
setpoint step, with different PID gains.

### Starting a test

- Send a command whose film button is checked (Commands window, or a chain), or press the film
  button of the Compare tab to record without sending anything (the test is named `Manual`).
- The download button of the Compare tab adds the data currently in the plots as a test.
- A test records every variable from its start, up to 1 000 000 samples. It is named after its
  command and the time (ex: `SP:20 14:03:15`); rename it with what changed (ex: `Kp 2`).

### What ends a test

A test ends by itself on the first of these that happens (or with the Stop button):

| Stop | Ends when | Shown as |
|---|---|---|
| **Settled** | the **Watched** variable (ex: the temperature) stays within the tolerance of the **Constant** (ex: the setpoint) for the **last N samples** | "Reached at 42.3 s": when it entered the band for good |
| **Custom condition** | a condition you write is true (see below) | condition met |
| **Max test time** | the test lasted this long (0: no limit) | not stable |
| **Auto stable** (optional) | the watched variable stopped changing, even far from the constant: the averages of its 3 highest and 3 lowest peaks are within the tolerance for a given time. An oscillation keeps the test going. **Smooth** averages the noise out first. | "Stable at 19.82 from 38.1 s" |

The tolerance is typed in percent, with the percent that 1 unit represents (ex: 1 °C = 1 %).

### Custom conditions

Evaluated on each new sample; the test ends when it is true. Examples:

```text
Temp >= 20
held(abs(Temp - 17) <= 0.1, 250)
t > 60 || abs(watch - start) >= 5
abs(slope(watch, 50)) < 0.01 && n > 500
```

| Kind | Available |
|---|---|
| Operators | numbers, `+ - * /`, `< <= > >= == !=`, `&& \|\| !`, `( )` |
| Variables | `Temp`, or the full name in quotes: `"Temp [°C]"` (needed with spaces) |
| Handles | `ref` (constant), `watch` (watched variable), `band` (tolerance in units), `N` (samples), `t` (seconds since the start), `n` (samples since the start), `start` (watched variable when the test started) |
| Functions | `abs(x)`, `min(a, b)`, `max(a, b)` |
| Over the last k samples | `held(cond, k)` (true on all of them), `avg(x, k)`, `lowest(x, k)`, `highest(x, k)`, `prev(x, k)` (k samples ago), `slope(x, k)` (change per second) |

A value that is missing, or a function without enough samples yet, gives no value, and a
comparison with no value is false. The note under the condition shows errors with their position
(unknown name, syntax error...).

### Device settings in the legend

The app can ask the device for its settings when each test starts (**Request**, ex: `CONFIG`) and
keep the reply (lines starting with **Reply starts**, ex: `Config;`). The reply is saved with the
test, and the legend shows the settings that the test's commands changed (ex: `SP:17.00,
kP:10.00`).

### Comparing

- **Up to 16 tests**, recorded or opened from CSV files.
- **1 to 4 graphs**, each showing up to 4 variables of the tests chosen for it (number buttons on
  each test and variable). The constant (ex: the setpoint) is drawn once, in gray; if it differs
  between tests, the app says so.
- Zoom by dragging on a graph; right-click or the reset button to go back.
- The legend can be dragged out of the way.
- Each test can be renamed, saved to CSV, removed; tests are opened back from CSV files.
- **Picture** button: saves the graphs as a PNG in the test bench folder.

## Chains of tests

Runs tests one after the other, unattended: for example the same setpoint step with 5 different
gains.

- **Up to 16 rows**. Each row's commands are separated by `|` (ex: `Kp:2 | SP:5`); the last one
  starts the test. **Between**: time between the commands of a row (up to 60 s).
- The next row is sent when the test ends by its condition, its max time or Auto stable. Stop by
  hand stops the chain.
- **Prepare**: commands that bring the setup back to its start point (ex: `SP:15`), sent before
  the first test and after each one, however it ended.
- **Start when** + **Held for**: waits until a condition stays true for some seconds before the
  next test. Ex: `abs(watch-start)<1` waits until the temperature is back near where the last
  test started.
- A chain needs a way to end its tests (a valid condition, Auto stable or a max test time).

## Test bench: auto save and report pictures

- **Auto save**: each test that ends is saved in the folder (default
  `Documents\Serial Dashboard`): a CSV file and a picture of the graphs, named
  `<date>_<time>_<test name>`.
- **Picture**: as on screen, or **Report**: on white, with text and lines sized for a printed page.
  Format (width:height, ex: `16:8`), width on the page (ex: 6.5 in) and resolution (ex: 300 ppi)
  give the size in pixels (100 to 6000 per side).
- Pictures are drawn in the background, so the app keeps running.
- **Notify**: a Windows notification when a test or a chain ends by itself (not when you stop it).

## Settings, profiles and display

- **Everything is saved** and comes back at the next start: ports, line format, variables, plots
  and their windows, commands, test settings, chains.
- **Profiles**: save the whole setup under a name and switch between projects. Loading a profile
  restarts the app.
- **Export / Import**: the whole setup, or only the plot layout, to a file (to share it or move it
  to another PC).
- **Language**: English or French.
- **UI scale**: 75 % to 200 %. **Max frame rate**: 15, 30, 60 or 120 fps.
- **Theme**: light or dark, with 7 accent colors. The window icon and title bar follow them.

## Files

- **Settings**: `%APPDATA%\SerialDashboard\settings.ini`, profiles in `profiles\` beside it.
- **CSV files** (plot export, saved tests, test bench): UTF-8 with BOM (Excel shows the units),
  comma separated, `.` decimals. Columns: sample number, date and time, seconds since the first
  row, then one per variable (`Name [unit]`, with gain and offset applied). Missing values are
  empty cells. A saved test ends with `#settings` and `#commands` rows. These files can be opened
  back in the Compare tab.

## Command line tool

`serial_dashboard_cli.exe` (in `cli\` in the release, built from `agent/`) runs the same engine
without a window, for scripts and AI agents: send commands, read replies, record tests with the
same stop conditions, run chains, export CSV files and report pictures, and make pictures from
saved CSV files. It uses the app's saved settings without changing them. Every command prints
JSON. See [`agent/AGENT.md`](agent/AGENT.md).

## Limits

| What | Max |
|---|---|
| Serial connections at once | 4 |
| Variables (shared by the connections) | 32 |
| Plot windows | 8 |
| Samples kept by the plots | 10 000 per variable |
| Saved commands | 32 |
| Tests in the Compare tab | 16 |
| Samples per test | 1 000 000 |
| Graphs / variables per graph | 4 / 4 |
| Rows of a chain | 16 |
| Received line | 1024 characters |

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

`serial_dashboard_tests` (built with the app) tests the engine without the UI: conditions, the end
of a recorded test (stop conditions, settled time, Auto stable, max time, chains), CSV export /
import, plateau detection, serial line parser, settings file.

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

git clone --recurse-submodules https://github.com/Artgameur/Serial-PID-Plotter.git && cd Serial-PID-Plotter
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
- `tests/`: the engine's tests. `agent/`: the command line tool.

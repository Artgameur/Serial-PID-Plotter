# Serial Dashboard

This project sets up a minimal standalone C application that:

- builds a standalone Windows executable with CMake
- uses LVGL for the UI layer
- enumerates COM ports and opens a COM port on Windows

## Prerequisites

- CMake 3.20+
- GCC / MinGW toolchain with Make
- Windows 10 or newer

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

## Notes

- The code opens a serial port using the Windows API and polls for incoming data.
- The LVGL UI updates a label with the detected COM ports and any bytes received.
- If you want to target a different COM port, edit the value in `main.c`.

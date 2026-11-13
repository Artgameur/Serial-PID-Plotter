@echo off
rem The compiler is not probed: its test program gets deleted on this PC.
rem Builds the agent's tool in agent\build (not in cmake-build) and puts it in agent\dist.
setlocal
set PATH=C:\w64devkit\bin;%PATH%
cd /d "%~dp0"
"C:\Program Files\CMake\bin\cmake.exe" -S . -B build -G "MinGW Makefiles" -DCMAKE_C_COMPILER=C:/w64devkit/bin/gcc.exe -DCMAKE_MAKE_PROGRAM=C:/w64devkit/bin/make.exe -DCMAKE_C_COMPILER_FORCED=TRUE -DCMAKE_C_COMPILER_ID=GNU -DCMAKE_C_COMPILER_ID_RUN=TRUE -DCMAKE_CXX_COMPILER_FORCED=TRUE -DCMAKE_CXX_COMPILER_ID=GNU -DCMAKE_CXX_COMPILER_ID_RUN=TRUE -DCMAKE_ASM_COMPILER_WORKS=1 || exit /b 1
"C:\Program Files\CMake\bin\cmake.exe" --build build --target serial_dashboard_cli -j 8 || exit /b 1
if not exist dist mkdir dist
copy /y build\serial_dashboard_cli.exe dist\ >nul
copy /y AGENT.md dist\ >nul
echo Done: %~dp0dist

@echo off
REM ============================================================
REM build.bat — Build the FreeRTOS Multi-Task Simulator
REM
REM Requirements (one of these two options):
REM   Option A (CMake): cmake + mingw32-make in PATH
REM   Option B (Direct): gcc in PATH (MSYS2/MinGW-w64)
REM
REM Usage:
REM   build.bat          — standard build
REM   build.bat clean    — clean and rebuild
REM   build.bat run      — build and run
REM   build.bat test     — build and run unit tests
REM ============================================================

setlocal

set BUILD_DIR=build
set EXE=%BUILD_DIR%\rtos_scheduler.exe
set TEST_EXE=%BUILD_DIR%\test_runner.exe

REM ---- Detect gcc location ----
set GCC=gcc
where gcc >nul 2>&1
if errorlevel 1 (
    if exist "C:\msys64\ucrt64\bin\gcc.exe" (
        set GCC=C:\msys64\ucrt64\bin\gcc.exe
    ) else if exist "C:\msys64\mingw64\bin\gcc.exe" (
        set GCC=C:\msys64\mingw64\bin\gcc.exe
    ) else (
        echo [ERROR] gcc not found! Install MinGW-w64 or MSYS2.
        exit /b 1
    )
)

REM Handle "clean" argument
if "%1"=="clean" (
    echo [BUILD] Cleaning build directory...
    if exist %BUILD_DIR% rmdir /s /q %BUILD_DIR%
    echo [BUILD] Clean complete.
    if "%2"=="" goto :eof
)

REM Create build directory
if not exist %BUILD_DIR% mkdir %BUILD_DIR%

REM Handle "test" argument directly
if "%1"=="test" goto :test

REM ---- Try CMake first, fall back to direct gcc ----
set USE_CMAKE=0
where cmake >nul 2>&1
if not errorlevel 1 (
    where mingw32-make >nul 2>&1
    if not errorlevel 1 set USE_CMAKE=1
)

if "%USE_CMAKE%"=="1" (
    echo [BUILD] Using CMake + MinGW Makefiles...
    cd %BUILD_DIR%
    cmake .. -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
    if errorlevel 1 ( cd .. && exit /b 1 )
    mingw32-make -j4
    if errorlevel 1 ( cd .. && exit /b 1 )
    cd ..
) else (
    echo [BUILD] Using direct gcc compilation...
    echo [BUILD] gcc: %GCC%
    echo.
    %GCC% -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-format-extra-args ^
        -Isrc ^
        src/freertos_sim.c ^
        src/sensor_task.c ^
        src/processing_task.c ^
        src/logging_task.c ^
        src/alert_task.c ^
        src/priority_inversion_demo.c ^
        src/main.c ^
        -o %EXE% ^
        -lwinmm -lkernel32 -lm
    if errorlevel 1 (
        echo [ERROR] Compilation failed! Check errors above.
        exit /b 1
    )
)

echo.
echo ============================================================
echo  BUILD SUCCESSFUL
echo  Executable: %EXE%
echo ============================================================
echo.

REM Handle "run" argument
if "%1"=="run" goto :run
if "%2"=="run" goto :run
if "%2"=="test" goto :test
goto :eof

:run
echo [BUILD] Running rtos_scheduler.exe...
echo.
%EXE%
goto :eof

:test
echo [BUILD] Compiling unit test suite...
echo [BUILD] gcc: %GCC%
echo.
%GCC% -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter ^
    -Isrc ^
    tests/unit_tests.c ^
    src/freertos_sim.c ^
    -o %TEST_EXE% ^
    -lwinmm -lkernel32 -lm
if errorlevel 1 (
    echo [ERROR] Test compilation failed! Check errors above.
    exit /b 1
)

echo.
echo ============================================================
echo  TEST SUITE COMPILED SUCCESSFULLY
echo  Executable: %TEST_EXE%
echo ============================================================
echo.
echo [BUILD] Executing unit tests...
echo.
%TEST_EXE%

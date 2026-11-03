@echo off
rem ---------------------------------------------------------------------------
rem  Firmware C unit tests (PC / GCC)
rem  Builds the REAL firmware sources, not a copy:
rem      ..\App\Src\app_proto.c  app_alarm.c  app_filter.c  app_diag.c
rem  Usage: run from this folder  ->  run_tests.bat
rem ---------------------------------------------------------------------------
setlocal

set "GCC="
if exist "%ProgramFiles(x86)%\Dev-Cpp\MinGW64\bin\gcc.exe" set "GCC=%ProgramFiles(x86)%\Dev-Cpp\MinGW64\bin\gcc.exe"
if not defined GCC if exist "%ProgramFiles%\Dev-Cpp\MinGW64\bin\gcc.exe" set "GCC=%ProgramFiles%\Dev-Cpp\MinGW64\bin\gcc.exe"
if not defined GCC if exist "C:\msys64\mingw64\bin\gcc.exe" set "GCC=C:\msys64\mingw64\bin\gcc.exe"
if not defined GCC for /f "delims=" %%i in ('where gcc 2^>nul') do if not defined GCC set "GCC=%%i"

if not defined GCC (
  echo [ERROR] gcc not found. Install MinGW-w64 or add gcc to PATH.
  exit /b 1
)

echo Compiler:
"%GCC%" --version | findstr /i "gcc"
echo.

"%GCC%" -std=c99 -Wall -Wextra -Werror -O2 -I..\App\Inc -I. ^
  main.c test_proto.c test_alarm.c test_filter.c test_diag.c ^
  ..\App\Src\app_proto.c ..\App\Src\app_alarm.c ..\App\Src\app_filter.c ^
  ..\App\Src\app_diag.c ^
  -o fw_tests.exe

if errorlevel 1 (
  echo.
  echo [ERROR] build failed
  exit /b 1
)

echo.
fw_tests.exe
if errorlevel 1 (
  echo.
  echo [FAIL] some assertions failed
  exit /b 1
)

echo.
echo [PASS] all assertions passed
exit /b 0

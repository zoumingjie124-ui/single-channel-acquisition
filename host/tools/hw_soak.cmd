@echo off
rem ==========================================================================
rem  Stage 7B-HW board soak: capture (read-only serial) + offline parse, one shot.
rem  Usage:  hw_soak.cmd [COMx] [minutes]      default COM6 10
rem  Output: ..\docs\soak_<timestamp>_raw.bin / _raw.txt  (timestamped, no overwrite)
rem  NOTE:   read-only; never modifies firmware/protocol. If COM is busy the
rem          capture exits with an error instead of corrupting anything.
rem          Keep this file ASCII-only (cmd.exe reads batch as ANSI/GBK).
rem ==========================================================================
setlocal
set "PORT=%~1"
if "%PORT%"=="" set "PORT=COM6"
set "MIN=%~2"
if "%MIN%"=="" set "MIN=10"
set "TOOLS=%~dp0"
set "DOCS=%~dp0..\docs"
set /a SECS=MIN*60
for /f %%t in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd_HHmmss"') do set "TAG=%%t"
set "RAW=%DOCS%\soak_%TAG%_raw.bin"

echo [hw_soak] port=%PORT%  minutes=%MIN%  tag=%TAG%
powershell -NoProfile -ExecutionPolicy Bypass -File "%TOOLS%hw_soak_capture.ps1" -Port %PORT% -Seconds %SECS% -OutDir "%DOCS%" -RunTag %TAG%
if errorlevel 1 goto :err
powershell -NoProfile -ExecutionPolicy Bypass -File "%TOOLS%hw_soak_parse.ps1" -RawPath "%RAW%" -OutTxt "%DOCS%\soak_%TAG%_raw.txt"
if errorlevel 1 goto :err
echo [hw_soak] done: %RAW%
exit /b 0

:err
echo [hw_soak] FAILED
exit /b 1

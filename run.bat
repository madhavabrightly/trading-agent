@echo off
setlocal
REM run.bat - launch the built edge_monitor
set ROOT=%~dp0
cd /d "%ROOT%"

if not exist build\bin\edge_monitor.exe (
    echo edge_monitor.exe not found. Run build.bat first.
    exit /b 1
)

build\bin\edge_monitor.exe %*
exit /b %errorlevel%

@echo off
setlocal
REM discover-edge.bat - list Edge pages currently open
set ROOT=%~dp0
cd /d "%ROOT%"

if not exist build\bin\edge_monitor.exe (
    echo edge_monitor.exe not found. Run build.bat first.
    exit /b 1
)

build\bin\edge_monitor.exe discover
exit /b %errorlevel%

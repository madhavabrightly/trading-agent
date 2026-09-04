@echo off
setlocal
REM start-monitor.bat - launch the monitoring engine (Ctrl+C to stop)
set ROOT=%~dp0
cd /d "%ROOT%"

if not exist build\bin\edge_monitor.exe (
    echo edge_monitor.exe not found. Run build.bat first.
    exit /b 1
)

build\bin\edge_monitor.exe start
exit /b %errorlevel%

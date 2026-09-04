@echo off
setlocal
REM clean-build.bat - wipe build dir and rebuild from scratch (Release/Ninja)
set ROOT=%~dp0
cd /d "%ROOT%"

echo [1/3] Cleaning build directory...
if exist build rmdir /s /q build

echo [2/3] Configuring...
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release || goto :err

echo [3/3] Building...
cmake --build build || goto :err

echo.
echo CLEAN BUILD OK: %ROOT%build\bin\edge_monitor.exe
exit /b 0

:err
echo.
echo CLEAN BUILD FAILED.
exit /b 1

@echo off
setlocal
REM build.bat - configure and build Release with Ninja
set ROOT=%~dp0
cd /d "%ROOT%"

if not exist build\CMakeCache.txt (
    echo [1/2] Configuring...
    cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release || goto :err
) else (
    echo [1/2] Build directory already configured.
)

echo [2/2] Building...
cmake --build build || goto :err

echo.
echo Build OK: %ROOT%build\bin\edge_monitor.exe
exit /b 0

:err
echo.
echo BUILD FAILED.
exit /b 1

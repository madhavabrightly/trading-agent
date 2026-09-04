@echo off
setlocal enabledelayedexpansion

:: History Monitor Script for Command Code
:: Continuously monitors session and extracts chat to history.md

set "SCRIPT_DIR=%~dp0"
set "HISTORY_FILE=%SCRIPT_DIR%history.md"
set "SESSION_START=%date% %time%"
set "LOG_FILE=%USERPROFILE%\.commandcode\projects\c-users-brigh-desktop-trying-new-dt-alpha-test-ext\7167239c-4f29-4de5-bb6f-05d077340b00.jsonl"
set "LAST_SIZE=0"

echo ============================================ > "%HISTORY_FILE%"
echo # Session History >> "%HISTORY_FILE%"
echo. >> "%HISTORY_FILE%"
echo **Started**: %SESSION_START% >> "%HISTORY_FILE%"
echo **Log Source**: %LOG_FILE% >> "%HISTORY_FILE%"
echo. >> "%HISTORY_FILE%"
echo --- >> "%HISTORY_FILE%"
echo. >> "%HISTORY_FILE%"

echo [Monitor] History logging started
echo [Monitor] Source: %LOG_FILE%
echo [Monitor] Output: %HISTORY_FILE%
echo [Monitor] Press Ctrl+C to stop...
echo.

:LOOP
if not exist "%LOG_FILE%" (
    timeout /t 5 /nobreak >nul
    goto :LOOP
)

for %%A in ("%LOG_FILE%") do set "CURRENT_SIZE=%%~zA"

if !CURRENT_SIZE! GTR !LAST_SIZE! (
    if !LAST_SIZE! EQU 0 (
        :: First run - get last 100 lines
        powershell -Command "Get-Content '%LOG_FILE%' -Tail 100 -Encoding UTF8 | ForEach-Object { try { $j = $_ | ConvertFrom-Json; '[' + $j.role + ']: ' + $j.content } catch { $_ } } | Out-String" >> "%HISTORY_FILE%"
    ) else (
        :: Incremental - get only new bytes
        powershell -Command "$old = %LAST_SIZE%; $new = (Get-Item '%LOG_FILE%').Length; $skip = $old; $take = $new - $old; if ($take -gt 0) { Get-Content '%LOG_FILE%' -Encoding UTF8 -Skip $skip -TotalCount $take | ForEach-Object { try { $j = $_ | ConvertFrom-Json; '[' + $j.role + ']: ' + $j.content } catch { $_ } } | Out-String" >> "%HISTORY_FILE%" 2>nul
    )
    set "LAST_SIZE=!CURRENT_SIZE!"
    echo [%date% %time%] Synced >> "%HISTORY_FILE%"
)

timeout /t 10 /nobreak >nul
goto :LOOP

@echo off
rem Wrapper so the uninstaller can be double-clicked from the install folder.
rem ASCII only (cmd.exe reads .cmd files in the ANSI codepage).
rem
rem Important: move the shell's working directory out of the install folder first.
rem This cmd window lives until the user presses a key, and Windows cannot delete
rem a directory that is some process's current directory - the final cleanup
rem step would fail and leave an empty folder behind.
setlocal
chcp 65001 >nul
cd /d "%TEMP%"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\uninstall.ps1" %*
echo.
pause
endlocal

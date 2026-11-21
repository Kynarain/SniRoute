@echo off
chcp 65001 >nul
rem Wrapper so the .ps1 can be launched from any working directory.
rem %~dp0 is this script's own folder (with trailing backslash).
rem ASCII only: cmd.exe reads .cmd files in the ANSI codepage, so non-ASCII
rem comments here would be garbled and executed as commands.
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0start-chrome-ech.ps1" %*
endlocal

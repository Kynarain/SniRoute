@echo off
chcp 65001 >nul
rem Build the Java GUI into build\gui-classes. ASCII only (cmd ANSI codepage).
setlocal enableextensions
set "REPO=%~dp0.."
pushd "%REPO%" || exit /b 1

where javac >nul 2>nul
if errorlevel 1 (
    echo javac not found. Install a JDK ^(11 or newer^) and put it on PATH.
    popd
    exit /b 1
)

if not exist "build\gui-classes" mkdir "build\gui-classes"

rem -encoding UTF-8 is explicit: the source is ASCII, but stating the encoding
rem means a future non-ASCII edit cannot silently depend on the platform default.
rem The -J flags force English diagnostics: the default locale prints compiler
rem errors in the console codepage, which shows up as mojibake when piped.
javac -J-Duser.language=en -J-Duser.country=US -encoding UTF-8 -d "build\gui-classes" "gui\SnirouteGui.java"
if errorlevel 1 (
    echo Build failed.
    popd
    exit /b 1
)

echo Built: build\gui-classes\SnirouteGui.class
popd
endlocal

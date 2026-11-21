@echo off
chcp 65001 >nul
rem Launch the sniroute GUI.
rem This is a tray application: it never shows a window.
rem
rem Two front ends exist and they behave identically, because both are thin
rem shells over scripts\sniroute.py:
rem   1. Java / Swing      gui\SnirouteGui.java   (preferred, needs build-gui.cmd)
rem   2. PowerShell forms  scripts\sniroute-gui.ps1 (fallback, needs nothing)
setlocal enableextensions
set "REPO=%~dp0.."
pushd "%REPO%" || exit /b 1

if not exist "build\gui-classes\SnirouteGui.class" goto try_ps
where javaw >nul 2>nul
if errorlevel 1 (
    where java >nul 2>nul
    if errorlevel 1 goto try_ps
    java -cp "build\gui-classes" SnirouteGui %*
    goto done
)
start "" javaw -cp "build\gui-classes" SnirouteGui %*
goto done

:try_ps
if not exist "scripts\sniroute-gui.ps1" goto nothing
echo Java build not found, falling back to the PowerShell UI.
echo Build the Java one with: scripts\build-gui.cmd
powershell -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "scripts\sniroute-gui.ps1" %*
goto done

:nothing
echo No GUI available. Build the Java one: scripts\build-gui.cmd
exit /b 1

:done
popd
endlocal

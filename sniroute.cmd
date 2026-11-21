@echo off
chcp 65001 >nul
rem sniroute: single entry point.
rem ASCII only -- cmd.exe reads .cmd files in the ANSI codepage, so any
rem non-ASCII character here would be garbled and executed as a command.
rem
rem Structure note: this file avoids nested parenthesised blocks that depend on
rem variables set inside the same block. cmd expands %VAR% at parse time, so
rem such blocks silently see an empty value. goto labels do not have that trap.
rem
rem Exit-code note: endlocal resets ERRORLEVEL, so the status is captured in RC
rem right after each interpreter runs and returned after endlocal. Without this
rem the tray saw 0 for every failure and reported "done" while nothing started.
setlocal enableextensions
set "RC=0"
set "REPO=%~dp0"
if "%REPO:~-1%"=="\" set "REPO=%REPO:~0,-1%"
set "SCRIPT=%REPO%\scripts\sniroute.py"
rem No arguments means: show the command list.
set "ARGS=%*"
if "%~1"=="" set "ARGS=--help"

if not exist "%SCRIPT%" (
    echo Cannot find "%SCRIPT%"
    exit /b 1
)

rem Candidate 0: the Python shipped inside this folder (portable package).
if not exist "%REPO%\python\python.exe" goto try_py
"%REPO%\python\python.exe" "%SCRIPT%" %ARGS%
set "RC=%ERRORLEVEL%"
goto done

rem Candidate 1: the official py launcher.
:try_py
where py >nul 2>nul
if errorlevel 1 goto try_python
py -3 -c "import sys" >nul 2>nul
if errorlevel 1 goto try_python
py -3 "%SCRIPT%" %ARGS%
set "RC=%ERRORLEVEL%"
goto done

rem Candidate 2: python on PATH. The Microsoft Store stub also answers to
rem "python" but does nothing, hence the -c probe.
:try_python
where python >nul 2>nul
if errorlevel 1 goto try_bundled
python -c "import sys" >nul 2>nul
if errorlevel 1 goto try_bundled
python "%SCRIPT%" %ARGS%
set "RC=%ERRORLEVEL%"
goto done

rem Candidate 3: the Python bundled with DSH Desktop.
:try_bundled
set "BUNDLED=%LOCALAPPDATA%\Programs\DSH Desktop\resources\office-runtime\primary-runtime\dependencies\python\python.exe"
if not exist "%BUNDLED%" goto no_python
"%BUNDLED%" "%SCRIPT%" %ARGS%
set "RC=%ERRORLEVEL%"
goto done

:no_python
echo.
echo No working Python 3 found (py -3, python, and the bundled runtime all failed).
echo Install Python 3 and make sure "python" runs. These scripts need the
echo standard library only -- there is nothing to pip install.
endlocal & exit /b 1

:done
endlocal & exit /b %RC%

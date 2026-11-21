@echo off
chcp 65001 >nul
rem Start the local DoH server. ASCII only (see start-chrome-ech.cmd).
rem Usage:  doh-ech.cmd [port]      default port 5444
setlocal enableextensions
set "REPO=%~dp0.."
set "PORT=%~1"
if "%PORT%"=="" set "PORT=5444"

rem 1) official launcher
where py >nul 2>nul
if not errorlevel 1 (
    py -3 -c "import sys" >nul 2>nul
    if not errorlevel 1 (
        py -3 "%REPO%\tools\doh_ech.py" --listen 127.0.0.1:%PORT%
        goto :done
    )
)

rem 2) python on PATH -- must actually run, the Store stub does not
where python >nul 2>nul
if not errorlevel 1 (
    python -c "import sys" >nul 2>nul
    if not errorlevel 1 (
        python "%REPO%\tools\doh_ech.py" --listen 127.0.0.1:%PORT%
        goto :done
    )
)

rem 3) the Python bundled with DSH Desktop, if present on this machine
set "BUNDLED=%LOCALAPPDATA%\Programs\DSH Desktop\resources\office-runtime\primary-runtime\dependencies\python\python.exe"
if exist "%BUNDLED%" (
    "%BUNDLED%" "%REPO%\tools\doh_ech.py" --listen 127.0.0.1:%PORT%
    goto :done
)

echo.
echo No working Python 3 found (py -3, python, and the DSH bundled runtime
echo all failed). Install Python 3 and make sure "python" runs.
echo The scripts need no third-party packages, the standard library is enough.
exit /b 1

:done
endlocal

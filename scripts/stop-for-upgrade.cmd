@echo off
rem Stops everything this install is running, so an upgrade can overwrite files.
rem Called by the MSI as a custom action before InstallFiles.
rem
rem ASCII only. Runs with the install folder as the working directory.
rem
rem Why this exists: python312.dll and nginx.exe are locked while the DoH server
rem and nginx are running from this folder. Without this step the upgrade fails
rem with "error writing file ... make sure you can access this directory".
rem
rem Order matters: stop the tray last, because stopping it does not stop the
rem background services, and stopping the services first avoids a tray restart
rem racing with us.

setlocal

rem 1. background services (precise: uses the pid files, not a blanket taskkill)
if exist "scripts\sniroute.py" (
    if exist "python\python.exe" (
        "python\python.exe" "scripts\sniroute.py" service down >nul 2>nul
        "python\python.exe" "scripts\sniroute.py" doh stop    >nul 2>nul
    )
)

rem 2. nginx: fall back to its pid file if the CLI did not manage it
if exist "logs\nginx.pid" (
    for /f "usebackq delims=" %%p in ("logs\nginx.pid") do (
        taskkill /F /PID %%p >nul 2>nul
    )
    del /q "logs\nginx.pid" >nul 2>nul
)

rem 3. the DoH process (python) started by sniroute.cmd, if still alive
if exist "logs\doh.pid" (
    for /f "usebackq delims=" %%p in ("logs\doh.pid") do (
        taskkill /F /PID %%p >nul 2>nul
    )
)

rem 4. the tray application itself
taskkill /F /IM Sniroute.exe >nul 2>nul

rem Give Windows a moment to release the file handles we just dropped.
rem ping is used because timeout.exe needs a console that may not exist here.
ping -n 3 127.0.0.1 >nul 2>nul

endlocal
exit /b 0

@echo off
setlocal
cd /d "%~dp0"
title LED Panel Editor
py -3 -c "import sys; sys.exit(sys.version_info < (3, 12))" >nul 2>&1
if not errorlevel 1 (
    py -3 tools\dev_server.py --open %*
    goto finished
)
python -c "import sys; sys.exit(sys.version_info < (3, 12))" >nul 2>&1
if not errorlevel 1 (
    python tools\dev_server.py --open %*
    goto finished
)
set "BUNDLED_PYTHON=%USERPROFILE%\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe"
if exist "%BUNDLED_PYTHON%" (
    "%BUNDLED_PYTHON%" tools\dev_server.py --open %*
    goto finished
)
echo Python 3.12 or later is required. Install Python and try again.
pause
exit /b 1

:finished
if errorlevel 1 (
    echo Editor startup failed. See the error above.
    pause
    exit /b 1
)
endlocal

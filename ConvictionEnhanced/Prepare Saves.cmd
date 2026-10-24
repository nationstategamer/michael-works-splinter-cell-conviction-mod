@echo off
setlocal
title Prepare private Conviction save backup
if defined MW_MOD_PYTHON (
    if not exist "%MW_MOD_PYTHON%" (
        echo MW_MOD_PYTHON must name a Python 3.10+ executable, without extra arguments.
        pause
        exit /b 1
    )
    "%MW_MOD_PYTHON%" "%~dp0public_setup.py" prepare-saves %*
) else (
    where py >nul 2>nul
    if not errorlevel 1 (
        py -3 "%~dp0public_setup.py" prepare-saves %*
    ) else (
        where python >nul 2>nul
        if errorlevel 1 (
            echo Install Python 3.10+ and enable its launcher or PATH option.
            echo You can also set MW_MOD_PYTHON to its executable path.
            pause
            exit /b 1
        )
        python "%~dp0public_setup.py" prepare-saves %*
    )
)
set "MW_MOD_RESULT=%errorlevel%"
if not "%MW_MOD_RESULT%"=="0" pause
exit /b %MW_MOD_RESULT%

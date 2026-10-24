@echo off
setlocal
title Install Michael Works Splinter Cell Conviction Mod
if defined MW_MOD_PYTHON (
    if not exist "%MW_MOD_PYTHON%" (
        echo MW_MOD_PYTHON must name a Python 3.10+ executable, without extra arguments.
        pause
        exit /b 1
    )
    "%MW_MOD_PYTHON%" "%~dp0public_setup.py" install %*
) else (
    where py >nul 2>nul
    if not errorlevel 1 (
        py -3 "%~dp0public_setup.py" install %*
    ) else (
        where python >nul 2>nul
        if errorlevel 1 (
            echo Install Python 3.10+ and enable its launcher or PATH option.
            echo You can also set MW_MOD_PYTHON to its executable path.
            pause
            exit /b 1
        )
        python "%~dp0public_setup.py" install %*
    )
)
set "MW_MOD_RESULT=%errorlevel%"
if not "%MW_MOD_RESULT%"=="0" pause
exit /b %MW_MOD_RESULT%

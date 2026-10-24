@echo off
setlocal
title Michael Works Splinter Cell Conviction Mod
where py >nul 2>nul
if not errorlevel 1 (
    py -3 "%~dp0enhanced.py" menu
) else (
    python "%~dp0enhanced.py" menu
)
if errorlevel 1 pause
endlocal

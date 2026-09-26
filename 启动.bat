@echo off
chcp 65001 >nul
title MCTier 局域网群组
cd /d "%~dp0"
where pythonw >nul 2>nul
if %errorlevel%==0 (
    start "" pythonw mctier_lan.py
) else (
    python mctier_lan.py
)

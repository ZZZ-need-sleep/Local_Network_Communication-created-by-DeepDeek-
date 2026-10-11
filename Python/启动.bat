@echo off
chcp 65001 >nul
title 内部网 局域网群组
cd /d "%~dp0"
where pythonw >nul 2>nul
if %errorlevel%==0 (
    start "" pythonw neiwang.py
) else (
    python neiwang.py
)

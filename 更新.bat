@echo off
chcp 65001 >nul
title 内部网 更新程序
cd /d "%~dp0"
where pythonw >nul 2>nul
if %errorlevel%==0 (
    start "" pythonw neiwang_update.py
) else (
    python neiwang_update.py
)

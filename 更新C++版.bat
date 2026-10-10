@echo off
chcp 65001 >nul
title 内部网 C++ 版 - 更新
cd /d "%~dp0"
if not exist "cpp\build\neiwang_update1.exe" (
    echo 还没有构建过，正在构建...
    call "%~dp0cpp\build.bat" app
    if errorlevel 1 ( pause & exit /b 1 )
)
start "" "%~dp0cpp\build\neiwang_update1.exe"

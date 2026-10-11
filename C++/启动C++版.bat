@echo off
chcp 65001 >nul
title 内部网 C++ 版
cd /d "%~dp0"
if not exist "build\neiwang.exe" (
    echo 还没有构建过，正在构建...
    call "%~dp0build.bat" app
    if errorlevel 1 ( pause & exit /b 1 )
)
start "" "%~dp0build\neiwang.exe"

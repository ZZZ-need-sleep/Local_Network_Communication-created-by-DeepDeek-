@echo off
chcp 65001 >nul
title 内部网 C++ 版 - 构建
cd /d "%~dp0"
echo 正在构建「内部网」C++ 版（纯 Win32 + GDI+，零第三方依赖）...
call "%~dp0build.bat" all
if errorlevel 1 (
    echo.
    echo [失败] 构建未通过，请看上面的错误信息。
    pause
    exit /b 1
)
echo.
echo [成功] 可执行文件在 build\ 目录：
echo   neiwang.exe          主程序
echo   neiwang_update1.exe  更新程序1
echo   neiwang_update2.exe  更新程序2
pause

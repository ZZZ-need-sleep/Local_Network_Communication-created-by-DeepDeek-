@echo off
rem ============================================================================
rem  内部网 Python 版 —— 打包更新包
rem  生成一个 zip（内容是本目录里除隐私数据/临时文件外的全部文件），
rem  把它上传到任意可下载的地址，再在 data\config.json 的 update_package 里填该地址，
rem  Python 版客户端就能用它更新（只更新 Python 版，不会碰到 C++ 版）。
rem ============================================================================
chcp 65001 >nul
title 内部网 Python 版 - 打包更新包
setlocal
cd /d "%~dp0"

set VER=
for /f "usebackq delims=" %%L in ("update.wenyi") do if not defined VER set "VER=%%L"
if "%VER%"=="" set VER=0.0.0

set STAGE=%TEMP%\_nwpack_python
if exist "%STAGE%" rd /s /q "%STAGE%"

echo 正在收集文件（跳过 data、customer_god、update_clone、__pycache__）...
robocopy "%CD%" "%STAGE%" /E /XD data customer_god update_clone _update_unzip __pycache__ /XF *.zip *.log >nul
if %ERRORLEVEL% GEQ 8 (
    echo [失败] 收集文件失败（robocopy 错误码 %ERRORLEVEL%）
    pause
    exit /b 1
)

set OUT=%~dp0..\Python_update_%VER%.zip
if exist "%OUT%" del "%OUT%"

echo 正在压缩 ...
powershell -NoProfile -ExecutionPolicy Bypass -Command "Compress-Archive -Path '%STAGE%\*' -DestinationPath '%OUT%' -Force"
if %ERRORLEVEL% NEQ 0 (
    echo [失败] 压缩失败
    rd /s /q "%STAGE%"
    pause
    exit /b 1
)
rd /s /q "%STAGE%"

echo.
echo [成功] 已生成：%OUT%
echo        把这个文件上传到可下载的地址，再把地址填进 data\config.json 的 update_package
pause

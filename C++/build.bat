@echo off
rem ============================================================================
rem  内部网 局域网群组 —— C++ 版构建脚本（纯 MSVC，零第三方依赖）
rem
rem  用法：  C++\build.bat            构建全部（主程序 + 两个更新程序 + 测试）
rem          C++\build.bat app        只构建主程序与两个更新程序
rem          C++\build.bat test       只构建测试
rem
rem  产物：  C++\build\neiwang.exe
rem          C++\build\neiwang_update1.exe
rem          C++\build\neiwang_update2.exe
rem          C++\build\test_*.exe
rem ============================================================================
setlocal enabledelayedexpansion
chcp 65001 >nul
cd /d "%~dp0"
set ROOT=%CD%
set SRC=%ROOT%\src
set OUT=%ROOT%\build
set OBJ=%OUT%\obj

set VS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat
if not exist "!VS!" (
  echo [错误] 找不到 MSVC vcvars64.bat：
  echo         !VS!
  echo         请安装 Visual Studio 2022 生成工具的「使用 C++ 的桌面开发」工作负载。
  echo         若你的 vcvars64.bat 在别处，请改本脚本顶部的 VS 变量。
  exit /b 1
)

call "!VS!" >nul
if errorlevel 1 ( echo [错误] 初始化 MSVC 环境失败 & exit /b 1 )

if exist "%OBJ%" rmdir /s /q "%OBJ%"
mkdir "%OBJ%" 2>nul
if not exist "%OUT%" mkdir "%OUT%"

rem /utf-8 必须保留：源码含大量中文，且仓库路径可能含中文
set CFLAGS=/nologo /c /std:c++17 /utf-8 /EHsc /MT /O2 /W3 /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /I "%SRC%"
set LDFLAGS=/nologo /SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF
set LIBS=user32.lib gdi32.lib gdiplus.lib shell32.lib ole32.lib comctl32.lib dwmapi.lib shlwapi.lib winhttp.lib ws2_32.lib advapi32.lib iphlpapi.lib bcrypt.lib

set WHAT=%1
if "%WHAT%"=="" set WHAT=all

rem ---------------------------------------------------------------- 源文件 --
set COMMON_SRC=json sha2 crypto util
set CORE_SRC=constants netio peercore peercore_net peercore_file peercore_activity peercore_proxy
set UI_SRC=app app_pages app_actions forms notify ui_common

set COMMON_OBJS=
set CORE_OBJS=
for %%N in (%COMMON_SRC%) do set COMMON_OBJS=!COMMON_OBJS! "%OBJ%\%%N.obj"
for %%N in (%CORE_SRC%) do set CORE_OBJS=!CORE_OBJS! "%OBJ%\%%N.obj"

echo === 编译 common ===
for %%N in (%COMMON_SRC%) do (
  echo   %%N.cpp
  cl %CFLAGS% "%SRC%\common\%%N.cpp" /Fo"%OBJ%\%%N.obj" || exit /b 1
)

echo === 编译 core ===
for %%N in (%CORE_SRC%) do (
  if exist "%SRC%\core\%%N.cpp" (
    echo   %%N.cpp
    cl %CFLAGS% "%SRC%\core\%%N.cpp" /Fo"%OBJ%\%%N.obj" || exit /b 1
  ) else (
    echo   [跳过] 尚未实现：%%N.cpp
  )
)

if "%WHAT%"=="test" goto :tests

set UI_OBJS=
echo === 编译 ui ===
for %%N in (%UI_SRC%) do (
  if exist "%SRC%\ui\%%N.cpp" (
    echo   %%N.cpp
    cl %CFLAGS% "%SRC%\ui\%%N.cpp" /Fo"%OBJ%\ui_%%N.obj" || exit /b 1
    set UI_OBJS=!UI_OBJS! "%OBJ%\ui_%%N.obj"
  )
)
echo   main.cpp
cl %CFLAGS% "%SRC%\main.cpp" /Fo"%OBJ%\main.obj" || exit /b 1

echo === 链接 neiwang.exe ===
link %LDFLAGS% %COMMON_OBJS% %CORE_OBJS% !UI_OBJS! "%OBJ%\main.obj" /OUT:"%OUT%\neiwang.exe" %LIBS% || exit /b 1
echo   已生成 %OUT%\neiwang.exe

if exist "%SRC%\updater\updater_common.cpp" (
  echo === 编译更新程序 ===
  cl %CFLAGS% "%SRC%\updater\updater_common.cpp" /Fo"%OBJ%\updater_common.obj" || exit /b 1
  cl %CFLAGS% "%SRC%\updater\updater1.cpp" /Fo"%OBJ%\updater1.obj" || exit /b 1
  cl %CFLAGS% "%SRC%\updater\updater2.cpp" /Fo"%OBJ%\updater2.obj" || exit /b 1
  link %LDFLAGS% %COMMON_OBJS% "%OBJ%\constants.obj" "%OBJ%\updater_common.obj" "%OBJ%\updater1.obj" /OUT:"%OUT%\neiwang_update1.exe" %LIBS% || exit /b 1
  link %LDFLAGS% %COMMON_OBJS% "%OBJ%\constants.obj" "%OBJ%\updater_common.obj" "%OBJ%\updater2.obj" /OUT:"%OUT%\neiwang_update2.exe" %LIBS% || exit /b 1
  echo   已生成 %OUT%\neiwang_update1.exe 与 %OUT%\neiwang_update2.exe
)

if "%WHAT%"=="app" goto :done

:tests
if not exist "%ROOT%\tests" goto :done
echo === 编译测试 ===
for %%F in ("%ROOT%\tests\*.cpp") do (
  echo   %%~nxF
  cl %CFLAGS% /I "%ROOT%\tests" /I "%SRC%\common" /I "%SRC%\core" "%%~fF" /Fo"%OBJ%\%%~nF.obj" /Fe"%OUT%\%%~nF.exe" /link /SUBSYSTEM:CONSOLE %COMMON_OBJS% %CORE_OBJS% "%OBJ%\%%~nF.obj" user32.lib shell32.lib ole32.lib ws2_32.lib advapi32.lib iphlpapi.lib bcrypt.lib || exit /b 1
)

:done
echo.
echo === 完成：%OUT% ===
dir /b "%OUT%\*.exe"
endlocal

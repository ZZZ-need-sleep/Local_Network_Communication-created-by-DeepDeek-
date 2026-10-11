// main.cpp —— neiwang.exe 入口：初始化、命令行、核心与主窗口装配
//
// 命令行：
//   --data-dir <path>   数据目录（默认 <exe 目录>/data）
//   --no-gui            只跑核心（测试用，不建窗口）
//
// 注意包含顺序：ui_common.h 内部先 winsock2.h 再 windows.h（避免 winsock 冲突）。
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "ui/app.h"

#include "common/util.h"
#include "core/core.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shlwapi.lib")

namespace {

// DPI 感知（Per-Monitor V2；动态取函数以兼容旧系统）
void setupDpiAwareness() {
    typedef BOOL(WINAPI * SetCtxFn)(DPI_AWARENESS_CONTEXT);
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        SetCtxFn fn = (SetCtxFn)(void*)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
        if (fn && fn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) return;
    }
    SetProcessDPIAware();
}

BOOL WINAPI ctrlHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_CLOSE_EVENT || type == CTRL_BREAK_EVENT) {
        PostQuitMessage(0);
        return TRUE;
    }
    return FALSE;
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR lpCmdLine, int) {
    setupDpiAwareness();

    std::string dataDir;
    bool noGui = false;
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        for (int i = 1; i < argc; ++i) {
            std::wstring a = argv[i];
            if (a == L"--no-gui") {
                noGui = true;
            } else if (a == L"--data-dir" && i + 1 < argc) {
                dataDir = nw::ws2u8(argv[++i]);
            } else if (a.rfind(L"--data-dir=", 0) == 0) {
                dataDir = nw::ws2u8(a.substr(11));
            }
        }
        if (argv) LocalFree(argv);
    }

    std::string exeDir = nw::get_exe_dir();
    if (dataDir.empty()) dataDir = nw::path_join(exeDir, "data");
    nw::ensure_dir(dataDir);

    if (!nw::net_init()) {
        MessageBoxW(nullptr, L"网络初始化失败（WSAStartup）", L"启动失败", MB_OK | MB_ICONERROR);
        return 1;
    }

    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    ULONG_PTR gdipToken = 0;
    {
        Gdiplus::GdiplusStartupInput in;
        if (Gdiplus::GdiplusStartup(&gdipToken, &in, nullptr) != Gdiplus::Ok) {
            MessageBoxW(nullptr, L"GDI+ 初始化失败", L"启动失败", MB_OK | MB_ICONERROR);
            if (SUCCEEDED(hrCo)) CoUninitialize();
            return 1;
        }
    }

    int rc = 0;
    {
        nw::Cfg cfg;
        nw::load_env_overrides(cfg);
        nw::Json jcfg = nw::load_config(dataDir, cfg, exeDir);

        nw::EventQueue events;
        nw::PeerCore* corePtr = nullptr;
        auto nameGetter = [&corePtr]() -> std::string {
            return corePtr ? corePtr->cfg.get("name").asStr("") : std::string();
        };
        nw::PeerCore core(jcfg, &events, nameGetter, dataDir, exeDir);
        corePtr = &core;

        if (noGui) {
            // 只跑核心：启动后台线程后阻塞等待（Ctrl+C / 终止信号退出）
            SetConsoleCtrlHandler(ctrlHandler, TRUE);
            core.start();
            const char* sec = std::getenv("NEIWANG_NOGUI_SECONDS");
            double limit = sec ? std::atof(sec) : 0.0;
            double t0 = nw::now_ts();
            MSG msg;
            while (true) {
                while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                    if (msg.message == WM_QUIT) { core.stop(); return 0; }
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
                Sleep(100);
                if (limit > 0 && nw::now_ts() - t0 >= limit) break;
            }
            core.stop();
            Gdiplus::GdiplusShutdown(gdipToken);
            if (SUCCEEDED(hrCo)) CoUninitialize();
            nw::net_shutdown();
            return 0;
        }

        ui::App app(&core, &events, dataDir, hInst);
        if (!app.createWindow()) {
            MessageBoxW(nullptr, L"程序启动失败，请查看控制台错误信息。", L"启动失败",
                        MB_OK | MB_ICONERROR);
            rc = 1;
        } else {
            core.start();       // 窗口就绪后再启动核心线程
            app.run();          // 消息循环（退出时 core.stop() 由 WM_CLOSE 调用）
        }
        core.stop();            // 兜底：确保数据落盘
    }

    Gdiplus::GdiplusShutdown(gdipToken);
    if (SUCCEEDED(hrCo)) CoUninitialize();
    nw::net_shutdown();
    return rc;
}

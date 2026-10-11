// updater2.cpp —— 更新程序2（neiwang_update2.exe）入口
// ============================================================================
// 更新程序2 只做一件事：**更新更新程序1 自己**。
//
//   neiwang_update2.exe --patch-updater1 [--wait-pid <pid>] [--root <path>]
//       不检查、不下载，直接把 update_clone/ 里的更新程序1
//       （neiwang_update1.exe，其次 neiwang_update1.py）覆盖到程序目录。
//       --wait-pid 用于等待正在运行的更新程序1 退出（OpenProcess(SYNCHRONIZE) +
//       WaitForSingleObject，超时 30 秒）；目标被占用时先把旧文件改名为
//       neiwang_update1.old.exe 再复制，仍失败则写出 neiwang_update1.new.exe 并提示。
//
//   其余参数（无 / --auto / --update / --check）与更新程序1 完全一致，可独立运行。
// 零第三方依赖：Win32 API + C++17 标准库。
// ============================================================================
#include "updater_common.h"   // 必须最先（内部先包含 winsock2.h）
#include "../common/util.h"   // nw::ws2u8

#include <windows.h>
#include <shellapi.h>

#include <string>
#include <vector>

#pragma comment(lib, "shell32.lib")

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    std::vector<std::string> args;
    int argc = 0;
    LPWSTR* wargv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
    if (wargv) {
        for (int i = 1; i < argc; ++i) args.push_back(nw::ws2u8(wargv[i]));
        ::LocalFree(wargv);
    }
    return nw::upd::run_main(args, 2);
}

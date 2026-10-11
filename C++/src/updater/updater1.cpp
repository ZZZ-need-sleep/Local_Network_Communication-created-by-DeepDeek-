// updater1.cpp —— 更新程序1（neiwang_update1.exe）入口
// ============================================================================
// 职责（design_v2 第 4 节）：
//   1) data/ -> customer_god/            （备份会话内容）
//   2) 取回新版本到 update_clone/        （git clone 或 ZIP 更新包）
//   3) customer_god/ -> update_clone/data/（回填会话内容）
//   4) 就地更新全部：update_clone/ -> 程序目录（跳过 data/、customer_god/、
//      update_clone/ 与更新程序1 自己），并删除已不存在的旧程序文件
//   5) 带 --patch-updater1 --wait-pid <pid> 调起更新程序2 更新更新程序1 自己
//
// 命令行：无参 / --auto / --update / --check / --help / --root <path> /
//         --wait-pid <pid>（兼容忽略）/ --console
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
    return nw::upd::run_main(args, 1);
}

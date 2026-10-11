// updater_common.h —— 双更新程序公共逻辑（design_v2 第 4 节 / docs/spec/04）
// ============================================================================
// 更新程序1（neiwang_update1.exe）：三步 + 就地更新全部（第四步）+ 调起更新程序2（第五步）
// 更新程序2（neiwang_update2.exe）：--patch-updater1 只把 update_clone/ 里的更新程序1
//                                   覆盖到程序目录；其余参数与更新程序1 完全一致。
//
// 零第三方依赖：只用 Win32 API（WinHTTP 联网、CreateProcessW 起子进程）+ C++17 标准库。
// 版本比较直接复用 core.h 的 nw::parse_update_manifest / nw::version_key /
// nw::is_newer_version（实现在 core/constants.cpp，链接时提供）。
// ============================================================================
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "../core/core.h"

namespace nw {
namespace upd {

// ---------------------------------------------------------------- 类型 --
using LogFn = std::function<void(const std::string&)>;

// ---------------------------------------------------------------- 常量 --
// 版本/程序名/目录名复用 core.h 里的 nw::APP_NAME、nw::UPDATE_MANIFEST_NAME、
// nw::MAIN_PROGRAM_NAME、nw::UPDATER1_PROGRAM_NAME、nw::UPDATER2_PROGRAM_NAME、
// nw::CUSTOMER_DATA_DIR、nw::UPDATE_CLONE_DIR（与主程序逐字一致）。
extern const char* UPDATER_PID_FILE;   // "_neiwang_update1.pid"
extern const char* APPLIED_MARKER;     // ".update_manifest_applied"
extern const char* ZIP_NAME;           // "update_package.zip"
extern const char* UNZIP_DIR;          // "_update_unzip"

constexpr int    CHECK_TIMEOUT    = 8;     // 清单请求超时（秒）
constexpr int    DOWNLOAD_TIMEOUT = 300;   // ZIP 更新包下载超时（秒）
constexpr int    GIT_TIMEOUT      = 900;   // git clone 超时（秒）
constexpr double WAIT_PID_TIMEOUT = 30.0;  // 更新程序2 等待更新程序1 退出的超时（秒）

// ---------------------------------------------------------------- 路径 --
// root      = --root <path> | NEIWANG_UPDATE_ROOT | exe 所在目录
// data_root = NEIWANG_DATA_DIR | <root>/data（并确保存在）
struct Paths {
    std::string root;
    std::string data_root;
};
Paths resolve_paths(const std::string& cli_root);

// ------------------------------------------------------------ 版本 / 配置 --
// 本地版本号 = <root>/update.wenyi 第一行；读不到返回 "0"。
std::string local_version(const std::string& root);
// 读取 <data_root>/config.json，缺失字段用内置默认值补齐（等价 Python load_config）。
Json load_cfg(const std::string& data_root);

// ---------------------------------------------------------------- 网络 --
// WinHTTP GET，支持 http/https。max_bytes > 0 时最多取回这么多字节（清单用 65536）。
bool http_get(const std::string& url, const std::string& ua, int timeout_sec,
              size_t max_bytes, std::string& body, std::string& err);
// WinHTTP 下载到文件（带每 2MB 一次的粗略进度日志）。
bool http_download(const std::string& url, const std::string& dest, const std::string& ua,
                   int timeout_sec, const LogFn& log, std::string& err);

// ---------------------------------------------------------------- 检查 --
struct CheckResult {
    std::string status;   // "update" | "current" | "offline"
    std::string local, remote, notes, reason;
};
CheckResult check_for_update(const Json& cfg, const std::string& root);

// ------------------------------------------------------- 三步 + 第四/五步 --
// 第一步：data_root -> backup（先清空 backup，再递归合并覆盖）。
bool backup_customer_data(const std::string& data_root, const std::string& backup,
                          std::string& err);
// 第三步：backup -> <target>/data（只遍历备份第一层，目录递归合并、文件覆盖）。
bool restore_customer_data(const std::string& backup, const std::string& target,
                           std::string& err);
// 第二步：取回新版本到 target。成功时 method = "git" | "zip"。
bool fetch_new_version(const Json& cfg, const std::string& root, const std::string& target,
                       const LogFn& log, std::string& method, std::string& reason);
// 第四步：就地更新全部（跳过 data/、customer_god/、update_clone/、neiwang_update1.*）。
bool apply_in_place(const std::string& clone, const std::string& root, const LogFn& log,
                    long long& copied, long long& removed, std::string& err);
// 第五步：update_clone/neiwang_update1.* -> root/（目标被占用时先改名再复制）。
bool patch_updater1(const std::string& root, const LogFn& log, int wait_pid,
                    std::string& version, std::string& reason);

struct UpdateResult {
    bool ok = false;
    std::string path, backup, method, reason, version;
    long long copied = 0, removed = 0;
    bool updater2 = false;      // 是否已调起更新程序2
};
// 完整更新：1) 备份 2) 取新版本 3) 回填（inplace 时 4) 就地更新 5) 调起更新程序2）。
UpdateResult perform_update(const Json& cfg, const std::string& root,
                            const std::string& data_root, const LogFn& log, bool inplace);

// ------------------------------------------------------- 日志 / 标记 / pid --
void append_clone_log(const std::string& root, const std::string& text);
bool write_applied_marker(const std::string& root, const std::string& version);
bool have_git();
bool pid_alive(int pid);
bool wait_for_pid(int pid, double timeout_sec, const LogFn& log);
void write_pid_file(const std::string& root, int pid);
int  read_pid_file(const std::string& root);

// ---------------------------------------------------------------- 杂项 --
std::string find_updater(const std::string& root, const std::string& clone,
                         const std::string& name);
// CreateProcessW + CREATE_NO_WINDOW 启动子进程（继承句柄关闭，无窗口）。
bool launch_process(const std::string& exe, const std::string& args, const std::string& cwd,
                    std::string& err);
void open_url(const std::string& url);
std::string usage_text(int role);
// --check：把 stdout 重定向到父控制台（AttachConsole(ATTACH_PARENT_PROCESS)）。
void attach_parent_console();
void console_println(const std::string& line);

// ---------------------------------------------------------------- 界面 --
void ui_info(const std::string& text, const std::string& title);
void ui_warn(const std::string& text, const std::string& title);
// 返回 IDYES(6) / IDNO(7) / IDCANCEL(2)
int  ui_ask_yes_no(const std::string& text, const std::string& title);

// ---------------------------------------------------------------- 入口 --
// 两个更新程序共用的命令行主逻辑（role = 1 或 2）。
int run_main(const std::vector<std::string>& argv, int role);

} // namespace upd
} // namespace nw

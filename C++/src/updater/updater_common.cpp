// updater_common.cpp —— 双更新程序公共逻辑实现（design_v2 第 4 节 / docs/spec/04）
// 零第三方依赖：Win32 API（WinHTTP / CreateProcessW / MessageBoxW / ShellExecuteW）+ C++17。
#include "updater_common.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>

#include <windows.h>
#include <shellapi.h>
#include <winhttp.h>

#include "../common/util.h"

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "ws2_32.lib")

namespace fs = std::filesystem;

namespace nw {
namespace upd {

// ---------------------------------------------------------------- 常量 --
const char* UPDATER_PID_FILE = "_neiwang_update1.pid";
const char* APPLIED_MARKER = ".update_manifest_applied";
const char* ZIP_NAME = "update_package.zip";
const char* UNZIP_DIR = "_update_unzip";

static const char* DEFAULT_GITHUB_OWNER = "ZZZ-need-sleep";
static const char* DEFAULT_GITHUB_REPO = "Local_Network_Communication-created-by-DeepDeek-";
static const char* DEFAULT_GITHUB_BRANCH = "main";

static const int kUnzipTimeout = 600;   // tar / Expand-Archive 解压超时（秒）

// ============================================================== 小工具 ==
static std::string env_str(const char* name) {
    std::wstring wname = u82ws(name);
    DWORD need = ::GetEnvironmentVariableW(wname.c_str(), nullptr, 0);
    if (need == 0) return std::string();
    std::wstring buf;
    buf.resize(need);
    DWORD got = ::GetEnvironmentVariableW(wname.c_str(), &buf[0], need);
    if (got == 0 || got >= need) return std::string();
    buf.resize(got);
    return ws2u8(buf);
}

static bool starts_with(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}

// 等价 Python str(value)：Json 数字/布尔也能转成字符串
static std::string jstr(const Json& v) {
    if (v.isStr()) return v.asStr();
    if (v.isNum()) {
        double d = v.asNum();
        long long i = static_cast<long long>(d);
        if (static_cast<double>(i) == d) return std::to_string(i);
        char b[64];
        std::snprintf(b, sizeof(b), "%g", d);
        return std::string(b);
    }
    if (v.isBool()) return v.asBool() ? "True" : "False";
    return std::string();
}

static std::string fmt_stamp(double ts) {
    std::time_t t = static_cast<std::time_t>(ts);
    std::tm tmv;
    std::memset(&tmv, 0, sizeof(tmv));
    if (::localtime_s(&tmv, &t) != 0) return std::string();
    char buf[64];
    if (std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv) == 0) return std::string();
    return std::string(buf);
}

static std::string json_escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(c);
        }
    }
    return out;
}

static std::string lower_ext(const std::string& name) {
    size_t pos = name.find_last_of('.');
    if (pos == std::string::npos) return std::string();
    return to_lower_ascii(name.substr(pos));
}

static std::string path_ext(const std::string& name) {
    size_t pos = name.find_last_of('.');
    if (pos == std::string::npos) return std::string();
    return name.substr(pos);
}

static std::string path_stem(const std::string& name) {
    size_t pos = name.find_last_of('.');
    if (pos == std::string::npos) return name;
    return name.substr(0, pos);
}

static std::string first_component(const std::string& rel) {
    size_t p = rel.find_first_of("\\/");
    return (p == std::string::npos) ? rel : rel.substr(0, p);
}

// 取 UTF-8 字符串末尾 n 个「字符」的安全近似：不切出半个序列
static std::string utf8_tail(const std::string& s, size_t n) {
    if (s.size() <= n) return s;
    size_t i = s.size() - n;
    while (i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) ++i;
    return s.substr(i);
}

static bool append_text_file(const std::string& path, const std::string& data) {
    HANDLE h = ::CreateFileW(u82ws(path).c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    BOOL ok = ::WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &wrote, nullptr);
    ::CloseHandle(h);
    return ok && wrote == data.size();
}

// ============================================================ 路径 / 版本 ==
// 转成绝对路径（子进程的 cwd 会被设为 root，相对路径会导致二次解析）
static std::string abs_path(const std::string& p) {
    std::wstring wp = u82ws(p);
    DWORD need = ::GetFullPathNameW(wp.c_str(), 0, nullptr, nullptr);
    if (need == 0) return p;
    std::wstring buf;
    buf.resize(need);
    DWORD got = ::GetFullPathNameW(wp.c_str(), need, &buf[0], nullptr);
    if (got == 0 || got >= need) return p;
    buf.resize(got);
    return ws2u8(buf);
}

Paths resolve_paths(const std::string& cli_root) {
    Paths p;
    std::string root = trim(cli_root);
    if (root.empty()) root = trim(env_str("NEIWANG_UPDATE_ROOT"));
    if (root.empty()) root = get_exe_dir();
    root = abs_path(root);
    while (root.size() > 3 && (root.back() == '\\' || root.back() == '/')) root.pop_back();
    p.root = root;

    std::string dr = trim(env_str("NEIWANG_DATA_DIR"));
    p.data_root = abs_path(dr.empty() ? path_join(root, "data") : dr);
    ensure_dir(p.data_root);
    return p;
}

std::string local_version(const std::string& root) {
    std::string text;
    if (!read_file_utf8(path_join(root, UPDATE_MANIFEST_NAME), text)) return "0";
    Json m = parse_update_manifest(text);
    std::string v = trim(jstr(m.get("version")));
    return v.empty() ? std::string("0") : v;
}

Json load_cfg(const std::string& data_root) {
    Json cfg = Json::obj();
    std::string url = std::string("https://raw.githubusercontent.com/") + DEFAULT_GITHUB_OWNER +
                      "/" + DEFAULT_GITHUB_REPO + "/" + DEFAULT_GITHUB_BRANCH +
                      "/C++/" + UPDATE_MANIFEST_NAME;
    std::string page = std::string("https://github.com/") + DEFAULT_GITHUB_OWNER + "/" +
                       DEFAULT_GITHUB_REPO;
    std::string repo = page + ".git";
    cfg.set("update_url", Json(url));
    cfg.set("update_page", Json(page));
    cfg.set("update_repo", Json(repo));
    cfg.set("update_package", Json(""));

    std::string text;
    if (read_file_utf8(path_join(data_root, "config.json"), text)) {
        bool ok = false;
        Json d = Json::parse(text, &ok);
        if (ok && d.isObj()) {
            for (const std::string& k : d.keys()) cfg.set(k, d.get(k));
        }
    }
    return cfg;
}

// ============================================================ 文件树工具 ==
static bool copy_tree_merge(const std::string& src, const std::string& dst, std::string& err) {
    if (!ensure_dir(dst)) {
        err = "无法创建目录：" + dst;
        return false;
    }
    std::vector<std::string> names = list_dir(src);
    std::sort(names.begin(), names.end());
    for (const std::string& n : names) {
        std::string s = path_join(src, n);
        std::string d = path_join(dst, n);
        if (is_dir(s)) {
            if (!copy_tree_merge(s, d, err)) return false;
        } else if (!copy_file(s, d, true)) {
            err = "复制失败：" + s + " -> " + d;
            return false;
        }
    }
    return true;
}

// 就地更新时跳过：运行时目录 + 更新程序1 自己
static bool inplace_skip(const std::string& rel) {
    if (rel.empty()) return true;
    std::string f = to_lower_ascii(first_component(rel));
    if (f == "data" || f == "customer_god" || f == "update_clone" || f == "_update_unzip" ||
        f == "__pycache__")
        return true;
    // 另一个版本（Python 版）的目录：C++ 版更新时绝不碰它
    if (f == "python") return true;
    // 编译中间产物不参与更新（build\obj\*.obj）
    std::string low = to_lower_ascii(rel);
    if (low.rfind("build\\obj", 0) == 0 || low.rfind("build/obj", 0) == 0) return true;
    std::string name = basename_utf8(rel);
    return starts_with(name, "neiwang_update1.");
}

// 「程序文件」白名单：*.py / *.exe / *.bat / update.wenyi / README.md / *.md（docs/ 除外）
static bool is_program_file(const std::string& rel) {
    std::string name = basename_utf8(rel);
    if (name == "update.wenyi" || name == "README.md") return true;
    std::string ext = lower_ext(name);
    if (ext == ".py" || ext == ".exe" || ext == ".bat") return true;
    if (ext == ".md") return to_lower_ascii(first_component(rel)) != "docs";
    return false;
}

static void walk_rel(const std::string& dir, const std::string& prefix,
                     std::vector<std::string>& out) {
    std::vector<std::string> names = list_dir(dir);
    std::sort(names.begin(), names.end());
    for (const std::string& n : names) {
        std::string full = path_join(dir, n);
        std::string rel = prefix.empty() ? n : prefix + "\\" + n;
        if (is_dir(full)) {
            if (inplace_skip(rel)) continue;
            walk_rel(full, rel, out);
        } else {
            out.push_back(rel);
        }
    }
}

static int rel_depth(const std::string& rel) {
    int d = 0;
    for (char c : rel)
        if (c == '\\' || c == '/') ++d;
    return d;
}

static std::string single_root(const std::string& folder) {
    std::vector<std::string> items = list_dir(folder);
    if (items.size() == 1) {
        std::string p = path_join(folder, items[0]);
        if (is_dir(p)) return p;
    }
    return folder;
}

static std::string version_from_manifest(const std::string& path) {
    std::string text;
    if (!read_file_utf8(path, text)) return std::string();
    Json m = parse_update_manifest(text);
    return trim(jstr(m.get("version")));
}

// ============================================================ 进程工具 ==
struct ProcResult {
    bool started = false;
    bool timed_out = false;
    DWORD exit_code = static_cast<DWORD>(-1);
};

// 起子进程：CREATE_NO_WINDOW，stdout+stderr 合并写 out_file（空则写 NUL）。
static ProcResult run_process(const std::string& exe, const std::string& args,
                              const std::string& cwd, const std::string& out_file,
                              int timeout_sec, bool truncate) {
    ProcResult r;
    std::wstring wcmd = L"\"" + u82ws(exe) + L"\"";
    if (!args.empty()) wcmd += L" " + u82ws(args);

    SECURITY_ATTRIBUTES sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    std::wstring wout = out_file.empty() ? std::wstring(L"NUL") : u82ws(out_file);
    HANDLE hOut = ::CreateFileW(wout.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                &sa, truncate ? CREATE_ALWAYS : OPEN_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hOut == INVALID_HANDLE_VALUE) return r;
    if (!truncate) ::SetFilePointer(hOut, 0, nullptr, FILE_END);
    HANDLE hIn = ::CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    STARTUPINFOW si;
    std::memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hOut;
    si.hStdError = hOut;
    si.hStdInput = hIn;
    PROCESS_INFORMATION pi;
    std::memset(&pi, 0, sizeof(pi));

    std::wstring wcwd = cwd.empty() ? std::wstring() : u82ws(cwd);
    std::vector<wchar_t> cmd(wcmd.begin(), wcmd.end());
    cmd.push_back(L'\0');

    BOOL ok = ::CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                               nullptr, wcwd.empty() ? nullptr : wcwd.c_str(), &si, &pi);
    ::CloseHandle(hOut);
    if (hIn != INVALID_HANDLE_VALUE) ::CloseHandle(hIn);
    if (!ok) return r;

    r.started = true;
    DWORD w = ::WaitForSingleObject(pi.hProcess,
                                    timeout_sec > 0 ? static_cast<DWORD>(timeout_sec) * 1000u
                                                    : INFINITE);
    if (w == WAIT_TIMEOUT) {
        r.timed_out = true;
        ::TerminateProcess(pi.hProcess, 1);
        ::WaitForSingleObject(pi.hProcess, 5000);
    }
    DWORD code = static_cast<DWORD>(-1);
    ::GetExitCodeProcess(pi.hProcess, &code);
    r.exit_code = code;
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    return r;
}

bool have_git() {
    ProcResult r = run_process("git", "--version", "", "", 5, true);
    return r.started && !r.timed_out && r.exit_code == 0;
}

bool launch_process(const std::string& exe, const std::string& args, const std::string& cwd,
                    std::string& err) {
    std::wstring wcmd = L"\"" + u82ws(exe) + L"\"";
    if (!args.empty()) wcmd += L" " + u82ws(args);
    std::wstring wcwd = cwd.empty() ? std::wstring() : u82ws(cwd);

    // 刚解压/刚写出的 exe 可能被杀毒软件短暂锁住（ERROR_ACCESS_DENIED），短重试几次
    DWORD last = 0;
    for (int attempt = 0; attempt < 5; ++attempt) {
        std::vector<wchar_t> cmd(wcmd.begin(), wcmd.end());
        cmd.push_back(L'\0');
        STARTUPINFOW si;
        std::memset(&si, 0, sizeof(si));
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi;
        std::memset(&pi, 0, sizeof(pi));

        BOOL ok = ::CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                                   nullptr, wcwd.empty() ? nullptr : wcwd.c_str(), &si, &pi);
        if (ok) {
            ::CloseHandle(pi.hThread);
            ::CloseHandle(pi.hProcess);
            return true;
        }
        last = ::GetLastError();
        if (last != ERROR_ACCESS_DENIED && last != ERROR_SHARING_VIOLATION) break;
        ::Sleep(400);
    }
    err = "CreateProcessW 失败（错误码 " + std::to_string(last) + "）：" + exe;
    return false;
}

bool pid_alive(int pid) {
    if (pid <= 0) return false;
    HANDLE h = ::OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                             static_cast<DWORD>(pid));
    if (!h) return false;
    DWORD code = 0;
    bool alive = false;
    if (::GetExitCodeProcess(h, &code)) alive = (code == STILL_ACTIVE);
    ::CloseHandle(h);
    return alive;
}

bool wait_for_pid(int pid, double timeout_sec, const LogFn& log) {
    if (pid <= 0) return true;
    HANDLE h = ::OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (!h) return true;   // 打不开 = 进程已不存在
    DWORD ms = static_cast<DWORD>(timeout_sec * 1000.0);
    DWORD w = ::WaitForSingleObject(h, ms);
    ::CloseHandle(h);
    if (w == WAIT_OBJECT_0) return true;
    if (log) log("等待超时，仍尝试覆盖（失败则写出 .new 文件）");
    return false;
}

// ============================================================ 日志 / 标记 ==
void append_clone_log(const std::string& root, const std::string& text) {
    if (root.empty()) return;
    append_text_file(path_join(root, "update_clone.log"),
                     "[" + fmt_stamp(now_ts()) + "] " + text + "\n");
}

bool write_applied_marker(const std::string& root, const std::string& version) {
    double ts = now_ts();
    std::string data = "{\n \"version\": \"" + json_escape(version) + "\",\n \"" "ts\": " +
                       std::to_string(ts) + ",\n \"time\": \"" + json_escape(fmt_stamp(ts)) +
                       "\"\n}";
    return write_file_utf8(path_join(root, APPLIED_MARKER), data);
}

void write_pid_file(const std::string& root, int pid) {
    if (pid <= 0) pid = static_cast<int>(::GetCurrentProcessId());
    write_file_utf8(path_join(root, UPDATER_PID_FILE), std::to_string(pid));
}

int read_pid_file(const std::string& root) {
    std::string text;
    if (!read_file_utf8(path_join(root, UPDATER_PID_FILE), text)) return -1;
    text = trim(text);
    if (text.empty()) return -1;
    try {
        return std::stoi(text);
    } catch (...) {
        return -1;
    }
}

// 前向声明（定义在下方程序定位区）：在 dir 或 dir\build 下找文件
static std::string find_program_in(const std::string& dir, const std::string& name);

std::string find_updater(const std::string& root, const std::string& clone,
                         const std::string& name) {
    const std::string bases[2] = {clone, root};
    for (int i = 0; i < 2; ++i) {
        // 同时找 <base>\name 与 <base>\build\name（C++ 版的 exe 在 build\ 下）
        std::string p = find_program_in(bases[i], name);
        if (!p.empty()) return p;
    }
    return std::string();
}

// ================================================================ HTTP ==
static std::string ua_for(const std::string& root) {
    return "NeiWangUpdater/" + local_version(root);
}

// url -> body(最多 max_bytes) 或文件(save_path 非空)
static bool http_core(const std::string& url, const std::string& ua, int timeout_sec,
                      size_t max_bytes, std::string* body_out, const std::string& save_path,
                      const LogFn& log, std::string& err) {
    std::wstring wurl = u82ws(url);
    URL_COMPONENTS uc;
    std::memset(&uc, 0, sizeof(uc));
    uc.dwStructSize = sizeof(uc);
    wchar_t host[512];
    wchar_t path[8192];
    std::memset(host, 0, sizeof(host));
    std::memset(path, 0, sizeof(path));
    uc.lpszHostName = host;
    uc.dwHostNameLength = 511;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 8191;
    uc.dwSchemeLength = static_cast<DWORD>(-1);

    if (!::WinHttpCrackUrl(wurl.c_str(), static_cast<DWORD>(wurl.size()), 0, &uc)) {
        err = "地址无法解析";
        return false;
    }
    std::wstring whost(host, uc.dwHostNameLength);
    std::wstring wpath(path, uc.dwUrlPathLength);
    if (wpath.empty()) wpath = L"/";
    bool https = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    INTERNET_PORT port = uc.nPort ? uc.nPort : (https ? 443 : 80);

    HINTERNET ses = ::WinHttpOpen(u82ws(ua).c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) {
        err = "初始化网络失败";
        return false;
    }
    int ms = timeout_sec > 0 ? timeout_sec * 1000 : 30000;
    ::WinHttpSetTimeouts(ses, ms, ms, ms, ms);

    HINTERNET con = ::WinHttpConnect(ses, whost.c_str(), port, 0);
    HINTERNET req = nullptr;
    if (con) {
        req = ::WinHttpOpenRequest(con, L"GET", wpath.c_str(), nullptr, WINHTTP_NO_REFERER,
                                   WINHTTP_DEFAULT_ACCEPT_TYPES,
                                   https ? WINHTTP_FLAG_SECURE : 0);
    }
    if (req) {
        DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
        ::WinHttpSetOption(req, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
    }
    bool sent = false;
    if (req) {
        sent = ::WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA,
                                    0, 0, 0) != FALSE;
        if (sent) sent = ::WinHttpReceiveResponse(req, nullptr) != FALSE;
    }
    if (!sent) {
        err = "无法连接服务器";
        if (req) ::WinHttpCloseHandle(req);
        if (con) ::WinHttpCloseHandle(con);
        ::WinHttpCloseHandle(ses);
        return false;
    }

    DWORD status = 0, len = sizeof(status);
    ::WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                          WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);
    if (status < 200 || status >= 300) {
        err = "HTTP " + std::to_string(status);
        ::WinHttpCloseHandle(req);
        ::WinHttpCloseHandle(con);
        ::WinHttpCloseHandle(ses);
        return false;
    }
    DWORD total = 0;
    len = sizeof(total);
    ::WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                          WINHTTP_HEADER_NAME_BY_INDEX, &total, &len, WINHTTP_NO_HEADER_INDEX);

    HANDLE hFile = INVALID_HANDLE_VALUE;
    if (!save_path.empty()) {
        std::string dir = save_path;
        size_t pos = dir.find_last_of("\\/");
        if (pos != std::string::npos) ensure_dir(dir.substr(0, pos));
        hFile = ::CreateFileW(u82ws(save_path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile == INVALID_HANDLE_VALUE) {
            err = "无法写入文件：" + save_path;
            ::WinHttpCloseHandle(req);
            ::WinHttpCloseHandle(con);
            ::WinHttpCloseHandle(ses);
            return false;
        }
        if (log) log("    正在下载：" + url);
    }

    std::vector<char> buf(64 * 1024);
    unsigned long long got = 0;
    long long last_mark = -1;
    bool ok = true;
    for (;;) {
        DWORD rd = 0;
        if (!::WinHttpReadData(req, buf.data(), static_cast<DWORD>(buf.size()), &rd)) {
            err = "读取数据失败";
            ok = false;
            break;
        }
        if (rd == 0) break;
        if (hFile != INVALID_HANDLE_VALUE) {
            DWORD wrote = 0;
            if (!::WriteFile(hFile, buf.data(), rd, &wrote, nullptr) || wrote != rd) {
                err = "写入文件失败：" + save_path;
                ok = false;
                break;
            }
            got += rd;
            long long mark = static_cast<long long>(got / (2ull * 1024ull * 1024ull));
            if (mark != last_mark) {
                last_mark = mark;
                if (log) {
                    if (total)
                        log("    已下载 " + std::to_string(got / 1024) + " KB / " +
                            std::to_string(total / 1024) + " KB");
                    else
                        log("    已下载 " + std::to_string(got / 1024) + " KB");
                }
            }
        } else if (body_out) {
            size_t take = rd;
            if (got >= max_bytes) take = 0;
            else if (got + take > max_bytes) take = max_bytes - static_cast<size_t>(got);
            if (take) body_out->append(buf.data(), take);
            got += rd;
            if (got >= max_bytes) break;
        } else {
            got += rd;
        }
    }
    if (hFile != INVALID_HANDLE_VALUE) {
        ::CloseHandle(hFile);
        if (ok && log) log("    下载完成：" + save_path + "（" + std::to_string(got / 1024) + " KB）");
    }
    ::WinHttpCloseHandle(req);
    ::WinHttpCloseHandle(con);
    ::WinHttpCloseHandle(ses);
    return ok;
}

bool http_get(const std::string& url, const std::string& ua, int timeout_sec, size_t max_bytes,
              std::string& body, std::string& err) {
    body.clear();
    return http_core(url, ua, timeout_sec, max_bytes, &body, "", LogFn(), err);
}

bool http_download(const std::string& url, const std::string& dest, const std::string& ua,
                   int timeout_sec, const LogFn& log, std::string& err) {
    return http_core(url, ua, timeout_sec, 0, nullptr, dest, log, err);
}

// =============================================================== 检查 ==
CheckResult check_for_update(const Json& cfg, const std::string& root) {
    CheckResult r;
    r.local = local_version(root);
    std::string url = trim(jstr(cfg.get("update_url")));
    if (url.empty()) {
        r.status = "offline";
        r.reason = "没有配置 update_url（更新清单地址）";
        return r;
    }
    std::string body, err;
    if (!http_get(url, ua_for(root), CHECK_TIMEOUT, 65536, body, err)) {
        r.status = "offline";
        r.reason = "无法连接更新服务器：" + url;
        return r;
    }
    Json m = parse_update_manifest(body);
    r.remote = trim(jstr(m.get("version")));
    r.notes = jstr(m.get("notes"));
    if (!r.remote.empty() && is_newer_version(r.remote, r.local)) {
        r.status = "update";
        return r;
    }
    r.status = "current";
    return r;
}

// ========================================================== ZIP 安全校验 ==
static bool read_at(std::ifstream& f, unsigned long long off, void* buf, size_t n) {
    f.clear();
    f.seekg(static_cast<std::streamoff>(off), std::ios::beg);
    if (!f) return false;
    f.read(static_cast<char*>(buf), static_cast<std::streamsize>(n));
    return f.gcount() == static_cast<std::streamsize>(n);
}

static unsigned rd16(const unsigned char* p) {
    return static_cast<unsigned>(p[0]) | (static_cast<unsigned>(p[1]) << 8);
}

static unsigned long long rd32(const unsigned char* p) {
    return static_cast<unsigned long long>(p[0]) | (static_cast<unsigned long long>(p[1]) << 8) |
           (static_cast<unsigned long long>(p[2]) << 16) |
           (static_cast<unsigned long long>(p[3]) << 24);
}

static unsigned long long rd64(const unsigned char* p) {
    return rd32(p) | (rd32(p + 4) << 32);
}

// 路径穿越校验（对应 Python safe_extract）：拒绝绝对路径 / 盘符 / 恰好为 ".." 的路径段
static bool zip_name_unsafe(const std::string& raw, std::string& why) {
    std::string name = raw;
    for (char& c : name)
        if (c == '\\') c = '/';
    bool bad = false;
    if (!name.empty() && name[0] == '/') bad = true;
    if (name.size() >= 2 && ((name[0] >= 'A' && name[0] <= 'Z') || (name[0] >= 'a' && name[0] <= 'z')) &&
        name[1] == ':')
        bad = true;
    if (!bad) {
        std::vector<std::string> segs = split(name, '/');
        for (const std::string& s : segs)
            if (s == "..") bad = true;
    }
    if (bad) why = "压缩包里含有不安全的路径，已中止：" + raw;
    return bad;
}

// 读取 ZIP 中央目录里的全部成员名（不解压），用于解压前的路径穿越校验
static bool zip_list_names(const std::string& zip_path, std::vector<std::string>& names,
                           std::string& err) {
    std::ifstream f(fs::path(u82ws(zip_path)), std::ios::binary);
    if (!f) {
        err = "无法打开更新包：" + zip_path;
        return false;
    }
    f.seekg(0, std::ios::end);
    std::streamoff total = f.tellg();
    if (total < 22) {
        err = "不是有效的 ZIP 更新包";
        return false;
    }
    size_t tail = static_cast<size_t>(std::min<std::streamoff>(total, 65557));
    std::vector<unsigned char> buf(tail);
    if (!read_at(f, static_cast<unsigned long long>(total) - tail, buf.data(), tail)) {
        err = "读取更新包失败";
        return false;
    }
    long long eocd = -1;
    for (long long i = static_cast<long long>(tail) - 22; i >= 0; --i) {
        if (buf[i] == 0x50 && buf[i + 1] == 0x4b && buf[i + 2] == 0x05 && buf[i + 3] == 0x06) {
            eocd = i;
            break;
        }
    }
    if (eocd < 0) {
        err = "不是有效的 ZIP 更新包";
        return false;
    }
    unsigned count = rd16(&buf[eocd + 10]);
    unsigned long long cd_off = rd32(&buf[eocd + 16]);
    if (count == 0xFFFF || cd_off == 0xFFFFFFFFull) {
        long long loc = eocd - 20;
        if (loc >= 0 && buf[loc] == 0x50 && buf[loc + 1] == 0x4b && buf[loc + 2] == 0x06 &&
            buf[loc + 3] == 0x07) {
            unsigned long long z64 = rd64(&buf[loc + 8]);
            unsigned char rec[56];
            if (read_at(f, z64, rec, sizeof(rec)) && rec[0] == 0x50 && rec[1] == 0x4b &&
                rec[2] == 0x06 && rec[3] == 0x06) {
                count = 0xFFFF;
                cd_off = rd64(rec + 48);
            }
        }
    }
    unsigned long long off = cd_off;
    for (unsigned i = 0; i < 400000; ++i) {
        if (count != 0xFFFF && i >= count) break;
        unsigned char h[46];
        if (!read_at(f, off, h, sizeof(h))) break;
        if (!(h[0] == 0x50 && h[1] == 0x4b && h[2] == 0x01 && h[3] == 0x02)) break;
        unsigned name_len = rd16(h + 28);
        unsigned extra_len = rd16(h + 30);
        unsigned com_len = rd16(h + 32);
        if (name_len == 0 || name_len > 4096) break;
        std::string name;
        name.resize(name_len);
        if (!read_at(f, off + 46, &name[0], name_len)) break;
        names.push_back(name);
        off += 46ull + name_len + extra_len + com_len;
    }
    if (names.empty()) {
        err = "更新包里没有任何文件";
        return false;
    }
    return true;
}

// 用 tar.exe 解压，失败回退 PowerShell Expand-Archive -Force
static bool extract_zip(const std::string& zip_path, const std::string& dest, const LogFn& log,
                        std::string& err) {
    ensure_dir(dest);
    ProcResult r1 = run_process("tar.exe", "-xf \"" + zip_path + "\" -C \"" + dest + "\"", "",
                                "", kUnzipTimeout, true);
    if (r1.started && !r1.timed_out && r1.exit_code == 0) return true;

    auto ps_quote = [](const std::string& s) {
        std::string o;
        for (char c : s) {
            if (c == '\'') o += "''";
            else o.push_back(c);
        }
        return o;
    };
    std::string cmd = "-NoProfile -NonInteractive -ExecutionPolicy Bypass -Command "
                      "\"Expand-Archive -LiteralPath '" + ps_quote(zip_path) +
                      "' -DestinationPath '" + ps_quote(dest) + "' -Force\"";
    ProcResult r2 = run_process("powershell.exe", cmd, "", "", kUnzipTimeout, true);
    if (r2.started && !r2.timed_out && r2.exit_code == 0) return true;

    err = "解压失败：tar 与 PowerShell Expand-Archive 都不可用或都执行失败";
    if (log) log("    " + err);
    return false;
}

// ========================================================== 更新三步 ==
bool backup_customer_data(const std::string& data_root, const std::string& backup,
                          std::string& err) {
    if (!ensure_dir(data_root)) {
        err = "无法创建数据目录：" + data_root;
        return false;
    }
    if (file_exists(backup)) remove_dir_recursive(backup);
    if (!ensure_dir(backup)) {
        err = "无法创建备份目录：" + backup;
        return false;
    }
    return copy_tree_merge(data_root, backup, err);
}

bool restore_customer_data(const std::string& backup, const std::string& target,
                           std::string& err) {
    std::string newdata = path_join(target, "data");
    if (!ensure_dir(newdata)) {
        err = "无法创建目录：" + newdata;
        return false;
    }
    if (!is_dir(backup)) {
        err = "备份目录不存在：" + backup;
        return false;
    }
    std::vector<std::string> items = list_dir(backup);
    std::sort(items.begin(), items.end());
    for (const std::string& name : items) {
        std::string src = path_join(backup, name);
        std::string dst = path_join(newdata, name);
        if (is_dir(src)) {
            if (!copy_tree_merge(src, dst, err)) return false;
        } else if (!copy_file(src, dst, true)) {
            err = "复制失败：" + src + " -> " + dst;
            return false;
        }
    }
    return true;
}

// git clone（输出直接写 update_clone.log，避免管道死锁）
static bool run_git_clone(const std::string& repo, const std::string& target,
                          const std::string& root, const LogFn& log, std::string& out) {
    std::string logfile = path_join(root, "update_clone.log");
    if (log) log("    git clone --depth 1 " + repo);
    ProcResult r = run_process("git", "clone --depth 1 \"" + repo + "\" \"" + target + "\"",
                               root, logfile, GIT_TIMEOUT, true);
    if (!r.started) {
        out = "未找到 git 命令：本机没有安装 Git";
        return false;
    }
    if (r.timed_out) {
        out = "TimeoutExpired: git clone 超过 " + std::to_string(GIT_TIMEOUT) + " 秒";
        return false;
    }
    std::string text;
    if (read_file_utf8(logfile, text)) out = trim(utf8_tail(text, 400));
    else out.clear();
    return r.exit_code == 0;
}

// 移动文件/目录（同盘符直接改名；跨盘符回退为复制+删除）
static bool move_path(const std::string& from, const std::string& to) {
    if (MoveFileExW(u82ws(from).c_str(), u82ws(to).c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED))
        return true;
    return false;
}

// C++ 版的程序文件在 build\ 下（neiwang.exe / neiwang_update1.exe / neiwang_update2.exe），
// 同时也兼容「直接放在版本目录根」的打包方式。
static std::string find_program_in(const std::string& dir, const std::string& name) {
    if (dir.empty()) return std::string();
    std::string a = path_join(dir, name);
    if (file_exists(a) && !is_dir(a)) return a;
    std::string b = path_join(path_join(dir, "build"), name);
    if (file_exists(b) && !is_dir(b)) return b;
    return std::string();
}

static bool has_main_program(const std::string& dir) {
    return !find_program_in(dir, MAIN_PROGRAM_NAME).empty();
}

// 单仓库（monorepo）兼容：仓库根下有 C++/ 与 Python/ 两个版本目录，
// git clone / ZIP 解压后可能拿到的是「仓库根」或「版本目录」。
// 若拿到仓库根，则**只保留本版目录的内容**（另一个版本目录会被丢掉），
// 保证 C++ 版更新时不会把 Python/ 目录塞进自己的安装目录。
static bool flatten_version_dir(const std::string& target, const char* ver_dir, std::string& err) {
    if (has_main_program(target)) return true;                       // 已经是版本目录
    std::string d = path_join(target, ver_dir);
    if (!has_main_program(d)) return false;                          // 不是仓库根结构
    std::string stage = target + "__ver";
    if (file_exists(stage)) remove_dir_recursive(stage);
    if (!move_path(d, stage)) {
        err = "移动版本目录失败：" + d;
        return false;
    }
    remove_dir_recursive(target);
    if (!move_path(stage, target)) {
        err = "恢复版本目录失败：" + stage;
        return false;
    }
    return has_main_program(target);
}

bool fetch_new_version(const Json& cfg, const std::string& root, const std::string& target,
                       const LogFn& log, std::string& method, std::string& reason) {
    method.clear();
    std::string repo = trim(jstr(cfg.get("update_repo")));
    std::string package = trim(jstr(cfg.get("update_package")));
    std::vector<std::string> tried;

    if (file_exists(target)) remove_dir_recursive(target);

    std::string flat_err;
    auto check_main = [&]() {
        // 只认 C++ 版自己的主程序（build\neiwang.exe 或 neiwang.exe）：
        // Python 版不会、也不应该被 C++ 版更新程序覆盖
        return flatten_version_dir(target, "C++", flat_err);
    };

    // 方式一：git clone
    if (!repo.empty() && repo.find('<') == std::string::npos) {
        if (have_git()) {
            std::string out;
            bool ok = run_git_clone(repo, target, root, log, out);
            if (ok && check_main()) {
                method = "git";
                return true;
            }
            if (out.empty())
                out = std::string("仓库内容不完整（没有找到 build\\") + MAIN_PROGRAM_NAME + "）";
            tried.push_back("Git 方式失败：" + out);
            remove_dir_recursive(target);
        } else {
            tried.push_back("本机没有安装 Git，无法用 Git 方式更新");
        }
    } else {
        tried.push_back("没有配置 update_repo（Git 仓库地址）");
    }

    // 方式二：ZIP 更新包
    if (!package.empty() && package.find('<') == std::string::npos) {
        std::string zip_path = path_join(root, ZIP_NAME);
        std::string unzip_dir = path_join(root, UNZIP_DIR);
        std::string err;
        if (!http_download(package, zip_path, ua_for(root), DOWNLOAD_TIMEOUT, log, err)) {
            tried.push_back("ZIP 更新包失败：DownloadError: " + err);
        } else {
            std::vector<std::string> names;
            std::string verr;
            if (!zip_list_names(zip_path, names, verr)) {
                tried.push_back("ZIP 更新包失败：ValueError: " + verr);
            } else {
                bool unsafe = false;
                for (const std::string& n : names) {
                    if (zip_name_unsafe(n, verr)) {
                        unsafe = true;
                        break;
                    }
                }
                if (unsafe) {
                    tried.push_back("ZIP 更新包失败：ValueError: " + verr);
                } else {
                    remove_dir_recursive(unzip_dir);
                    ensure_dir(unzip_dir);
                    if (log) log("    正在解压更新包 ...");
                    std::string xerr;
                    if (!extract_zip(zip_path, unzip_dir, log, xerr)) {
                        tried.push_back("ZIP 更新包失败：ExtractError: " + xerr);
                    } else {
                        std::string src = single_root(unzip_dir);
                        ensure_dir(target);
                        if (!copy_tree_merge(src, target, err)) {
                            tried.push_back("ZIP 更新包失败：OSError: " + err);
                        } else if (check_main()) {
                            remove_dir_recursive(unzip_dir);
                            method = "zip";
                            return true;
                        } else {
                            tried.push_back(std::string("ZIP 更新包里没有找到 build\\") +
                                            MAIN_PROGRAM_NAME + "（包内容不完整）");
                        }
                    }
                }
            }
        }
        remove_dir_recursive(unzip_dir);
    } else {
        tried.push_back("没有配置 update_package（ZIP 更新包地址）");
    }

    reason = "取回新版本失败：\n- " + join(tried, "\n- ");
    return false;
}

// ====================================================== 第四步：就地更新 ==
bool apply_in_place(const std::string& clone, const std::string& root, const LogFn& log,
                    long long& copied, long long& removed, std::string& err) {
    copied = 0;
    removed = 0;
    if (!is_dir(clone)) {
        err = "新版本目录不存在：" + clone;
        return false;
    }

    // 1) 覆盖：update_clone/ -> root/（保持相对路径）
    std::vector<std::string> files;
    walk_rel(clone, "", files);
    std::sort(files.begin(), files.end());
    for (const std::string& rel : files) {
        if (inplace_skip(rel)) continue;
        std::string src = path_join(clone, rel);
        std::string dst = path_join(root, rel);
        std::string dir = dst;
        size_t pos = dir.find_last_of("\\/");
        if (pos != std::string::npos) ensure_dir(dir.substr(0, pos));
        if (!copy_file(src, dst, true)) {
            err = "覆盖 " + rel + " 失败（目标可能被占用）";
            return false;
        }
        ++copied;
    }

    // 2) 删除 update_clone/ 中已不存在、但属于「程序文件」的旧文件
    std::vector<std::string> olds;
    walk_rel(root, "", olds);
    std::stable_sort(olds.begin(), olds.end(), [](const std::string& a, const std::string& b) {
        int da = rel_depth(a), db = rel_depth(b);
        if (da != db) return da > db;
        return a > b;
    });
    for (const std::string& rel : olds) {
        std::string dst = path_join(root, rel);
        if (inplace_skip(rel)) continue;
        if (file_exists(path_join(clone, rel))) continue;
        if (!is_program_file(rel)) continue;
        if (remove_file(dst)) {
            ++removed;
            if (log) log("    已删除旧文件：" + rel);
        }
    }
    return true;
}

// ================================================ 第五步：更新程序1 自更新 ==
bool patch_updater1(const std::string& root, const LogFn& log, int wait_pid,
                    std::string& version, std::string& reason) {
    version.clear();
    std::string clone = path_join(root, UPDATE_CLONE_DIR);

    int pid = wait_pid > 0 ? wait_pid : read_pid_file(root);
    if (pid > 0) {
        if (log)
            log("等待更新程序1（pid " + std::to_string(pid) + "）退出（最多 " +
                std::to_string(static_cast<int>(WAIT_PID_TIMEOUT)) + " 秒）...");
        if (wait_for_pid(pid, WAIT_PID_TIMEOUT, log)) {
            if (log) log("更新程序1 已退出");
        }
    }

    // 找 update_clone\neiwang_update1.* 或 update_clone\build\neiwang_update1.*
    std::vector<std::pair<std::string, std::string>> sources;   // (文件名, 相对子目录)
    {
        const std::string dirs[2] = {clone, path_join(clone, "build")};
        const char* subs[2] = {"", "build"};
        for (int d = 0; d < 2; ++d) {
            if (!is_dir(dirs[d])) continue;
            std::vector<std::string> names = list_dir(dirs[d]);
            std::sort(names.begin(), names.end());
            for (const std::string& n : names) {
                if (!starts_with(n, "neiwang_update1.")) continue;
                if (is_dir(path_join(dirs[d], n))) continue;
                sources.push_back({n, std::string(subs[d])});
            }
        }
    }
    if (sources.empty()) {
        reason = clone + " 里没有找到 neiwang_update1.*（先运行更新程序1）";
        return false;
    }

    for (const auto& item : sources) {
        const std::string& name = item.first;
        std::string sub = item.second;
        std::string dst_dir = sub.empty() ? root : path_join(root, sub);
        ensure_dir(dst_dir);
        std::string src = path_join(sub.empty() ? clone : path_join(clone, sub), name);
        std::string dst = path_join(dst_dir, name);
        if (copy_file(src, dst, true)) continue;

        // 目标被占用：先把旧文件改名，再复制
        std::string oldname = path_stem(name) + ".old" + path_ext(name);
        if (file_exists(dst)) {
            std::wstring wdst = u82ws(dst);
            std::wstring wold = u82ws(path_join(dst_dir, oldname));
            ::MoveFileExW(wdst.c_str(), wold.c_str(), MOVEFILE_REPLACE_EXISTING);
        }
        if (copy_file(src, dst, true)) continue;

        std::string newname = path_stem(name) + ".new" + path_ext(name);
        copy_file(src, path_join(dst_dir, newname), true);
        reason = "覆盖 " + name + " 失败（目标被占用）；已写出 " + newname +
                 "，请关闭本程序后手动改名重启";
        return false;
    }

    version = version_from_manifest(path_join(clone, UPDATE_MANIFEST_NAME));
    if (version.empty()) version = local_version(root);
    std::string text = "更新程序1 已由更新程序2 更新到 v" + version;
    if (log) log(text);
    append_clone_log(root, text);
    return true;
}

// ==================================================== 完整更新流程 ==
static bool launch_updater2(const std::string& root, const std::string& clone, const LogFn& log) {
    std::string exe = find_updater(root, clone, UPDATER2_PROGRAM_NAME);
    if (exe.empty()) {
        if (log)
            log(std::string("没有找到更新程序2（") + UPDATER2_PROGRAM_NAME +
                "），跳过「更新程序1 更新自己」这一步");
        return false;
    }
    int pid = static_cast<int>(::GetCurrentProcessId());
    write_pid_file(root, pid);
    std::string args = "--patch-updater1 --wait-pid " + std::to_string(pid) + " --root \"" +
                       root + "\"";
    std::string err;
    if (!launch_process(exe, args, root, err)) {
        if (log) log("调起更新程序2 失败：" + err);
        return false;
    }
    if (log)
        log(std::string("已调起更新程序2 ") + basename_utf8(exe) + "（--patch-updater1 --wait-pid " +
            std::to_string(pid) + "）更新更新程序1");
    return true;
}

UpdateResult perform_update(const Json& cfg, const std::string& root,
                            const std::string& data_root, const LogFn& log, bool inplace) {
    UpdateResult res;
    std::string backup = path_join(root, CUSTOMER_DATA_DIR);
    std::string target = path_join(root, UPDATE_CLONE_DIR);
    std::string err;

    // 1) 备份用户所有会话内容
    if (log) log(std::string("1/4 正在把会话内容备份到 ") + CUSTOMER_DATA_DIR + "/ ...");
    if (!backup_customer_data(data_root, backup, err)) {
        res.reason = "备份会话内容失败：" + err;
        return res;
    }
    if (log) log("    会话内容已备份：" + backup);

    // 2) 取回新版本
    if (log) log("2/4 正在取回新版本（Git 或 ZIP 更新包）...");
    std::string method, reason;
    if (!fetch_new_version(cfg, root, target, log, method, reason)) {
        res.reason = reason.empty() ? std::string("取回新版本失败") : reason;
        return res;
    }
    if (log)
        log("    新版本已就绪：" + target + "（方式：" + (method == "git" ? "Git" : "ZIP 更新包") +
            "）");

    // 3) 会话内容回填到新版本
    if (log) log("3/4 正在把会话内容回填到新版本 ...");
    if (!restore_customer_data(backup, target, err)) {
        res.path = target;
        res.backup = backup;
        res.reason = "回填会话内容失败：" + err;
        return res;
    }

    res.path = target;
    res.backup = backup;
    res.method = method;

    if (!inplace) {
        if (log) log("更新完成，新版本在：" + target);
        res.ok = true;
        return res;
    }

    // 4) 就地更新全部
    if (log) log("4/4 正在就地更新程序文件（跳过 data/ 等运行时目录）...");
    std::string ver = version_from_manifest(path_join(target, UPDATE_MANIFEST_NAME));
    long long copied = 0, removed = 0;
    if (!apply_in_place(target, root, log, copied, removed, err)) {
        res.reason = err.empty() ? std::string("就地更新失败") : err;
        return res;
    }
    if (log)
        log("    就地更新完成：覆盖 " + std::to_string(copied) + " 个文件，删除 " +
            std::to_string(removed) + " 个旧文件");
    write_applied_marker(root, ver);
    append_clone_log(root, "更新程序1 已就地更新到 v" + ver + "（覆盖 " +
                               std::to_string(copied) + "，删除 " + std::to_string(removed) + "）");

    // 5) 调起更新程序2 更新更新程序1 自己
    res.updater2 = launch_updater2(root, target, log);
    if (log) log("更新完成，新版本在：" + target);
    res.copied = copied;
    res.removed = removed;
    res.version = ver;
    res.ok = true;
    return res;
}

// ============================================================ 控制台 / 界面 ==
void attach_parent_console() {
    ::AttachConsole(ATTACH_PARENT_PROCESS);
    // 若 stdout 已被重定向（管道/文件，测试脚本常用），保留它，不要抢到 CONOUT$；
    // GUI 子系统双击启动时没有 stdout，此时才把 stdout/stderr 挂到父控制台。
    HANDLE h = ::GetStdHandle(STD_OUTPUT_HANDLE);
    if (h != nullptr && h != INVALID_HANDLE_VALUE && ::GetFileType(h) != FILE_TYPE_UNKNOWN) return;
    FILE* f = nullptr;
    freopen_s(&f, "CONOUT$", "w", stdout);
    freopen_s(&f, "CONOUT$", "w", stderr);
}

void console_println(const std::string& line) {
    std::string s = line + "\n";
    std::fwrite(s.data(), 1, s.size(), stdout);
    std::fflush(stdout);
}

static std::wstring wmsg(const std::string& s) { return u82ws(s); }

void ui_info(const std::string& text, const std::string& title) {
    ::MessageBoxW(nullptr, wmsg(text).c_str(), wmsg(title).c_str(),
                  MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
}

void ui_warn(const std::string& text, const std::string& title) {
    ::MessageBoxW(nullptr, wmsg(text).c_str(), wmsg(title).c_str(),
                  MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
}

int ui_ask_yes_no(const std::string& text, const std::string& title) {
    return ::MessageBoxW(nullptr, wmsg(text).c_str(), wmsg(title).c_str(),
                         MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND);
}

void open_url(const std::string& url) {
    if (url.empty()) return;
    ::ShellExecuteW(nullptr, L"open", wmsg(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

std::string usage_text(int role) {
    std::string name = role == 1 ? "更新程序1" : "更新程序2（只更新更新程序1）";
    const char* file = role == 1 ? "neiwang_update1.exe" : "neiwang_update2.exe";
    std::string extra;
    if (role != 1)
        extra = std::string("  ") + file +
                " --patch-updater1 [--wait-pid <pid>]\n"
                "                                  把 update_clone/ 里的更新程序1 覆盖到程序目录\n";
    return std::string(APP_NAME) + " " + name + "（独立程序，可单独运行）\n" +
           "用法：\n" +
           "  " + file + "           检查并更新（有界面）\n" +
           "  " + file + " --auto    静默检查：无网络/版本相同不打扰，有新版本才弹窗\n" +
           "  " + file + " --update  直接开始更新\n" +
           "  " + file + " --check   无界面检查，打印一行结果\n" +
           "  " + file + " --console 附加到父控制台输出进度（便于测试）\n" + extra;
}

// ============================================================== 入口 ==
int run_main(const std::vector<std::string>& argv, int role) {
    bool help = false, check = false, auto_mode = false, update_mode = false, console = false,
         patch = false;
    std::string cli_root;
    int wait_pid = -1;

    for (size_t i = 0; i < argv.size(); ++i) {
        std::string a = to_lower_ascii(trim(argv[i]));
        if (a == "-h" || a == "--help" || a == "/?") {
            help = true;
        } else if (a == "-c" || a == "--check") {
            check = true;
        } else if (a == "-a" || a == "--auto") {
            auto_mode = true;
        } else if (a == "-u" || a == "--update") {
            update_mode = true;
        } else if (a == "--patch-updater1") {
            patch = true;
        } else if (a == "--console") {
            console = true;
        } else if (a == "--root" && i + 1 < argv.size()) {
            cli_root = argv[i + 1];
        } else if (starts_with(a, "--root=")) {
            cli_root = argv[i].substr(7);
        } else if ((a == "--wait-pid" || a == "-w") && i + 1 < argv.size()) {
            wait_pid = std::atoi(argv[i + 1].c_str());
        } else if (starts_with(a, "--wait-pid=")) {
            wait_pid = std::atoi(argv[i].substr(11).c_str());
        }
    }

    if (help) {
        attach_parent_console();
        console_println(usage_text(role));
        return 0;
    }

    Paths paths = resolve_paths(cli_root);

    // ---- 更新程序2 的唯一职责：把 update_clone/ 里的更新程序1 覆盖到程序目录 ----
    if (role == 2 && patch) {
        LogFn log;
        if (console) {
            attach_parent_console();
            log = [](const std::string& s) { console_println(s); };
        }
        std::string ver, reason;
        if (!patch_updater1(paths.root, log, wait_pid, ver, reason)) {
            std::string line = "PATCH_UPDATER1_FAILED " + reason;
            if (console) console_println(line);
            else ui_warn(reason, std::string(APP_NAME) + " 更新程序2");
            return 1;
        }
        return 0;
    }

    // ---- --check：无界面，恰好一行 ----
    if (check) {
        attach_parent_console();
        Json cfg = load_cfg(paths.data_root);
        CheckResult r = check_for_update(cfg, paths.root);
        if (r.status == "update") console_println("UPDATE_AVAILABLE " + r.remote);
        else if (r.status == "current") console_println("UP_TO_DATE " + r.local);
        else console_println("OFFLINE");
        return 0;
    }

    Json cfg = load_cfg(paths.data_root);
    if (console) attach_parent_console();

    LogFn log;
    if (console) {
        log = [](const std::string& s) { console_println(s); };
    } else {
        std::string root_copy = paths.root;
        log = [root_copy](const std::string& s) { append_clone_log(root_copy, s); };
    }

    auto do_update = [&]() -> int {
        UpdateResult res = perform_update(cfg, paths.root, paths.data_root, log, true);
        if (!res.ok) {
            std::string reason = res.reason.empty() ? std::string("未知错误") : res.reason;
            // 失败原因一定要落到 update_clone.log，方便排查（GUI 模式下看不到控制台）
            append_clone_log(paths.root, "更新失败：" + reason);
            if (console) console_println("更新失败：" + reason);
            else ui_warn(reason, "更新失败");
            return 1;
        }
        std::string tail = res.updater2
                               ? "已调起更新程序2 更新更新程序1 自己，本程序即将退出。"
                               : "没能找到更新程序2，更新程序1 自己需要下次更新时才会替换。";
        std::string msg = "已就地更新到 v" + (res.version.empty() ? "?" : res.version) + "：\n" +
                          res.path +
                          "\n\n你的所有会话内容（群组、聊天、文件）已备份到 customer_god/，"
                          "并已回填进新版本的 data/ 目录。\n\n" + tail +
                          "\n\n请关闭主程序，然后重新双击「启动.bat」使用新版本。";
        if (console) console_println(msg);
        else ui_info(msg, "更新完成");
        return 0;
    };

    // ---- --update：不询问，直接开始 ----
    if (update_mode) return do_update();

    // ---- 手动 / 静默：先检查 ----
    CheckResult r = check_for_update(cfg, paths.root);

    if (r.status == "current") {
        if (auto_mode) return 0;   // 静默：不打扰
        std::string msg = "已经是最新版本（v" + r.local + "）。";
        if (console) console_println(msg);
        else ui_info(msg, std::string(APP_NAME) + " 更新程序");
        return 0;
    }

    if (r.status == "offline") {
        if (auto_mode) return 0;   // 静默：不打扰
        std::string msg = "无法连接更新服务器：" + r.reason +
                          "\n\n请检查网络连接，或确认 data/config.json 里的 update_url 是否正确。";
        if (console) console_println(msg);
        else ui_warn(msg, std::string(APP_NAME) + " 更新程序");
        return 0;
    }

    // status == "update"
    std::string ask = "发现新版本 —— v" + r.remote + "（当前 v" + r.local + "）\n\n";
    if (!r.notes.empty()) ask += "更新内容：\n" + r.notes + "\n\n";
    ask += "是否现在更新？";

    if (console) {
        console_println(ask);
        return do_update();
    }
    if (auto_mode) {
        int ans = ui_ask_yes_no(ask, "发现新版本");
        if (ans == IDYES) return do_update();
        if (ans == IDNO) open_url(trim(jstr(cfg.get("update_page"))));
        return 0;
    }
    int ans = ui_ask_yes_no(ask, "发现新版本");
    if (ans == IDYES) return do_update();
    if (ans == IDNO) open_url(trim(jstr(cfg.get("update_page"))));
    return 0;
}

} // namespace upd
} // namespace nw

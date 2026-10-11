// peercore.cpp —— PeerCore 的「主体」实现（与 neiwang.py 的 PeerCore 协议完全兼容）
//
//   本文件负责：
//     · 构造/析构、start()/stop()、8 个后台线程的启动
//     · UDP 传输层：_make_udp / _udp_loop / _dispatch_udp / _prune_seen / _broadcast
//     · 在线表：_on_presence / _sweep_loop / is_online / group_members / peer_list
//     · 群组：create_group / join_group / leave_group / send_chat / _on_chat /
//             _on_group_announce / _persist_groups / my_gid_gname
//     · 私聊：send_dm / _on_dm / send_dm_auto（离线转存线程由 file agent 实现）
//     · 改名投票：rename / start_rename_vote / cast_rename_vote / _on_rename_req /
//                 _on_rename_vote / _finish_rename_vote / _apply_rename
//     · 公共日志：broadcast_log / _on_log / emit_log / log_lines
//     · 下载工作线程（调度主体 do_download 由 file agent 实现）
//     · 检查更新 check_update（WinHTTP）
//
//   其余成员（内网 / 文件 / 活动历史 / 代理）由别的 .cpp 实现；本文件只调用，不定义。
//
//   事件通道与 Python 的 self._emit(ev, *payload) 完全等价：
//       Event{kind, args}，args 是数组（见 emit() 实现）。
//
//   相对 Python 的两处有意改进（任务要求）：
//     1) stop() 会关闭 TCP 监听 socket，使阻塞在 accept() 的 _tcp_loop 立即返回；
//     2) 所有 sleep 都切成 100ms 小片，running=false 后线程能迅速退出。

#include "core.h"

#include "../common/crypto.h"
#include "../common/sha2.h"
#include "../common/util.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winhttp.lib")

namespace nw {
namespace {

// ---------------------------------------------------------------- 小工具 --
const int kSeenLimit = 4000;      // _seen 字典长度上限（Python 第 766 行）
const int kDefaultLogLines = 800; // LOG_MAX_LINES

// 进程级常量（含 NEIWANG_* 环境变量覆盖），等价 Python 的模块级常量
const Cfg& base_cfg() {
    static const Cfg c = []() {
        Cfg x;
        load_env_overrides(x);
        return x;
    }();
    return c;
}

double cfg_num(const Json& c, const char* key, double def) {
    Json v = c.get(key);
    if (v.isNum()) return v.asNum(def);
    if (v.isStr()) {
        try { return std::stod(trim(v.asStr(""))); } catch (...) { return def; }
    }
    return def;
}

int cfg_int(const Json& c, const char* key, int def) {
    Json v = c.get(key);
    if (v.isNum()) return (int)v.asInt(def);
    if (v.isStr()) {
        try { return std::stoi(trim(v.asStr(""))); } catch (...) { return def; }
    }
    return def;
}

// 等价 Python 的布尔真值判断（数字 0 / 空串 / 空容器 / null 均为假）
bool truthy(const Json& v) {
    switch (v.type()) {
        case Json::T::Bool: return v.asBool(false);
        case Json::T::Num:  return v.asNum(0.0) != 0.0;
        case Json::T::Str:  return !v.asStr("").empty();
        case Json::T::Arr:  return !v.items().empty();
        case Json::T::Obj:  return v.keys().size() > 0;
        default:            return false;
    }
}

// 按 UTF-8 码点截断（等价 Python 的 s[:n]，绝不切出半个字符）
std::string utf8_prefix(const std::string& s, size_t max_chars) {
    size_t count = 0, i = 0;
    while (i < s.size() && count < max_chars) {
        unsigned char c = (unsigned char)s[i];
        size_t extra = 0;
        if (c < 0x80) extra = 0;
        else if ((c & 0xE0) == 0xC0) extra = 1;
        else if ((c & 0xF0) == 0xE0) extra = 2;
        else if ((c & 0xF8) == 0xF0) extra = 3;
        if (i + extra >= s.size()) break; // 尾部残缺，丢弃
        i += extra + 1;
        ++count;
    }
    return s.substr(0, i);
}

// 小片睡眠：running 变 false 后最多 100ms 就返回
void sleep_ms_checked(const std::atomic<bool>& running, int total_ms) {
    int left = total_ms;
    while (left > 0 && running.load()) {
        int step = left > 100 ? 100 : left;
        std::this_thread::sleep_for(std::chrono::milliseconds(step));
        left -= step;
    }
}

// 自己这个进程的版本号：程序目录 update.wenyi 的第一行，读不到用兜底版本
std::string app_version(const std::string& exe_dir) {
    static std::mutex m;
    static std::string cached_dir, cached_ver;
    std::lock_guard<std::mutex> g(m);
    if (!cached_ver.empty() && cached_dir == exe_dir) return cached_ver;
    std::string ver;
    const std::string path = path_join(exe_dir, UPDATE_MANIFEST_NAME);
    std::string text;
    if (file_exists(path) && read_file_utf8(path, text)) {
        Json mft = parse_update_manifest(text);
        ver = trim(mft.get("version").asStr(""));
    }
    if (ver.empty()) ver = APP_VERSION_FALLBACK;
    cached_dir = exe_dir;
    cached_ver = ver;
    return ver;
}

// UDP 单播发送（仅用于 broadcast 的 only_ip 分支与广播目标）
bool udp_send_to(SOCKET s, const std::string& data, const std::string& ip, int port) {
    if (s == INVALID_SOCKET || ip.empty() || port <= 0) return false;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((u_short)port);
    if (inet_pton(AF_INET, ip.c_str(), &a.sin_addr) != 1) return false;
    int n = sendto(s, data.data(), (int)data.size(), 0, (sockaddr*)&a, sizeof(a));
    return n != SOCKET_ERROR;
}

// ------------------------------------------------------------ 改名投票辅助 --
int vote_need_of(const PeerCore& c) {
    Json v = c.cfg.get("rename_vote_need");
    if (v.isNum()) return (int)v.asInt(base_cfg().rename_vote_need);
    if (v.isStr()) {
        try { return std::stoi(trim(v.asStr(""))); } catch (...) { return base_cfg().rename_vote_need; }
    }
    return base_cfg().rename_vote_need;
}

long long rename_count_of(const PeerCore& c) {
    Json v = c.cfg.get("rename_count");
    if (v.isNum()) return v.asInt(0);
    if (v.isStr()) {
        try { return std::stoll(trim(v.asStr(""))); } catch (...) { return 0; }
    }
    return 0;
}

bool contains_str(const Json& arr, const std::string& s) {
    if (!arr.isArr()) return false;
    for (const auto& x : arr.items()) {
        if (x.isStr() ? x.asStr("") == s : x.dump(false) == s) return true;
    }
    return false;
}

void arr_add_str(Json& holder, const std::string& key, const std::string& s) {
    Json a = holder.get(key);
    if (!a.isArr()) a = Json::arr();
    a.push(Json(s));
    holder.set(key, a);
}

int arr_size(const Json& v) { return v.isArr() ? (int)v.items().size() : 0; }

// _finish_rename_vote（Python 第 1217 行；core.h 未声明，用文件内静态函数实现）
void finish_rename_vote(PeerCore& c, const std::string& pid) {
    Json v;
    {
        std::lock_guard<std::recursive_mutex> g(c.mtx);
        auto it = c.rename_votes.find(pid);
        if (it == c.rename_votes.end()) return;
        if (it->second.get("done").asBool(false)) return;
        it->second.set("done", Json(true));
        v = it->second;
    }
    const int need = vote_need_of(c);
    const int approves = arr_size(v.get("approvers"));
    const std::string new_name = v.get("new").asStr("");
    if (approves > need) {
        std::string note = "（" + std::to_string(approves) + " 名用户投票通过）";
        c.apply_rename(new_name, v.get("old").asStr(""), note);
        c.emit("rename_vote_done",
               {Json(pid), Json(true),
                Json("投票通过：" + std::to_string(approves) + " 名用户同意（需超过 " +
                     std::to_string(need) + " 名）"),
                Json(new_name)});
    } else {
        c.emit("rename_vote_done",
               {Json(pid), Json(false),
                Json("同意票不足：收到 " + std::to_string(approves) + " 票，需超过 " +
                     std::to_string(need) + " 票"),
                Json(new_name)});
    }
    {
        std::lock_guard<std::recursive_mutex> g(c.mtx);
        c.rename_votes.erase(pid);
    }
}

// _rename_vote_waiter（Python 第 1209 行；core.h 未声明）
void rename_vote_waiter(PeerCore& c, const std::string& pid) {
    double deadline = 0.0;
    {
        std::lock_guard<std::recursive_mutex> g(c.mtx);
        auto it = c.rename_votes.find(pid);
        if (it == c.rename_votes.end()) return;
        deadline = it->second.get("deadline").asNum(0.0);
    }
    while (c.running.load()) {
        bool done = false;
        {
            std::lock_guard<std::recursive_mutex> g(c.mtx);
            auto it = c.rename_votes.find(pid);
            if (it == c.rename_votes.end()) return;
            done = it->second.get("done").asBool(false);
        }
        if (done) return;
        if (now_ts() >= deadline) break;
        sleep_ms_checked(c.running, 300);
    }
    finish_rename_vote(c, pid);
}

// 起一个 detach 的临时线程并登记进 threads（等价 Python 的 Thread+start+append）
void spawn_thread(PeerCore& c, const std::function<void()>& fn) {
    std::lock_guard<std::recursive_mutex> g(c.mtx);
    c.threads.emplace_back([fn]() {
        try { fn(); } catch (...) {}
    });
    c.threads.back().detach();
}

// ------------------------------------------------------------- WinHTTP GET --
// 等价 Python：urllib.request.Request(url, headers={"User-Agent": ua})，
// timeout=8，r.read(65536)。成功（HTTP 200）返回 true 并把正文放进 out。
bool winhttp_get(const std::string& url, const std::string& ua, int timeout_ms,
                 size_t max_bytes, std::string& out) {
    out.clear();
    std::wstring wurl = u82ws(url);
    if (wurl.empty()) return false;

    URL_COMPONENTS uc;
    std::memset(&uc, 0, sizeof(uc));
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {0};
    wchar_t path[2048] = {0};
    wchar_t extra[1024] = {0};
    uc.lpszHostName = host;
    uc.dwHostNameLength = (DWORD)(sizeof(host) / sizeof(host[0]) - 1);
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = (DWORD)(sizeof(path) / sizeof(path[0]) - 1);
    uc.lpszExtraInfo = extra;
    uc.dwExtraInfoLength = (DWORD)(sizeof(extra) / sizeof(extra[0]) - 1);
    if (!WinHttpCrackUrl(wurl.c_str(), (DWORD)wurl.size(), 0, &uc)) return false;

    std::wstring wpath = path;
    if (wpath.empty()) wpath = L"/";
    wpath += extra;

    const bool https = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    std::wstring wua = u82ws(ua);

    HINTERNET ses = WinHttpOpen(wua.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) return false;
    WinHttpSetTimeouts(ses, timeout_ms, timeout_ms, timeout_ms, timeout_ms);

    HINTERNET con = WinHttpConnect(ses, host, uc.nPort, 0);
    if (!con) {
        WinHttpCloseHandle(ses);
        return false;
    }
    HINTERNET req = WinHttpOpenRequest(con, L"GET", wpath.c_str(), nullptr,
                                       WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                       https ? WINHTTP_FLAG_SECURE : 0);
    if (!req) {
        WinHttpCloseHandle(con);
        WinHttpCloseHandle(ses);
        return false;
    }

    BOOL ok = WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                 WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (ok) ok = WinHttpReceiveResponse(req, nullptr);

    DWORD status = 0;
    DWORD slen = sizeof(status);
    if (ok) {
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &slen,
                            WINHTTP_NO_HEADER_INDEX);
    }
    if (ok && status == 200) {
        for (;;) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0) break;
            std::string chunk;
            chunk.resize((size_t)avail);
            DWORD got = 0;
            if (!WinHttpReadData(req, &chunk[0], avail, &got) || got == 0) break;
            chunk.resize((size_t)got);
            out += chunk;
            if (out.size() >= max_bytes) {
                out.resize(max_bytes);
                break;
            }
        }
    }

    WinHttpCloseHandle(req);
    WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    return ok && status == 200;
}

} // namespace

// ================================================================ 构造/析构 ==
PeerCore::PeerCore(Json cfg_in, EventQueue* events_in,
                   std::function<std::string()> name_getter_in,
                   std::string data_root_in, std::string exe_dir_in)
    : cfg(std::move(cfg_in)),
      events(events_in),
      name_getter(std::move(name_getter_in)),
      data_root(std::move(data_root_in)),
      exe_dir(std::move(exe_dir_in)) {
    uid = cfg.get("uid").asStr("");
    if (!name_getter) {
        name_getter = [this]() { return cfg.get("name").asStr(""); };
    }

    // self.groups = {g["gid"]: {"gid","gname"} for g in cfg.get("groups", [])}
    Json gs = cfg.get("groups");
    if (gs.isArr()) {
        for (const auto& g : gs.items()) {
            if (!g.isObj()) continue;
            const std::string gid = g.get("gid").asStr("");
            if (gid.empty()) continue;
            Json gg = Json::obj();
            gg.set("gid", Json(gid));
            gg.set("gname", Json(g.get("gname").asStr("")));
            groups[gid] = gg;
        }
    }

    running = true;
    udp_sock = INVALID_SOCKET;
    tcp_srv = INVALID_SOCKET;

    // 由别的 agent 实现：索引 / 保密转存 / 内网 / 活动历史 / 代理
    load_index();
    load_secrets();
    load_recv_secrets();
    load_net();
    load_activity();
    load_proxies();

    // design_v2 第 3 节：空设备判定必须在自动组网（net_create）之前取快照，
    // 否则 net_loop 建好内网后就永远判不出「空设备」了。
    blank_at_start = is_blank_device();
}

PeerCore::~PeerCore() { stop(); }

// ================================================================ 启动/停止 ==
void PeerCore::start() {
    running = true;

    int bound = 0;
    {
        std::lock_guard<std::recursive_mutex> g(mtx);
        const int want_udp = cfg_int(cfg, "udp_port", base_cfg().udp_port);
        udp_sock = make_udp_socket(want_udp, bound);
        const int want_tcp = cfg_int(cfg, "tcp_port", base_cfg().tcp_port);
        bound = 0;
        tcp_srv = make_tcp_server(want_tcp, bound);
        if (tcp_srv != INVALID_SOCKET && bound != want_tcp) {
            // Python：端口被占用时改写 cfg["tcp_port"] 并 save_config
            cfg.set("tcp_port", Json((long long)bound));
            save_config(data_root, cfg);
        }
    }

    // 8 个后台线程（net/act/proxy 的循环体由别的 agent 实现，这里只负责线程体包装）
    // 注意：**不 detach**，让 stop() 能 join 它们。否则进程退出/对象析构时，
    // 后台线程可能还在访问已销毁的 PeerCore，导致 0xC0000005 崩溃。
    auto spawn_loop = [this](void (PeerCore::*fn)()) {
        std::lock_guard<std::recursive_mutex> g(mtx);
        threads.emplace_back([this, fn]() {
            try {
                (this->*fn)();
            } catch (...) {
                // 等价 Python 的 _safe()：异常不外泄
            }
        });
    };
    spawn_loop(&PeerCore::udp_loop);
    spawn_loop(&PeerCore::tcp_loop);
    spawn_loop(&PeerCore::presence_loop);
    spawn_loop(&PeerCore::sweep_loop);
    spawn_loop(&PeerCore::download_worker);
    spawn_loop(&PeerCore::net_loop);
    spawn_loop(&PeerCore::act_loop);
    spawn_loop(&PeerCore::proxy_loop);

    emit("syslog", {Json("程序启动完成，局域网地址 " + join(local_ipv4_list(), ", "))});
}

void PeerCore::stop() {
    running = false;
    {
        std::lock_guard<std::recursive_mutex> g(mtx);
        // 与 Python 的差异（任务要求的改进）：同时关闭 TCP 监听 socket，
        // 让阻塞在 accept() 的 tcp_loop 立刻返回。
        if (udp_sock != INVALID_SOCKET) {
            close_socket(udp_sock);
            udp_sock = INVALID_SOCKET;
        }
        if (tcp_srv != INVALID_SOCKET) {
            close_socket(tcp_srv);
            tcp_srv = INVALID_SOCKET;
        }
    }
    // 等 8 个后台线程真正退出（它们不再 detach）：socket 已关、running=false，
    // 各循环最多 1 秒左右就会返回。这样 stop() 返回后对象可以安全析构。
    for (auto& t : threads) {
        if (t.joinable()) t.join();
    }
    save_activity(true);
    save_proxies();
    // 注意：net_shutdown() 由 main 调用，这里不能调。
}

// ================================================================ 基本属性 ==
std::string PeerCore::my_name() const {
    if (!name_getter) return cfg.get("name").asStr("");
    return name_getter();
}

std::vector<Json> PeerCore::my_groups() const {
    std::lock_guard<std::recursive_mutex> g(const_cast<PeerCore*>(this)->mtx);
    std::vector<Json> out;
    out.reserve(groups.size());
    for (const auto& kv : groups) out.push_back(kv.second);
    return out;
}

int PeerCore::my_tcp_port() const {
    std::lock_guard<std::recursive_mutex> g(const_cast<PeerCore*>(this)->mtx);
    if (tcp_srv != INVALID_SOCKET) {
        sockaddr_in a{};
        int alen = sizeof(a);
        if (getsockname(tcp_srv, (sockaddr*)&a, &alen) == 0) return (int)ntohs(a.sin_port);
    }
    return cfg_int(cfg, "tcp_port", base_cfg().tcp_port);
}

// in_net() 由 peercore_net.cpp 定义（内网模块的归属）

void PeerCore::require_net() {
    if (!in_net()) {
        throw std::runtime_error(
            "还没有加入内网：请先在「内网」里申请加入或自行建立一个内网");
    }
}

// ================================================================ 查询（GUI） ==
bool PeerCore::is_online(const std::string& uid_in) {
    std::lock_guard<std::recursive_mutex> g(mtx);
    auto it = peers.find(uid_in);
    if (it == peers.end()) return false;
    if (now_ts() - it->second.ts > base_cfg().presence_ttl) return false;
    return it->second.tcp_port != 0;
}

std::vector<Json> PeerCore::group_members(const std::string& gid) {
    std::vector<Json> out;
    Json self = Json::obj();
    self.set("name", Json(my_name()));
    self.set("uid", Json(uid));
    self.set("ip", Json("本机"));
    self.set("self", Json(true));
    out.push_back(self);

    const double ttl = base_cfg().presence_ttl;
    const double ts = now_ts();
    std::lock_guard<std::recursive_mutex> g(mtx);
    for (const auto& kv : peers) {
        const PeerInfo& p = kv.second;
        if (ts - (p.ts > 0 ? p.ts : 0.0) > ttl) continue;
        bool member = false;
        for (const auto& gi : p.groups.items()) {
            if (gi.isObj() && gi.get("gid").asStr("") == gid) {
                member = true;
                break;
            }
        }
        if (!member) continue;
        Json o = Json::obj();
        o.set("name", Json(p.name));
        o.set("uid", Json(p.uid));
        o.set("ip", Json(p.ip));
        o.set("self", Json(false));
        out.push_back(o);
    }
    return out;
}

// 在线表快照（供 GUI 刷新成员/在线人数）；online = TTL 内且 tcp_port 非 0
Json PeerCore::peer_list() {
    Json out = Json::arr();
    const double ttl = base_cfg().presence_ttl;
    const double ts = now_ts();
    std::lock_guard<std::recursive_mutex> g(mtx);
    for (const auto& kv : peers) {
        const PeerInfo& p = kv.second;
        const bool online = (ts - (p.ts > 0 ? p.ts : 0.0) <= ttl) && p.tcp_port != 0;
        Json o = Json::obj();
        o.set("uid", Json(p.uid));
        o.set("name", Json(p.name));
        o.set("ip", Json(p.ip));
        o.set("tcp_port", Json((long long)p.tcp_port));
        o.set("groups", p.groups.isArr() ? p.groups : Json::arr());
        o.set("ver", Json(p.ver));
        o.set("ts", Json(p.ts));
        o.set("online", Json(online));
        out.push(o);
    }
    return out;
}

std::vector<std::string> PeerCore::log_lines() {
    std::lock_guard<std::recursive_mutex> g(mtx);
    return std::vector<std::string>(logs.begin(), logs.end());
}

// ================================================================ 事件/日志 ==
void PeerCore::emit(const std::string& kind, std::initializer_list<Json> args) {
    if (!events) return;
    Json a = Json::arr();
    for (const auto& x : args) a.push(x);
    events->push(Event(kind, a));
}

// 公共日志（GUI 只读）。注意：活动历史（record_activity）由调用方负责，
// 见 broadcast_log / _on_log —— 避免同一行被记两次（eid/seq 重复消耗）。
void PeerCore::emit_log(const std::string& line, double /*ts*/) {
    int max_lines = kDefaultLogLines;
    Json ml = cfg.get("log_max_lines");
    if (ml.isNum()) max_lines = (int)ml.asInt(kDefaultLogLines);
    if (max_lines < 1) max_lines = kDefaultLogLines;
    {
        std::lock_guard<std::recursive_mutex> g(mtx);
        logs.push_back(line);
        while ((int)logs.size() > max_lines) logs.pop_front();
    }
    emit("log", {Json(line)});
}

// 等价 Python _broadcast(msg, addr=None)：补公共信封后 UDP 广播/单播
void PeerCore::broadcast(Json msg, const std::string& only_ip) {
    if (!msg.isObj()) msg = Json::obj();
    if (!msg.has("uid")) msg.set("uid", Json(uid));
    if (!msg.has("name")) msg.set("name", Json(my_name()));
    if (!msg.has("msg_id")) {
        const long long c = ++msg_counter;
        msg.set("msg_id", Json(new_msg_id(uid, c)));
    }
    const std::string data = msg.dump(false);

    std::lock_guard<std::recursive_mutex> g(mtx);
    const SOCKET s = udp_sock;
    if (s == INVALID_SOCKET) return;
    const int port = cfg_int(cfg, "udp_port", base_cfg().udp_port);

    if (!only_ip.empty()) {
        udp_send_to(s, data, only_ip, port);
        return;
    }
    // 255.255.255.255 + 各网段 x.x.x.255（broadcast_targets），再补本机各 IPv4 单播
    std::set<std::string> targets;
    for (const auto& ip : broadcast_targets()) targets.insert(ip);
    for (const auto& ip : local_ipv4_list()) targets.insert(ip);
    for (const auto& ip : targets) udp_send_to(s, data, ip, port);
}

void PeerCore::broadcast_log(const std::string& text) {
    const double ts = now_ts();
    std::string body = text;
    if (body.size() > 300) body = utf8_prefix(body, 300);

    // 先记活动历史（design_v2 第 1.2 节），再广播，再本地显示；只记一次
    record_activity("log", my_name() + ": " + body);

    Json m = Json::obj();
    m.set("t", Json("log"));
    m.set("text", Json(body));
    m.set("ts", Json(ts));
    broadcast(m);

    emit_log("[" + fmt_time(ts) + "] " + my_name() + ": " + text);
}

void PeerCore::on_log(Json msg) {
    if (!member_ok(msg.get("uid").asStr(""))) return; // 内网外的机器不能写公共日志
    const std::string name = msg.get("name").asStr("?");
    const std::string text = msg.get("text").asStr("");
    const double ts = msg.get("ts").isNum() ? msg.get("ts").asNum(now_ts()) : now_ts();

    record_activity("log", name + ": " + text, ts); // 只记一次（ts 用对方的）
    emit_log("[" + fmt_time(ts) + "] " + name + ": " + text);
}

// ================================================================ UDP 传输层 ==
void PeerCore::udp_loop() {
    std::vector<char> buf(65536);
    while (running.load()) {
        SOCKET s;
        {
            std::lock_guard<std::recursive_mutex> g(mtx);
            s = udp_sock;
        }
        if (s == INVALID_SOCKET) break;

        sockaddr_in from{};
        int flen = sizeof(from);
        const int n = recvfrom(s, buf.data(), (int)buf.size(), 0, (sockaddr*)&from, &flen);
        if (n == SOCKET_ERROR) {
            const int err = WSAGetLastError();
            if (err == WSAETIMEDOUT || err == WSAEWOULDBLOCK) continue; // settimeout(1.0)
            break; // socket 被 stop() 关闭
        }
        if (n <= 0) continue;

        bool ok = false;
        Json msg = Json::parse(std::string(buf.data(), (size_t)n), &ok);
        if (!ok || !msg.isObj()) continue;
        if (!msg.get("uid").asStr("").empty() && msg.get("uid").asStr("") == uid) continue;

        char ipbuf[64] = {0};
        if (inet_ntop(AF_INET, &from.sin_addr, ipbuf, sizeof(ipbuf)) == nullptr) ipbuf[0] = '\0';
        dispatch_udp(msg, std::string(ipbuf));
    }
}

void PeerCore::dispatch_udp(Json msg, const std::string& from_ip) {
    const std::string kind = msg.get("t").asStr("");
    const std::string mid = msg.get("msg_id").asStr("");
    if (!mid.empty()) {
        bool dup = false;
        {
            std::lock_guard<std::recursive_mutex> g(mtx);
            if (seen.find(mid) != seen.end()) dup = true;
            else {
                seen[mid] = now_ts();
                if ((int)seen.size() > kSeenLimit) prune_seen();
            }
        }
        if (dup) return;
    }

    if (kind == "presence") on_presence(msg, from_ip);
    else if (kind == "chat") on_chat(msg);
    else if (kind == "file_pub") on_file_pub(msg);
    else if (kind == "file_req") on_file_req(msg, from_ip);
    else if (kind == "file_have") on_file_have(msg);
    else if (kind == "file_sync_req") on_sync_req(msg);
    else if (kind == "log") on_log(msg);
    else if (kind == "group_announce") on_group_announce(msg);
    else if (kind == "rename_req") on_rename_req(msg);
    else if (kind == "rename_vote") on_rename_vote(msg);
    else if (kind == "dm") on_dm(msg);
    // ---------------- 内网 ----------------
    else if (kind == "net_scan") on_net_scan(msg, from_ip);
    else if (kind == "net_announce") on_net_announce(msg, from_ip);
    else if (kind == "net_join_req") on_net_join_req(msg, from_ip);
    else if (kind == "net_join_vote") on_net_join_vote(msg);
    else if (kind == "net_registry") on_net_registry(msg, from_ip);
    else if (kind == "net_member_left") on_net_member_left(msg);
    else if (kind == "net_join_result") on_net_join_result(msg);
    // ---------------- v2.0.0 活动历史 ----------------
    else if (kind == "act_hello") on_act_hello(msg, from_ip);
    else if (kind == "act_have") on_act_have(msg);
    // ---------------- v2.0.0 数据代理 ----------------
    else if (kind == "proxy_req") on_proxy_req(msg, from_ip);
    else if (kind == "proxy_ack") on_proxy_ack(msg);
    else if (kind == "proxy_online") on_proxy_online(msg);
    else if (kind == "proxy_consumed") on_proxy_consumed(msg);
    // 未识别的 t：静默忽略
}

void PeerCore::prune_seen() {
    const double ts = now_ts();
    const double ttl = base_cfg().seen_ttl;
    std::lock_guard<std::recursive_mutex> g(mtx);
    for (auto it = seen.begin(); it != seen.end();) {
        if (ts - it->second > ttl) it = seen.erase(it);
        else ++it;
    }
}

// ================================================================ 在线表 ==
void PeerCore::on_presence(Json msg, const std::string& from_ip) {
    const std::string puid = msg.get("uid").asStr("");
    if (puid.empty()) return;

    std::string name = msg.get("name").asStr("");
    std::string ip = msg.get("ip").asStr("");
    if (ip.empty()) ip = from_ip; // 缺失时回退用 UDP 源地址
    const int port = (int)msg.get("port").asInt(0);
    Json glist = msg.get("groups");
    if (!glist.isArr()) glist = Json::arr();

    PeerInfo pi;
    pi.uid = puid;
    pi.name = name;
    pi.ip = ip;
    pi.tcp_port = port;
    pi.groups = glist;
    pi.ver = msg.get("ver").asStr("");
    pi.ts = now_ts();

    std::vector<std::string> gids;
    {
        std::lock_guard<std::recursive_mutex> g(mtx);
        peers[puid] = pi;
        for (const auto& gi : pi.groups.items()) {
            if (!gi.isObj()) continue;
            const std::string gid = gi.get("gid").asStr("");
            if (gid.empty()) continue;
            gids.push_back(gid);
            if (discovered.find(gid) == discovered.end()) {
                std::string gname;
                if (gi.has("gname")) gname = gi.get("gname").asStr("");
                else gname = "群" + gid.substr(0, 6);
                Json d = Json::obj();
                d.set("gname", Json(gname));
                d.set("host_uid", Json(puid));
                d.set("host_name", Json(name));
                d.set("ts", Json(now_ts()));
                discovered[gid] = d;
            }
        }
    }

    bool first_seen = false;
    {
        std::lock_guard<std::recursive_mutex> g(mtx);
        first_seen = online_known.insert(puid).second;
    }
    if (first_seen) {
        const std::string body = "上线提醒：「" + name + "」（物理地址 " + fmt_mac(puid) + "）已上线";
        record_activity("sys", body); // 只记一次（活动历史存正文，显示行再补时间戳）
        emit_log("[" + fmt_time(now_ts()) + "] " + body);
    }

    on_presence_hook_deliver(puid);                 // file agent：保密转存送达
    proxy_on_peer_online(puid, puid, name, gids);   // proxy agent：代理推送
    emit("peers_changed", {});
}

void PeerCore::on_group_announce(Json msg) {
    const std::string gid = msg.get("gid").asStr("");
    if (gid.empty()) return;
    std::string gname;
    if (msg.has("gname")) gname = msg.get("gname").asStr("");
    else gname = "群" + gid.substr(0, 6);
    Json d = Json::obj();
    d.set("gname", Json(gname));
    d.set("host_uid", msg.get("uid"));
    d.set("host_name", Json(msg.get("name").asStr("")));
    d.set("ts", Json(now_ts()));
    {
        std::lock_guard<std::recursive_mutex> g(mtx);
        discovered[gid] = d; // 覆盖式写入
    }
    emit("peers_changed", {});
}

// ================================================================ 线程循环 ==
void PeerCore::presence_loop() {
    while (running.load()) {
        try {
            Json m = Json::obj();
            m.set("t", Json("presence"));
            m.set("ip", Json(local_ipv4()));
            m.set("port", Json((long long)my_tcp_port()));
            Json gl = Json::arr();
            for (const auto& g : my_groups()) {
                Json o = Json::obj();
                o.set("gid", g.get("gid"));
                o.set("gname", g.get("gname"));
                gl.push(o);
            }
            m.set("groups", gl);
            m.set("ver", Json(app_version(exe_dir)));
            broadcast(m);
        } catch (...) {
            // 等价 Python：_presence_loop 里异常被吞
        }
        sleep_ms_checked(running, (int)(base_cfg().presence_interval * 1000.0));
    }
}

void PeerCore::sweep_loop() {
    const double ttl = base_cfg().presence_ttl;
    while (running.load()) {
        sleep_ms_checked(running, 3000);
        if (!running.load()) break;
        const double ts = now_ts();
        bool changed = false;
        {
            std::lock_guard<std::recursive_mutex> g(mtx);
            for (auto it = peers.begin(); it != peers.end();) {
                if (ts - it->second.ts > ttl * 2.0) {
                    online_known.erase(it->first);   // 掉线重上时要能再出一次「上线提醒」
                    it = peers.erase(it);
                    changed = true;
                } else {
                    ++it;
                }
            }
            for (auto it = discovered.begin(); it != discovered.end();) {
                const double dts = it->second.get("ts").asNum(0.0);
                if (ts - dts > ttl * 4.0) {
                    it = discovered.erase(it);
                    changed = true;
                } else {
                    ++it;
                }
            }
        }
        if (changed) emit("peers_changed", {});
    }
}

void PeerCore::tcp_loop() {
    while (running.load()) {
        SOCKET srv;
        {
            std::lock_guard<std::recursive_mutex> g(mtx);
            srv = tcp_srv;
        }
        if (srv == INVALID_SOCKET) break;

        sockaddr_in from{};
        int flen = sizeof(from);
        const SOCKET conn = accept(srv, (sockaddr*)&from, &flen);
        if (conn == INVALID_SOCKET) {
            if (!running.load()) break;
            const int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK || err == WSAETIMEDOUT) continue;
            break; // stop() 关闭监听 socket
        }
        char ipbuf[64] = {0};
        if (inet_ntop(AF_INET, &from.sin_addr, ipbuf, sizeof(ipbuf)) == nullptr) ipbuf[0] = '\0';
        const std::string peer_ip = ipbuf;

        // 每个连接一个 detach 线程（等价 Python name="tcp-conn" daemon）
        std::thread([this, conn, peer_ip]() {
            try {
                handle_conn(conn, peer_ip);
            } catch (...) {
            }
        }).detach();
    }
}

void PeerCore::download_worker() {
    while (running.load()) {
        DlTask task;
        bool got = false;
        {
            std::lock_guard<std::recursive_mutex> g(mtx);
            if (!download_queue.empty()) {
                task = download_queue.front();
                download_queue.pop_front();
                got = true;
            }
        }
        if (!got) {
            sleep_ms_checked(running, 200);
            continue;
        }
        try {
            do_download(task, task.attempt);
        } catch (...) {
            if (running.load()) {
                Json st = Json::obj();
                st.set("id", Json(task.fid));
                st.set("status", Json("error"));
                st.set("note", Json("下载失败，可手动重试"));
                emit("file_status", {st});
            }
        }
        {
            std::lock_guard<std::recursive_mutex> g(mtx);
            downloading.erase(task.fid);
        }
    }
}

// ================================================================ 聊天/私聊 ==
void PeerCore::send_chat(const std::string& gid, const std::string& text_in) {
    std::string text = utf8_prefix(trim(text_in), (size_t)base_cfg().max_chat_len);
    if (text.empty()) return;

    Json g;
    {
        std::lock_guard<std::recursive_mutex> gl(mtx);
        auto it = groups.find(gid);
        if (it == groups.end()) return;
        g = it->second;
    }
    require_net();

    const std::string gname = g.get("gname").asStr("");
    const double ts = now_ts();
    Json m = Json::obj();
    m.set("t", Json("chat"));
    m.set("gid", Json(gid));
    m.set("gname", Json(gname));
    m.set("text", Json(text));
    m.set("ts", Json(ts));
    broadcast(m);

    record_activity("chat", my_name() + ": " + text);

    Json ev = Json::obj();
    ev.set("gid", Json(gid));
    ev.set("gname", Json(gname));
    ev.set("from_name", Json(my_name()));
    ev.set("from_uid", Json(uid));
    ev.set("text", Json(text));
    ev.set("ts", Json(ts));
    ev.set("self", Json(true));
    emit("chat", {ev});
}

void PeerCore::on_chat(Json msg) {
    const std::string gid = msg.get("gid").asStr("");
    bool known = false;
    {
        std::lock_guard<std::recursive_mutex> g(mtx);
        known = groups.find(gid) != groups.end();
    }
    if (!known) return; // 不是本机加入的群
    if (!member_ok(msg.get("uid").asStr(""))) return; // 内网外的机器一律忽略

    const std::string from_name = msg.get("name").asStr("?");
    const std::string text = utf8_prefix(msg.get("text").asStr(""),
                                         (size_t)base_cfg().max_chat_len);
    const double ts = msg.get("ts").isNum() ? msg.get("ts").asNum(now_ts()) : now_ts();

    record_activity("chat", from_name + ": " + text);

    Json ev = Json::obj();
    ev.set("gid", Json(gid));
    ev.set("gname", Json(msg.get("gname").asStr("")));
    ev.set("from_name", Json(from_name));
    ev.set("from_uid", Json(msg.get("uid").asStr("")));
    ev.set("text", Json(text));
    ev.set("ts", Json(ts));
    ev.set("self", Json(false));
    emit("chat", {ev});
}

bool PeerCore::send_dm(const std::string& to_uid_in, const std::string& text_in) {
    std::string text = utf8_prefix(trim(text_in), (size_t)base_cfg().max_chat_len);
    const std::string to_uid = to_uid_in;
    if (text.empty() || to_uid.empty() || to_uid == uid) return false;

    const double ts = now_ts();
    Json m = Json::obj();
    m.set("t", Json("dm"));
    m.set("to_uid", Json(to_uid));
    m.set("text", Json(text));
    m.set("ts", Json(ts));
    broadcast(m);

    Json ev = Json::obj();
    ev.set("to_uid", Json(to_uid));
    ev.set("from_uid", Json(uid));
    ev.set("from_name", Json(my_name()));
    ev.set("text", Json(text));
    ev.set("ts", Json(ts));
    ev.set("self", Json(true));
    emit("dm", {ev});
    return true;
}

void PeerCore::on_dm(Json msg) {
    if (msg.get("to_uid").asStr("") != uid) return; // 不是发给自己的：静默丢弃
    if (!member_ok(msg.get("uid").asStr(""))) return;

    const std::string text = utf8_prefix(msg.get("text").asStr(""),
                                         (size_t)base_cfg().max_chat_len);
    const double ts = msg.get("ts").isNum() ? msg.get("ts").asNum(now_ts()) : now_ts();

    Json ev = Json::obj();
    ev.set("to_uid", Json(uid));
    ev.set("from_uid", Json(msg.get("uid").asStr("")));
    ev.set("from_name", Json(msg.get("name").asStr("?")));
    ev.set("text", Json(text));
    ev.set("ts", Json(ts));
    ev.set("self", Json(false));
    emit("dm", {ev});
}

Json PeerCore::send_dm_auto(const std::string& to_uid_in, const std::string& text_in) {
    std::string text = utf8_prefix(trim(text_in), (size_t)base_cfg().max_chat_len);
    const std::string to_uid = to_uid_in;

    Json out = Json::obj();
    if (text.empty() || to_uid.empty() || to_uid == uid) {
        out.set("ok", Json(false));
        out.set("reason", Json("参数无效"));
        return out;
    }
    if (is_online(to_uid)) {
        send_dm(to_uid, text);
        out.set("ok", Json(true));
        out.set("direct", Json(true));
        return out;
    }
    // 离线：交给 _relay_dm_worker（file agent 实现）异步转存
    spawn_thread(*this, [this, to_uid, text]() { relay_dm_worker(to_uid, text); });
    out.set("ok", Json(true));
    out.set("pending", Json(true));
    return out;
}

// ================================================================ 群组管理 ==
Json PeerCore::create_group(const std::string& gname_in) {
    require_net();
    const std::string gname = sanitize_name(gname_in, 40);
    const std::string gid = random_hex_upper(3); // 6 位大写十六进制
    Json g = Json::obj();
    g.set("gid", Json(gid));
    g.set("gname", Json(gname));
    {
        std::lock_guard<std::recursive_mutex> gl(mtx);
        groups[gid] = g;
    }
    persist_groups();

    Json m = Json::obj();
    m.set("t", Json("group_announce"));
    m.set("gid", Json(gid));
    m.set("gname", Json(gname));
    broadcast(m);

    broadcast_log("「" + my_name() + "」创建了群「" + gname + "」");
    sync_req(gid); // file agent：请求群文件列表
    emit("groups_changed", {});
    return g;
}

Json PeerCore::join_group(const std::string& gid_in, const std::string& gname_in) {
    require_net();
    std::string gid = trim(gid_in);
    for (char& c : gid) {
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    }
    if (gid.empty()) throw std::runtime_error("群号不能为空");
    {
        std::lock_guard<std::recursive_mutex> gl(mtx);
        auto it = groups.find(gid);
        if (it != groups.end()) return it->second; // 已在群里：直接返回
    }

    std::string gname = gname_in;
    if (gname.empty()) {
        std::lock_guard<std::recursive_mutex> gl(mtx);
        auto it = discovered.find(gid);
        if (it != discovered.end() && it->second.has("gname"))
            gname = it->second.get("gname").asStr("");
        else
            gname = "群" + gid.substr(0, 6);
    }
    Json g = Json::obj();
    g.set("gid", Json(gid));
    g.set("gname", Json(sanitize_name(gname, 40)));
    {
        std::lock_guard<std::recursive_mutex> gl(mtx);
        groups[gid] = g;
    }
    persist_groups();
    broadcast_log("「" + my_name() + "」加入了群「" + g.get("gname").asStr("") + "」");
    sync_req(gid); // file agent
    emit("groups_changed", {});
    return g;
}

void PeerCore::leave_group(const std::string& gid) {
    Json g;
    bool had = false;
    {
        std::lock_guard<std::recursive_mutex> gl(mtx);
        auto it = groups.find(gid);
        if (it != groups.end()) {
            g = it->second;
            groups.erase(it);
            had = true;
        }
    }
    if (!had) return;
    persist_groups();
    broadcast_log("「" + my_name() + "」退出了群「" + g.get("gname").asStr("") + "」");
    emit("groups_changed", {});
}

void PeerCore::persist_groups() {
    Json gs = Json::arr();
    {
        std::lock_guard<std::recursive_mutex> gl(mtx);
        for (const auto& kv : groups) {
            Json o = Json::obj();
            o.set("gid", Json(kv.first));
            o.set("gname", kv.second.get("gname"));
            gs.push(o);
        }
    }
    cfg.set("groups", gs);
    save_config(data_root, cfg);
}

// 群号 -> 群名：优先本机已加入的群，其次 discovered，最后用 fallback
std::string PeerCore::my_gid_gname(const std::string& gid, const std::string& fallback) {
    std::lock_guard<std::recursive_mutex> g(mtx);
    auto it = groups.find(gid);
    if (it != groups.end()) {
        const std::string n = it->second.get("gname").asStr("");
        if (!n.empty()) return n;
    }
    auto d = discovered.find(gid);
    if (d != discovered.end()) {
        const std::string n = d->second.get("gname").asStr("");
        if (!n.empty()) return n;
    }
    return fallback;
}

// ================================================================ 改名投票 ==
void PeerCore::rename_self(const std::string& new_name_in) {
    const std::string new_name = sanitize_name(new_name_in, 40);
    const std::string old = my_name();
    if (new_name == old) return;
    if (rename_count_of(*this) > 0) {
        throw std::runtime_error("已改过一次名字，之后的修改需要超过 " +
                                 std::to_string(vote_need_of(*this)) + " 名用户投票同意");
    }
    apply_rename(new_name, old, "");
}

void PeerCore::apply_rename(const std::string& new_name, const std::string& old_name,
                            const std::string& note) {
    const long long next_count = rename_count_of(*this) + 1;
    cfg.set("name", Json(new_name));
    cfg.set("rename_count", Json(next_count));
    save_config(data_root, cfg);

    std::string line = "「" + old_name + "」更名为「" + new_name + "」" + note;
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t' ||
                             line.back() == '\r' || line.back() == '\n'))
        line.pop_back(); // 等价 Python 的 .rstrip()
    broadcast_log(line);
    emit("name_changed", {Json(new_name)});
}

std::string PeerCore::start_rename_vote(const std::string& new_name_in) {
    const std::string new_name = sanitize_name(new_name_in, 40);
    const std::string old = my_name();
    if (new_name == old) throw std::runtime_error("新名字与当前名字相同");
    if (rename_count_of(*this) == 0)
        throw std::runtime_error("第一次改名无需投票，请直接保存");

    {
        std::lock_guard<std::recursive_mutex> g(mtx);
        for (const auto& kv : rename_votes) {
            if (!kv.second.get("done").asBool(false))
                throw std::runtime_error("已有一个改名投票正在进行，请等待结果");
        }
    }

    const long long c = ++msg_counter;
    const std::string pid = new_msg_id(uid, c);
    {
        std::lock_guard<std::recursive_mutex> g(mtx);
        Json v = Json::obj();
        v.set("old", Json(old));
        v.set("new", Json(new_name));
        v.set("approvers", Json::arr());
        v.set("rejectors", Json::arr());
        v.set("done", Json(false));
        v.set("deadline", Json(now_ts() + base_cfg().rename_vote_wait));
        rename_votes[pid] = v;
    }

    Json m = Json::obj();
    m.set("t", Json("rename_req"));
    m.set("pid", Json(pid));
    m.set("old", Json(old));
    m.set("new", Json(new_name));
    m.set("ts", Json(now_ts()));
    broadcast(m);

    broadcast_log("「" + old + "」申请更名为「" + new_name + "」：需要超过 " +
                  std::to_string(vote_need_of(*this)) + " 名用户投票同意后生效");
    emit("rename_vote_progress", {Json(pid), Json((long long)0), Json((long long)0)});

    // 等待线程（核心把投票结果在 120 秒内或票数达标时结算）
    spawn_thread(*this, [this, pid]() { rename_vote_waiter(*this, pid); });
    return pid;
}

void PeerCore::cast_rename_vote(const std::string& pid_in, bool approve) {
    const std::string pid = pid_in;
    if (pid.empty()) return;
    {
        std::lock_guard<std::recursive_mutex> g(mtx);
        if (my_votes.find(pid) != my_votes.end()) return; // 同一提案只计一票
        if (my_votes.size() > 1000) my_votes.clear();     // Python：超过即整体清空
        my_votes.insert(pid);
    }
    Json m = Json::obj();
    m.set("t", Json("rename_vote"));
    m.set("pid", Json(pid));
    m.set("approve", Json(approve ? 1 : 0)); // 整数而非布尔
    m.set("ts", Json(now_ts()));
    broadcast(m);
}

void PeerCore::on_rename_req(Json msg) {
    const std::string pid = msg.get("pid").asStr("");
    if (pid.empty()) return;
    {
        std::lock_guard<std::recursive_mutex> g(mtx);
        if (rename_requests.find(pid) != rename_requests.end()) return; // 每个提案只弹一次
    }
    if (!member_ok(msg.get("uid").asStr(""))) return;
    {
        std::lock_guard<std::recursive_mutex> g(mtx);
        if (rename_requests.size() > 1000) rename_requests.clear();
        rename_requests.insert(pid);
    }
    const double ts = msg.get("ts").asNum(0.0);
    if (now_ts() - ts > base_cfg().rename_vote_wait + 10.0) return; // 过期提案不打扰用户

    Json ev = Json::obj();
    ev.set("pid", Json(pid));
    ev.set("from_uid", Json(msg.get("uid").asStr("")));
    ev.set("from_name", Json(msg.get("name").asStr("?")));
    ev.set("old", Json(msg.get("old").asStr("")));
    ev.set("new", Json(msg.get("new").asStr("")));
    emit("rename_request", {ev});
}

void PeerCore::on_rename_vote(Json msg) {
    const std::string pid = msg.get("pid").asStr("");
    int approves = 0;
    int rejects = 0;
    {
        std::lock_guard<std::recursive_mutex> g(mtx);
        auto it = rename_votes.find(pid);
        if (it == rename_votes.end()) return;
        Json& v = it->second;
        if (v.get("done").asBool(false)) return;

        const std::string voter = msg.get("uid").asStr("");
        if (voter.empty()) return; // 同一 uid 只能投一次，不可改票
        if (!v.get("approvers").isArr()) v.set("approvers", Json::arr());
        if (!v.get("rejectors").isArr()) v.set("rejectors", Json::arr());
        if (contains_str(v.get("approvers"), voter) ||
            contains_str(v.get("rejectors"), voter))
            return;

        if (truthy(msg.get("approve"))) arr_add_str(v, "approvers", voter);
        else arr_add_str(v, "rejectors", voter);
        approves = arr_size(v.get("approvers"));
        rejects = arr_size(v.get("rejectors"));
    }
    emit("rename_vote_progress",
         {Json(pid), Json((long long)approves), Json((long long)rejects)});
    if (approves > vote_need_of(*this)) finish_rename_vote(*this, pid);
}

// ================================================================ 空设备判定 ==
bool PeerCore::is_blank_device() const {
    // design_v2 第 3.1 节：未入网 + 无活动数据 + config.groups 为空
    if (in_net()) return false;
    {
        std::lock_guard<std::recursive_mutex> g(const_cast<PeerCore*>(this)->mtx);
        if (!act_events.empty()) return false;
    }
    Json g = cfg.get("groups");
    if (g.isArr() && g.size() > 0) return false;
    return true;
}

// ================================================================ 检查更新 ==
void PeerCore::check_update(bool /*manual*/) {
    std::thread([this]() {
        Json out = Json::obj();
        try {
            const std::string url = trim(cfg.get("update_url").asStr(""));
            if (url.empty()) {
                out.set("ok", Json(false));
                out.set("reason", Json("no-url"));
                emit("update", {out});
                return;
            }
            const std::string local = app_version(exe_dir);
            std::string body;
            if (!winhttp_get(url, "NeiWangLAN/" + local, 8000, 65536, body)) {
                out.set("ok", Json(false));
                out.set("reason", Json("offline"));
                emit("update", {out});
                return;
            }
            Json mft = parse_update_manifest(body);
            const std::string remote = mft.get("version").asStr("");
            const std::string notes = mft.get("notes").asStr("");
            if (!remote.empty() && is_newer_version(remote, local)) {
                out.set("ok", Json(true));
                out.set("found", Json(true));
                out.set("remote", Json(remote));
                out.set("notes", Json(notes));
                out.set("local", Json(local));
                out.set("page", Json(cfg.get("update_page").asStr("")));
                out.set("repo", Json(cfg.get("update_repo").asStr("")));
                out.set("package", Json(cfg.get("update_package").asStr("")));
                emit("update", {out});
            } else {
                out.set("ok", Json(true));
                out.set("found", Json(false));
                emit("update", {out});
            }
        } catch (...) {
            Json err = Json::obj();
            err.set("ok", Json(false));
            err.set("reason", Json("offline"));
            emit("update", {err});
        }
    }).detach();
}

} // namespace nw

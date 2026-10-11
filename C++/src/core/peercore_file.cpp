// peercore_file.cpp —— 文件系统 / 点对点下载 / TCP 服务 / 保密转存
// 严格复刻 neiwang.py 第 944–1093（保密转存）与第 2036–2440（文件 + TCP 服务）行，
// 以及 docs/design_v2.md 第 2 节（v2.0.0 设备数据传输代理）涉及的接口调用。
//
// 本文只实现 core.h 中归属本文件的成员：
//   cache_dir / downloads_dir / index_path / load_index / save_index
//   load_secrets / save_secrets / load_recv_secrets / save_recv_secrets
//   has_file / index_record / register_file / file_view / files_of_group
//   send_file_to_group / on_file_pub / sync_req / on_sync_req
//   do_download / public_request / on_file_req / on_file_have
//   fetch_from / finish_download / handle_conn
//   relay_dm_worker / store_secret_on / on_presence_hook_deliver
//   deliver_secrets_to / deliver_one / on_secret_store / on_secret_deliver
// download_worker() 与 tcp_loop() 由 peercore.cpp 实现。

#include "core.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include "../common/crypto.h"
#include "../common/sha2.h"
#include "../common/util.h"

namespace nw {

namespace fs = std::filesystem;

// ---------------------------------------------------------------- 内部工具 --
namespace {

// 与 SHA256_FILECHUNK = 128 * 1024 对应；FILE_CHUNK 由 Cfg 提供，此处为兜底。
const size_t kFileChunkFallback = 128 * 1024;

// 仅本文件使用的时间/配置静态缓存（与 net agent 相同方式）
Cfg& file_cfg() {
    static Cfg c;
    static bool inited = false;
    if (!inited) {
        load_env_overrides(c);
        inited = true;
    }
    return c;
}

size_t chunk_size() {
    int c = file_cfg().file_chunk;
    return c > 0 ? static_cast<size_t>(c) : kFileChunkFallback;
}

// Python 的 `bool(rec.get("has_local"))`：只有 true 为真
bool truthy(const Json& v) {
    return v.isBool() ? v.asBool() : false;
}

fs::path to_fspath(const std::string& utf8) {
    return fs::u8path(utf8);
}

// 等价 Python os.path.splitext(name)
std::pair<std::string, std::string> split_ext(const std::string& name) {
    size_t slash = name.find_last_of("/\\");
    size_t dot = name.find_last_of('.');
    if (dot == std::string::npos) return {name, std::string()};
    if (slash != std::string::npos && dot < slash) return {name, std::string()};
    if (dot == 0) return {name, std::string()};   // 与 Python 一致：".bashrc" 无扩展名
    return {name.substr(0, dot), name.substr(dot)};
}

// 等价 Python hmac.compare_digest（等长时逐字节异或累积）
bool const_time_eq(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    unsigned char acc = 0;
    for (size_t i = 0; i < a.size(); ++i)
        acc |= static_cast<unsigned char>(a[i] ^ b[i]);
    return acc == 0;
}

// 连接超时（SO_RCVTIMEO + SO_SNDTIMEO；C++ 侧收发超时是两个独立选项）
void set_sock_timeout(SOCKET s, int seconds) {
    if (s == INVALID_SOCKET) return;
    DWORD ms = static_cast<DWORD>(seconds) * 1000u;
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms), sizeof(ms));
    ::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&ms), sizeof(ms));
}

// 等价 Python `msg.get("x") or 默认值`
std::string str_or(const Json& v, const std::string& def) {
    std::string s = v.asStr();
    return s.empty() ? def : s;
}

// 文件正文发送（FILE_CHUNK 分块）
bool send_file_body(SOCKET conn, const std::string& path) {
    std::error_code ec;
    fs::path p = to_fspath(path);
    if (!fs::is_regular_file(p, ec)) return false;
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    std::vector<char> buf(chunk_size());
    while (in) {
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        std::streamsize got = in.gcount();
        if (got <= 0) break;
        if (!send_all(conn, buf.data(), static_cast<size_t>(got))) return false;
    }
    return true;
}

} // namespace

// ============================================================ 目录 / 索引 ==

std::string PeerCore::cache_dir() {
    std::string d = path_join(data_root, "cache");
    ensure_dir(d);
    return d;
}

std::string PeerCore::downloads_dir(const std::string& gid, const std::string& gname) {
    std::string sub = sanitize_name(gname, 30) + "_" + gid.substr(0, 6);
    std::string d = path_join(path_join(data_root, "downloads"), sub);
    ensure_dir(d);
    return d;
}

std::string PeerCore::index_path() {
    return path_join(data_root, "index.json");
}

void PeerCore::load_index() {
    std::lock_guard<std::recursive_mutex> lk(mtx);
    index.clear();
    std::string data;
    if (!read_file_utf8(index_path(), data)) return;   // 不存在 / 读失败 -> 空索引
    bool ok = false;
    Json v = Json::parse(data, &ok);
    if (!ok || !v.isObj()) return;                     // 解析失败 -> 空索引
    for (const std::string& k : v.keys()) index[k] = v.get(k);
}

void PeerCore::save_index() {
    std::lock_guard<std::recursive_mutex> lk(mtx);
    Json o = Json::obj();
    for (const auto& kv : index) o.set(kv.first, kv.second);
    // 与 Python 一致：写失败静默吞掉
    write_file_utf8(index_path(), o.dump());
}

// ------------------------------------------------------------- 保密转存落盘 --

void PeerCore::load_secrets() {
    std::lock_guard<std::recursive_mutex> lk(mtx);
    secrets.clear();
    std::string data;
    if (!read_file_utf8(path_join(data_root, "secret_relay.json"), data)) return;
    bool ok = false;
    Json v = Json::parse(data, &ok);
    if (!ok || !v.isObj()) return;
    for (const std::string& k : v.keys()) secrets[k] = v.get(k);
}

void PeerCore::save_secrets() {
    std::lock_guard<std::recursive_mutex> lk(mtx);
    Json o = Json::obj();
    for (const auto& kv : secrets) o.set(kv.first, kv.second);
    write_file_utf8(path_join(data_root, "secret_relay.json"), o.dump());
}

void PeerCore::load_recv_secrets() {
    std::lock_guard<std::recursive_mutex> lk(mtx);
    recv_secrets.clear();
    std::string data;
    if (!read_file_utf8(path_join(data_root, "secret_received.json"), data)) return;
    bool ok = false;
    Json v = Json::parse(data, &ok);
    if (!ok || !v.isArr()) return;
    for (const Json& it : v.items()) {
        if (it.isStr()) recv_secrets.insert(it.asStr());
    }
}

void PeerCore::save_recv_secrets() {
    std::lock_guard<std::recursive_mutex> lk(mtx);
    Json a = Json::arr();
    for (const std::string& s : recv_secrets) a.push(Json(s));   // std::set 已按字典序
    write_file_utf8(path_join(data_root, "secret_received.json"), a.dump());
}

// ================================================================ 文件索引 ==

bool PeerCore::has_file(const std::string& file_id) {
    std::lock_guard<std::recursive_mutex> lk(mtx);
    auto it = index.find(file_id);
    if (it == index.end()) return false;
    if (!truthy(it->second.get("has_local"))) return false;
    std::error_code ec;
    return fs::is_regular_file(to_fspath(path_join(cache_dir(), file_id)), ec);
}

Json PeerCore::index_record(const std::string& file_id) {
    std::lock_guard<std::recursive_mutex> lk(mtx);
    auto it = index.find(file_id);
    return it == index.end() ? Json() : it->second;
}

void PeerCore::register_file(const std::string& fid, Json rec) {
    std::lock_guard<std::recursive_mutex> lk(mtx);
    Json old = index.count(fid) ? index[fid] : Json::obj();
    if (!rec.isObj()) rec = Json::obj();
    if (!rec.has("groups")) rec.set("groups", Json::arr());
    // groups = sorted(set(old.groups) | set(rec.groups))
    std::vector<std::string> gs;
    for (const Json& g : old.get("groups").items()) {
        if (g.isStr()) gs.push_back(g.asStr());
    }
    for (const Json& g : rec.get("groups").items()) {
        if (g.isStr()) gs.push_back(g.asStr());
    }
    std::sort(gs.begin(), gs.end());
    gs.erase(std::unique(gs.begin(), gs.end()), gs.end());
    Json arr = Json::arr();
    for (const std::string& g : gs) arr.push(Json(g));
    rec.set("groups", arr);
    index[fid] = rec;
    save_index();
}

Json PeerCore::file_view(const std::string& fid) {
    std::lock_guard<std::recursive_mutex> lk(mtx);
    auto it = index.find(fid);
    Json r = (it == index.end()) ? Json::obj() : it->second;
    std::error_code ec;
    bool cache_ok = fs::is_regular_file(to_fspath(path_join(cache_dir(), fid)), ec);
    Json v = Json::obj();
    v.set("id", Json(fid));
    v.set("name", Json(str_or(r.get("name"), "?")));
    v.set("size", Json(r.get("size").asInt(0)));
    v.set("src_uid", Json(r.get("src_uid").asStr()));
    v.set("src_name", Json(r.get("src_name").asStr()));
    v.set("ts", Json(r.get("ts").asNum(0.0)));
    Json gs = Json::arr();
    for (const Json& g : r.get("groups").items()) {
        if (g.isStr()) gs.push(Json(g.asStr()));
    }
    v.set("groups", gs);
    v.set("has_local", Json(truthy(r.get("has_local")) && cache_ok));
    return v;
}

std::vector<Json> PeerCore::files_of_group(const std::string& gid) {
    std::lock_guard<std::recursive_mutex> lk(mtx);
    std::vector<Json> out;
    for (const auto& kv : index) {
        bool in = false;
        for (const Json& g : kv.second.get("groups").items()) {
            if (g.isStr() && g.asStr() == gid) { in = true; break; }
        }
        if (in) out.push_back(file_view(kv.first));
    }
    return out;
}

// ============================================================ 发送文件到群 ==

void PeerCore::send_file_to_group(const std::string& file_path, const std::string& gid) {
    require_net();
    if (!file_exists(file_path)) throw std::runtime_error("文件不存在");
    std::string gname;
    {
        std::lock_guard<std::recursive_mutex> lk(mtx);
        auto it = groups.find(gid);
        if (it == groups.end()) throw std::runtime_error("群组不存在");
        gname = it->second.get("gname").asStr();
    }
    long long fsize = file_size(file_path);
    std::string fname = basename_utf8(file_path);

    emit("syschat", {Json(gid),
                     Json("正在计算文件指纹（" + human_size(static_cast<double>(fsize)) + "）...")});

    std::string fid;
    std::wstring wpath = u82ws(file_path);
    if (!sha256_file_hex(wpath, fid)) throw std::runtime_error("文件不存在");

    // 复制进缓存（作为发送者，提供服务）
    std::string cache_file = path_join(cache_dir(), fid);
    if (!file_exists(cache_file)) {
        if (!copy_file(file_path, cache_file, true)) throw std::runtime_error("文件不存在");
    }

    Json rec = Json::obj();
    rec.set("name", Json(fname));
    rec.set("size", Json(fsize));
    rec.set("ts", Json(now_ts()));
    rec.set("src_uid", Json(uid));
    rec.set("src_name", Json(my_name()));
    Json grp = Json::arr();
    grp.push(Json(gid));
    rec.set("groups", grp);
    rec.set("has_local", Json(true));
    register_file(fid, rec);

    Json file_obj = Json::obj();
    file_obj.set("id", Json(fid));
    file_obj.set("name", Json(fname));
    file_obj.set("size", Json(fsize));

    Json pub = Json::obj();
    pub.set("t", Json("file_pub"));
    pub.set("gid", Json(gid));
    pub.set("gname", Json(gname));
    pub.set("file", file_obj);
    pub.set("ts", Json(now_ts()));
    pub.set("sender_uid", Json(uid));
    pub.set("sender_name", Json(my_name()));
    broadcast(pub);

    broadcast_log("「" + my_name() + "」在群「" + gname + "」发送了文件「" + fname + "」");

    Json ev = Json::obj();
    ev.set("file", file_view(fid));
    ev.set("gid", Json(gid));
    ev.set("from_self", Json(true));
    emit("file_new", {ev});

    // ---- v2.0.0：群内没有其他在线成员 -> 启动设备数据传输代理 ----
    bool others_online = false;
    for (const Json& m : group_members(gid)) {
        std::string muid = m.get("uid").asStr();
        if (muid.empty() || muid == uid) continue;
        if (is_online(muid)) { others_online = true; break; }
    }
    if (!others_online) {
        Json info = Json::obj();
        info.set("id", Json(fid));
        info.set("name", Json(fname));
        info.set("size", Json(fsize));
        // proxy_start 会阻塞等待在线应答（最长 proxy_req_wait + 每次寄存的 TCP 超时），
        // 绝不能占用 GUI 线程：放到后台线程执行（Python 版同样是独立线程）。
        std::thread([this, gid, info]() {
            try {
                if (running.load()) proxy_start("file", gid, info, "");
            } catch (...) {
            }
        }).detach();
    }
}

// ============================================================ 收到文件公告 ==

void PeerCore::on_file_pub(Json msg) {
    std::string gid = msg.get("gid").asStr();
    {
        std::lock_guard<std::recursive_mutex> lk(mtx);
        if (groups.find(gid) == groups.end()) return;
    }
    if (!member_ok(msg.get("uid").asStr())) return;   // 内网外的机器发来的文件公告忽略
    Json f = msg.get("file");
    std::string fid = f.get("id").asStr();
    if (fid.empty()) return;
    std::string gname, fallback;
    {
        std::lock_guard<std::recursive_mutex> lk(mtx);
        auto it = groups.find(gid);
        fallback = (it == groups.end()) ? std::string() : it->second.get("gname").asStr();
    }
    gname = str_or(msg.get("gname"), fallback);
    // 使用原始发送人信息（转发 / 重播时保持一致，确保通知显示「原作者」）
    std::string sender = str_or(msg.get("sender_name"), str_or(msg.get("name"), "?"));
    std::string sender_uid = str_or(msg.get("sender_uid"), msg.get("uid").asStr());
    double ts = msg.get("ts").asNum(now_ts());
    std::string fname = str_or(f.get("name"), "?");
    long long fsize = f.get("size").asInt(0);

    // 公共日志：全部可见
    emit("log", {Json("[" + fmt_time(ts) + "] " + sender +
                      ": 在群「" + gname + "」发送了文件「" + fname + "」")});

    Json fv = Json::obj();
    fv.set("id", Json(fid));
    fv.set("name", Json(fname));
    fv.set("size", Json(fsize));
    fv.set("src_uid", Json(sender_uid));
    fv.set("src_name", Json(sender));
    fv.set("ts", Json(ts));
    Json fv_groups = Json::arr();
    fv_groups.push(Json(gid));
    fv.set("groups", fv_groups);
    bool have = has_file(fid);
    fv.set("has_local", Json(have));
    Json ev = Json::obj();
    ev.set("file", fv);
    ev.set("gid", Json(gid));
    ev.set("from_self", Json(false));
    emit("file_new", {ev});

    if (have) return;
    {
        std::lock_guard<std::recursive_mutex> lk(mtx);
        if (downloading.count(fid) || active_downloads.count(fid)) return;
    }
    DlTask task;
    task.fid = fid;
    task.name = fname;
    task.size = fsize;
    task.gid = gid;
    task.gname = gname;
    task.sender_uid = sender_uid;
    task.sender_name = sender;
    task.attempt = 0;
    {
        std::lock_guard<std::recursive_mutex> lk(mtx);
        download_queue.push_back(task);
    }
}

// ================================================================ 同步请求 ==

void PeerCore::sync_req(const std::string& gid) {
    std::string gname;
    {
        std::lock_guard<std::recursive_mutex> lk(mtx);
        auto it = groups.find(gid);
        if (it != groups.end()) gname = it->second.get("gname").asStr();
    }
    Json m = Json::obj();
    m.set("t", Json("file_sync_req"));
    m.set("gid", Json(gid));
    m.set("gname", Json(gname));
    broadcast(m);
}

void PeerCore::on_sync_req(Json msg) {
    std::string gid = msg.get("gid").asStr();
    if (gid.empty()) return;
    if (!member_ok(msg.get("uid").asStr())) return;
    std::string gname = msg.get("gname").asStr();
    // 快照，避免广播期间长时间持锁
    std::vector<std::pair<std::string, Json>> owned;
    {
        std::lock_guard<std::recursive_mutex> lk(mtx);
        for (const auto& kv : index) {
            bool in = false;
            for (const Json& g : kv.second.get("groups").items()) {
                if (g.isStr() && g.asStr() == gid) { in = true; break; }
            }
            if (!in) continue;
            if (!truthy(kv.second.get("has_local"))) continue;
            std::error_code ec;
            if (!fs::is_regular_file(to_fspath(path_join(cache_dir(), kv.first)), ec)) continue;
            owned.push_back(kv);
        }
    }
    for (const auto& kv : owned) {
        const std::string& fid = kv.first;
        const Json& r = kv.second;
        Json file_obj = Json::obj();
        file_obj.set("id", Json(fid));
        file_obj.set("name", Json(str_or(r.get("name"), "?")));
        file_obj.set("size", Json(r.get("size").asInt(0)));
        Json m = Json::obj();
        m.set("t", Json("file_pub"));
        m.set("gid", Json(gid));
        m.set("gname", Json(gname));
        m.set("file", file_obj);
        m.set("ts", Json(now_ts()));
        m.set("resync", Json(true));
        m.set("sender_uid", Json(r.get("src_uid").asStr()));
        m.set("sender_name", Json(r.get("src_name").asStr()));
        broadcast(m);
    }
}

// ============================================================== 下载主逻辑 ==

void PeerCore::do_download(DlTask task, int attempt) {
    std::string fid = task.fid;
    {
        std::lock_guard<std::recursive_mutex> lk(mtx);
        downloading.insert(fid);
        Json t = Json::obj();
        active_downloads[fid] = t;   // 占位；下方立即写入真实任务字段
    }
    auto store_active = [&]() {
        std::lock_guard<std::recursive_mutex> lk(mtx);
        Json t = Json::obj();
        t.set("fid", Json(task.fid));
        t.set("name", Json(task.name));
        t.set("size", Json(task.size));
        t.set("gid", Json(task.gid));
        t.set("gname", Json(task.gname));
        t.set("sender_uid", Json(task.sender_uid));
        t.set("sender_name", Json(task.sender_name));
        t.set("attempt", Json(attempt));
        active_downloads[fid] = t;
    };
    store_active();

    Json st0 = Json::obj();
    st0.set("id", Json(fid));
    st0.set("status", Json("downloading"));
    st0.set("progress", Json(0.0));
    st0.set("note", Json("正在下载（点对点）..."));
    emit("file_status", {st0});
    emit("syschat", {Json(task.gid),
                     Json("检测到「" + task.gname + "」群「" + task.sender_name +
                          "」的新文件「" + task.name + "」，开始自动下载...")});

    // 1) 发送人在线 -> 点对点直连
    if (is_online(task.sender_uid)) {
        PeerInfo p;
        bool found = false;
        {
            std::lock_guard<std::recursive_mutex> lk(mtx);
            auto it = peers.find(task.sender_uid);
            if (it != peers.end()) { p = it->second; found = true; }
        }
        bool direct_ok = false;
        if (found) {
            try {
                direct_ok = fetch_from(p.ip, p.tcp_port, fid, task);
            } catch (...) {
                direct_ok = false;
            }
        }
        if (direct_ok) {
            finish_download(task);
            return;
        }
    }

    // 2) 离线/失败 -> 公共询问（日志全网可见），等第一个应答
    std::string req_id = new_msg_id(uid, msg_counter.fetch_add(1) + 1);
    public_request(task, req_id);
    double deadline = now_ts() + file_cfg().offer_wait;
    while (running.load() && now_ts() < deadline) {
        Json offer;
        bool have_offer = false;
        {
            std::lock_guard<std::recursive_mutex> lk(mtx);
            auto it = offers.find(req_id);
            if (it != offers.end() && !it->second.empty()) {
                offer = it->second.front();
                have_offer = true;
            }
        }
        if (have_offer) {
            std::string ip = (offer.isArr() && offer.size() > 0) ? offer.at(0).asStr() : std::string();
            int port = (offer.isArr() && offer.size() > 1)
                           ? static_cast<int>(offer.at(1).asInt(0)) : 0;
            bool ok = false;
            try {
                ok = fetch_from(ip, port, fid, task);
            } catch (...) {
                ok = false;
            }
            if (ok) {
                finish_download(task);
                std::lock_guard<std::recursive_mutex> lk(mtx);
                offers.erase(req_id);
                return;
            }
            // 第一个失效 -> 尝试下一个
            std::lock_guard<std::recursive_mutex> lk(mtx);
            auto it = offers.find(req_id);
            if (it != offers.end() && !it->second.empty()) it->second.erase(it->second.begin());
            continue;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }

    if (attempt < file_cfg().fallback_retry) {
        emit("syschat", {Json(task.gid),
                         Json("文件「" + task.name + "」暂无人可提供，稍后自动重试（" +
                              std::to_string(attempt + 1) + "/" +
                              std::to_string(file_cfg().fallback_retry) + "）...")});
        task.attempt = attempt + 1;
        std::lock_guard<std::recursive_mutex> lk(mtx);
        download_queue.push_back(task);
    } else {
        Json st = Json::obj();
        st.set("id", Json(fid));
        st.set("status", Json("error"));
        st.set("note", Json("发送人离线且无缓存应答，可稍后重试"));
        emit("file_status", {st});
        std::lock_guard<std::recursive_mutex> lk(mtx);
        active_downloads.erase(fid);
    }
}

void PeerCore::public_request(DlTask task, const std::string& req_id) {
    {
        std::lock_guard<std::recursive_mutex> lk(mtx);
        offers[req_id] = std::vector<Json>();
    }
    Json file_obj = Json::obj();
    file_obj.set("id", Json(task.fid));
    file_obj.set("name", Json(task.name));
    file_obj.set("size", Json(task.size));
    Json m = Json::obj();
    m.set("t", Json("file_req"));
    m.set("req_id", Json(req_id));
    m.set("gid", Json(task.gid));
    m.set("gname", Json(task.gname));
    m.set("sender_uid", Json(task.sender_uid));
    m.set("sender_name", Json(task.sender_name));
    m.set("file", file_obj);
    m.set("ts", Json(now_ts()));
    broadcast(m);
    // 本机也在日志里看到这条公共询问（请求方自己可见）
    emit("log", {Json("[" + fmt_time(now_ts()) + "] " + my_name() + ": 公共询问文件「" +
                      task.name + "」（发送人 " + str_or(Json(task.sender_name), "?") +
                      "，群「" + task.gname + "」）")});
}

void PeerCore::on_file_req(Json msg, const std::string& from_ip) {
    (void)from_ip;   // Python 同样不使用 addr
    Json f = msg.get("file");
    std::string fid = f.get("id").asStr();
    std::string req_id = msg.get("req_id").asStr();
    if (fid.empty() || req_id.empty()) return;
    std::string sender_name = str_or(msg.get("sender_name"), "?");
    emit("log", {Json("[" + fmt_time(msg.get("ts").asNum(now_ts())) + "] " +
                      str_or(msg.get("name"), "?") + ": 公共询问文件「" +
                      str_or(f.get("name"), "?") + "」（发送人 " + sender_name +
                      "，群「" + str_or(msg.get("gname"), "?") + "」）")});
    if (has_file(fid)) {
        std::vector<std::string> ips = local_ipv4_list();
        std::string ip = ips.empty() ? local_ipv4() : ips[0];
        Json m = Json::obj();
        m.set("t", Json("file_have"));
        m.set("req_id", Json(req_id));
        m.set("file_id", Json(fid));
        m.set("ip", Json(ip));
        m.set("port", Json(my_tcp_port()));
        m.set("ts", Json(now_ts()));
        broadcast(m);
    }
}

void PeerCore::on_file_have(Json msg) {
    std::string req_id = msg.get("req_id").asStr();
    std::lock_guard<std::recursive_mutex> lk(mtx);
    auto it = offers.find(req_id);
    if (it == offers.end()) return;
    Json tup = Json::arr();
    tup.push(Json(msg.get("ip").asStr()));
    tup.push(Json(static_cast<long long>(msg.get("port").asInt(0))));
    tup.push(Json(msg.get("uid").asStr()));
    it->second.push_back(tup);
}

// ============================================================== 从某处下载 ==

bool PeerCore::fetch_from(const std::string& ip, int port, const std::string& fid, const DlTask& task) {
    (void)task;
    if (ip.empty() || port == 0) return false;
    SOCKET sock = tcp_connect(ip, port, 8000);   // 连接 + 收头部超时 8 秒
    if (sock == INVALID_SOCKET) return false;
    long long size = 0;
    try {
        Json req = Json::obj();
        req.set("cmd", Json("get"));
        req.set("file_id", Json(fid));
        if (!send_json(sock, req)) throw std::runtime_error("发送请求失败");
        Json hdr;
        if (!recv_json(sock, hdr)) throw std::runtime_error("接收应答失败");
        if (hdr.get("cmd").asStr() != "data") {
            throw std::runtime_error("对方应答：" + str_or(hdr.get("err"), "未知错误"));
        }
        size = hdr.get("size").asInt(0);
    } catch (...) {
        set_sock_timeout(sock, 60);   // finally: 无论成败都改成 60 秒
        close_socket(sock);
        throw;
    }
    set_sock_timeout(sock, 60);       // 正文接收超时 60 秒

    std::string tmp = path_join(cache_dir(), fid + ".part");
    Sha256Ctx h;
    h.init();
    long long done = 0;
    double last_emit = 0.0;
    bool io_error = false;
    {
        std::ofstream out(to_fspath(tmp), std::ios::binary | std::ios::trunc);
        if (!out) {
            close_socket(sock);
            throw std::runtime_error("无法写临时文件");
        }
        std::vector<char> buf(chunk_size());
        while (running.load()) {
            int n = ::recv(sock, buf.data(), static_cast<int>(buf.size()), 0);
            if (n <= 0) {
                if (n < 0) io_error = true;   // 超时 / 连接错误
                break;
            }
            out.write(buf.data(), n);
            h.update(buf.data(), static_cast<size_t>(n));
            done += n;
            double prog = size ? (static_cast<double>(done) / static_cast<double>(size)) : 1.0;
            if (now_ts() - last_emit > 0.2 || prog >= 1.0) {
                Json st = Json::obj();
                st.set("id", Json(fid));
                st.set("status", Json("downloading"));
                st.set("progress", Json((std::min)(prog, 1.0)));
                st.set("note", Json("正在下载..."));
                emit("file_status", {st});
                last_emit = now_ts();
            }
        }
    }
    close_socket(sock);
    if (io_error) throw std::runtime_error("接收数据超时");
    if (done != size) {
        throw std::runtime_error("数据不完整：期望 " + std::to_string(size) +
                                 "，实际 " + std::to_string(done));
    }
    if (h.hex() != fid) throw std::runtime_error("文件校验失败（指纹不一致）");

    std::string final_path = path_join(cache_dir(), fid);
    std::error_code ec;
    if (fs::exists(to_fspath(final_path), ec)) remove_file(final_path);
    std::error_code ec2;
    fs::rename(to_fspath(tmp), to_fspath(final_path), ec2);
    if (ec2) {
        // 跨卷等场景兜底：复制后删除
        if (!copy_file(tmp, final_path, true)) throw std::runtime_error("保存文件失败");
        remove_file(tmp);
    }
    return true;
}

void PeerCore::finish_download(const DlTask& task) {
    std::string fid = task.fid;
    std::string cache_file = path_join(cache_dir(), fid);
    std::string gdir = downloads_dir(task.gid, task.gname);
    std::string name = sanitize_name(task.name.empty() ? std::string("file") : task.name);
    std::string target = path_join(gdir, name);
    if (file_exists(target) && file_size(target) != task.size) {
        std::pair<std::string, std::string> se = split_ext(name);
        int n = 1;
        while (file_exists(target)) {
            target = path_join(gdir, se.first + " (" + std::to_string(n) + ")" + se.second);
            ++n;
        }
    }
    copy_file(cache_file, target, true);

    Json rec = Json::obj();
    rec.set("name", Json(name));
    rec.set("size", Json(task.size));
    rec.set("ts", Json(now_ts()));
    rec.set("src_uid", Json(task.sender_uid));
    rec.set("src_name", Json(task.sender_name));
    Json gs = Json::arr();
    gs.push(Json(task.gid));
    rec.set("groups", gs);
    rec.set("has_local", Json(true));
    rec.set("local_path", Json(target));
    register_file(fid, rec);

    {
        std::lock_guard<std::recursive_mutex> lk(mtx);
        active_downloads.erase(fid);
    }

    // 系统 API 通知：按你要求的格式
    emit("notify", {Json("文件下载完成"),
                    Json("「" + task.gname + "」群「" + str_or(Json(task.sender_name), "?") +
                         "」的「" + task.name + "」下载完毕")});
    Json st = Json::obj();
    st.set("id", Json(fid));
    st.set("status", Json("done"));
    st.set("path", Json(target));
    st.set("note", Json("已下载"));
    emit("file_status", {st});
    emit("syschat", {Json(task.gid), Json("文件「" + task.name + "」下载完毕：" + target)});
    broadcast_log("「" + my_name() + "」下载了「" + task.gname + "」群「" +
                  str_or(Json(task.sender_name), "?") + "」的文件「" + task.name + "」");
}

// ================================================================ TCP 服务 ==

void PeerCore::handle_conn(SOCKET conn, const std::string& peer_ip) {
    (void)peer_ip;   // Python 侧同样不使用 addr
    if (conn == INVALID_SOCKET) return;
    set_sock_timeout(conn, 30);
    try {
        Json req;
        if (!recv_json(conn, req)) throw std::runtime_error("接收请求失败");
        std::string cmd = req.get("cmd").asStr();
        if (cmd == "get") {
            std::string fid = req.get("file_id").asStr();
            std::string serve_path;
            long long serve_size = 0;
            bool found = false;
            if (!fid.empty()) {
                std::string cache_file = path_join(cache_dir(), fid);
                if (file_exists(cache_file) && !is_dir(cache_file)) {
                    serve_path = cache_file;
                    found = true;
                } else {
                    // ---- v2.0.0：代理文件按同样格式提供 ----
                    std::string pid = str_or(req.get("proxy_id"), "");
                    if (!pid.empty()) {
                        ProxyRec prec;
                        bool have_rec = false;
                        {
                            std::lock_guard<std::recursive_mutex> lk(mtx);
                            auto it = proxies.find(pid);
                            if (it != proxies.end()) { prec = it->second; have_rec = true; }
                        }
                        std::string ppath = path_join(proxy_dir(), pid + ".proxy");
                        if (have_rec && prec.file_id == fid && file_exists(ppath) && !is_dir(ppath)) {
                            serve_path = ppath;
                            found = true;
                        }
                    }
                }
                if (found) serve_size = file_size(serve_path);
            }
            if (!fid.empty() && found) {
                // 与 Python 一致：name 恒取 self.index.get(fid, {}).get("name", "")
                std::string serve_name;
                {
                    std::lock_guard<std::recursive_mutex> lk(mtx);
                    auto it = index.find(fid);
                    if (it != index.end()) serve_name = it->second.get("name").asStr();
                }
                Json resp = Json::obj();
                resp.set("cmd", Json("data"));
                resp.set("file_id", Json(fid));
                resp.set("name", Json(serve_name));
                resp.set("size", Json(serve_size));
                send_json(conn, resp);
                send_file_body(conn, serve_path);
            } else {
                Json resp = Json::obj();
                resp.set("cmd", Json("err"));
                resp.set("err", Json("没有该文件"));
                send_json(conn, resp);
            }
        } else if (cmd == "secret_store") {
            on_secret_store(req);
            Json resp = Json::obj();
            resp.set("cmd", Json("secret_stored"));
            resp.set("store_id", Json(req.get("store_id").asStr()));
            send_json(conn, resp);
        } else if (cmd == "secret") {
            bool ok = on_secret_deliver(req);
            Json resp = Json::obj();
            resp.set("cmd", Json("secret_ack"));
            resp.set("store_id", Json(req.get("store_id").asStr()));
            resp.set("ok", Json(ok));
            send_json(conn, resp);
        } else if (cmd == "act_get") {
            // v2.0.0：由 activity agent 实现，应答帧由它自己发送
            act_on_get(req, conn);
        } else if (cmd == "proxy_store") {
            // v2.0.0：由 proxy agent 实现，应答帧由它自己发送
            proxy_store_received(req, conn);
        } else if (cmd == "proxy_push") {
            // v2.0.0：由 proxy agent 实现，应答帧由它自己发送
            proxy_push_received(req, conn);
        } else {
            Json resp = Json::obj();
            resp.set("cmd", Json("err"));
            resp.set("err", Json("未知命令"));
            send_json(conn, resp);
        }
    } catch (...) {
        // 任何异常被静默吞掉
    }
    close_socket(conn);
}

// ================================================================ 保密转存 ==

void PeerCore::relay_dm_worker(const std::string& to_uid, const std::string& text) {
    std::vector<std::pair<std::string, PeerInfo>> cands;
    {
        std::lock_guard<std::recursive_mutex> lk(mtx);
        for (const auto& kv : peers) {
            if (kv.first == to_uid || kv.first == uid) continue;
            cands.push_back(kv);
        }
    }
    // 过滤在线 + 随机顺序（Python: random.shuffle）
    std::vector<std::pair<std::string, PeerInfo>> online_cands;
    for (const auto& c : cands) {
        if (is_online(c.first)) online_cands.push_back(c);
    }
    std::random_device rd;
    std::mt19937 rng(rd());
    std::shuffle(online_cands.begin(), online_cands.end(), rng);

    for (const auto& c : online_cands) {
        const std::string& puid = c.first;
        const PeerInfo& p = c.second;
        std::string store_id = new_msg_id(uid, msg_counter.fetch_add(1) + 1);
        std::string key = random_bytes(32);
        Json rec = Json::obj();
        rec.set("to_uid", Json(to_uid));
        rec.set("from_uid", Json(uid));
        rec.set("from_name", Json(my_name()));
        rec.set("blob", Json(secret_encrypt(text, key)));
        rec.set("key", Json(hex_encode(key)));
        rec.set("sig", Json(secret_sign(key, {to_uid, uid, store_id, text})));
        double rts = now_ts();
        rec.set("ts", Json(rts));
        rec.set("retries", Json(static_cast<long long>(0)));
        if (store_secret_on(p, store_id, rec)) {
            Json ev = Json::obj();
            ev.set("to_uid", Json(to_uid));
            ev.set("ok", Json(true));
            ev.set("direct", Json(false));
            ev.set("relay_name", Json(p.name.empty() ? std::string("?") : p.name));
            ev.set("text", Json(text));
            ev.set("ts", Json(rts));
            emit("dm_sent", {ev});
            (void)puid;
            return;
        }
    }
    Json ev = Json::obj();
    ev.set("to_uid", Json(to_uid));
    ev.set("ok", Json(false));
    ev.set("reason", Json("对方不在线，且没有其他在线同学可代为转存"));
    emit("dm_sent", {ev});
}

bool PeerCore::store_secret_on(const PeerInfo& peer, const std::string& store_id, Json rec) {
    const std::string& ip = peer.ip;
    int port = peer.tcp_port;
    if (ip.empty() || port == 0) return false;
    SOCKET sock = tcp_connect(ip, port, 6000);
    if (sock == INVALID_SOCKET) return false;
    bool ok = false;
    try {
        Json req = Json::obj();
        req.set("cmd", Json("secret_store"));
        req.set("store_id", Json(store_id));
        for (const std::string& k : rec.keys()) req.set(k, rec.get(k));
        if (!send_json(sock, req)) {
            close_socket(sock);
            return false;
        }
        Json resp;
        if (recv_json(sock, resp)) {
            ok = (resp.get("cmd").asStr() == "secret_stored" &&
                  resp.get("store_id").asStr() == store_id);
        }
    } catch (...) {
        ok = false;
    }
    close_socket(sock);
    return ok;
}

void PeerCore::on_presence_hook_deliver(const std::string& uid) {
    {
        std::lock_guard<std::recursive_mutex> lk(mtx);
        if (delivering.count(uid)) return;   // 并发去重
        delivering.insert(uid);
    }
    PeerCore* self = this;
    std::thread t([self, uid]() {
        try {
            self->deliver_secrets_to(uid);
        } catch (...) {
        }
    });
    {
        std::lock_guard<std::recursive_mutex> lk(mtx);
        threads.push_back(std::move(t));
    }
}

void PeerCore::deliver_secrets_to(const std::string& uid) {
    try {
        std::vector<std::pair<std::string, Json>> entries;
        PeerInfo p;
        {
            std::lock_guard<std::recursive_mutex> lk(mtx);
            for (const auto& kv : secrets) {
                if (kv.second.get("to_uid").asStr() == uid) entries.push_back(kv);
            }
            auto it = peers.find(uid);
            if (it != peers.end()) p = it->second;
        }
        if (entries.empty()) return;
        for (const auto& e : entries) {
            const std::string& sid = e.first;
            Json rec = e.second;
            bool ok = deliver_one(p, sid, rec);
            {
                std::lock_guard<std::recursive_mutex> lk(mtx);
                auto it = secrets.find(sid);
                if (ok) {
                    secrets.erase(sid);
                } else if (it != secrets.end()) {
                    long long retries = it->second.get("retries").asInt(0) + 1;
                    it->second.set("retries", Json(retries));
                    if (retries > file_cfg().secret_relay_retry) secrets.erase(sid);
                }
            }
        }
        save_secrets();
    } catch (...) {
    }
    {
        std::lock_guard<std::recursive_mutex> lk(mtx);
        delivering.erase(uid);
    }
}

bool PeerCore::deliver_one(const PeerInfo& peer, const std::string& store_id, Json rec) {
    if (peer.ip.empty() || peer.tcp_port == 0) return false;
    SOCKET sock = tcp_connect(peer.ip, peer.tcp_port, 8000);
    if (sock == INVALID_SOCKET) return false;
    bool ok = false;
    try {
        Json req = Json::obj();
        req.set("cmd", Json("secret"));
        req.set("store_id", Json(store_id));
        req.set("to_uid", Json(rec.get("to_uid").asStr()));
        req.set("from_uid", Json(rec.get("from_uid").asStr()));
        req.set("from_name", Json(rec.get("from_name").asStr()));
        req.set("blob", Json(rec.get("blob").asStr()));
        req.set("key", Json(rec.get("key").asStr()));
        req.set("sig", Json(rec.get("sig").asStr()));
        req.set("ts", Json(rec.get("ts").asNum(0.0)));
        if (!send_json(sock, req)) {
            close_socket(sock);
            return false;
        }
        Json resp;
        if (recv_json(sock, resp)) {
            ok = (resp.get("cmd").asStr() == "secret_ack") && resp.get("ok").asBool(false);
        }
    } catch (...) {
        ok = false;
    }
    close_socket(sock);
    return ok;
}

void PeerCore::on_secret_store(Json req) {
    std::string sid = req.get("store_id").asStr();
    if (sid.empty()) return;
    if (req.get("to_uid").asStr().empty()) return;
    if (req.get("blob").asStr().empty()) return;
    if (req.get("key").asStr().empty()) return;
    {
        std::lock_guard<std::recursive_mutex> lk(mtx);
        if (secrets.count(sid)) return;   // 重复存不覆盖
    }
    Json rec = Json::obj();
    rec.set("to_uid", Json(req.get("to_uid").asStr()));
    rec.set("from_uid", Json(req.get("from_uid").asStr()));
    rec.set("from_name", Json(req.get("from_name").asStr()));
    rec.set("blob", Json(req.get("blob").asStr()));
    rec.set("key", Json(req.get("key").asStr()));
    rec.set("sig", Json(req.get("sig").asStr()));
    rec.set("ts", Json(req.has("ts") ? req.get("ts").asNum(now_ts()) : now_ts()));
    rec.set("retries", Json(static_cast<long long>(0)));
    {
        std::lock_guard<std::recursive_mutex> lk(mtx);
        secrets[sid] = rec;
    }
    save_secrets();
}

bool PeerCore::on_secret_deliver(Json req) {
    std::string sid = req.get("store_id").asStr();
    if (req.get("to_uid").asStr() != uid) return false;
    {
        std::lock_guard<std::recursive_mutex> lk(mtx);
        if (!sid.empty() && recv_secrets.count(sid)) return true;   // 已收过：幂等回执
    }
    // Python bytes.fromhex 失败 -> 异常 -> 返回 false
    std::string key;
    if (!hex_decode(req.get("key").asStr(), key)) return false;
    bool dok = false;
    std::string text = secret_decrypt(req.get("blob").asStr(), key, &dok);
    if (!dok) return false;
    std::string expect = secret_sign(key, {req.get("to_uid").asStr(),
                                           req.get("from_uid").asStr(), sid, text});
    if (!const_time_eq(req.get("sig").asStr(), expect)) return false;
    if (!sid.empty()) {
        {
            std::lock_guard<std::recursive_mutex> lk(mtx);
            recv_secrets.insert(sid);
            if (recv_secrets.size() > 2000) {
                // 保留字典序最大的 2000 个
                std::vector<std::string> v(recv_secrets.begin(), recv_secrets.end());
                std::sort(v.begin(), v.end());
                recv_secrets = std::set<std::string>(v.end() - 2000, v.end());
            }
        }
        save_recv_secrets();
    }
    Json ev = Json::obj();
    ev.set("to_uid", Json(uid));
    ev.set("from_uid", Json(req.get("from_uid").asStr()));
    ev.set("from_name", Json(str_or(req.get("from_name"), "?")));
    ev.set("text", Json(text));
    ev.set("ts", Json(req.has("ts") ? req.get("ts").asNum(now_ts()) : now_ts()));
    ev.set("self", Json(false));
    ev.set("relayed", Json(true));
    emit("dm", {ev});
    return true;
}

} // namespace nw
